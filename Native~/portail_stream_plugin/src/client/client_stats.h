#pragma once

#include <cmath>
#include <cstdint>
#include <limits>
#include <sstream>
#include <string>

namespace streamproto::client
{

	struct ClientStats
	{
		std::uint64_t recv_chunks = 0;
		std::uint64_t recv_bytes = 0;
		std::uint64_t video_bytes = 0;
		std::uint64_t assembled_frames = 0;
		std::uint64_t decoded_frames = 0;
		std::uint64_t audio_packets = 0;
		std::uint64_t audio_bytes = 0;
		std::uint64_t audio_frames = 0;
		std::uint64_t audio_decode_fail = 0;
		std::uint64_t dropped_frames = 0;
		std::uint64_t decode_fail = 0;
		std::uint64_t keyframe_requests_sent = 0;
		std::uint64_t gpu_frames = 0;
		std::uint64_t gpu_copy_fail = 0;
		std::uint64_t preview_hw_transfer_fail = 0;
		std::uint64_t latency_samples = 0;
		double last_video_reassemble_ms = 0.0;
		double last_decode_core_ms = 0.0;
		double last_post_decode_ms = 0.0;
		double last_preview_ms = 0.0;
		double last_video_texture_copy_ms = 0.0;
		double last_net_latency_ms = 0.0;
		double last_total_latency_ms = 0.0;
		double last_video_capture_to_texture_ms = 0.0;
		double last_audio_decode_ms = 0.0;
		double last_audio_capture_to_push_ms = 0.0;
		double last_audio_capture_to_unity_push_ms = 0.0;
		std::uint64_t last_audio_capture_ts_us = 0;
		double avg_net_latency_ms = 0.0;
		double avg_total_latency_ms = 0.0;
		double max_total_latency_ms = 0.0;
		bool clock_synced = false;
		double clock_offset_ms = 0.0;
		double clock_rtt_ms = 0.0;
		double clock_rtt_min_ms = 0.0;
	};

	struct ClockSyncState
	{
		bool valid = false;
		double offset_us = 0.0;
		double min_rtt_ms = std::numeric_limits<double>::infinity();
		double last_rtt_ms = 0.0;
		std::uint64_t samples = 0;
		std::uint32_t next_ping_sequence = 1;
		std::uint64_t last_ping_ms = 0;
	};

	inline std::string BuildClientStatsLine(
		const ClientStats &stats,
		const std::string &decoder_name,
		const char *path_name,
		bool connected,
		std::uint64_t delta_ms,
		const ClientStats &last_snapshot)
	{
		const double secs = static_cast<double>(delta_ms) / 1000.0;
		const std::uint64_t delta_decoded = stats.decoded_frames - last_snapshot.decoded_frames;
		const std::uint64_t delta_bytes = stats.recv_bytes - last_snapshot.recv_bytes;
		const std::uint64_t delta_chunks = stats.recv_chunks - last_snapshot.recv_chunks;
		const std::uint64_t delta_gpu = stats.gpu_frames - last_snapshot.gpu_frames;
		const std::uint64_t delta_audio_frames = stats.audio_frames - last_snapshot.audio_frames;
		const double fps = secs > 0.0 ? static_cast<double>(delta_decoded) / secs : 0.0;
		const double mbps = secs > 0.0 ? (static_cast<double>(delta_bytes) * 8.0 / 1'000'000.0) / secs : 0.0;
		const char *display_path = connected ? (path_name != nullptr ? path_name : "sdr") : "none";

		std::ostringstream line;
		line
			<< "[CLIENT] path=" << display_path
			<< " decoder=" << decoder_name
			<< " fps=" << static_cast<int>(std::round(fps))
			<< " rx=" << mbps << "Mbps"
			<< " vtexlat=" << static_cast<int>(std::llround(stats.last_video_capture_to_texture_ms)) << "ms"
			<< " alat=" << static_cast<int>(std::llround(stats.last_audio_capture_to_unity_push_ms)) << "ms"
			<< " rtt=" << static_cast<int>(std::llround(stats.clock_rtt_ms)) << "ms"
			<< " reasm=" << stats.last_video_reassemble_ms << "ms"
			<< " dec=" << stats.last_decode_core_ms << "ms"
			<< " copy=" << stats.last_video_texture_copy_ms << "ms"
			<< " chunks/s=" << (secs > 0.0 ? static_cast<std::uint64_t>(std::llround(static_cast<double>(delta_chunks) / secs)) : 0)
			<< " aud/s=" << (secs > 0.0 ? static_cast<std::uint64_t>(std::llround(static_cast<double>(delta_audio_frames) / secs)) : 0)
			<< " gpu/s=" << (secs > 0.0 ? static_cast<std::uint64_t>(std::llround(static_cast<double>(delta_gpu) / secs)) : 0)
			<< " dropped=" << stats.dropped_frames
			<< " keyreq=" << stats.keyframe_requests_sent
			<< " dec_fail=" << stats.decode_fail
			<< " aud_fail=" << stats.audio_decode_fail
			<< " gpu_fail=" << stats.gpu_copy_fail;
		return line.str();
	}

} // namespace streamproto::client
