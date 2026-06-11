#pragma once

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <limits>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <windows.h>
#include <d3d11.h>
#include <d3d11_4.h>
#include <dxgi.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

extern "C"
{
#include <libavcodec/avcodec.h>
#include <libavutil/error.h>
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_d3d11va.h>
#include <libavutil/pixdesc.h>
#include <libavutil/opt.h>
}

#include "common/codec_utils.h"
#include "common/protocol.h"
#include "host/encoded_packet.h"
#include "host/host_options.h"

namespace streamproto::host
{

	using Microsoft::WRL::ComPtr;
	using FfmpegEncoderLogCallback = void (*)(const std::string &message);

	inline FfmpegEncoderLogCallback &FfmpegEncoderInfoLogCallback()
	{
		static FfmpegEncoderLogCallback callback = nullptr;
		return callback;
	}

	inline FfmpegEncoderLogCallback &FfmpegEncoderWarningLogCallback()
	{
		static FfmpegEncoderLogCallback callback = nullptr;
		return callback;
	}

	inline void SetFfmpegEncoderLogCallbacks(FfmpegEncoderLogCallback info, FfmpegEncoderLogCallback warning)
	{
		FfmpegEncoderInfoLogCallback() = info;
		FfmpegEncoderWarningLogCallback() = warning;
	}

	class FfmpegEncoderLogStream
	{
	public:
		explicit FfmpegEncoderLogStream(FfmpegEncoderLogCallback callback) : callback_(callback) {}
		FfmpegEncoderLogStream(const FfmpegEncoderLogStream &) = delete;
		FfmpegEncoderLogStream &operator=(const FfmpegEncoderLogStream &) = delete;
		FfmpegEncoderLogStream(FfmpegEncoderLogStream &&other) noexcept
			: callback_(other.callback_), buffer_(std::move(other.buffer_)), active_(other.active_)
		{
			other.active_ = false;
		}
		~FfmpegEncoderLogStream()
		{
			if (active_ && callback_ != nullptr)
			{
				callback_(buffer_.str());
			}
		}
		template <typename T>
		FfmpegEncoderLogStream &operator<<(const T &value)
		{
			buffer_ << value;
			return *this;
		}
	private:
		FfmpegEncoderLogCallback callback_ = nullptr;
		std::ostringstream buffer_;
		bool active_ = true;
	};

	inline FfmpegEncoderLogStream LogInfoStream()
	{
		return FfmpegEncoderLogStream(FfmpegEncoderInfoLogCallback());
	}

	inline FfmpegEncoderLogStream LogWarningStream()
	{
		return FfmpegEncoderLogStream(FfmpegEncoderWarningLogCallback());
	}
	inline AVCodecID CodecIdFromString(const std::string &codec)
	{
		if (codec == "hevc")
		{
			return AV_CODEC_ID_HEVC;
		}
		if (codec == "av1")
		{
			return AV_CODEC_ID_AV1;
		}
		return AV_CODEC_ID_H264;
	}

	inline std::vector<std::string> BuildEncoderCandidates(const HostOptions &options)
	{
		std::vector<std::string> names;
		auto push_codec = [&](std::string_view suffix)
		{
			names.emplace_back(options.codec + "_" + std::string(suffix));
		};

		if (options.encoder_pref == "auto")
		{
			push_codec("nvenc");
			push_codec("qsv");
			push_codec("amf");
			return names;
		}

		if (options.encoder_pref == "nvenc" || options.encoder_pref == "qsv" || options.encoder_pref == "amf")
		{
			push_codec(options.encoder_pref);
			return names;
		}

		names.emplace_back(options.encoder_pref);
		return names;
	}

	inline constexpr auto IsHevcIrapNalType = streamproto::codec::IsHevcIrapNalType;
	inline constexpr auto FindAnnexBStartCode = streamproto::codec::FindAnnexBStartCode;
	inline constexpr auto ReadBe32 = streamproto::codec::ReadBe32;

	inline bool IsH264KeyframeAnnexB(const std::uint8_t *data, std::size_t size)
	{
		std::size_t cursor = 0;
		while (true)
		{
			const std::size_t sc = FindAnnexBStartCode(data, size, cursor);
			if (sc == size)
			{
				return false;
			}
			std::size_t nal_start = sc + 3;
			if (sc + 3 < size && data[sc + 2] == 0 && data[sc + 3] == 1)
			{
				nal_start = sc + 4;
			}
			if (nal_start >= size)
			{
				return false;
			}
			const std::uint8_t nal_type = static_cast<std::uint8_t>(data[nal_start] & 0x1F);
			if (nal_type == 5)
			{
				return true;
			}
			cursor = nal_start + 1;
		}
	}

	inline bool IsHevcKeyframeAnnexB(const std::uint8_t *data, std::size_t size)
	{
		std::size_t cursor = 0;
		while (true)
		{
			const std::size_t sc = FindAnnexBStartCode(data, size, cursor);
			if (sc == size)
			{
				return false;
			}
			std::size_t nal_start = sc + 3;
			if (sc + 3 < size && data[sc + 2] == 0 && data[sc + 3] == 1)
			{
				nal_start = sc + 4;
			}
			if (nal_start >= size)
			{
				return false;
			}
			const std::uint8_t nal_type = static_cast<std::uint8_t>((data[nal_start] >> 1) & 0x3F);
			if (IsHevcIrapNalType(nal_type))
			{
				return true;
			}
			cursor = nal_start + 1;
		}
	}

	inline bool IsH264KeyframeLengthPrefixed(const std::uint8_t *data, std::size_t size)
	{
		std::size_t offset = 0;
		while (offset + 4 <= size)
		{
			const std::uint32_t nal_size = ReadBe32(data + offset);
			offset += 4;
			if (nal_size == 0 || offset + nal_size > size)
			{
				return false;
			}
			const std::uint8_t nal_type = static_cast<std::uint8_t>(data[offset] & 0x1F);
			if (nal_type == 5)
			{
				return true;
			}
			offset += nal_size;
		}
		return false;
	}

	inline bool IsHevcKeyframeLengthPrefixed(const std::uint8_t *data, std::size_t size)
	{
		std::size_t offset = 0;
		while (offset + 4 <= size)
		{
			const std::uint32_t nal_size = ReadBe32(data + offset);
			offset += 4;
			if (nal_size == 0 || offset + nal_size > size)
			{
				return false;
			}
			const std::uint8_t nal_type = static_cast<std::uint8_t>((data[offset] >> 1) & 0x3F);
			if (IsHevcIrapNalType(nal_type))
			{
				return true;
			}
			offset += nal_size;
		}
		return false;
	}

