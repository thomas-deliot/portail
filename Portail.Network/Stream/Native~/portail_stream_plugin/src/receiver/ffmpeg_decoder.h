#pragma once

#include <algorithm>
#include <chrono>
#include <cctype>
#include <climits>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <string>
#include <thread>
#include <vector>

extern "C"
{
#include <libavcodec/avcodec.h>
#include <libavutil/error.h>
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_d3d11va.h>
}

namespace streamproto::receiver
{

	inline std::vector<std::string> BuildDecoderCandidates(AVCodecID codec_id)
	{
		auto append_unique = [](std::vector<std::string> &out, const char *name)
		{
			if (name == nullptr || *name == '\0')
			{
				return;
			}
			if (std::find(out.begin(), out.end(), name) == out.end())
			{
				out.emplace_back(name);
			}
		};

		auto decoder_priority = [](const std::string &name_in)
		{
			std::string name = name_in;
			std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c)
						   { return static_cast<char>(std::tolower(c)); });
			if (name.find("d3d11va") != std::string::npos)
			{
				return 0;
			}
			if (name.find("qsv") != std::string::npos)
			{
				return 1;
			}
			if (name.find("cuvid") != std::string::npos || name.find("nvdec") != std::string::npos)
			{
				return 2;
			}
			if (name.find("dxva2") != std::string::npos)
			{
				return 3;
			}
			if (name == "h264" || name == "hevc" || name == "av1" || name == "libdav1d")
			{
				return 10;
			}
			return 20;
		};

		std::vector<std::string> candidates;
		if (codec_id == AV_CODEC_ID_HEVC)
		{
			append_unique(candidates, "hevc_d3d11va");
			append_unique(candidates, "hevc_qsv");
			append_unique(candidates, "hevc_cuvid");
			append_unique(candidates, "hevc");
		}
		else if (codec_id == AV_CODEC_ID_AV1)
		{
			append_unique(candidates, "av1_d3d11va");
			append_unique(candidates, "av1_qsv");
			append_unique(candidates, "av1_cuvid");
			append_unique(candidates, "libdav1d");
			append_unique(candidates, "av1");
		}
		else
		{
			append_unique(candidates, "h264_d3d11va");
			append_unique(candidates, "h264_qsv");
			append_unique(candidates, "h264_cuvid");
			append_unique(candidates, "h264");
		}

		void *iter = nullptr;
		while (const AVCodec *codec = av_codec_iterate(&iter))
		{
			if (codec == nullptr || !av_codec_is_decoder(codec) || codec->id != codec_id || codec->name == nullptr)
			{
				continue;
			}
			append_unique(candidates, codec->name);
		}

		std::stable_sort(candidates.begin(), candidates.end(), [&](const std::string &a, const std::string &b)
						 {
			const int pa = decoder_priority(a);
			const int pb = decoder_priority(b);
			if (pa != pb)
			{
				return pa < pb;
			}
			return a < b;
		});
		return candidates;
	}

	class FfmpegDecoder
	{
	public:
		FfmpegDecoder() = default;
		~FfmpegDecoder() { Shutdown(); }

		bool Init(
			AVCodecID codec_id,
			int width,
			int height,
			int fps,
			AVBufferRef *shared_hw_device_ctx,
			std::string &error)
		{
			Shutdown();
			codec_id_ = codec_id;
			config_width_ = std::max(width, 0);
			config_height_ = std::max(height, 0);
			config_fps_ = std::max(fps, 0);

			std::vector<std::string> candidates = BuildDecoderCandidates(codec_id_);
			std::string first_hw_error;
			for (const std::string &candidate : candidates)
			{
				std::string candidate_error;
				if (TryOpenDecoderByName(candidate, true, shared_hw_device_ctx, candidate_error))
				{
					active_decoder_ = candidate;
					return true;
				}
				if (first_hw_error.empty() && !candidate_error.empty())
				{
					first_hw_error = candidate_error;
				}
			}

			if (!first_hw_error.empty())
			{
				error = first_hw_error;
			}
			else
			{
				error = "No usable hardware decoder found for codec id.";
			}
			return false;
		}

		bool Init(AVCodecID codec_id, std::string &error)
		{
			return Init(codec_id, 0, 0, 0, nullptr, error);
		}

		bool Decode(
			const std::uint8_t *data,
			std::size_t bytes,
			bool keyframe,
			double &decode_core_ms,
			double &post_decode_ms,
			std::uint64_t &decoded_frames,
			const std::function<void(const AVFrame *)> &frame_callback)
		{
			if (ctx_ == nullptr || frame_ == nullptr || data == nullptr || bytes == 0 || bytes > INT_MAX)
			{
				return false;
			}

			auto start = std::chrono::steady_clock::now();
			double callback_ms_accum = 0.0;

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
			if (keyframe)
			{
				packet->flags |= AV_PKT_FLAG_KEY;
			}

			int send = avcodec_send_packet(ctx_, packet);
			av_packet_free(&packet);
			if (send < 0)
			{
				return false;
			}

			std::uint64_t produced = 0;
			while (true)
			{
				int recv = avcodec_receive_frame(ctx_, frame_);
				if (recv == AVERROR(EAGAIN) || recv == AVERROR_EOF)
				{
					break;
				}
				if (recv < 0)
				{
					return false;
				}

				auto callback_start = std::chrono::steady_clock::now();
				frame_callback(frame_);
				auto callback_end = std::chrono::steady_clock::now();
				callback_ms_accum += std::chrono::duration<double, std::milli>(callback_end - callback_start).count();
				produced++;
				av_frame_unref(frame_);
			}

			auto end = std::chrono::steady_clock::now();
			const double total_ms = std::chrono::duration<double, std::milli>(end - start).count();
			decode_core_ms = std::max(0.0, total_ms - callback_ms_accum);
			post_decode_ms = callback_ms_accum;
			decoded_frames += produced;
			return true;
		}

		void Flush()
		{
			if (ctx_ != nullptr)
			{
				avcodec_flush_buffers(ctx_);
			}
		}

		void Shutdown()
		{
			if (ctx_ != nullptr)
			{
				avcodec_free_context(&ctx_);
				ctx_ = nullptr;
			}
			if (frame_ != nullptr)
			{
				av_frame_free(&frame_);
				frame_ = nullptr;
			}
			if (hw_device_ctx_ != nullptr)
			{
				av_buffer_unref(&hw_device_ctx_);
				hw_device_ctx_ = nullptr;
			}
			active_decoder_.clear();
			hw_pix_fmt_ = AV_PIX_FMT_NONE;
			using_hw_decode_ = false;
			config_width_ = 0;
			config_height_ = 0;
			config_fps_ = 0;
		}

		[[nodiscard]] const std::string &ActiveDecoder() const { return active_decoder_; }
		[[nodiscard]] bool Ready() const { return ctx_ != nullptr && frame_ != nullptr; }
		[[nodiscard]] AVCodecID CodecId() const { return codec_id_; }
		[[nodiscard]] bool UsingHardwareDecode() const { return using_hw_decode_; }
		[[nodiscard]] AVPixelFormat HardwarePixelFormat() const { return hw_pix_fmt_; }
		[[nodiscard]] AVBufferRef *HardwareDeviceContext() const { return hw_device_ctx_; }

		[[nodiscard]] bool IsHardwareFrame(const AVFrame *frame) const
		{
			if (!using_hw_decode_ || frame == nullptr)
			{
				return false;
			}
			const AVPixelFormat frame_fmt = static_cast<AVPixelFormat>(frame->format);
			if (frame_fmt == hw_pix_fmt_ || frame_fmt == AV_PIX_FMT_D3D11)
			{
				return true;
			}
			if (frame->hw_frames_ctx != nullptr && frame->hw_frames_ctx->data != nullptr)
			{
				const auto *hw_frames_ctx = reinterpret_cast<const AVHWFramesContext *>(frame->hw_frames_ctx->data);
				if (hw_frames_ctx != nullptr && hw_frames_ctx->format == AV_PIX_FMT_D3D11)
				{
					return true;
				}
			}
			return false;
		}

	private:
		static AVPixelFormat GetHardwareFormat(AVCodecContext *ctx, const AVPixelFormat *pix_fmts)
		{
			auto *self = reinterpret_cast<FfmpegDecoder *>(ctx->opaque);
			if (self != nullptr && self->hw_pix_fmt_ != AV_PIX_FMT_NONE)
			{
				for (const AVPixelFormat *p = pix_fmts; p != nullptr && *p != AV_PIX_FMT_NONE; ++p)
				{
					if (*p == self->hw_pix_fmt_)
					{
						return *p;
					}
				}
			}
			return pix_fmts[0];
		}

		static void ApplyLowLatencyDecodeOptions(AVCodecContext *ctx, bool using_hw_decode)
		{
			if (ctx == nullptr)
			{
				return;
			}

			ctx->flags |= AV_CODEC_FLAG_LOW_DELAY;
			ctx->flags |= AV_CODEC_FLAG_OUTPUT_CORRUPT;
			ctx->flags2 |= AV_CODEC_FLAG2_SHOW_ALL;
			ctx->flags2 |= AV_CODEC_FLAG2_FAST;
			ctx->err_recognition = AV_EF_EXPLODE;

			if (using_hw_decode)
			{
				ctx->thread_count = 1;
				return;
			}

			ctx->thread_type = FF_THREAD_SLICE;
			const unsigned int hw_threads = std::thread::hardware_concurrency();
			const int desired_threads = hw_threads == 0 ? 4 : static_cast<int>(hw_threads);
			ctx->thread_count = std::clamp(desired_threads, 2, 16);
		}

		bool AttachD3D11HardwareContext(
			const AVCodec *codec,
			AVCodecContext *ctx,
			AVBufferRef *shared_hw_device_ctx,
			AVPixelFormat &out_hw_pix_fmt,
			AVBufferRef *&out_hw_device,
			std::string &error)
		{
			out_hw_pix_fmt = AV_PIX_FMT_NONE;
			out_hw_device = nullptr;
			if (codec == nullptr || ctx == nullptr)
			{
				return false;
			}

			for (int i = 0;; ++i)
			{
				const AVCodecHWConfig *hw_config = avcodec_get_hw_config(codec, i);
				if (hw_config == nullptr)
				{
					break;
				}
				if ((hw_config->methods & AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX) &&
					hw_config->device_type == AV_HWDEVICE_TYPE_D3D11VA)
				{
					out_hw_pix_fmt = hw_config->pix_fmt;
					break;
				}
			}

			if (out_hw_pix_fmt == AV_PIX_FMT_NONE)
			{
				return false;
			}

			if (shared_hw_device_ctx != nullptr)
			{
				auto *dev = reinterpret_cast<AVHWDeviceContext *>(shared_hw_device_ctx->data);
				if (dev == nullptr || dev->type != AV_HWDEVICE_TYPE_D3D11VA)
				{
					error = "Shared decoder hardware context is not D3D11VA.";
					return false;
				}
				out_hw_device = av_buffer_ref(shared_hw_device_ctx);
				if (out_hw_device == nullptr)
				{
					error = "av_buffer_ref for shared decoder hw_device_ctx failed.";
					return false;
				}
			}
			else
			{
				int hw_create = av_hwdevice_ctx_create(&out_hw_device, AV_HWDEVICE_TYPE_D3D11VA, nullptr, nullptr, 0);
				if (hw_create < 0)
				{
					char errbuf[256] = {};
					av_strerror(hw_create, errbuf, sizeof(errbuf));
					error = std::string("av_hwdevice_ctx_create(D3D11VA) failed: ") + errbuf;
					return false;
				}
			}

			ctx->hw_device_ctx = av_buffer_ref(out_hw_device);
			if (ctx->hw_device_ctx == nullptr)
			{
				av_buffer_unref(&out_hw_device);
				error = "av_buffer_ref for decoder hw_device_ctx failed.";
				return false;
			}

			return true;
		}

		bool TryOpenDecoderByName(
			const std::string &name,
			bool try_hardware,
			AVBufferRef *shared_hw_device_ctx,
			std::string &error)
		{
			const AVCodec *codec = avcodec_find_decoder_by_name(name.c_str());
			if (codec == nullptr)
			{
				return false;
			}
			if (codec->id != codec_id_)
			{
				return false;
			}

			AVCodecContext *ctx = avcodec_alloc_context3(codec);
			if (ctx == nullptr)
			{
				error = "avcodec_alloc_context3 failed for " + name;
				return false;
			}
			if (config_width_ > 0)
			{
				ctx->width = config_width_;
			}
			if (config_height_ > 0)
			{
				ctx->height = config_height_;
			}
			if (config_fps_ > 0)
			{
				ctx->framerate = AVRational{config_fps_, 1};
			}

			AVBufferRef *local_hw_device = nullptr;
			AVPixelFormat local_hw_pix_fmt = AV_PIX_FMT_NONE;
			bool hw_ready = false;
			if (try_hardware)
			{
				std::string hw_error;
				if (AttachD3D11HardwareContext(codec, ctx, shared_hw_device_ctx, local_hw_pix_fmt, local_hw_device, hw_error))
				{
					hw_ready = true;
					hw_pix_fmt_ = local_hw_pix_fmt;
					ctx->opaque = this;
					ctx->get_format = &FfmpegDecoder::GetHardwareFormat;
				}
				else if (!hw_error.empty())
				{
					error = hw_error;
				}

				if (!hw_ready)
				{
					av_buffer_unref(&local_hw_device);
					avcodec_free_context(&ctx);
					if (error.empty())
					{
						error = "No D3D11 hardware context for decoder " + name;
					}
					return false;
				}
			}

			ApplyLowLatencyDecodeOptions(ctx, hw_ready);
			if (hw_ready)
			{
				ctx->extra_hw_frames = 8;
			}

			int open = avcodec_open2(ctx, codec, nullptr);
			if (open < 0)
			{
				av_buffer_unref(&local_hw_device);
				avcodec_free_context(&ctx);
				return false;
			}

			AVFrame *frame = av_frame_alloc();
			if (frame == nullptr)
			{
				av_buffer_unref(&local_hw_device);
				avcodec_free_context(&ctx);
				error = "av_frame_alloc failed for " + name;
				return false;
			}

			const bool using_hw_decode =
				hw_ready &&
				local_hw_pix_fmt == AV_PIX_FMT_D3D11 &&
				ctx->hw_device_ctx != nullptr;
			if (try_hardware && !using_hw_decode)
			{
				av_frame_free(&frame);
				av_buffer_unref(&local_hw_device);
				avcodec_free_context(&ctx);
				if (error.empty())
				{
					error = "Decoder opened without D3D11 output: " + name;
				}
				return false;
			}

			ctx_ = ctx;
			frame_ = frame;
			using_hw_decode_ = using_hw_decode;
			if (using_hw_decode_ && ctx_->hw_device_ctx != nullptr)
			{
				hw_device_ctx_ = av_buffer_ref(ctx_->hw_device_ctx);
				if (hw_device_ctx_ == nullptr)
				{
					av_buffer_unref(&local_hw_device);
					av_frame_free(&frame_);
					avcodec_free_context(&ctx_);
					using_hw_decode_ = false;
					hw_pix_fmt_ = AV_PIX_FMT_NONE;
					error = "av_buffer_ref for decoder hw_device_ctx failed.";
					return false;
				}
				hw_pix_fmt_ = local_hw_pix_fmt;
			}
			else
			{
				hw_pix_fmt_ = AV_PIX_FMT_NONE;
			}

			av_buffer_unref(&local_hw_device);
			return true;
		}

		AVCodecID codec_id_ = AV_CODEC_ID_H264;
		AVCodecContext *ctx_ = nullptr;
		AVFrame *frame_ = nullptr;
		AVBufferRef *hw_device_ctx_ = nullptr;
		AVPixelFormat hw_pix_fmt_ = AV_PIX_FMT_NONE;
		bool using_hw_decode_ = false;
		int config_width_ = 0;
		int config_height_ = 0;
		int config_fps_ = 0;
		std::string active_decoder_;
	};

} // namespace streamproto::receiver
