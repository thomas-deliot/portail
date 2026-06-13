#pragma once

#include <cstdint>

extern "C"
{

	struct SSPR_StartParams
	{
		std::uint32_t app_id;
		std::uint64_t sender_steam_id;
		std::int32_t disable_ice;
	};

	struct SSPR_VideoLodConfig
	{
		std::int32_t enabled;
		std::int32_t width;
		std::int32_t height;
		std::int32_t fps;
		std::int32_t codec;
	};

	struct SSPR_Stats
	{
		std::uint64_t recv_chunks;
		std::uint64_t recv_bytes;
		std::uint64_t video_bytes;
		std::uint64_t decoded_frames;
		std::uint64_t audio_packets;
		std::uint64_t audio_bytes;
		std::uint64_t audio_frames;
		double last_video_reassemble_ms;
		double last_decode_ms;
		double last_post_decode_ms;
		double last_video_texture_copy_ms;
		double last_total_latency_ms;
		double last_video_capture_to_texture_ms;
		double last_audio_decode_ms;
		double last_audio_capture_to_push_ms;
		double last_audio_capture_to_unity_push_ms;
		std::uint64_t local_steam_id;
		std::int32_t connected;
		std::int32_t preview_width;
		std::int32_t preview_height;
		std::int32_t paused_by_sender;
	};

	struct SSPR_PeerStats
	{
		std::uint64_t sender_steam_id;
		std::uint64_t recv_chunks;
		std::uint64_t recv_bytes;
		std::uint64_t video_bytes;
		std::uint64_t decoded_frames;
		std::uint64_t audio_packets;
		std::uint64_t audio_bytes;
		std::uint64_t audio_frames;
		double last_video_reassemble_ms;
		double last_decode_ms;
		double last_post_decode_ms;
		double last_video_texture_copy_ms;
		double last_total_latency_ms;
		double last_video_capture_to_texture_ms;
		double last_audio_decode_ms;
		double last_audio_capture_to_push_ms;
		double last_audio_capture_to_unity_push_ms;
		std::int32_t connected;
		std::int32_t connection_path;
		std::int32_t preview_width;
		std::int32_t preview_height;
		std::int32_t paused_by_sender;
		std::int32_t assigned_video_lod;
		std::int32_t assigned_audio_lod;
		std::int32_t max_video_lod;
		std::int32_t max_audio_lod;
		std::int32_t available_video_lod;
		std::int32_t available_audio_lod;
		std::int32_t effective_video_lod;
		std::int32_t effective_audio_lod;
	};

} // extern "C"