	inline bool IsBitstreamKeyframe(AVCodecID codec_id, const std::uint8_t *data, std::size_t size)
	{
		if (data == nullptr || size < 5)
		{
			return false;
		}

		const bool looks_annexb = streamproto::codec::LooksAnnexB(data, size);

		if (codec_id == AV_CODEC_ID_H264)
		{
			if (looks_annexb)
			{
				return IsH264KeyframeAnnexB(data, size);
			}
			return IsH264KeyframeLengthPrefixed(data, size);
		}
		if (codec_id == AV_CODEC_ID_HEVC)
		{
			if (looks_annexb)
			{
				return IsHevcKeyframeAnnexB(data, size);
			}
			return IsHevcKeyframeLengthPrefixed(data, size);
		}
		return false;
	}

	inline std::int64_t BitrateBitsFromKbps(int bitrate_kbps)
	{
		return static_cast<std::int64_t>(std::clamp(bitrate_kbps, 100, 200000)) * 1000LL;
	}

	inline int ClampRcBufferBits(std::int64_t buffer_bits)
	{
		if (buffer_bits > static_cast<std::int64_t>(std::numeric_limits<int>::max()))
		{
			return std::numeric_limits<int>::max();
		}
		return static_cast<int>(std::max<std::int64_t>(buffer_bits, 1));
	}

	inline std::int64_t VbrAverageBitrateBits(std::int64_t max_bit_rate)
	{
		return std::max<std::int64_t>(100000, (max_bit_rate * 3) / 4);
	}

	inline int ComputeCbrBufferBits(std::int64_t bit_rate, int fps)
	{
		return ClampRcBufferBits(bit_rate / std::max(fps, 1));
	}

	inline int ComputeVbrBufferBits(std::int64_t max_bit_rate, int fps)
	{
		const std::int64_t half_second_buffer = max_bit_rate / 2;
		const std::int64_t one_frame_buffer = max_bit_rate / std::max(fps, 1);
		return ClampRcBufferBits(std::max(half_second_buffer, one_frame_buffer));
	}

	inline void ApplyRateControlContext(AVCodecContext *ctx, int bitrate_kbps, int fps, VideoRateControl mode)
	{
		if (ctx == nullptr)
		{
			return;
		}

		const std::int64_t max_bit_rate = BitrateBitsFromKbps(bitrate_kbps);
		if (IsVbr(mode))
		{
			ctx->bit_rate = VbrAverageBitrateBits(max_bit_rate);
			ctx->rc_min_rate = 0;
			ctx->rc_max_rate = max_bit_rate;
			ctx->rc_buffer_size = ComputeVbrBufferBits(max_bit_rate, fps);
			return;
		}

		ctx->bit_rate = max_bit_rate;
		ctx->rc_min_rate = max_bit_rate;
		ctx->rc_max_rate = max_bit_rate;
		ctx->rc_buffer_size = ComputeCbrBufferBits(max_bit_rate, fps);
	}

	inline void ApplyPrivateRateControlOptions(AVCodecContext *ctx, int bitrate_kbps, int fps, VideoRateControl mode)
	{
		if (ctx == nullptr || ctx->priv_data == nullptr)
		{
			return;
		}

		const std::int64_t max_bit_rate = BitrateBitsFromKbps(bitrate_kbps);
		const std::int64_t bit_rate = IsVbr(mode) ? VbrAverageBitrateBits(max_bit_rate) : max_bit_rate;
		const std::int64_t min_rate = IsVbr(mode) ? 0 : max_bit_rate;
		const int buffer_bits = IsVbr(mode)
									? ComputeVbrBufferBits(max_bit_rate, fps)
									: ComputeCbrBufferBits(max_bit_rate, fps);

		av_opt_set_int(ctx->priv_data, "bit_rate", bit_rate, 0);
		av_opt_set_int(ctx->priv_data, "b", bit_rate, 0);
		av_opt_set_int(ctx->priv_data, "maxrate", max_bit_rate, 0);
		av_opt_set_int(ctx->priv_data, "minrate", min_rate, 0);
		av_opt_set_int(ctx->priv_data, "bufsize", buffer_bits, 0);
	}

	inline void SetOptionalEncoderOption(AVCodecContext *ctx, const char *key, const char *value)
	{
		if (ctx == nullptr || ctx->priv_data == nullptr || key == nullptr || value == nullptr)
		{
			return;
		}
		const int result = av_opt_set(ctx->priv_data, key, value, 0);
		if (result < 0 && result != AVERROR_OPTION_NOT_FOUND)
		{
			char errbuf[256] = {};
			av_strerror(result, errbuf, sizeof(errbuf));
			LogWarningStream() << "Encoder option " << key << "=" << value << " rejected: " << errbuf;
		}
	}

	class FfmpegEncoder
	{
	public:
		FfmpegEncoder() = default;
		~FfmpegEncoder() { Shutdown(); }

		bool Init(
			const HostOptions &options,
			ID3D11Device *capture_device,
			ID3D11DeviceContext *capture_context,
			std::string &error)
		{
			Shutdown();
			codec_id_ = CodecIdFromString(options.codec);
			if (capture_device != nullptr)
			{
				capture_device_ = capture_device;
			}
			if (capture_context != nullptr)
			{
				capture_context_ = capture_context;
			}
			fps_ = std::max(options.fps, 1);
			current_bitrate_kbps_ = std::max(options.ice_bitrate_kbps, 100);
			rate_control_mode_ = options.video_rate_control;

			std::vector<std::string> candidates = BuildEncoderCandidates(options);
			if (candidates.empty())
			{
				error = "No GPU-only encoder candidates generated.";
				return false;
			}

			std::ostringstream init_errors;
			for (const std::string &candidate : candidates)
			{
				std::string candidate_error;
				if (TryOpenEncoder(candidate, options, candidate_error))
				{
					active_encoder_ = candidate;
					return true;
				}
				if (!candidate_error.empty())
				{
					if (!init_errors.str().empty())
					{
						init_errors << " | ";
					}
					init_errors << candidate << ": " << candidate_error;
				}
			}

			error = init_errors.str();
			if (error.empty())
			{
				error = "No usable GPU hardware encoder found.";
			}
			return false;
		}

