#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

extern "C"
{
#include <libavcodec/avcodec.h>
#include <libavutil/buffer.h>
}

#include "receiver/receiver_protocol_sender.h"
#include "receiver/receiver_video_decode_queue.h"
#include "receiver/ffmpeg_decoder.h"
#include "receiver/frame_assembler.h"
#include "receiver/gpu_texture_bridge.h"
#include "common/lod_utils.h"
#include "common/protocol.h"

namespace streamproto::receiver
{

	struct VideoDecoderConfigKey
	{
		std::uint16_t codec = 0;
		std::uint16_t width = 0;
		std::uint16_t height = 0;
		std::uint16_t fps = 0;

		[[nodiscard]] bool Valid() const
		{
			return codec != 0 && width != 0 && height != 0 && fps != 0;
		}

		bool operator==(const VideoDecoderConfigKey &rhs) const
		{
			return codec == rhs.codec &&
				   width == rhs.width &&
				   height == rhs.height &&
				   fps == rhs.fps;
		}
	};

	struct VideoDecoderConfigKeyHash
	{
		std::size_t operator()(const VideoDecoderConfigKey &key) const
		{
			std::size_t h = std::hash<std::uint16_t>{}(key.codec);
			h ^= std::hash<std::uint16_t>{}(key.width) + 0x9e3779b9U + (h << 6U) + (h >> 2U);
			h ^= std::hash<std::uint16_t>{}(key.height) + 0x9e3779b9U + (h << 6U) + (h >> 2U);
			h ^= std::hash<std::uint16_t>{}(key.fps) + 0x9e3779b9U + (h << 6U) + (h >> 2U);
			return h;
		}
	};

	struct ConfiguredVideoLod
	{
		bool enabled = true;
		std::uint16_t codec = static_cast<std::uint16_t>(proto::Codec::kH264);
		std::uint16_t width = 1920;
		std::uint16_t height = 1080;
		std::uint16_t fps = 60;
	};

	struct ReceiverVideoState
	{
		FrameAssembler assembler{};
		ReceiverVideoDecodeQueue decode_queue{};
		mutable std::mutex decode_mutex;
		std::unordered_map<VideoDecoderConfigKey, std::unique_ptr<FfmpegDecoder>, VideoDecoderConfigKeyHash> decoders_by_config;
		AVBufferRef *shared_decoder_hw_device_ctx = nullptr;
		std::int32_t active_decoder_lod = lod::kStreamLodOff;
		VideoDecoderConfigKey active_decoder_key{};
		bool has_active_decoder_key = false;
		AVCodecID codec_id = AV_CODEC_ID_NONE;
		std::vector<ConfiguredVideoLod> configured_lods;
		std::unordered_map<std::uint64_t, proto::StreamConfigMessage> stream_configs_by_key;
		GpuTextureBridge gpu_bridge{};
		bool has_stream_config = false;
		proto::StreamConfigMessage current_cfg{};
		bool has_cfg = false;
		std::string active_decoder = "n/a";
		std::uint32_t last_completed_frame = 0;
		std::uint32_t last_assembled_frame = 0;
		std::uint32_t last_gpu_frame = 0;
		std::uint64_t last_gpu_capture_ts_us = 0;
		std::uint64_t last_keyreq_ms = 0;
		bool pending_keyframe_request = false;
		std::uint32_t pending_keyframe_first_missing = 1;
		bool deferred_loss_recovery = false;
		std::uint32_t deferred_loss_after_frame = 0;
		std::uint32_t deferred_loss_first_missing = 1;
		std::uint64_t deferred_loss_start_ms = 0;
		bool waiting_for_keyframe = true;
		bool skip_next_recovery_flush = false;
		std::uint32_t no_output_packets = 0;
		std::uint32_t consecutive_decode_failures = 0;
	};

} // namespace streamproto::receiver
