#pragma once

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <limits>
#include <unordered_map>
#include <vector>

#include "common/audio_stream.h"
#include "common/protocol.h"
#include "common/steam_sdr.h"
#include "sender/encoded_packet.h"
#include "sender/sender_peer_sender.h"

namespace streamproto::sender
{

	struct SenderMediaTarget
	{
		std::uint64_t steam_id = 0;
		std::uint32_t generation = 0;
	};

	struct SenderPeerSendDelta
	{
		std::uint64_t sent_video_chunks = 0;
		std::uint64_t sent_video_bytes = 0;
		std::uint64_t sent_audio_frames = 0;
		std::uint64_t sent_audio_bytes = 0;
	};

	struct SenderMediaSendResult
	{
		bool sent = false;
		bool queue_drop = false;
		std::uint64_t encoded_video_bytes = 0;
		std::uint64_t encoded_audio_frames = 0;
		std::uint64_t encoded_audio_bytes = 0;
		std::uint64_t sent_chunks = 0;
		std::uint64_t sent_bytes = 0;
		std::uint64_t sent_video_bytes = 0;
		std::uint64_t sent_audio_frames = 0;
		std::uint64_t sent_audio_bytes = 0;
		std::unordered_map<std::uint64_t, SenderPeerSendDelta> peer_deltas;
	};

	inline SenderMediaSendResult SendAudioFrame(
		const SenderPeerSenderMap &peer_senders,
		const std::vector<SenderMediaTarget> &targets,
		const streamproto::audio::AudioFrame &frame,
		const std::vector<std::uint8_t> &payload,
		int lod_index)
	{
		namespace proto = streamproto::proto;

		SenderMediaSendResult result{};
		if (payload.empty() || payload.size() > 1200)
		{
			return result;
		}

		const std::size_t packet_size = sizeof(proto::AudioFrameHeader) + payload.size();
		result.encoded_audio_frames = 1;
		result.encoded_audio_bytes = payload.size();

		for (const SenderMediaTarget &target : targets)
		{
			if (target.steam_id == 0 || target.generation == 0)
			{
				continue;
			}
			auto sender_it = peer_senders.find(target.steam_id);
			if (sender_it == peer_senders.end() || sender_it->second == nullptr)
			{
				continue;
			}

			std::vector<std::uint8_t> packet(packet_size);
			auto *header = reinterpret_cast<proto::AudioFrameHeader *>(packet.data());
			proto::InitMessageHeader(*header, proto::MessageType::kAudioFrame);
			header->sequence = frame.sequence;
			header->payload_bytes = static_cast<std::uint32_t>(payload.size());
			header->sample_frames = static_cast<std::uint16_t>(frame.sample_frames);
			header->channels = static_cast<std::uint16_t>(streamproto::audio::kChannels);
			header->lod_index = lod_index;
			header->stream_generation = target.generation;
			header->capture_timestamp_us = frame.capture_timestamp_us;
			std::memcpy(packet.data() + sizeof(proto::AudioFrameHeader), payload.data(), payload.size());

			SenderSendEnqueueResult enqueue = sender_it->second->Enqueue(std::move(packet), false, streamproto::SdrLane::kAudio);
			if (enqueue.dropped_old || enqueue.dropped_new)
			{
				result.queue_drop = true;
			}
			if (enqueue.queued)
			{
				result.sent = true;
				result.sent_audio_frames++;
				result.sent_audio_bytes += packet_size;
				result.sent_bytes += packet_size;
				SenderPeerSendDelta &delta = result.peer_deltas[target.steam_id];
				delta.sent_audio_frames++;
				delta.sent_audio_bytes += packet_size;
			}
		}
		return result;
	}