		[[nodiscard]] const std::string &ActiveEncoder() const { return active_encoder_; }
		[[nodiscard]] AVCodecID CodecId() const { return codec_id_; }
		[[nodiscard]] std::uint16_t ProtocolCodec() const
		{
			if (codec_id_ == AV_CODEC_ID_HEVC)
			{
				return static_cast<std::uint16_t>(streamproto::proto::Codec::kHEVC);
			}
			if (codec_id_ == AV_CODEC_ID_AV1)
			{
				return static_cast<std::uint16_t>(streamproto::proto::Codec::kAV1);
			}
			return static_cast<std::uint16_t>(streamproto::proto::Codec::kH264);
		}
		[[nodiscard]] const std::string &EncodeInputPath() const { return encode_input_path_; }
		[[nodiscard]] int CurrentBitrateKbps() const { return current_bitrate_kbps_; }

		bool SetBitrateKbps(int bitrate_kbps)
		{
			if (ctx_ == nullptr)
			{
				return false;
			}

			const int clamped_kbps = std::clamp(bitrate_kbps, 100, 200000);
			if (clamped_kbps == current_bitrate_kbps_)
			{
				return true;
			}

			ApplyRateControlContext(ctx_, clamped_kbps, fps_, rate_control_mode_);

			if (ctx_->priv_data != nullptr)
			{
				ApplyPrivateRateControlOptions(ctx_, clamped_kbps, fps_, rate_control_mode_);
			}

			current_bitrate_kbps_ = clamped_kbps;
			return true;
		}

		bool EncodeFrameGpu(
			ID3D11Texture2D *texture,
			int source_width,
			int source_height,
			bool force_keyframe,
			std::uint64_t capture_ts_us,
			std::vector<EncodedPacket> &out_packets,
			double &encode_ms)
		{
			out_packets.clear();
			if (ctx_ == nullptr || !use_hw_frames_ || hw_frames_ctx_ == nullptr || texture == nullptr || capture_context_ == nullptr)
			{
				return false;
			}

			auto encode_start = std::chrono::steady_clock::now();

			AVFrame *source_hw_frame = av_frame_alloc();
			AVFrame *encoded_hw_frame = nullptr;
			if (source_hw_frame == nullptr)
			{
				return false;
			}

			if (hw_frame_format_ == AV_PIX_FMT_QSV)
			{
				if (input_hw_frames_ctx_ == nullptr)
				{
					av_frame_free(&source_hw_frame);
					return false;
				}
				source_hw_frame->format = AV_PIX_FMT_D3D11;
				source_hw_frame->width = ctx_->width;
				source_hw_frame->height = ctx_->height;
				source_hw_frame->color_range = AVCOL_RANGE_JPEG;
				source_hw_frame->color_primaries = AVCOL_PRI_BT709;
				source_hw_frame->color_trc = AVCOL_TRC_BT709;
				source_hw_frame->colorspace = AVCOL_SPC_BT709;
				if (av_hwframe_get_buffer(input_hw_frames_ctx_, source_hw_frame, 0) < 0)
				{
					av_frame_free(&source_hw_frame);
					return false;
				}
				if (!PopulateD3D11FrameFromTexture(source_hw_frame, texture, source_width, source_height))
				{
					av_frame_free(&source_hw_frame);
					return false;
				}

				encoded_hw_frame = av_frame_alloc();
				if (encoded_hw_frame == nullptr)
				{
					av_frame_free(&source_hw_frame);
					return false;
				}
				encoded_hw_frame->format = AV_PIX_FMT_QSV;
				encoded_hw_frame->width = ctx_->width;
				encoded_hw_frame->height = ctx_->height;
				encoded_hw_frame->color_range = AVCOL_RANGE_JPEG;
				encoded_hw_frame->color_primaries = AVCOL_PRI_BT709;
				encoded_hw_frame->color_trc = AVCOL_TRC_BT709;
				encoded_hw_frame->colorspace = AVCOL_SPC_BT709;
				encoded_hw_frame->hw_frames_ctx = av_buffer_ref(hw_frames_ctx_);
				if (encoded_hw_frame->hw_frames_ctx == nullptr)
				{
					av_frame_free(&encoded_hw_frame);
					av_frame_free(&source_hw_frame);
					return false;
				}
				if (av_hwframe_map(encoded_hw_frame, source_hw_frame, AV_HWFRAME_MAP_READ) < 0)
				{
					av_frame_free(&encoded_hw_frame);
					av_frame_free(&source_hw_frame);
					return false;
				}
			}
			else
			{
				source_hw_frame->format = AV_PIX_FMT_D3D11;
				source_hw_frame->width = ctx_->width;
				source_hw_frame->height = ctx_->height;
				source_hw_frame->color_range = AVCOL_RANGE_JPEG;
				source_hw_frame->color_primaries = AVCOL_PRI_BT709;
				source_hw_frame->color_trc = AVCOL_TRC_BT709;
				source_hw_frame->colorspace = AVCOL_SPC_BT709;
				if (av_hwframe_get_buffer(hw_frames_ctx_, source_hw_frame, 0) < 0)
				{
					av_frame_free(&source_hw_frame);
					return false;
				}
				if (!PopulateD3D11FrameFromTexture(source_hw_frame, texture, source_width, source_height))
				{
					av_frame_free(&source_hw_frame);
					return false;
				}
				encoded_hw_frame = source_hw_frame;
			}

			encoded_hw_frame->pts = next_pts_++;
			encoded_hw_frame->pict_type = force_keyframe ? AV_PICTURE_TYPE_I : AV_PICTURE_TYPE_NONE;
			if (force_keyframe)
			{
				encoded_hw_frame->flags |= AV_FRAME_FLAG_KEY;
			}
			else
			{
				encoded_hw_frame->flags &= ~AV_FRAME_FLAG_KEY;
			}

			const int send = avcodec_send_frame(ctx_, encoded_hw_frame);
			if (encoded_hw_frame != source_hw_frame)
			{
				av_frame_free(&encoded_hw_frame);
			}
			av_frame_free(&source_hw_frame);
			if (send < 0)
			{
				return false;
			}
			if (!DrainPackets(force_keyframe, capture_ts_us, out_packets))
			{
				return false;
			}

			auto encode_end = std::chrono::steady_clock::now();
			encode_ms = std::chrono::duration<double, std::milli>(encode_end - encode_start).count();
			return true;
		}

