#include "common/audio_stream.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <mutex>

extern "C"
{
#include <libavcodec/avcodec.h>
#include <libavutil/channel_layout.h>
#include <libavutil/error.h>
#include <libavutil/mathematics.h>
#include <libavutil/opt.h>
#include <libavutil/samplefmt.h>
#include <libswresample/swresample.h>
}

namespace streamproto::audio
{
	namespace
	{
		std::string AvErrorString(int err)
		{
			char buffer[AV_ERROR_MAX_STRING_SIZE] = {};
			av_strerror(err, buffer, sizeof(buffer));
			return std::string(buffer);
		}

		std::int16_t FloatToS16(float value)
		{
			value = std::clamp(value, -1.0f, 1.0f);
			return static_cast<std::int16_t>(std::lrintf(value * 32767.0f));
		}

		bool IsPlanarSampleFormat(AVSampleFormat fmt)
		{
			return av_sample_fmt_is_planar(fmt) != 0;
		}

		AVSampleFormat PickOpusSampleFormat(const AVCodec *codec)
		{
			if (codec == nullptr || codec->sample_fmts == nullptr)
			{
				return AV_SAMPLE_FMT_FLT;
			}

			const AVSampleFormat preferred[] = {
				AV_SAMPLE_FMT_FLT,
				AV_SAMPLE_FMT_FLTP,
				AV_SAMPLE_FMT_S16,
				AV_SAMPLE_FMT_S16P,
			};
			for (AVSampleFormat want : preferred)
			{
				for (const AVSampleFormat *fmt = codec->sample_fmts; *fmt != AV_SAMPLE_FMT_NONE; ++fmt)
				{
					if (*fmt == want)
					{
						return want;
					}
				}
			}
			return codec->sample_fmts[0];
		}

		const AVCodec *FindOpusEncoder(std::string &active_name)
		{
			if (const AVCodec *codec = avcodec_find_encoder_by_name("libopus"))
			{
				active_name = "libopus";
				return codec;
			}
			if (const AVCodec *codec = avcodec_find_encoder(AV_CODEC_ID_OPUS))
			{
				active_name = codec->name != nullptr ? codec->name : "opus";
				return codec;
			}
			return nullptr;
		}

		const AVCodec *FindOpusDecoder(std::string &active_name)
		{
			if (const AVCodec *codec = avcodec_find_decoder_by_name("libopus"))
			{
				active_name = "libopus";
				return codec;
			}
			if (const AVCodec *codec = avcodec_find_decoder(AV_CODEC_ID_OPUS))
			{
				active_name = codec->name != nullptr ? codec->name : "opus";
				return codec;
			}
			return nullptr;
		}

	} // namespace

	int ClampAudioBitrateKbps(int bitrate_kbps)
	{
		return std::clamp(bitrate_kbps <= 0 ? kDefaultBitrateKbps : bitrate_kbps, 32, 512);
	}

	PcmRingBuffer::PcmRingBuffer(std::size_t capacity_samples) : data_(std::max<std::size_t>(capacity_samples, 1)) {}

	void PcmRingBuffer::Push(const float *samples, std::size_t sample_count)
	{
		if (samples == nullptr || sample_count == 0 || data_.empty())
		{
			return;
		}

		std::lock_guard<std::mutex> lock(mutex_);
		if (sample_count >= data_.size())
		{
			samples += sample_count - data_.size();
			sample_count = data_.size();
			read_pos_ = 0;
			write_pos_ = 0;
			size_ = 0;
		}

		const std::size_t free_samples = data_.size() - size_;
		if (sample_count > free_samples)
		{
			const std::size_t drop = sample_count - free_samples;
			read_pos_ = (read_pos_ + drop) % data_.size();
			size_ -= drop;
		}

		std::size_t remaining = sample_count;
		while (remaining > 0)
		{
			const std::size_t contiguous = std::min(remaining, data_.size() - write_pos_);
			std::memcpy(data_.data() + write_pos_, samples + (sample_count - remaining), contiguous * sizeof(float));
			write_pos_ = (write_pos_ + contiguous) % data_.size();
			size_ += contiguous;
			remaining -= contiguous;
		}
	}

