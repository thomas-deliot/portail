#pragma once

#include <algorithm>
#include <cstdint>
#include <vector>

#include "common/audio_stream.h"
#include "common/lod_utils.h"

namespace streamproto::sender
{

	inline constexpr int kMaxConfiguredLods = 32;

	struct SenderVideoLodConfigState
	{
		bool enabled = true;
		int width = 1920;
		int height = 1080;
		int fps = 60;
		int target_bitrate_kbps = 12000;
		int video_rate_control = 0;
	};

	struct SenderAudioLodConfigState
	{
		bool enabled = true;
		int bitrate_kbps = streamproto::audio::kDefaultBitrateKbps;
	};

	struct SenderPeerLodAssignment
	{
		std::int32_t video_lod = 0;
		std::int32_t audio_lod = 0;
	};

	struct SenderPeerExportState
	{
		std::uint64_t receiver_steam_id = 0;
		std::uint64_t encoded_frames = 0;
		std::uint64_t encoded_video_bytes = 0;
		std::uint64_t sent_chunks = 0;
		std::uint64_t sent_bytes = 0;
		std::uint64_t sent_video_bytes = 0;
		std::uint64_t sent_audio_frames = 0;
		std::uint64_t sent_audio_bytes = 0;
		std::uint64_t encoded_audio_frames = 0;
		std::uint64_t encoded_audio_bytes = 0;
		std::int32_t video_lod_used = -1;
		std::int32_t audio_lod_used = -1;
		std::int32_t connected = 0;
		std::int32_t connection_path = 0;
		std::int32_t preview_width = 0;
		std::int32_t preview_height = 0;
		std::int32_t paused_by_receiver = 0;
		std::int32_t assigned_video_lod = 0;
		std::int32_t assigned_audio_lod = 0;
		std::int32_t max_video_lod = 0;
		std::int32_t max_audio_lod = 0;
		std::int32_t available_video_lod = streamproto::lod::kStreamLodOff;
		std::int32_t available_audio_lod = streamproto::lod::kStreamLodOff;
		std::int32_t effective_video_lod = streamproto::lod::kStreamLodOff;
		std::int32_t effective_audio_lod = streamproto::lod::kStreamLodOff;
		double last_video_send_ms = 0.0;
		double last_audio_send_ms = 0.0;
	};

	struct SenderVideoLodExportState
	{
		std::int32_t index = 0;
		std::int32_t enabled = 0;
		std::int32_t width = 0;
		std::int32_t height = 0;
		std::int32_t fps = 0;
		std::int32_t target_bitrate_kbps = 0;
		std::int32_t receiver_count = 0;
		std::uint64_t encoded_frames = 0;
		std::uint64_t encoded_video_bytes = 0;
		double last_encode_ms = 0.0;
		double last_send_ms = 0.0;
	};

	struct SenderAudioLodExportState
	{
		std::int32_t index = 0;
		std::int32_t enabled = 0;
		std::int32_t bitrate_kbps = 0;
		std::int32_t receiver_count = 0;
		std::uint64_t encoded_audio_frames = 0;
		std::uint64_t encoded_audio_bytes = 0;
		double last_encode_ms = 0.0;
		double last_send_ms = 0.0;
	};

	inline SenderVideoLodConfigState SanitizeVideoLodConfig(const SenderVideoLodConfigState &input)
	{
		SenderVideoLodConfigState cfg = input;
		cfg.width = std::clamp(cfg.width, 16, 8192);
		cfg.height = std::clamp(cfg.height, 16, 8192);
		cfg.fps = std::clamp(cfg.fps, 1, 240);
		cfg.target_bitrate_kbps = std::clamp(cfg.target_bitrate_kbps, 100, 200000);
		cfg.video_rate_control = cfg.video_rate_control == 1 ? 1 : 0;
		return cfg;
	}

	inline SenderAudioLodConfigState SanitizeAudioLodConfig(const SenderAudioLodConfigState &input)
	{
		SenderAudioLodConfigState cfg = input;
		cfg.bitrate_kbps = streamproto::audio::ClampAudioBitrateKbps(cfg.bitrate_kbps);
		return cfg;
	}

	inline std::vector<SenderVideoLodConfigState> DefaultVideoLodConfigs()
	{
		return {
			SenderVideoLodConfigState{true, 1920, 1080, 60, 12000, 0},
			SenderVideoLodConfigState{true, 1280, 720, 60, 6000, 0},
			SenderVideoLodConfigState{true, 854, 480, 60, 3000, 0},
			SenderVideoLodConfigState{true, 640, 360, 60, 1500, 0},
		};
	}

	inline std::vector<SenderAudioLodConfigState> DefaultAudioLodConfigs()
	{
		return {SenderAudioLodConfigState{true, streamproto::audio::kDefaultBitrateKbps}};
	}

} // namespace streamproto::sender