		void Shutdown()
		{
			if (ctx_ != nullptr)
			{
				avcodec_free_context(&ctx_);
				ctx_ = nullptr;
			}
			if (hw_frames_ctx_ != nullptr)
			{
				av_buffer_unref(&hw_frames_ctx_);
				hw_frames_ctx_ = nullptr;
			}
			if (input_hw_frames_ctx_ != nullptr)
			{
				av_buffer_unref(&input_hw_frames_ctx_);
				input_hw_frames_ctx_ = nullptr;
			}
			if (input_hw_device_ctx_ != nullptr)
			{
				av_buffer_unref(&input_hw_device_ctx_);
				input_hw_device_ctx_ = nullptr;
			}
			if (hw_device_ctx_ != nullptr)
			{
				av_buffer_unref(&hw_device_ctx_);
				hw_device_ctx_ = nullptr;
			}
			video_proc_.Reset();
			vp_enum_.Reset();
			video_context_.Reset();
			video_device_.Reset();
			vp_src_width_ = 0;
			vp_src_height_ = 0;
			vp_dst_width_ = 0;
			vp_dst_height_ = 0;
			vp_src_format_ = DXGI_FORMAT_UNKNOWN;
			vp_dst_format_ = DXGI_FORMAT_UNKNOWN;
			vp_output_texture_.Reset();
			vp_output_width_ = 0;
			vp_output_height_ = 0;
			vp_output_format_ = DXGI_FORMAT_UNKNOWN;
			logged_gpu_path_ = false;
			capture_context_.Reset();
			capture_device_.Reset();
			use_hw_frames_ = false;
			hw_frame_format_ = AV_PIX_FMT_NONE;
			encode_input_path_ = "none";
			current_bitrate_kbps_ = 0;
			fps_ = 60;
			rate_control_mode_ = VideoRateControl::kCbr;
			active_encoder_.clear();
			next_pts_ = 0;
		}

	private:
		bool CreateD3D11DeviceContext(AVBufferRef *&out_hw_device_ctx, std::string &error)
		{
			out_hw_device_ctx = nullptr;
			if (capture_device_ == nullptr || capture_context_ == nullptr)
			{
				error = "Capture D3D11 device/context not available.";
				return false;
			}

			AVBufferRef *hw_device_ctx = av_hwdevice_ctx_alloc(AV_HWDEVICE_TYPE_D3D11VA);
			if (hw_device_ctx == nullptr)
			{
				error = "av_hwdevice_ctx_alloc(D3D11VA) failed.";
				return false;
			}

			auto *device_ctx = reinterpret_cast<AVHWDeviceContext *>(hw_device_ctx->data);
			auto *d3d11_ctx = reinterpret_cast<AVD3D11VADeviceContext *>(device_ctx->hwctx);
			d3d11_ctx->device = capture_device_.Get();
			if (d3d11_ctx->device != nullptr)
			{
				d3d11_ctx->device->AddRef();
			}
			d3d11_ctx->device_context = capture_context_.Get();
			if (d3d11_ctx->device_context != nullptr)
			{
				d3d11_ctx->device_context->AddRef();
			}
			if (av_hwdevice_ctx_init(hw_device_ctx) < 0)
			{
				av_buffer_unref(&hw_device_ctx);
				error = "av_hwdevice_ctx_init(D3D11VA) failed.";
				return false;
			}

			out_hw_device_ctx = hw_device_ctx;
			return true;
		}

		bool CreateD3D11FramesContext(
			AVBufferRef *hw_device_ctx,
			const HostOptions &options,
			AVPixelFormat sw_format,
			AVBufferRef *&out_hw_frames_ctx,
			std::string &error)
		{
			out_hw_frames_ctx = nullptr;
			if (hw_device_ctx == nullptr)
			{
				error = "D3D11 hardware device context is null.";
				return false;
			}

			AVBufferRef *hw_frames_ctx = av_hwframe_ctx_alloc(hw_device_ctx);
			if (hw_frames_ctx == nullptr)
			{
				error = "av_hwframe_ctx_alloc failed.";
				return false;
			}

			auto *frames_ctx = reinterpret_cast<AVHWFramesContext *>(hw_frames_ctx->data);
			auto *d3d11_frames_ctx = reinterpret_cast<AVD3D11VAFramesContext *>(frames_ctx->hwctx);
			frames_ctx->format = AV_PIX_FMT_D3D11;
			frames_ctx->sw_format = sw_format;
			frames_ctx->width = options.width;
			frames_ctx->height = options.height;
			// NV12 array textures can be rejected by some Intel drivers during hwframe pool init.
			// For QSV upload surfaces, let FFmpeg allocate individual textures on demand instead.
			frames_ctx->initial_pool_size = (sw_format == AV_PIX_FMT_NV12) ? 0 : 8;
			if (d3d11_frames_ctx != nullptr)
			{
				d3d11_frames_ctx->BindFlags = 0;
				d3d11_frames_ctx->MiscFlags = 0;
			}

			const int init = av_hwframe_ctx_init(hw_frames_ctx);
			if (init < 0)
			{
				char errbuf[256] = {};
				av_strerror(init, errbuf, sizeof(errbuf));
				av_buffer_unref(&hw_frames_ctx);
				std::ostringstream message;
				message << "av_hwframe_ctx_init(D3D11VA, sw_format="
						<< (av_get_pix_fmt_name(sw_format) != nullptr ? av_get_pix_fmt_name(sw_format) : "unknown")
						<< ", size=" << options.width << "x" << options.height
						<< ") failed: " << errbuf;
				error = message.str();
				return false;
			}

			out_hw_frames_ctx = hw_frames_ctx;
			return true;
		}