	std::size_t PcmRingBuffer::Read(float *out_samples, std::size_t max_samples)
	{
		if (out_samples == nullptr || max_samples == 0 || data_.empty())
		{
			return 0;
		}

		std::lock_guard<std::mutex> lock(mutex_);
		const std::size_t to_read = std::min(max_samples, size_);
		std::size_t remaining = to_read;
		while (remaining > 0)
		{
			const std::size_t contiguous = std::min(remaining, data_.size() - read_pos_);
			std::memcpy(out_samples + (to_read - remaining), data_.data() + read_pos_, contiguous * sizeof(float));
			read_pos_ = (read_pos_ + contiguous) % data_.size();
			size_ -= contiguous;
			remaining -= contiguous;
		}
		return to_read;
	}

	std::size_t PcmRingBuffer::ReadLatest(float *out_samples, std::size_t max_samples, std::size_t max_buffered_samples)
	{
		if (out_samples == nullptr || max_samples == 0 || data_.empty())
		{
			return 0;
		}

		std::lock_guard<std::mutex> lock(mutex_);
		max_buffered_samples = std::max(max_buffered_samples, max_samples);
		if (size_ > max_buffered_samples)
		{
			const std::size_t drop = size_ - max_buffered_samples;
			read_pos_ = (read_pos_ + drop) % data_.size();
			size_ -= drop;
		}

		const std::size_t to_read = std::min(max_samples, size_);
		std::size_t remaining = to_read;
		while (remaining > 0)
		{
			const std::size_t contiguous = std::min(remaining, data_.size() - read_pos_);
			std::memcpy(out_samples + (to_read - remaining), data_.data() + read_pos_, contiguous * sizeof(float));
			read_pos_ = (read_pos_ + contiguous) % data_.size();
			size_ -= contiguous;
			remaining -= contiguous;
		}
		return to_read;
	}

	void PcmRingBuffer::Clear()
	{
		std::lock_guard<std::mutex> lock(mutex_);
		read_pos_ = 0;
		write_pos_ = 0;
		size_ = 0;
	}

	std::size_t PcmRingBuffer::Available() const
	{
		std::lock_guard<std::mutex> lock(mutex_);
		return size_;
	}

	struct FfmpegOpusEncoder::Impl
	{
		AVCodecContext *ctx = nullptr;
		AVFrame *frame = nullptr;
		AVSampleFormat sample_fmt = AV_SAMPLE_FMT_NONE;
		int frame_samples = kFrameSamples;
		int bitrate_kbps = kDefaultBitrateKbps;
		std::int64_t next_pts = 0;
		std::string active_encoder;
	};

	FfmpegOpusEncoder::FfmpegOpusEncoder() = default;

	FfmpegOpusEncoder::~FfmpegOpusEncoder()
	{
		Shutdown();
	}

	bool FfmpegOpusEncoder::Init(int bitrate_kbps, int frame_samples, std::string &error)
	{
		Shutdown();

		std::string active_name;
		const AVCodec *codec = FindOpusEncoder(active_name);
		if (codec == nullptr)
		{
			error = "No FFmpeg Opus encoder found.";
			return false;
		}

		AVCodecContext *ctx = avcodec_alloc_context3(codec);
		if (ctx == nullptr)
		{
			error = "avcodec_alloc_context3(Opus encoder) failed.";
			return false;
		}

		const int clamped_bitrate = ClampAudioBitrateKbps(bitrate_kbps);
		frame_samples = std::clamp(frame_samples <= 0 ? kFrameSamples : frame_samples, 120, 2880);

		ctx->sample_rate = kSampleRate;
		ctx->sample_fmt = PickOpusSampleFormat(codec);
		ctx->bit_rate = static_cast<std::int64_t>(clamped_bitrate) * 1000LL;
		ctx->time_base = AVRational{1, kSampleRate};
		av_channel_layout_default(&ctx->ch_layout, kChannels);

		if (ctx->priv_data != nullptr)
		{
			av_opt_set(ctx->priv_data, "application", "lowdelay", 0);
			av_opt_set(ctx->priv_data, "vbr", "off", 0);
			av_opt_set(ctx->priv_data, "frame_duration", std::to_string(kFrameMs).c_str(), 0);
			av_opt_set_int(ctx->priv_data, "packet_loss", 0, 0);
		}

		int open = avcodec_open2(ctx, codec, nullptr);
		if (open < 0)
		{
			error = "avcodec_open2(Opus encoder) failed: " + AvErrorString(open);
			avcodec_free_context(&ctx);
			return false;
		}

		int effective_frame_samples = ctx->frame_size > 0 ? ctx->frame_size : frame_samples;
		if (effective_frame_samples <= 0)
		{
			effective_frame_samples = kFrameSamples;
		}

		AVFrame *frame = av_frame_alloc();
		if (frame == nullptr)
		{
			error = "av_frame_alloc(Opus encoder frame) failed.";
			avcodec_free_context(&ctx);
			return false;
		}
		frame->nb_samples = effective_frame_samples;
		frame->format = ctx->sample_fmt;
		frame->sample_rate = kSampleRate;
		if (av_channel_layout_copy(&frame->ch_layout, &ctx->ch_layout) < 0)
		{
			av_frame_free(&frame);
			avcodec_free_context(&ctx);
			error = "av_channel_layout_copy(Opus encoder frame) failed.";
			return false;
		}
		int buffer = av_frame_get_buffer(frame, 0);
		if (buffer < 0)
		{
			av_frame_free(&frame);
			avcodec_free_context(&ctx);
			error = "av_frame_get_buffer(Opus encoder frame) failed: " + AvErrorString(buffer);
			return false;
		}

		impl_ = std::make_unique<Impl>();
		impl_->ctx = ctx;
		impl_->frame = frame;
		impl_->sample_fmt = ctx->sample_fmt;
		impl_->frame_samples = effective_frame_samples;
		impl_->bitrate_kbps = clamped_bitrate;
		impl_->active_encoder = active_name;
		return true;
	}

