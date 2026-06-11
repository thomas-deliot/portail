#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <unordered_map>
#include <vector>

#include "common/protocol.h"
#include "common/protocol_validation.h"

namespace streamproto::client
{

	struct ReassembledFrame
	{
		std::uint32_t frame_id = 0;
		std::vector<std::uint8_t> bytes;
		bool keyframe = false;
		std::uint64_t capture_ts_us = 0;
	};

	class FrameAssembler
	{
	public:
		std::optional<ReassembledFrame> PushChunk(
			const proto::VideoChunkHeader &header,
			const std::uint8_t *payload,
			std::size_t payload_bytes,
			std::uint64_t now_ms)
		{
			if (payload == nullptr || !proto::ValidateVideoChunkHeader(header, payload_bytes))
			{
				return std::nullopt;
			}

			const std::size_t chunk_payload = header.chunk_payload_bytes;
			const std::size_t expected_data_chunks =
				(static_cast<std::size_t>(header.frame_size_bytes) + chunk_payload - 1U) / chunk_payload;
			if (expected_data_chunks == 0)
			{
				return std::nullopt;
			}

			auto frame_it = frames_.find(header.frame_id);
			if (frame_it == frames_.end())
			{
				if (frames_.size() >= kMaxPartialFrames)
				{
					frames_.erase(frames_.begin());
				}
				frame_it = frames_.emplace(header.frame_id, PartialFrame{}).first;
			}
			PartialFrame &frame = frame_it->second;
			if (!frame.initialized)
			{
				frame.initialized = true;
				frame.frame_size_bytes = header.frame_size_bytes;
				frame.chunk_payload_bytes = header.chunk_payload_bytes;
				frame.chunk_count = header.chunk_count;
				frame.capture_ts_us = header.capture_timestamp_us;
				frame.keyframe = (header.flags & proto::kChunkFlagKeyframe) != 0;
				frame.first_seen_ms = now_ms;
				frame.last_seen_ms = now_ms;
				frame.data_chunks.resize(expected_data_chunks, std::vector<std::uint8_t>(chunk_payload, 0));
				frame.data_sizes.resize(expected_data_chunks, 0);
				frame.has_chunk.resize(expected_data_chunks, false);
			}
			else if (frame.frame_size_bytes != header.frame_size_bytes ||
					 frame.chunk_payload_bytes != header.chunk_payload_bytes ||
					 frame.chunk_count != header.chunk_count)
			{
				frames_.erase(header.frame_id);
				return std::nullopt;
			}

			frame.keyframe = frame.keyframe || ((header.flags & proto::kChunkFlagKeyframe) != 0);
			frame.capture_ts_us = header.capture_timestamp_us;
			frame.last_seen_ms = now_ms;

			const bool is_parity = (header.flags & proto::kChunkFlagParity) != 0;
			const std::size_t chunk_id = header.chunk_id;
			if (!is_parity)
			{
				if (chunk_id >= frame.data_chunks.size() || frame.has_chunk[chunk_id])
				{
					return std::nullopt;
				}
				std::memcpy(frame.data_chunks[chunk_id].data(), payload, payload_bytes);
				frame.data_sizes[chunk_id] = header.chunk_original_bytes;
				frame.has_chunk[chunk_id] = true;
			}
			else
			{
				frame.parity.resize(frame.chunk_payload_bytes, 0);
				std::memcpy(frame.parity.data(), payload, payload_bytes);
				frame.has_parity = true;
			}

			return TryAssemble(header.frame_id);
		}

		std::uint64_t DropTimedOut(std::uint64_t now_ms, std::uint64_t timeout_ms, std::optional<std::uint32_t> &first_dropped_frame)
		{
			std::vector<std::uint32_t> to_drop;
			for (const auto &[frame_id, frame] : frames_)
			{
				if (!frame.initialized)
				{
					continue;
				}
				if (now_ms > frame.last_seen_ms && (now_ms - frame.last_seen_ms) >= timeout_ms)
				{
					to_drop.push_back(frame_id);
				}
			}

			if (!to_drop.empty())
			{
				std::sort(to_drop.begin(), to_drop.end());
				first_dropped_frame = to_drop.front();
				for (std::uint32_t frame_id : to_drop)
				{
					frames_.erase(frame_id);
				}
			}
			return to_drop.size();
		}

		void Reset()
		{
			frames_.clear();
		}

	private:
		struct PartialFrame
		{
			bool initialized = false;
			std::uint32_t frame_size_bytes = 0;
			std::uint16_t chunk_payload_bytes = 0;
			std::uint16_t chunk_count = 0;
			std::uint64_t capture_ts_us = 0;
			bool keyframe = false;
			std::uint64_t first_seen_ms = 0;
			std::uint64_t last_seen_ms = 0;

			std::vector<std::vector<std::uint8_t>> data_chunks;
			std::vector<std::uint16_t> data_sizes;
			std::vector<bool> has_chunk;

			std::vector<std::uint8_t> parity;
			bool has_parity = false;
		};

		std::optional<ReassembledFrame> TryAssemble(std::uint32_t frame_id)
		{
			auto it = frames_.find(frame_id);
			if (it == frames_.end())
			{
				return std::nullopt;
			}
			PartialFrame &frame = it->second;

			std::size_t missing = 0;
			std::size_t missing_index = 0;
			for (std::size_t i = 0; i < frame.has_chunk.size(); ++i)
			{
				if (!frame.has_chunk[i])
				{
					missing++;
					missing_index = i;
				}
			}

			if (missing == 1 && frame.has_parity)
			{
				std::vector<std::uint8_t> recovered(frame.chunk_payload_bytes, 0);
				for (std::size_t b = 0; b < frame.chunk_payload_bytes; ++b)
				{
					std::uint8_t value = frame.parity[b];
					for (std::size_t i = 0; i < frame.data_chunks.size(); ++i)
					{
						if (i == missing_index || !frame.has_chunk[i])
						{
							continue;
						}
						value ^= frame.data_chunks[i][b];
					}
					recovered[b] = value;
				}
				frame.data_chunks[missing_index] = std::move(recovered);
				const std::size_t offset = missing_index * frame.chunk_payload_bytes;
				const std::size_t bytes_left = static_cast<std::size_t>(frame.frame_size_bytes) - offset;
				frame.data_sizes[missing_index] = static_cast<std::uint16_t>(std::min<std::size_t>(bytes_left, frame.chunk_payload_bytes));
				frame.has_chunk[missing_index] = true;
				missing = 0;
			}

			if (missing != 0)
			{
				return std::nullopt;
			}

			ReassembledFrame out{};
			out.frame_id = frame_id;
			out.keyframe = frame.keyframe;
			out.capture_ts_us = frame.capture_ts_us;
			out.bytes.resize(frame.frame_size_bytes);

			for (std::size_t i = 0; i < frame.data_chunks.size(); ++i)
			{
				const std::size_t offset = i * frame.chunk_payload_bytes;
				if (offset >= out.bytes.size())
				{
					frames_.erase(it);
					return std::nullopt;
				}
				const std::size_t bytes_to_copy = std::min<std::size_t>(frame.data_sizes[i], out.bytes.size() - offset);
				std::memcpy(out.bytes.data() + offset, frame.data_chunks[i].data(), bytes_to_copy);
			}

			frames_.erase(it);
			return out;
		}

		std::unordered_map<std::uint32_t, PartialFrame> frames_;
		static constexpr std::size_t kMaxPartialFrames = 128;
	};

} // namespace streamproto::client