	inline SenderMediaSendResult SendEncodedPacket(
		const SenderPeerSenderMap &peer_senders,
		const std::vector<SenderMediaTarget> &targets,
		std::uint32_t frame_id,
		const EncodedPacket &packet,
		int chunk_payload_bytes,
		int unreliable_chunk_payload_limit,
		int parity_shards,
		bool reliable_video,
		bool reliable_keyframes,
		int lod_index)
	{
		namespace proto = streamproto::proto;

		SenderMediaSendResult result{};
		if (packet.bytes.empty())
		{
			return result;
		}
		if (packet.bytes.size() > std::numeric_limits<std::uint32_t>::max())
		{
			return result;
		}

		const bool send_reliable_for_frame = reliable_video || (reliable_keyframes && packet.keyframe);
		int effective_chunk_payload_bytes = std::clamp(chunk_payload_bytes, 512, 60000);
		if (!send_reliable_for_frame && unreliable_chunk_payload_limit > 0)
		{
			effective_chunk_payload_bytes = std::min(effective_chunk_payload_bytes, std::max(512, unreliable_chunk_payload_limit));
		}

		const std::size_t frame_size = packet.bytes.size();
		result.encoded_video_bytes = frame_size;
		const std::size_t data_chunks = (frame_size + static_cast<std::size_t>(effective_chunk_payload_bytes) - 1U) /
										static_cast<std::size_t>(effective_chunk_payload_bytes);
		const std::size_t extra_parity_chunks = (parity_shards > 0) ? 1U : 0U;
		const std::size_t total_chunks = data_chunks + extra_parity_chunks;
		if (total_chunks == 0 || total_chunks > std::numeric_limits<std::uint16_t>::max())
		{
			return result;
		}

		std::vector<std::uint8_t> parity(static_cast<std::size_t>(effective_chunk_payload_bytes), 0);

		for (std::size_t i = 0; i < data_chunks; ++i)
		{
			const std::size_t offset = i * static_cast<std::size_t>(effective_chunk_payload_bytes);
			const std::size_t bytes_left = frame_size - offset;
			const std::size_t payload_bytes = std::min<std::size_t>(bytes_left, static_cast<std::size_t>(effective_chunk_payload_bytes));

			for (std::size_t b = 0; b < payload_bytes; ++b)
			{
				parity[b] ^= packet.bytes[offset + b];
			}

			const std::size_t msg_size = sizeof(proto::VideoChunkHeader) + payload_bytes;
			bool sent_any = false;
			for (const SenderMediaTarget &target : targets)
			{
				if (target.steam_id == 0 || target.generation == 0)
				{
					continue;
				}
				auto sender_it = peer_senders.find(target.steam_id);
				if (sender_it == peer_senders.end() || sender_it->second == nullptr)
				{
					continue;
				}

				std::vector<std::uint8_t> queued_msg(msg_size);
				auto *header = reinterpret_cast<proto::VideoChunkHeader *>(queued_msg.data());
				proto::InitMessageHeader(*header, proto::MessageType::kVideoChunk);
				header->frame_id = frame_id;
				header->frame_size_bytes = static_cast<std::uint32_t>(frame_size);
				header->chunk_id = static_cast<std::uint16_t>(i);
				header->chunk_count = static_cast<std::uint16_t>(total_chunks);
				header->chunk_payload_bytes = static_cast<std::uint16_t>(effective_chunk_payload_bytes);
				header->chunk_original_bytes = static_cast<std::uint16_t>(payload_bytes);
				header->flags = static_cast<std::uint8_t>(packet.keyframe ? proto::kChunkFlagKeyframe : 0);
				header->reserved = 0;
				header->lod_index = lod_index;
				header->stream_generation = target.generation;
				header->capture_timestamp_us = packet.capture_ts_us;
				std::memcpy(queued_msg.data() + sizeof(proto::VideoChunkHeader), packet.bytes.data() + offset, payload_bytes);

				SenderSendEnqueueResult enqueue = sender_it->second->Enqueue(std::move(queued_msg), send_reliable_for_frame, streamproto::SdrLane::kVideo);
				if (enqueue.dropped_old || enqueue.dropped_new)
				{
					result.queue_drop = true;
				}
				if (enqueue.queued)
				{
					sent_any = true;
					result.sent = true;
					result.sent_chunks++;
					result.sent_bytes += msg_size;
					result.sent_video_bytes += msg_size;
					SenderPeerSendDelta &delta = result.peer_deltas[target.steam_id];
					delta.sent_video_chunks++;
					delta.sent_video_bytes += msg_size;
				}
			}
			if (!sent_any)
			{
				result.sent = false;
				return result;
			}
		}

		if (extra_parity_chunks > 0)
		{
			const std::size_t parity_msg_size = sizeof(proto::VideoChunkHeader) + parity.size();
			bool sent_any = false;
			for (const SenderMediaTarget &target : targets)
			{
				if (target.steam_id == 0 || target.generation == 0)
				{
					continue;
				}
				auto sender_it = peer_senders.find(target.steam_id);
				if (sender_it == peer_senders.end() || sender_it->second == nullptr)
				{
					continue;
				}

				std::vector<std::uint8_t> queued_msg(parity_msg_size);
				auto *header = reinterpret_cast<proto::VideoChunkHeader *>(queued_msg.data());
				proto::InitMessageHeader(*header, proto::MessageType::kVideoChunk);
				header->frame_id = frame_id;
				header->frame_size_bytes = static_cast<std::uint32_t>(frame_size);
				header->chunk_id = static_cast<std::uint16_t>(data_chunks);
				header->chunk_count = static_cast<std::uint16_t>(total_chunks);
				header->chunk_payload_bytes = static_cast<std::uint16_t>(effective_chunk_payload_bytes);
				header->chunk_original_bytes = static_cast<std::uint16_t>(effective_chunk_payload_bytes);
				header->flags = static_cast<std::uint8_t>((packet.keyframe ? proto::kChunkFlagKeyframe : 0) | proto::kChunkFlagParity);
				header->reserved = 0;
				header->lod_index = lod_index;
				header->stream_generation = target.generation;
				header->capture_timestamp_us = packet.capture_ts_us;
				std::memcpy(queued_msg.data() + sizeof(proto::VideoChunkHeader), parity.data(), parity.size());

				SenderSendEnqueueResult enqueue = sender_it->second->Enqueue(std::move(queued_msg), send_reliable_for_frame, streamproto::SdrLane::kVideo);
				if (enqueue.dropped_old || enqueue.dropped_new)
				{
					result.queue_drop = true;
				}
				if (enqueue.queued)
				{
					sent_any = true;
					result.sent = true;
					result.sent_chunks++;
					result.sent_bytes += parity_msg_size;
					result.sent_video_bytes += parity_msg_size;
					SenderPeerSendDelta &delta = result.peer_deltas[target.steam_id];
					delta.sent_video_chunks++;
					delta.sent_video_bytes += parity_msg_size;
				}
			}
			if (!sent_any)
			{
				result.sent = false;
				return result;
			}
		}

		return result;
	}

} // namespace streamproto::sender