	bool FfmpegOpusEncoder::Encode(const float *interleaved_samples, int sample_frames, std::vector<std::uint8_t> &out_packet)
	{
		out_packet.clear();
		if (impl_ == nullptr || impl_->ctx == nullptr || impl_->frame == nullptr ||
			interleaved_samples == nullptr || sample_frames != impl_->frame_samples)
		{
			return false;
		}

		if (av_frame_make_writable(impl_->frame) < 0)
		{
			return false;
		}

		const AVSampleFormat fmt = impl_->sample_fmt;
		const bool planar = IsPlanarSampleFormat(fmt);
		if (fmt == AV_SAMPLE_FMT_FLT && !planar)
		{
			std::memcpy(impl_->frame->data[0], interleaved_samples, static_cast<std::size_t>(sample_frames * kChannels) * sizeof(float));
		}
		else if (fmt == AV_SAMPLE_FMT_FLTP && planar)
		{
			auto *left = reinterpret_cast<float *>(impl_->frame->data[0]);
			auto *right = reinterpret_cast<float *>(impl_->frame->data[1]);
			for (int i = 0; i < sample_frames; ++i)
			{
				left[i] = interleaved_samples[i * 2];
				right[i] = interleaved_samples[i * 2 + 1];
			}
		}
		else if (fmt == AV_SAMPLE_FMT_S16 && !planar)
		{
			auto *dst = reinterpret_cast<std::int16_t *>(impl_->frame->data[0]);
			for (int i = 0; i < sample_frames * kChannels; ++i)
			{
				dst[i] = FloatToS16(interleaved_samples[i]);
			}
		}
		else if (fmt == AV_SAMPLE_FMT_S16P && planar)
		{
			auto *left = reinterpret_cast<std::int16_t *>(impl_->frame->data[0]);
			auto *right = reinterpret_cast<std::int16_t *>(impl_->frame->data[1]);
			for (int i = 0; i < sample_frames; ++i)
			{
				left[i] = FloatToS16(interleaved_samples[i * 2]);
				right[i] = FloatToS16(interleaved_samples[i * 2 + 1]);
			}
		}
		else
		{
			return false;
		}

		impl_->frame->pts = impl_->next_pts;
		impl_->next_pts += sample_frames;

		if (avcodec_send_frame(impl_->ctx, impl_->frame) < 0)
		{
			return false;
		}

		bool got_packet = false;
		while (true)
		{
			AVPacket *packet = av_packet_alloc();
			if (packet == nullptr)
			{
				return false;
			}

			int recv = avcodec_receive_packet(impl_->ctx, packet);
			if (recv == AVERROR(EAGAIN) || recv == AVERROR_EOF)
			{
				av_packet_free(&packet);
				break;
			}
			if (recv < 0)
			{
				av_packet_free(&packet);
				return false;
			}

			if (!got_packet && packet->data != nullptr && packet->size > 0)
			{
				out_packet.assign(packet->data, packet->data + packet->size);
				got_packet = true;
			}
			av_packet_free(&packet);
		}
		return got_packet;
	}

