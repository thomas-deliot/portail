#pragma once

#include <cstddef>
#include <cstdint>

#include "common/protocol.h"

namespace streamproto::proto
{

	constexpr std::size_t kMaxEncodedVideoFrameBytes = 64ULL * 1024ULL * 1024ULL;
	constexpr std::size_t kMaxVideoChunksPerFrame = 4096;
	constexpr std::size_t kMaxVideoChunkPayloadBytes = 60000;
	constexpr std::size_t kMaxAudioFramePayloadBytes = 16ULL * 1024ULL;
	constexpr std::uint16_t kMaxAudioSampleFrames = 5760;

	inline bool IsKnownCodec(std::uint16_t codec)
	{
		return codec == static_cast<std::uint16_t>(Codec::kH264) ||
			   codec == static_cast<std::uint16_t>(Codec::kHEVC) ||
			   codec == static_cast<std::uint16_t>(Codec::kAV1);
	}

	inline bool IsKnownAudioCodec(std::uint16_t codec)
	{
		return codec == static_cast<std::uint16_t>(AudioCodec::kOpus);
	}

	inline bool ValidateExactMessageSize(const void *data, std::size_t bytes, MessageType expected, std::size_t expected_bytes)
	{
		return bytes == expected_bytes && ValidatePacketHeader(data, bytes, expected);
	}

	inline bool ValidateStreamConfigMessage(const StreamConfigMessage &msg)
	{
		return msg.header.magic == kMagic &&
			   msg.header.version == kProtocolVersion &&
			   msg.header.type == static_cast<std::uint8_t>(MessageType::kStreamConfig) &&
			   msg.width > 0 && msg.width <= 8192 &&
			   msg.height > 0 && msg.height <= 8192 &&
			   msg.fps > 0 && msg.fps <= 240 &&
			   IsKnownCodec(msg.codec) &&
			   msg.bitrate_kbps >= 100 && msg.bitrate_kbps <= 200000 &&
			   msg.chunk_payload_bytes >= 512 && msg.chunk_payload_bytes <= kMaxVideoChunkPayloadBytes &&
			   msg.parity_shards <= 1 &&
			   msg.stream_generation != 0;
	}

	inline bool ValidateAudioConfigMessage(const AudioConfigMessage &msg)
	{
		return msg.header.magic == kMagic &&
			   msg.header.version == kProtocolVersion &&
			   msg.header.type == static_cast<std::uint8_t>(MessageType::kAudioConfig) &&
			   msg.sample_rate >= 8000 && msg.sample_rate <= 192000 &&
			   msg.channels > 0 && msg.channels <= 8 &&
			   msg.samples_per_frame > 0 && msg.samples_per_frame <= kMaxAudioSampleFrames &&
			   IsKnownAudioCodec(msg.codec) &&
			   msg.bitrate_kbps > 0 && msg.bitrate_kbps <= 1000 &&
			   msg.stream_generation != 0;
	}

	inline bool ValidateVideoChunkHeader(const VideoChunkHeader &header, std::size_t payload_bytes)
	{
		if (header.header.magic != kMagic ||
			header.header.version != kProtocolVersion ||
			header.header.type != static_cast<std::uint8_t>(MessageType::kVideoChunk))
		{
			return false;
		}
		if (header.frame_size_bytes == 0 ||
			header.frame_size_bytes > kMaxEncodedVideoFrameBytes ||
			header.chunk_payload_bytes == 0 ||
			header.chunk_payload_bytes > kMaxVideoChunkPayloadBytes ||
			header.chunk_original_bytes == 0 ||
			header.chunk_original_bytes > header.chunk_payload_bytes ||
			payload_bytes != header.chunk_original_bytes ||
			header.stream_generation == 0)
		{
			return false;
		}
		const std::uint8_t known_flags = static_cast<std::uint8_t>(kChunkFlagKeyframe | kChunkFlagParity);
		if ((header.flags & ~known_flags) != 0)
		{
			return false;
		}

		const std::size_t chunk_payload = static_cast<std::size_t>(header.chunk_payload_bytes);
		const std::size_t expected_data_chunks =
			(static_cast<std::size_t>(header.frame_size_bytes) + chunk_payload - 1U) / chunk_payload;
		if (expected_data_chunks == 0 ||
			expected_data_chunks > kMaxVideoChunksPerFrame ||
			header.chunk_count < expected_data_chunks ||
			header.chunk_count > expected_data_chunks + 1U ||
			header.chunk_count > kMaxVideoChunksPerFrame)
		{
			return false;
		}

		const bool is_parity = (header.flags & kChunkFlagParity) != 0;
		if (is_parity)
		{
			return header.chunk_count == expected_data_chunks + 1U &&
				   header.chunk_id == expected_data_chunks &&
				   header.chunk_original_bytes == header.chunk_payload_bytes;
		}
		return header.chunk_id < expected_data_chunks;
	}

	inline bool ValidateAudioFrameHeader(const AudioFrameHeader &header, std::size_t payload_bytes)
	{
		return header.header.magic == kMagic &&
			   header.header.version == kProtocolVersion &&
			   header.header.type == static_cast<std::uint8_t>(MessageType::kAudioFrame) &&
			   header.payload_bytes > 0 &&
			   header.payload_bytes <= kMaxAudioFramePayloadBytes &&
			   payload_bytes == header.payload_bytes &&
			   header.sample_frames > 0 &&
			   header.sample_frames <= kMaxAudioSampleFrames &&
			   header.channels > 0 &&
			   header.channels <= 8 &&
			   header.stream_generation != 0;
	}

} // namespace streamproto::proto
