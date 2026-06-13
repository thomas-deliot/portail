#pragma once

#include <cstdint>

#include "common/lod_utils.h"
#include "common/protocol.h"
#include "common/steam_sdr.h"

namespace streamproto::receiver
{

	struct ReceiverMaxAcceptedQuality
	{
		std::int32_t video_lod = 0;
		std::int32_t audio_lod = 0;
	};

	inline std::uint64_t StreamConfigKey(std::int32_t lod, std::uint32_t generation)
	{
		const std::uint32_t lod_key = static_cast<std::uint32_t>(lod::Normalize(lod) + 1);
		return (static_cast<std::uint64_t>(generation) << 32U) | lod_key;
	}

	inline bool SendKeyframeRequest(
		SteamSdr &sdr,
		std::uint32_t first_missing_frame,
		std::uint32_t last_completed_frame,
		std::int32_t lod_index,
		std::uint32_t stream_generation)
	{
		proto::KeyframeRequestMessage msg{};
		proto::InitMessageHeader(msg, proto::MessageType::kKeyframeRequest);
		msg.first_missing_frame = first_missing_frame;
		msg.last_completed_frame = last_completed_frame;
		msg.lod_index = lod_index;
		msg.stream_generation = stream_generation;
		return sdr.Send(&msg, sizeof(msg), true);
	}

	inline bool SendPing(SteamSdr &sdr, std::uint32_t sequence, std::uint64_t receiver_send_ts_us)
	{
		proto::PingMessage msg{};
		proto::InitMessageHeader(msg, proto::MessageType::kPing);
		msg.sequence = sequence;
		msg.reserved = 0;
		msg.receiver_send_timestamp_us = receiver_send_ts_us;
		return sdr.Send(&msg, sizeof(msg), true);
	}

	inline bool SendStreamControl(
		SteamSdr &sdr,
		proto::StreamControlCommand command,
		std::uint64_t sender_steam_id)
	{
		proto::StreamControlMessage msg{};
		proto::InitMessageHeader(msg, proto::MessageType::kStreamControl);
		msg.command = static_cast<std::uint8_t>(command);
		msg.reserved[0] = 0;
		msg.reserved[1] = 0;
		msg.reserved[2] = 0;
		msg.sender_steam_id = sender_steam_id;
		return sdr.Send(&msg, sizeof(msg), true);
	}

	inline bool SendReceiverMaxAcceptedQuality(SteamSdr &sdr, const ReceiverMaxAcceptedQuality &quality, std::uint64_t sender_steam_id)
	{
		proto::StreamQualityMessage msg{};
		proto::InitMessageHeader(msg, proto::MessageType::kStreamQuality);
		msg.command = static_cast<std::uint8_t>(proto::StreamQualityCommand::kReceiverMaxAccepted);
		msg.max_video_lod = lod::Normalize(quality.video_lod);
		msg.max_audio_lod = lod::Normalize(quality.audio_lod);
		msg.assigned_video_lod = 0;
		msg.assigned_audio_lod = 0;
		msg.effective_video_lod = lod::kStreamLodOff;
		msg.effective_audio_lod = lod::kStreamLodOff;
		msg.available_video_lod_mask = 0;
		msg.available_audio_lod_mask = 0;
		msg.sender_steam_id = sender_steam_id;
		msg.sequence = 0;
		msg.profile_id = 0;
		return sdr.Send(&msg, sizeof(msg), true);
	}

	inline bool SendReceiverAppliedQuality(
		SteamSdr &sdr,
		std::uint64_t sender_steam_id,
		std::int32_t effective_video_lod,
		std::int32_t effective_audio_lod,
		std::uint32_t video_generation,
		std::uint32_t audio_generation)
	{
		proto::StreamQualityMessage msg{};
		proto::InitMessageHeader(msg, proto::MessageType::kStreamQuality);
		msg.command = static_cast<std::uint8_t>(proto::StreamQualityCommand::kReceiverApplied);
		msg.max_video_lod = lod::kStreamLodOff;
		msg.max_audio_lod = lod::kStreamLodOff;
		msg.assigned_video_lod = lod::kStreamLodOff;
		msg.assigned_audio_lod = lod::kStreamLodOff;
		msg.effective_video_lod = lod::Normalize(effective_video_lod);
		msg.effective_audio_lod = lod::Normalize(effective_audio_lod);
		msg.available_video_lod_mask = 0;
		msg.available_audio_lod_mask = 0;
		msg.sender_steam_id = sender_steam_id;
		msg.sequence = video_generation;
		msg.profile_id = audio_generation;
		return sdr.Send(&msg, sizeof(msg), true);
	}

} // namespace streamproto::receiver
