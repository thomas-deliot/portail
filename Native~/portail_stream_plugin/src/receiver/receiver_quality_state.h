#pragma once

#include <cstdint>

#include "receiver/receiver_protocol_sender.h"
#include "common/lod_utils.h"

namespace streamproto::receiver
{

	struct ReceiverPeerQualityState
	{
		ReceiverMaxAcceptedQuality max_accepted{};
		ReceiverMaxAcceptedQuality last_max_accepted_sent{
			lod::kInvalidSentLod,
			lod::kInvalidSentLod};
		std::int32_t assigned_video_lod = 0;
		std::int32_t assigned_audio_lod = 0;
		std::int32_t available_video_lod = lod::kStreamLodOff;
		std::int32_t available_audio_lod = lod::kStreamLodOff;
		std::uint64_t available_video_lod_mask = 0;
		std::int32_t effective_video_lod = lod::kStreamLodOff;
		std::int32_t effective_audio_lod = lod::kStreamLodOff;
		std::int32_t desired_video_lod = lod::kStreamLodOff;
		std::int32_t desired_audio_lod = lod::kStreamLodOff;
		std::uint32_t desired_video_generation = 0;
		std::uint32_t active_video_generation = 0;
		std::uint32_t ack_pending_video_generation = 0;
		std::uint32_t last_acked_video_generation = 0;
		std::uint32_t desired_audio_generation = 0;
		std::uint32_t active_audio_generation = 0;
		std::uint32_t ack_pending_audio_generation = 0;
		std::uint32_t last_acked_audio_generation = 0;
		bool max_accepted_update_pending = true;

		void Reset()
		{
			max_accepted = {};
			last_max_accepted_sent = {lod::kInvalidSentLod, lod::kInvalidSentLod};
			assigned_video_lod = 0;
			assigned_audio_lod = 0;
			available_video_lod = lod::kStreamLodOff;
			available_audio_lod = lod::kStreamLodOff;
			available_video_lod_mask = 0;
			effective_video_lod = lod::kStreamLodOff;
			effective_audio_lod = lod::kStreamLodOff;
			desired_video_lod = lod::kStreamLodOff;
			desired_audio_lod = lod::kStreamLodOff;
			desired_video_generation = 0;
			active_video_generation = 0;
			ack_pending_video_generation = 0;
			last_acked_video_generation = 0;
			desired_audio_generation = 0;
			active_audio_generation = 0;
			ack_pending_audio_generation = 0;
			last_acked_audio_generation = 0;
			max_accepted_update_pending = true;
		}

		bool SetMaxAcceptedQuality(const ReceiverMaxAcceptedQuality &quality)
		{
			ReceiverMaxAcceptedQuality normalized{};
			normalized.video_lod = lod::Normalize(quality.video_lod);
			normalized.audio_lod = lod::Normalize(quality.audio_lod);
			if (max_accepted.video_lod == normalized.video_lod &&
				max_accepted.audio_lod == normalized.audio_lod &&
				!max_accepted_update_pending)
			{
				return false;
			}
			max_accepted = normalized;
			max_accepted_update_pending = true;
			return true;
		}

		bool NeedsMaxAcceptedSend() const
		{
			return max_accepted_update_pending ||
				   max_accepted.video_lod != last_max_accepted_sent.video_lod ||
				   max_accepted.audio_lod != last_max_accepted_sent.audio_lod;
		}

		void MarkMaxAcceptedSent()
		{
			last_max_accepted_sent = max_accepted;
			max_accepted_update_pending = false;
		}

		void MarkDisconnected()
		{
			max_accepted_update_pending = true;
		}

		bool VideoAckPending() const
		{
			return ack_pending_video_generation != 0 &&
				   ack_pending_video_generation != last_acked_video_generation;
		}

		bool AudioAckPending() const
		{
			return ack_pending_audio_generation != 0 &&
				   ack_pending_audio_generation != last_acked_audio_generation;
		}

		void MarkVideoAckSent()
		{
			if (ack_pending_video_generation != 0)
			{
				last_acked_video_generation = ack_pending_video_generation;
				ack_pending_video_generation = 0;
			}
		}

		void MarkAudioAckSent()
		{
			if (ack_pending_audio_generation != 0)
			{
				last_acked_audio_generation = ack_pending_audio_generation;
				ack_pending_audio_generation = 0;
			}
		}
	};

} // namespace streamproto::receiver