		bool PopulateD3D11FrameFromTexture(
			AVFrame *hw_frame,
			ID3D11Texture2D *texture,
			int source_width,
			int source_height)
		{
			if (hw_frame == nullptr || texture == nullptr || capture_context_ == nullptr)
			{
				return false;
			}

			ID3D11Texture2D *dst_texture = reinterpret_cast<ID3D11Texture2D *>(hw_frame->data[0]);
			const UINT dst_subresource = static_cast<UINT>(reinterpret_cast<uintptr_t>(hw_frame->data[1]));
			if (dst_texture == nullptr)
			{
				return false;
			}

			D3D11_TEXTURE2D_DESC src_desc{};
			D3D11_TEXTURE2D_DESC dst_desc{};
			texture->GetDesc(&src_desc);
			dst_texture->GetDesc(&dst_desc);

			const UINT effective_src_width = std::clamp<UINT>(
				static_cast<UINT>(std::max(source_width, 1)),
				1,
				src_desc.Width);
			const UINT effective_src_height = std::clamp<UINT>(
				static_cast<UINT>(std::max(source_height, 1)),
				1,
				src_desc.Height);

			const UINT dst_mips = std::max<UINT>(1, dst_desc.MipLevels);
			const UINT dst_array_slice = dst_subresource / dst_mips;
			if (dst_array_slice >= std::max<UINT>(1, dst_desc.ArraySize))
			{
				return false;
			}

			const bool size_changed = effective_src_width != dst_desc.Width || effective_src_height != dst_desc.Height;
			const bool format_changed = src_desc.Format != dst_desc.Format;
			const bool direct_copy = !size_changed && !format_changed;

			if (!logged_gpu_path_)
			{
				LogInfoStream() << "path=sdr GPU encode input: " << effective_src_width << "x" << effective_src_height
								<< " (surface " << src_desc.Width << "x" << src_desc.Height << ")"
								<< " -> " << dst_desc.Width << "x" << dst_desc.Height
								<< (direct_copy ? " (copy)"
												: (size_changed && format_changed ? " (scale+convert)"
																				  : (size_changed ? " (scale)" : " (convert)")));
				logged_gpu_path_ = true;
			}

			if (direct_copy)
			{
				D3D11_BOX src_box{};
				src_box.left = 0;
				src_box.top = 0;
				src_box.front = 0;
				src_box.right = effective_src_width;
				src_box.bottom = effective_src_height;
				src_box.back = 1;
				capture_context_->CopySubresourceRegion(dst_texture, dst_subresource, 0, 0, 0, texture, 0, &src_box);
				return true;
			}

			if (!EnsureVideoScaler(src_desc, dst_desc))
			{
				return false;
			}

			if (!EnsureVideoScaleOutputTexture(dst_desc))
			{
				return false;
			}

			D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC out_desc{};
			out_desc.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
			out_desc.Texture2D.MipSlice = 0;

			ComPtr<ID3D11VideoProcessorOutputView> output_view;
			if (FAILED(video_device_->CreateVideoProcessorOutputView(vp_output_texture_.Get(), vp_enum_.Get(), &out_desc, output_view.GetAddressOf())) ||
				output_view == nullptr)
			{
				return false;
			}

			D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC in_desc{};
			in_desc.FourCC = 0;
			in_desc.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
			in_desc.Texture2D.MipSlice = 0;
			in_desc.Texture2D.ArraySlice = 0;

			ComPtr<ID3D11VideoProcessorInputView> input_view;
			if (FAILED(video_device_->CreateVideoProcessorInputView(texture, vp_enum_.Get(), &in_desc, input_view.GetAddressOf())) ||
				input_view == nullptr)
			{
				return false;
			}

			RECT src_rect{0, 0, static_cast<LONG>(effective_src_width), static_cast<LONG>(effective_src_height)};
			RECT dst_rect{0, 0, static_cast<LONG>(dst_desc.Width), static_cast<LONG>(dst_desc.Height)};
			video_context_->VideoProcessorSetStreamSourceRect(video_proc_.Get(), 0, TRUE, &src_rect);
			video_context_->VideoProcessorSetStreamDestRect(video_proc_.Get(), 0, TRUE, &dst_rect);
			video_context_->VideoProcessorSetOutputTargetRect(video_proc_.Get(), TRUE, &dst_rect);
			video_context_->VideoProcessorSetStreamFrameFormat(video_proc_.Get(), 0, D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE);
			const D3D11_VIDEO_PROCESSOR_COLOR_SPACE color_space = BuildFullRangeBt709ColorSpace();
			video_context_->VideoProcessorSetStreamColorSpace(video_proc_.Get(), 0, &color_space);
			video_context_->VideoProcessorSetOutputColorSpace(video_proc_.Get(), &color_space);
			video_context_->VideoProcessorSetStreamAutoProcessingMode(video_proc_.Get(), 0, FALSE);

			D3D11_VIDEO_PROCESSOR_STREAM stream{};
			stream.Enable = TRUE;
			stream.pInputSurface = input_view.Get();
			if (FAILED(video_context_->VideoProcessorBlt(video_proc_.Get(), output_view.Get(), 0, 1, &stream)))
			{
				return false;
			}

			capture_context_->CopySubresourceRegion(dst_texture, dst_subresource, 0, 0, 0, vp_output_texture_.Get(), 0, nullptr);
			return true;
		}

		bool EnsureVideoScaler(const D3D11_TEXTURE2D_DESC &src_desc, const D3D11_TEXTURE2D_DESC &dst_desc)
		{
			if (capture_device_ == nullptr || capture_context_ == nullptr)
			{
				return false;
			}
			if (video_device_ == nullptr)
			{
				if (FAILED(capture_device_.As(&video_device_)) || video_device_ == nullptr)
				{
					return false;
				}
			}
			if (video_context_ == nullptr)
			{
				if (FAILED(capture_context_.As(&video_context_)) || video_context_ == nullptr)
				{
					return false;
				}
			}

			if (vp_enum_ != nullptr && video_proc_ != nullptr &&
				vp_src_width_ == static_cast<int>(src_desc.Width) &&
				vp_src_height_ == static_cast<int>(src_desc.Height) &&
				vp_dst_width_ == static_cast<int>(dst_desc.Width) &&
				vp_dst_height_ == static_cast<int>(dst_desc.Height) &&
				vp_src_format_ == src_desc.Format &&
				vp_dst_format_ == dst_desc.Format)
			{
				return true;
			}

			video_proc_.Reset();
			vp_enum_.Reset();

			D3D11_VIDEO_PROCESSOR_CONTENT_DESC content{};
			content.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
			content.InputWidth = src_desc.Width;
			content.InputHeight = src_desc.Height;
			content.OutputWidth = dst_desc.Width;
			content.OutputHeight = dst_desc.Height;
			content.Usage = D3D11_VIDEO_USAGE_PLAYBACK_NORMAL;

			if (FAILED(video_device_->CreateVideoProcessorEnumerator(&content, vp_enum_.GetAddressOf())) || vp_enum_ == nullptr)
			{
				return false;
			}
			if (FAILED(video_device_->CreateVideoProcessor(vp_enum_.Get(), 0, video_proc_.GetAddressOf())) || video_proc_ == nullptr)
			{
				return false;
			}

			vp_src_width_ = static_cast<int>(src_desc.Width);
			vp_src_height_ = static_cast<int>(src_desc.Height);
			vp_dst_width_ = static_cast<int>(dst_desc.Width);
			vp_dst_height_ = static_cast<int>(dst_desc.Height);
			vp_src_format_ = src_desc.Format;
			vp_dst_format_ = dst_desc.Format;
			return true;
		}

		static D3D11_VIDEO_PROCESSOR_COLOR_SPACE BuildFullRangeBt709ColorSpace()
		{
			D3D11_VIDEO_PROCESSOR_COLOR_SPACE color_space{};
			color_space.Usage = 0;
			color_space.RGB_Range = 0;
			color_space.YCbCr_Matrix = 1;
			color_space.YCbCr_xvYCC = 0;
			color_space.Nominal_Range = D3D11_VIDEO_PROCESSOR_NOMINAL_RANGE_0_255;
			return color_space;
		}