	void FfmpegOpusEncoder::Shutdown()
	{
		if (impl_ == nullptr)
		{
			return;
		}
		if (impl_->frame != nullptr)
		{
			av_frame_free(&impl_->frame);
		}
		if (impl_->ctx != nullptr)
		{
			avcodec_free_context(&impl_->ctx);
		}
		impl_.reset();
	}

	bool FfmpegOpusEncoder::Ready() const
	{
		return impl_ != nullptr && impl_->ctx != nullptr && impl_->frame != nullptr;
	}

	int FfmpegOpusEncoder::BitrateKbps() const
	{
		return impl_ != nullptr ? impl_->bitrate_kbps : 0;
	}

	int FfmpegOpusEncoder::FrameSamples() const
	{
		return impl_ != nullptr ? impl_->frame_samples : kFrameSamples;
	}

	const std::string &FfmpegOpusEncoder::ActiveEncoder() const
	{
		static const std::string empty;
		return impl_ != nullptr ? impl_->active_encoder : empty;
	}

	struct FfmpegOpusDecoder::Impl
	{
		AVCodecContext *ctx = nullptr;
		AVFrame *frame = nullptr;
		SwrContext *swr = nullptr;
		AVSampleFormat swr_input_format = AV_SAMPLE_FMT_NONE;
		int swr_input_rate = 0;
		AVChannelLayout swr_input_layout{};
		bool has_swr_input_layout = false;
		std::string active_decoder;
	};

	FfmpegOpusDecoder::FfmpegOpusDecoder() = default;

	FfmpegOpusDecoder::~FfmpegOpusDecoder()
	{
		Shutdown();
	}

	bool FfmpegOpusDecoder::Init(const proto::AudioConfigMessage &config, std::string &error)
	{
		Shutdown();
		if (config.codec != static_cast<std::uint16_t>(proto::AudioCodec::kOpus) ||
			config.sample_rate != kSampleRate ||
			config.channels != kChannels)
		{
			error = "Unsupported audio stream config.";
			return false;
		}

		std::string active_name;
		const AVCodec *codec = FindOpusDecoder(active_name);
		if (codec == nullptr)
		{
			error = "No FFmpeg Opus decoder found.";
			return false;
		}

		AVCodecContext *ctx = avcodec_alloc_context3(codec);
		if (ctx == nullptr)
		{
			error = "avcodec_alloc_context3(Opus decoder) failed.";
			return false;
		}
		ctx->sample_rate = kSampleRate;
		av_channel_layout_default(&ctx->ch_layout, kChannels);

		int open = avcodec_open2(ctx, codec, nullptr);
		if (open < 0)
		{
			error = "avcodec_open2(Opus decoder) failed: " + AvErrorString(open);
			avcodec_free_context(&ctx);
			return false;
		}

		AVFrame *frame = av_frame_alloc();
		if (frame == nullptr)
		{
			avcodec_free_context(&ctx);
			error = "av_frame_alloc(Opus decoder frame) failed.";
			return false;
		}

		impl_ = std::make_unique<Impl>();
		impl_->ctx = ctx;
		impl_->frame = frame;
		impl_->active_decoder = active_name;
		return true;
	}

