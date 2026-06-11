#pragma once

#include <cstdint>

extern "C"
{

	struct SSPS_StartParams
	{
		std::uint32_t app_id;
		std::int32_t width;
		std::int32_t height;
		std::int32_t fps;
		std::int32_t target_bitrate_kbps;
		std::int32_t video_rate_control;
		std::int32_t disable_ice;
		std::int32_t chunk_payload_bytes;
		std::int32_t parity_shards;
		std::int32_t reliable_video;
		std::int32_t reliable_keyframes;
		std::int32_t max_queue_ms;
		std::int32_t enable_audio;
		std::int32_t audio_bitrate_kbps;
		const char *codec;
		const char *encoder;
	};

	struct SSPS_Stats
	{
		std::uint64_t capture_frames;
		std::uint64_t encoded_frames;
		std::uint64_t sent_chunks;
		std::uint64_t sent_bytes;
		double last_capture_ms;
		double last_encode_ms;
		double last_send_ms;
		double last_video_encode_ms;
		double last_video_send_ms;
		double last_audio_encode_ms;
		double last_audio_send_ms;
		std::uint64_t local_steam_id;
		std::int32_t connected;
		std::int32_t connected_ice_receivers;
		std::int32_t connected_sdr_receivers;
		std::int32_t preview_width;
		std::int32_t preview_height;
		std::int32_t encoded_width;
		std::int32_t encoded_height;
		std::int32_t encoded_fps;
		std::uint64_t sent_audio_frames;
		std::uint64_t sent_audio_bytes;
		std::int32_t audio_capture_state;
		std::int32_t audio_target_pid;
		std::int32_t reserved_audio_muted_sessions;
		std::uint64_t local_audio_samples_read;
		std::uint64_t encoded_video_bytes;
		std::uint64_t sent_video_bytes;
		std::uint64_t encoded_audio_frames;
		std::uint64_t encoded_audio_bytes;
	};

	struct SSPS_PeerStats
	{
		std::uint64_t receiver_steam_id;
		std::uint64_t encoded_frames;
		std::uint64_t sent_chunks;
		std::uint64_t sent_bytes;
		std::uint64_t sent_audio_frames;
		std::uint64_t sent_audio_bytes;
		std::int32_t connected;
		std::int32_t connection_path;
		std::int32_t preview_width;
		std::int32_t preview_height;
		std::int32_t paused_by_receiver;
		std::int32_t assigned_video_lod;
		std::int32_t assigned_audio_lod;
		std::int32_t max_video_lod;
		std::int32_t max_audio_lod;
		std::int32_t available_video_lod;
		std::int32_t available_audio_lod;
		std::int32_t effective_video_lod;
		std::int32_t effective_audio_lod;
		std::uint64_t encoded_video_bytes;
		std::uint64_t sent_video_bytes;
		std::uint64_t encoded_audio_frames;
		std::uint64_t encoded_audio_bytes;
		std::int32_t video_lod_used;
		std::int32_t audio_lod_used;
		double last_video_send_ms;
		double last_audio_send_ms;
	};

	struct SSPS_VideoLodConfig
	{
		std::int32_t enabled;
		std::int32_t width;
		std::int32_t height;
		std::int32_t fps;
		std::int32_t target_bitrate_kbps;
		std::int32_t video_rate_control;
	};

	struct SSPS_AudioLodConfig
	{
		std::int32_t enabled;
		std::int32_t bitrate_kbps;
	};

	struct SSPS_VideoLodStats
	{
		std::int32_t index;
		std::int32_t enabled;
		std::int32_t width;
		std::int32_t height;
		std::int32_t fps;
		std::int32_t target_bitrate_kbps;
		std::int32_t receiver_count;
		std::uint64_t encoded_frames;
		std::uint64_t encoded_video_bytes;
		double last_encode_ms;
		double last_send_ms;
	};

	struct SSPS_AudioLodStats
	{
		std::int32_t index;
		std::int32_t enabled;
		std::int32_t bitrate_kbps;
		std::int32_t receiver_count;
		std::uint64_t encoded_audio_frames;
		std::uint64_t encoded_audio_bytes;
		double last_encode_ms;
		double last_send_ms;
	};

} // extern "C"