		bool EnsureVideoScaleOutputTexture(const D3D11_TEXTURE2D_DESC &dst_desc)
		{
			if (capture_device_ == nullptr)
			{
				return false;
			}

			if (vp_output_texture_ != nullptr &&
				vp_output_width_ == static_cast<int>(dst_desc.Width) &&
				vp_output_height_ == static_cast<int>(dst_desc.Height) &&
				vp_output_format_ == dst_desc.Format)
			{
				return true;
			}

			vp_output_texture_.Reset();

			D3D11_TEXTURE2D_DESC temp_desc{};
			temp_desc.Width = dst_desc.Width;
			temp_desc.Height = dst_desc.Height;
			temp_desc.MipLevels = 1;
			temp_desc.ArraySize = 1;
			temp_desc.Format = dst_desc.Format;
			temp_desc.SampleDesc.Count = 1;
			temp_desc.SampleDesc.Quality = 0;
			temp_desc.Usage = D3D11_USAGE_DEFAULT;
			temp_desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
			temp_desc.CPUAccessFlags = 0;
			temp_desc.MiscFlags = 0;

			if (FAILED(capture_device_->CreateTexture2D(&temp_desc, nullptr, vp_output_texture_.GetAddressOf())) ||
				vp_output_texture_ == nullptr)
			{
				vp_output_width_ = 0;
				vp_output_height_ = 0;
				vp_output_format_ = DXGI_FORMAT_UNKNOWN;
				return false;
			}

			vp_output_width_ = static_cast<int>(dst_desc.Width);
			vp_output_height_ = static_cast<int>(dst_desc.Height);
			vp_output_format_ = dst_desc.Format;
			return true;
		}

		static bool SupportsPixelFormat(const AVCodec *codec, AVPixelFormat format)
		{
			if (codec == nullptr || codec->pix_fmts == nullptr)
			{
				return false;
			}
			for (const AVPixelFormat *fmt = codec->pix_fmts; *fmt != AV_PIX_FMT_NONE; ++fmt)
			{
				if (*fmt == format)
				{
					return true;
				}
			}
			return false;
		}

		bool DrainPackets(bool force_keyframe, std::uint64_t capture_ts_us, std::vector<EncodedPacket> &out_packets)
		{
			while (true)
			{
				AVPacket *packet = av_packet_alloc();
				if (packet == nullptr)
				{
					return false;
				}

				const int recv = avcodec_receive_packet(ctx_, packet);
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

				EncodedPacket encoded{};
				encoded.bytes.assign(packet->data, packet->data + packet->size);
				const bool packet_key = (packet->flags & AV_PKT_FLAG_KEY) != 0;
				encoded.keyframe = packet_key || IsBitstreamKeyframe(codec_id_, packet->data, packet->size);
				encoded.capture_ts_us = capture_ts_us;
				out_packets.emplace_back(std::move(encoded));
				av_packet_free(&packet);
			}
			if (force_keyframe && !out_packets.empty() && !HasKeyframePacket(out_packets))
			{
				LogWarningStream() << "Encoder did not produce an IDR/key packet when requested.";
			}
			return true;
		}

		bool CreateD3D11HardwareContexts(AVCodecContext *ctx, const HostOptions &options, std::string &error)
		{
			if (ctx == nullptr || capture_device_ == nullptr || capture_context_ == nullptr)
			{
				return false;
			}

			AVBufferRef *hw_device_ctx = nullptr;
			if (!CreateD3D11DeviceContext(hw_device_ctx, error))
			{
				return false;
			}

			AVBufferRef *hw_frames_ctx = nullptr;
			if (!CreateD3D11FramesContext(hw_device_ctx, options, AV_PIX_FMT_BGRA, hw_frames_ctx, error))
			{
				av_buffer_unref(&hw_device_ctx);
				return false;
			}

			ctx->pix_fmt = AV_PIX_FMT_D3D11;
			ctx->hw_device_ctx = av_buffer_ref(hw_device_ctx);
			ctx->hw_frames_ctx = av_buffer_ref(hw_frames_ctx);
			if (ctx->hw_device_ctx == nullptr || ctx->hw_frames_ctx == nullptr)
			{
				if (ctx->hw_device_ctx != nullptr)
				{
					av_buffer_unref(&ctx->hw_device_ctx);
				}
				if (ctx->hw_frames_ctx != nullptr)
				{
					av_buffer_unref(&ctx->hw_frames_ctx);
				}
				av_buffer_unref(&hw_frames_ctx);
				av_buffer_unref(&hw_device_ctx);
				error = "Failed to reference D3D11 hw contexts for encoder.";
				return false;
			}

			hw_device_ctx_ = hw_device_ctx;
			hw_frames_ctx_ = hw_frames_ctx;
			use_hw_frames_ = true;
			hw_frame_format_ = AV_PIX_FMT_D3D11;
			encode_input_path_ = "d3d11-hwframe";
			return true;
		}

