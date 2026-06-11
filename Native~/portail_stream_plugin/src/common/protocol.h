#pragma once

#include <cstddef>
#include <cstdint>

namespace streamproto::proto
{

	constexpr std::uint32_t kMagic = 0x4D525453U; // "STRM" in little-endian memory.
	constexpr std::uint8_t kProtocolVersion = 3;

	enum class MessageType : std::uint8_t
	{
		kHello = 1,
		kStreamConfig = 2,
		kVideoChunk = 3,
		kKeyframeRequest = 4,
		kPing = 5,
		kPong = 6,
		kStreamControl = 7,
		kAudioConfig = 8,
		kAudioFrame = 9,
		kStreamQuality = 10,
	};

	enum class StreamControlCommand : std::uint8_t
	{
		kSenderPause = 1,
		kSenderResume = 2,
		kReceiverPause = 3,
		kReceiverResume = 4,
	};

	enum class StreamQualityCommand : std::uint8_t
	{
		kReceiverMaxAccepted = 1,
		kSenderAssigned = 2,
		kReceiverApplied = 3,
	};

	enum class Codec : std::uint16_t
	{
		kH264 = 1,
		kHEVC = 2,
		kAV1 = 3,
	};

	enum class AudioCodec : std::uint16_t
	{
		kOpus = 1,
	};

	enum ChunkFlags : std::uint8_t
	{
		kChunkFlagKeyframe = 1 << 0,
		kChunkFlagParity = 1 << 1,
	};

#pragma pack(push, 1)

	struct PacketHeader
	{
		std::uint32_t magic;
		std::uint8_t version;
		std::uint8_t type;
		std::uint16_t reserved;
	};

	struct HelloMessage
	{
		PacketHeader header;
		std::uint32_t app_id;
		std::uint64_t sender_steam_id;
	};

	struct StreamConfigMessage
	{
		PacketHeader header;
		std::uint16_t width;
		std::uint16_t height;
		std::uint16_t fps;
		std::uint16_t codec;
		std::uint32_t bitrate_kbps;
		std::uint16_t chunk_payload_bytes;
		std::uint8_t parity_shards;
		std::uint8_t reserved;
		std::int32_t lod_index;
		std::uint32_t stream_generation;
	};

	struct AudioConfigMessage
	{
		PacketHeader header;
		std::uint16_t sample_rate;
		std::uint16_t channels;
		std::uint16_t samples_per_frame;
		std::uint16_t codec;
		std::uint32_t bitrate_kbps;
		std::uint32_t reserved;
		std::int32_t lod_index;
		std::uint32_t stream_generation;
	};

	struct KeyframeRequestMessage
	{
		PacketHeader header;
		std::uint32_t first_missing_frame;
		std::uint32_t last_completed_frame;
		std::int32_t lod_index;
		std::uint32_t stream_generation;
	};

	struct PingMessage
	{
		PacketHeader header;
		std::uint32_t sequence;
		std::uint32_t reserved;
		std::uint64_t receiver_send_timestamp_us;
	};

	struct PongMessage
	{
		PacketHeader header;
		std::uint32_t sequence;
		std::uint32_t reserved;
		std::uint64_t receiver_send_timestamp_us;
		std::uint64_t sender_recv_timestamp_us;
		std::uint64_t sender_send_timestamp_us;
	};

	struct StreamControlMessage
	{
		PacketHeader header;
		std::uint8_t command;
		std::uint8_t reserved[3];
		std::uint64_t sender_steam_id;
	};

	struct StreamQualityMessage
	{
		PacketHeader header;
		std::uint8_t command;
		std::uint8_t reserved[3];
		std::int32_t max_video_lod;
		std::int32_t max_audio_lod;
		std::int32_t assigned_video_lod;
		std::int32_t assigned_audio_lod;
		std::int32_t effective_video_lod;
		std::int32_t effective_audio_lod;
		std::uint64_t available_video_lod_mask;
		std::uint64_t available_audio_lod_mask;
		std::uint64_t sender_steam_id;
		// For sender assignment and receiver ACKs, sequence is the video generation
		// and profile_id is the audio generation.
		std::uint32_t sequence;
		std::uint32_t profile_id;
	};

	struct VideoChunkHeader
	{
		PacketHeader header;
		std::uint32_t frame_id;
		std::uint32_t frame_size_bytes;
		std::uint16_t chunk_id;
		std::uint16_t chunk_count;
		std::uint16_t chunk_payload_bytes;
		std::uint16_t chunk_original_bytes;
		std::uint8_t flags;
		std::uint8_t reserved;
		std::int32_t lod_index;
		std::uint32_t stream_generation;
		std::uint64_t capture_timestamp_us;
	};

	struct AudioFrameHeader
	{
		PacketHeader header;
		std::uint32_t sequence;
		std::uint32_t payload_bytes;
		std::uint16_t sample_frames;
		std::uint16_t channels;
		std::int32_t lod_index;
		std::uint32_t stream_generation;
		std::uint64_t capture_timestamp_us;
	};

#pragma pack(pop)

	template <typename T>
	inline void InitMessageHeader(T &msg, MessageType type)
	{
		msg.header.magic = kMagic;
		msg.header.version = kProtocolVersion;
		msg.header.type = static_cast<std::uint8_t>(type);
		msg.header.reserved = 0;
	}

	inline bool ValidatePacketHeader(const void *data, std::size_t bytes, MessageType expected)
	{
		if (data == nullptr || bytes < sizeof(PacketHeader))
		{
			return false;
		}

		auto *header = static_cast<const PacketHeader *>(data);
		return header->magic == kMagic &&
			   header->version == kProtocolVersion &&
			   header->type == static_cast<std::uint8_t>(expected);
	}

} // namespace streamproto::proto