	bool FfmpegOpusDecoder::Decode(const std::uint8_t *data, std::size_t bytes, std::vector<float> &out_interleaved)
	{
		out_interleaved.clear();
		if (impl_ == nullptr || impl_->ctx == nullptr || impl_->frame == nullptr ||
			data == nullptr || bytes == 0 || bytes > static_cast<std::size_t>(std::numeric_limits<int>::max()))
		{
			return false;
		}

		AVPacket *packet = av_packet_alloc();
		if (packet == nullptr)
		{
			return false;
		}
		if (av_new_packet(packet, static_cast<int>(bytes)) < 0)
		{
			av_packet_free(&packet);
			return false;
		}
		std::memcpy(packet->data, data, bytes);

		int send = avcodec_send_packet(impl_->ctx, packet);
		av_packet_free(&packet);
		if (send < 0)
		{
			return false;
		}

		bool decoded_any = false;
		while (true)
		{
			int recv = avcodec_receive_frame(impl_->ctx, impl_->frame);
			if (recv == AVERROR(EAGAIN) || recv == AVERROR_EOF)
			{
				break;
			}
			if (recv < 0)
			{
				return false;
			}

			AVChannelLayout input_layout{};
			bool input_layout_owned = false;
			if (impl_->frame->ch_layout.nb_channels > 0)
			{
				if (av_channel_layout_copy(&input_layout, &impl_->frame->ch_layout) < 0)
				{
					av_frame_unref(impl_->frame);
					return false;
				}
				input_layout_owned = true;
			}
			else
			{
				av_channel_layout_default(&input_layout, kChannels);
				input_layout_owned = true;
			}

			const AVSampleFormat input_format = static_cast<AVSampleFormat>(impl_->frame->format);
			const int input_rate = impl_->frame->sample_rate > 0 ? impl_->frame->sample_rate : kSampleRate;
			const bool recreate_swr =
				impl_->swr == nullptr ||
				impl_->swr_input_format != input_format ||
				impl_->swr_input_rate != input_rate ||
				!impl_->has_swr_input_layout ||
				av_channel_layout_compare(&impl_->swr_input_layout, &input_layout) != 0;

			if (recreate_swr)
			{
				if (impl_->swr != nullptr)
				{
					swr_free(&impl_->swr);
				}
				if (impl_->has_swr_input_layout)
				{
					av_channel_layout_uninit(&impl_->swr_input_layout);
					impl_->has_swr_input_layout = false;
				}

				AVChannelLayout output_layout{};
				av_channel_layout_default(&output_layout, kChannels);
				int alloc = swr_alloc_set_opts2(
					&impl_->swr,
					&output_layout,
					AV_SAMPLE_FMT_FLT,
					kSampleRate,
					&input_layout,
					input_format,
					input_rate,
					0,
					nullptr);
				av_channel_layout_uninit(&output_layout);
				if (alloc < 0 || impl_->swr == nullptr || swr_init(impl_->swr) < 0)
				{
					if (impl_->swr != nullptr)
					{
						swr_free(&impl_->swr);
					}
					if (input_layout_owned)
					{
						av_channel_layout_uninit(&input_layout);
					}
					av_frame_unref(impl_->frame);
					return false;
				}
				impl_->swr_input_format = input_format;
				impl_->swr_input_rate = input_rate;
				if (av_channel_layout_copy(&impl_->swr_input_layout, &input_layout) == 0)
				{
					impl_->has_swr_input_layout = true;
				}
			}

			const int out_samples = std::max(1, swr_get_out_samples(impl_->swr, impl_->frame->nb_samples));
			std::vector<float> converted(static_cast<std::size_t>(out_samples * kChannels));
			std::uint8_t *out_data[1] = {reinterpret_cast<std::uint8_t *>(converted.data())};
			const std::uint8_t *in_data[AV_NUM_DATA_POINTERS] = {};
			for (int i = 0; i < AV_NUM_DATA_POINTERS; ++i)
			{
				in_data[i] = impl_->frame->extended_data != nullptr ? impl_->frame->extended_data[i] : nullptr;
			}
			const int converted_frames = swr_convert(impl_->swr, out_data, out_samples, in_data, impl_->frame->nb_samples);
			if (input_layout_owned)
			{
				av_channel_layout_uninit(&input_layout);
			}
			av_frame_unref(impl_->frame);
			if (converted_frames < 0)
			{
				return false;
			}
			converted.resize(static_cast<std::size_t>(converted_frames * kChannels));
			out_interleaved.insert(out_interleaved.end(), converted.begin(), converted.end());
			decoded_any = true;
		}
		return decoded_any;
	}

	void FfmpegOpusDecoder::Shutdown()
	{
		if (impl_ == nullptr)
		{
			return;
		}
		if (impl_->swr != nullptr)
		{
			swr_free(&impl_->swr);
		}
		if (impl_->has_swr_input_layout)
		{
			av_channel_layout_uninit(&impl_->swr_input_layout);
		}
		if (impl_->frame != nullptr)
		{
			av_frame_free(&impl_->frame);
		}
		if (impl_->ctx != nullptr)
		{
			avcodec_free_context(&impl_->ctx);
		}
		impl_.reset();
	}

	bool FfmpegOpusDecoder::Ready() const
	{
		return impl_ != nullptr && impl_->ctx != nullptr && impl_->frame != nullptr;
	}

} // namespace streamproto::audio