		bool CreateQsvHardwareContexts(
			AVCodecContext *ctx,
			const AVCodec *codec,
			const HostOptions &options,
			std::string &error)
		{
			if (ctx == nullptr || codec == nullptr || capture_device_ == nullptr || capture_context_ == nullptr)
			{
				return false;
			}
			if (!SupportsPixelFormat(codec, AV_PIX_FMT_QSV))
			{
				error = "QSV encoder does not expose AV_PIX_FMT_QSV input.";
				return false;
			}

			AVBufferRef *d3d11_device_ctx = nullptr;
			if (!CreateD3D11DeviceContext(d3d11_device_ctx, error))
			{
				return false;
			}

			AVBufferRef *d3d11_frames_ctx = nullptr;
			if (!CreateD3D11FramesContext(d3d11_device_ctx, options, AV_PIX_FMT_NV12, d3d11_frames_ctx, error))
			{
				av_buffer_unref(&d3d11_device_ctx);
				return false;
			}

			AVBufferRef *qsv_device_ctx = nullptr;
			const int derived_device = av_hwdevice_ctx_create_derived(&qsv_device_ctx, AV_HWDEVICE_TYPE_QSV, d3d11_device_ctx, 0);
			if (derived_device < 0 ||
				qsv_device_ctx == nullptr)
			{
				char errbuf[256] = {};
				av_strerror(derived_device, errbuf, sizeof(errbuf));
				av_buffer_unref(&d3d11_frames_ctx);
				av_buffer_unref(&d3d11_device_ctx);
				error = std::string("av_hwdevice_ctx_create_derived(QSV from D3D11VA) failed: ") + errbuf;
				return false;
			}

			AVBufferRef *qsv_frames_ctx = nullptr;
			const int derived_frames = av_hwframe_ctx_create_derived(&qsv_frames_ctx, AV_PIX_FMT_QSV, qsv_device_ctx, d3d11_frames_ctx, 0);
			if (derived_frames < 0 ||
				qsv_frames_ctx == nullptr)
			{
				char errbuf[256] = {};
				av_strerror(derived_frames, errbuf, sizeof(errbuf));
				av_buffer_unref(&qsv_device_ctx);
				av_buffer_unref(&d3d11_frames_ctx);
				av_buffer_unref(&d3d11_device_ctx);
				error = std::string("av_hwframe_ctx_create_derived(QSV from D3D11 frames) failed: ") + errbuf;
				return false;
			}

			ctx->pix_fmt = AV_PIX_FMT_QSV;
			ctx->hw_device_ctx = av_buffer_ref(qsv_device_ctx);
			ctx->hw_frames_ctx = av_buffer_ref(qsv_frames_ctx);
			if (ctx->hw_device_ctx == nullptr || ctx->hw_frames_ctx == nullptr)
			{
				if (ctx->hw_device_ctx != nullptr)
				{
					av_buffer_unref(&ctx->hw_device_ctx);
				}
				if (ctx->hw_frames_ctx != nullptr)
				{
					av_buffer_unref(&ctx->hw_frames_ctx);
				}
				av_buffer_unref(&qsv_frames_ctx);
				av_buffer_unref(&qsv_device_ctx);
				av_buffer_unref(&d3d11_frames_ctx);
				av_buffer_unref(&d3d11_device_ctx);
				error = "Failed to reference QSV/D3D11 hw contexts for encoder.";
				return false;
			}

			hw_device_ctx_ = qsv_device_ctx;
			hw_frames_ctx_ = qsv_frames_ctx;
			input_hw_device_ctx_ = d3d11_device_ctx;
			input_hw_frames_ctx_ = d3d11_frames_ctx;
			use_hw_frames_ = true;
			hw_frame_format_ = AV_PIX_FMT_QSV;
			encode_input_path_ = "qsv-hwframe";
			return true;
		}

