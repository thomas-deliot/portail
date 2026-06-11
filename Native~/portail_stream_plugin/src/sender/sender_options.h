#pragma once

#include <cstdint>
#include <string>

#include "common/audio_stream.h"

namespace streamproto::sender
{

	enum class VideoRateControl : std::int32_t
	{
		kCbr = 0,
		kVbr = 1,
	};

	inline VideoRateControl NormalizeVideoRateControl(std::int32_t value)
	{
		return value == static_cast<std::int32_t>(VideoRateControl::kVbr)
				   ? VideoRateControl::kVbr
				   : VideoRateControl::kCbr;
	}

	inline bool IsVbr(VideoRateControl mode)
	{
		return mode == VideoRateControl::kVbr;
	}

	struct SenderOptions
	{
		std::uint32_t app_id = 0;
		int width = 1280;
		int height = 720;
		int fps = 60;
		int ice_bitrate_kbps = 20000;
		int sdr_bitrate_kbps = 7000;
		bool disable_ice = false;
		std::string codec = "h264";
		std::string encoder_pref = "auto";
		int chunk_payload_bytes = 24000;
		int parity_shards = 0;
		int gop = 0;
		VideoRateControl video_rate_control = VideoRateControl::kCbr;
		int encoder_queue_depth = 1;
		bool reliable_video = false;
		bool reliable_keyframes = false;
		int max_queue_ms = 120;
		bool enable_audio = true;
		int audio_bitrate_kbps = streamproto::audio::kDefaultBitrateKbps;
	};

} // namespace streamproto::sender
