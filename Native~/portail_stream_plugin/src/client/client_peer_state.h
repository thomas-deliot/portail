#pragma once

#include <cstdint>

#include <windows.h>
#include <d3d11.h>
#include <wrl/client.h>

namespace streamproto::client
{

	struct PeerExportState
	{
		std::uint64_t host_steam_id = 0;
		std::uint64_t shared_handle = 0;
		std::uint64_t recv_chunks = 0;
		std::uint64_t recv_bytes = 0;
		std::uint64_t video_bytes = 0;
		std::uint64_t decoded_frames = 0;
		std::uint64_t audio_packets = 0;
		std::uint64_t audio_bytes = 0;
		std::uint64_t audio_frames = 0;
		double last_video_reassemble_ms = 0.0;
		double last_decode_ms = 0.0;
		double last_post_decode_ms = 0.0;
		double last_video_texture_copy_ms = 0.0;
		double last_total_latency_ms = 0.0;
		double last_video_capture_to_texture_ms = 0.0;
		double last_audio_decode_ms = 0.0;
		double last_audio_capture_to_push_ms = 0.0;
		double last_audio_capture_to_unity_push_ms = 0.0;
		std::uint64_t last_video_capture_ts_us = 0;
		std::uint64_t last_audio_capture_ts_us = 0;
		double clock_offset_ms = 0.0;
		std::int32_t clock_synced = 0;
		std::int32_t connected = 0;
		std::int32_t connection_path = 0;
		std::int32_t preview_width = 0;
		std::int32_t preview_height = 0;
		std::int32_t paused_by_host = 0;
		std::int32_t assigned_video_lod = 0;
		std::int32_t assigned_audio_lod = 0;
		std::int32_t max_video_lod = 0;
		std::int32_t max_audio_lod = 0;
		std::int32_t available_video_lod = -1;
		std::int32_t available_audio_lod = -1;
		std::int32_t effective_video_lod = -1;
		std::int32_t effective_audio_lod = -1;
	};

	struct UnityOutputBinding
	{
		ID3D11Texture2D *target_texture = nullptr;
		HANDLE opened_source_handle = nullptr;
		Microsoft::WRL::ComPtr<ID3D11Texture2D> opened_source_texture;
	};

} // namespace streamproto::client