		bool TryOpenEncoder(const std::string &name, const HostOptions &options, std::string &error)
		{
			const AVCodec *codec = avcodec_find_encoder_by_name(name.c_str());
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

			const bool can_try_d3d11 =
				capture_device_ != nullptr &&
				capture_context_ != nullptr &&
				(name.find("nvenc") != std::string::npos || name.find("amf") != std::string::npos) &&
				SupportsPixelFormat(codec, AV_PIX_FMT_D3D11);
			const bool can_try_qsv_hw =
				capture_device_ != nullptr &&
				capture_context_ != nullptr &&
				name.find("qsv") != std::string::npos;
			ctx->codec_id = codec_id_;
			ctx->width = options.width;
			ctx->height = options.height;
			ctx->framerate = AVRational{options.fps, 1};
			ctx->time_base = AVRational{1, options.fps};
			ctx->color_range = AVCOL_RANGE_JPEG;
			ctx->color_primaries = AVCOL_PRI_BT709;
			ctx->color_trc = AVCOL_TRC_BT709;
			ctx->colorspace = AVCOL_SPC_BT709;
			ApplyRateControlContext(ctx, options.ice_bitrate_kbps, options.fps, options.video_rate_control);
			const int gop_size = options.gop == 0 ? std::numeric_limits<int>::max() : std::max(options.gop, 1);
			ctx->gop_size = gop_size;
			ctx->keyint_min = gop_size;
			ctx->max_b_frames = 0;
			ctx->flags |= AV_CODEC_FLAG_CLOSED_GOP;
			ctx->flags |= AV_CODEC_FLAG_LOW_DELAY;
			ctx->flags2 |= AV_CODEC_FLAG2_FAST;

			const std::string encoder_queue_depth = std::to_string(std::clamp(options.encoder_queue_depth, 1, 8));
			if (name.find("nvenc") != std::string::npos)
			{
				SetOptionalEncoderOption(ctx, "preset", "p1");
				SetOptionalEncoderOption(ctx, "tune", "ull");
				SetOptionalEncoderOption(ctx, "rc", IsVbr(options.video_rate_control) ? "vbr" : "cbr");
				SetOptionalEncoderOption(ctx, "zerolatency", "1");
				SetOptionalEncoderOption(ctx, "delay", "0");
				SetOptionalEncoderOption(ctx, "forced-idr", "1");
				SetOptionalEncoderOption(ctx, "repeat_spspps", "1");
				SetOptionalEncoderOption(ctx, "surfaces", encoder_queue_depth.c_str());
				SetOptionalEncoderOption(ctx, "cbr_padding", "0");
				if (IsVbr(options.video_rate_control))
				{
					SetOptionalEncoderOption(ctx, "rc-lookahead", "0");
					ApplyPrivateRateControlOptions(ctx, options.ice_bitrate_kbps, options.fps, options.video_rate_control);
				}
			}
			else if (name.find("qsv") != std::string::npos)
			{
				const std::string max_interval = std::to_string(std::numeric_limits<int>::max());
				SetOptionalEncoderOption(ctx, "preset", "veryfast");
				SetOptionalEncoderOption(ctx, "async_depth", encoder_queue_depth.c_str());
				SetOptionalEncoderOption(ctx, "low_delay_brc", "1");
				SetOptionalEncoderOption(ctx, "low_power", "1");
				SetOptionalEncoderOption(ctx, "look_ahead", "0");
				SetOptionalEncoderOption(ctx, "forced_idr", "1");
				SetOptionalEncoderOption(ctx, "repeat_pps", "1");
				SetOptionalEncoderOption(ctx, "idr_interval", max_interval.c_str());
				SetOptionalEncoderOption(ctx, "recovery_point_sei", "0");
				SetOptionalEncoderOption(ctx, "pic_timing_sei", "0");
				SetOptionalEncoderOption(ctx, "max_dec_frame_buffering", "1");
				if (IsVbr(options.video_rate_control))
				{
					SetOptionalEncoderOption(ctx, "rc_mode", "vbr");
					ApplyPrivateRateControlOptions(ctx, options.ice_bitrate_kbps, options.fps, options.video_rate_control);
				}
			}
			else if (name.find("amf") != std::string::npos)
			{
				SetOptionalEncoderOption(ctx, "usage", "ultralowlatency");
				SetOptionalEncoderOption(ctx, "quality", IsVbr(options.video_rate_control) ? "balanced" : "speed");
				SetOptionalEncoderOption(ctx, "rc", IsVbr(options.video_rate_control) ? "vbr_latency" : "cbr");
				SetOptionalEncoderOption(ctx, "forced_idr", "1");
				SetOptionalEncoderOption(ctx, "header_insertion_mode", "idr");
				SetOptionalEncoderOption(ctx, "gops_per_idr", "1");
				SetOptionalEncoderOption(ctx, "latency", "1");
				SetOptionalEncoderOption(ctx, "filler_data", "0");
				if (IsVbr(options.video_rate_control))
				{
					ApplyPrivateRateControlOptions(ctx, options.ice_bitrate_kbps, options.fps, options.video_rate_control);
				}
			}
			else if (name.find("libx264") != std::string::npos || name.find("libx265") != std::string::npos)
			{
				SetOptionalEncoderOption(ctx, "preset", "ultrafast");
				SetOptionalEncoderOption(ctx, "tune", "zerolatency");
				SetOptionalEncoderOption(ctx, "forced-idr", "1");
				SetOptionalEncoderOption(ctx, "x264-params", "keyint=-1:scenecut=0");
				SetOptionalEncoderOption(ctx, "x265-params", "info=0:keyint=-1");
				if (IsVbr(options.video_rate_control))
				{
					SetOptionalEncoderOption(ctx, "crf", "23");
					ApplyPrivateRateControlOptions(ctx, options.ice_bitrate_kbps, options.fps, options.video_rate_control);
				}
			}

			AVBufferRef *old_hw_device = hw_device_ctx_;
			AVBufferRef *old_hw_frames = hw_frames_ctx_;
			AVBufferRef *old_input_hw_device = input_hw_device_ctx_;
			AVBufferRef *old_input_hw_frames = input_hw_frames_ctx_;
			hw_device_ctx_ = nullptr;
			hw_frames_ctx_ = nullptr;
			input_hw_device_ctx_ = nullptr;
			input_hw_frames_ctx_ = nullptr;
			use_hw_frames_ = false;
			hw_frame_format_ = AV_PIX_FMT_NONE;
			encode_input_path_ = "none";
			if (old_hw_device != nullptr)
			{
				av_buffer_unref(&old_hw_device);
			}
			if (old_hw_frames != nullptr)
			{
				av_buffer_unref(&old_hw_frames);
			}
			if (old_input_hw_device != nullptr)
			{
				av_buffer_unref(&old_input_hw_device);
			}
			if (old_input_hw_frames != nullptr)
			{
				av_buffer_unref(&old_input_hw_frames);
			}
			if (can_try_qsv_hw)
			{
				std::string hw_error;
				if (!CreateQsvHardwareContexts(ctx, codec, options, hw_error))
				{
					error = "Failed to create QSV hwframe encode path for " + name + ": " + hw_error;
					avcodec_free_context(&ctx);
					return false;
				}
			}
			else if (can_try_d3d11)
			{
				std::string hw_error;
				if (!CreateD3D11HardwareContexts(ctx, options, hw_error))
				{
					error = "Failed to create D3D11 hwframe encode path for " + name + ": " + hw_error;
					avcodec_free_context(&ctx);
					return false;
				}
			}
			else
			{
				error = "Encoder " + name + " does not expose a supported GPU hwframe input path.";
				avcodec_free_context(&ctx);
				return false;
			}

			int open = avcodec_open2(ctx, codec, nullptr);
			if (open < 0)
			{
				char errbuf[256] = {};
				av_strerror(open, errbuf, sizeof(errbuf));
				error = "Failed to open encoder " + name + ": " + errbuf;
				avcodec_free_context(&ctx);
				if (hw_device_ctx_ != nullptr)
				{
					av_buffer_unref(&hw_device_ctx_);
					hw_device_ctx_ = nullptr;
				}
				if (hw_frames_ctx_ != nullptr)
				{
					av_buffer_unref(&hw_frames_ctx_);
					hw_frames_ctx_ = nullptr;
				}
				if (input_hw_device_ctx_ != nullptr)
				{
					av_buffer_unref(&input_hw_device_ctx_);
					input_hw_device_ctx_ = nullptr;
				}
				if (input_hw_frames_ctx_ != nullptr)
				{
					av_buffer_unref(&input_hw_frames_ctx_);
					input_hw_frames_ctx_ = nullptr;
				}
				use_hw_frames_ = false;
				hw_frame_format_ = AV_PIX_FMT_NONE;
				encode_input_path_ = "none";
				return false;
			}

			ctx_ = ctx;
			current_bitrate_kbps_ = options.ice_bitrate_kbps;
			next_pts_ = 0;
			return true;
		}

		AVCodecContext *ctx_ = nullptr;
		AVBufferRef *hw_device_ctx_ = nullptr;
		AVBufferRef *hw_frames_ctx_ = nullptr;
		AVBufferRef *input_hw_device_ctx_ = nullptr;
		AVBufferRef *input_hw_frames_ctx_ = nullptr;
		AVCodecID codec_id_ = AV_CODEC_ID_H264;
		std::string active_encoder_;
		std::string encode_input_path_{"none"};
		std::int64_t next_pts_ = 0;
		bool use_hw_frames_ = false;
		AVPixelFormat hw_frame_format_ = AV_PIX_FMT_NONE;
		int current_bitrate_kbps_ = 0;
		int fps_ = 60;
		VideoRateControl rate_control_mode_ = VideoRateControl::kCbr;
		ComPtr<ID3D11Device> capture_device_;
		ComPtr<ID3D11DeviceContext> capture_context_;
		ComPtr<ID3D11VideoDevice> video_device_;
		ComPtr<ID3D11VideoContext> video_context_;
		ComPtr<ID3D11VideoProcessorEnumerator> vp_enum_;
		ComPtr<ID3D11VideoProcessor> video_proc_;
		int vp_src_width_ = 0;
		int vp_src_height_ = 0;
		int vp_dst_width_ = 0;
		int vp_dst_height_ = 0;
		DXGI_FORMAT vp_src_format_ = DXGI_FORMAT_UNKNOWN;
		DXGI_FORMAT vp_dst_format_ = DXGI_FORMAT_UNKNOWN;
		ComPtr<ID3D11Texture2D> vp_output_texture_;
		int vp_output_width_ = 0;
		int vp_output_height_ = 0;
		DXGI_FORMAT vp_output_format_ = DXGI_FORMAT_UNKNOWN;
		bool logged_gpu_path_ = false;
	};

} // namespace streamproto::host
