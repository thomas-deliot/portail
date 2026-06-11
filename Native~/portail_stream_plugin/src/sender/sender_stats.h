#pragma once

#include <cmath>
#include <cstdint>
#include <sstream>
#include <string>

namespace streamproto::sender
{

	struct SenderStats
	{
		std::uint64_t capture_frames = 0;
		std::uint64_t encoded_frames = 0;
		std::uint64_t encoded_video_bytes = 0;
		std::uint64_t encoded_packets = 0;
		std::uint64_t sent_chunks = 0;
		std::uint64_t sent_bytes = 0;
		std::uint64_t sent_video_bytes = 0;
		std::uint64_t sent_audio_frames = 0;
		std::uint64_t sent_audio_bytes = 0;
		std::uint64_t encoded_audio_frames = 0;
		std::uint64_t encoded_audio_bytes = 0;
		std::uint64_t audio_encode_fail = 0;
		std::uint64_t capture_fail = 0;
		std::uint64_t encode_fail = 0;
		std::uint64_t keyframe_requests = 0;
		std::uint64_t queue_drop = 0;
		std::uint64_t realtime_drop = 0;
		int current_bitrate_kbps = 0;
		int pending_unreliable_bytes = 0;
		int queue_time_ms = 0;
		int send_rate_kbps = 0;
		double last_capture_ms = 0.0;
		double last_encode_ms = 0.0;
		double last_send_ms = 0.0;
		double last_video_encode_ms = 0.0;
		double last_video_send_ms = 0.0;
		double last_audio_encode_ms = 0.0;
		double last_audio_send_ms = 0.0;
	};

	inline std::string BuildSenderStatsLine(
		const SenderStats &stats,
		const std::string &encoder_name,
		const char *path_name,
		bool connected,
		std::uint64_t delta_ms,
		const SenderStats &last_snapshot)
	{
		const double secs = static_cast<double>(delta_ms) / 1000.0;
		const std::uint64_t frame_count = stats.encoded_frames > 0 ? stats.encoded_frames : stats.capture_frames;
		const std::uint64_t last_frame_count = last_snapshot.encoded_frames > 0 ? last_snapshot.encoded_frames : last_snapshot.capture_frames;
		const std::uint64_t delta_frames = frame_count >= last_frame_count ? frame_count - last_frame_count : 0;
		const std::uint64_t delta_bytes = stats.sent_bytes - last_snapshot.sent_bytes;
		const std::uint64_t delta_chunks = stats.sent_chunks - last_snapshot.sent_chunks;
		const std::uint64_t delta_audio_frames = stats.sent_audio_frames - last_snapshot.sent_audio_frames;
		const double fps = secs > 0.0 ? static_cast<double>(delta_frames) / secs : 0.0;
		const double mbps = secs > 0.0 ? (static_cast<double>(delta_bytes) * 8.0 / 1'000'000.0) / secs : 0.0;
		const char *display_path = connected ? (path_name != nullptr ? path_name : "sdr") : "none";

		std::ostringstream line;
		line
			<< "[SENDER] path=" << display_path
			<< " encoder=" << encoder_name
			<< " fps=" << static_cast<int>(std::round(fps))
			<< " tx=" << mbps << "Mbps"
			<< " cap=" << stats.last_capture_ms << "ms"
			<< " venc=" << stats.last_video_encode_ms << "ms"
			<< " vsend=" << stats.last_video_send_ms << "ms"
			<< " aenc=" << stats.last_audio_encode_ms << "ms"
			<< " asend=" << stats.last_audio_send_ms << "ms"
			<< " srate=" << stats.send_rate_kbps << "KBps"
			<< " q=" << (stats.pending_unreliable_bytes / 1024) << "KB"
			<< " qms=" << stats.queue_time_ms
			<< " chunks/s=" << (secs > 0.0 ? static_cast<std::uint64_t>(std::llround(static_cast<double>(delta_chunks) / secs)) : 0)
			<< " aud/s=" << (secs > 0.0 ? static_cast<std::uint64_t>(std::llround(static_cast<double>(delta_audio_frames) / secs)) : 0)
			<< " keyreq=" << stats.keyframe_requests
			<< " qdrop=" << stats.queue_drop
			<< " rtdrop=" << stats.realtime_drop
			<< " cap_fail=" << stats.capture_fail
			<< " enc_fail=" << stats.encode_fail
			<< " aud_fail=" << stats.audio_encode_fail
			<< "   ";
		return line.str();
	}

} // namespace streamproto::sender
