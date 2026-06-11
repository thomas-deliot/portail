#pragma once

#include <chrono>
#include <cstdint>
#include <limits>
#include <unordered_map>

#include "common/audio_stream.h"
#include "common/lod_utils.h"
#include "sender/ffmpeg_encoder.h"
#include "sender/sender_lod_config.h"
#include "sender/sender_options.h"

namespace streamproto::sender
{

	struct SenderPeerStreamState
	{
		std::int32_t max_video_lod = 0;
		std::int32_t max_audio_lod = 0;
		SenderPeerLodAssignment assigned_lod{};
		std::int32_t effective_video_lod = streamproto::lod::kStreamLodOff;
		std::int32_t effective_audio_lod = streamproto::lod::kStreamLodOff;
		std::int32_t media_video_lod = streamproto::lod::kStreamLodOff;
		std::int32_t media_audio_lod = streamproto::lod::kStreamLodOff;
		std::uint32_t target_video_generation = 1;
		std::uint32_t media_video_generation = 0;
		std::uint32_t receiver_applied_video_generation = 0;
		std::uint64_t video_transition_started_ms = 0;
		std::uint32_t target_audio_generation = 1;
		std::uint32_t media_audio_generation = 0;
		std::uint32_t receiver_applied_audio_generation = 0;
		std::uint64_t audio_transition_started_ms = 0;
		std::uint64_t last_quality_send_ms = 0;
		std::uint64_t last_sent_available_video_lod_mask = std::numeric_limits<std::uint64_t>::max();
		std::uint64_t last_sent_available_audio_lod_mask = std::numeric_limits<std::uint64_t>::max();
		std::int32_t last_sent_effective_video_lod = streamproto::lod::kInvalidSentLod;
		std::int32_t last_sent_effective_audio_lod = streamproto::lod::kInvalidSentLod;
		std::int32_t last_sent_assigned_video_lod = streamproto::lod::kInvalidSentLod;
		std::int32_t last_sent_assigned_audio_lod = streamproto::lod::kInvalidSentLod;
		std::uint32_t last_sent_video_generation = 0;
		std::uint32_t last_sent_audio_generation = 0;
		std::unordered_map<std::int32_t, std::uint32_t> sent_video_config_generation_by_lod;
		std::unordered_map<std::int32_t, std::uint32_t> sent_audio_config_generation_by_lod;
		bool hello_sent = false;
		bool sender_pause_state_sent = false;
		bool last_sender_pause_sent = false;
		std::uint64_t sent_video_bytes = 0;
		std::uint64_t sent_audio_bytes = 0;
		std::uint64_t sent_video_chunks = 0;
		std::uint64_t sent_audio_frames = 0;
		double last_video_send_ms = 0.0;
		double last_audio_send_ms = 0.0;
	};

	struct VideoLodRuntime
	{
		SenderVideoLodConfigState config{};
		SenderOptions options{};
		FfmpegEncoder encoder{};
		bool encoder_ready = false;
		bool encoder_failed = false;
		std::uint64_t next_encoder_retry_ms = 0;
		std::uint32_t frame_id = 1;
		bool force_keyframe = true;
		std::uint64_t last_forced_keyframe_ms = 0;
		std::chrono::steady_clock::time_point next_frame_time = std::chrono::steady_clock::now();
		std::uint64_t encoded_frames = 0;
		std::uint64_t encoded_video_bytes = 0;
		double last_encode_ms = 0.0;
		double last_send_ms = 0.0;
		int receiver_count = 0;
		std::uint64_t disabled_grace_until_ms = 0;
	};

	struct AudioLodRuntime
	{
		SenderAudioLodConfigState config{};
		streamproto::audio::FfmpegOpusEncoder encoder{};
		bool encoder_failed = false;
		std::uint64_t next_encoder_retry_ms = 0;
		std::uint64_t encoded_audio_frames = 0;
		std::uint64_t encoded_audio_bytes = 0;
		double last_encode_ms = 0.0;
		double last_send_ms = 0.0;
		int receiver_count = 0;
		std::uint64_t disabled_grace_until_ms = 0;
	};

	inline bool VideoLodCanProduce(const VideoLodRuntime &lod, std::uint64_t now_ms)
	{
		return (lod.config.enabled || now_ms < lod.disabled_grace_until_ms) &&
			   (!lod.encoder_failed || now_ms >= lod.next_encoder_retry_ms);
	}

	inline bool VideoLodCanWarmEncoder(const VideoLodRuntime &lod, std::uint64_t now_ms)
	{
		return !lod.encoder_failed || now_ms >= lod.next_encoder_retry_ms;
	}

	inline bool AudioLodCanProduce(const AudioLodRuntime &lod, std::uint64_t now_ms)
	{
		return (lod.config.enabled || now_ms < lod.disabled_grace_until_ms) &&
			   (!lod.encoder_failed || now_ms >= lod.next_encoder_retry_ms);
	}

	inline std::uint32_t NextStreamGeneration(std::uint32_t current)
	{
		const std::uint32_t next = current + 1U;
		return next == 0 ? 1U : next;
	}

	inline void BeginVideoGenerationTransition(SenderPeerStreamState &state, std::uint64_t now_ms)
	{
		state.target_video_generation = NextStreamGeneration(state.target_video_generation);
		state.receiver_applied_video_generation = 0;
		state.video_transition_started_ms = now_ms;
		state.last_quality_send_ms = 0;
		state.last_sent_video_generation = 0;
		state.sent_video_config_generation_by_lod.erase(state.effective_video_lod);
	}

	inline void BeginAudioGenerationTransition(SenderPeerStreamState &state, std::uint64_t now_ms)
	{
		state.target_audio_generation = NextStreamGeneration(state.target_audio_generation);
		state.receiver_applied_audio_generation = 0;
		state.audio_transition_started_ms = now_ms;
		state.last_quality_send_ms = 0;
		state.last_sent_audio_generation = 0;
		state.sent_audio_config_generation_by_lod.erase(state.effective_audio_lod);
	}

	inline bool HasPendingVideoGeneration(const SenderPeerStreamState &state)
	{
		return state.media_video_generation != state.target_video_generation;
	}

	inline bool HasPendingAudioGeneration(const SenderPeerStreamState &state)
	{
		return state.media_audio_generation != state.target_audio_generation;
	}

} // namespace streamproto::sender
