#pragma once

#include <cstddef>
#include <cstdint>

extern "C"
{
#include <libavcodec/avcodec.h>
}

#include "common/codec_utils.h"

namespace streamproto::receiver
{

	struct RecoveryUnitInfo
	{
		bool keyframe = false;
		bool headers = false;
		bool valid = false;
		const char *reason = "not-keyframe";
	};

	inline RecoveryUnitInfo InspectH264AnnexB(const std::uint8_t *data, std::size_t size)
	{
		bool saw_sps = false;
		bool saw_pps = false;
		bool saw_idr = false;
		std::size_t cursor = 0;
		while (true)
		{
			const std::size_t sc = codec::FindAnnexBStartCode(data, size, cursor);
			if (sc == size)
			{
				break;
			}
			std::size_t nal_start = sc + 3;
			if (sc + 3 < size && data[sc + 2] == 0 && data[sc + 3] == 1)
			{
				nal_start = sc + 4;
			}
			if (nal_start >= size)
			{
				break;
			}

			const std::uint8_t nal_type = static_cast<std::uint8_t>(data[nal_start] & 0x1F);
			if (nal_type == 7)
			{
				saw_sps = true;
			}
			else if (nal_type == 8)
			{
				saw_pps = true;
			}
			else if (nal_type == 5)
			{
				saw_idr = true;
				if (saw_sps && saw_pps)
				{
					return {true, true, true, "h264-idr"};
				}
			}
			cursor = nal_start + 1;
		}

		return {saw_idr, saw_sps && saw_pps, false, saw_idr ? "h264-idr-without-sps-pps" : "not-keyframe"};
	}

	inline RecoveryUnitInfo InspectH264LengthPrefixed(const std::uint8_t *data, std::size_t size)
	{
		bool saw_sps = false;
		bool saw_pps = false;
		bool saw_idr = false;
		std::size_t offset = 0;
		while (offset + 4 <= size)
		{
			const std::uint32_t nal_size = codec::ReadBe32(data + offset);
			offset += 4;
			if (nal_size == 0 || offset + nal_size > size)
			{
				break;
			}
			const std::uint8_t nal_type = static_cast<std::uint8_t>(data[offset] & 0x1F);
			if (nal_type == 7)
			{
				saw_sps = true;
			}
			else if (nal_type == 8)
			{
				saw_pps = true;
			}
			else if (nal_type == 5)
			{
				saw_idr = true;
				if (saw_sps && saw_pps)
				{
					return {true, true, true, "h264-idr"};
				}
			}
			offset += nal_size;
		}

		return {saw_idr, saw_sps && saw_pps, false, saw_idr ? "h264-idr-without-sps-pps" : "not-keyframe"};
	}

	inline RecoveryUnitInfo InspectHevcAnnexB(const std::uint8_t *data, std::size_t size)
	{
		bool saw_vps = false;
		bool saw_sps = false;
		bool saw_pps = false;
		bool saw_irap = false;
		std::size_t cursor = 0;
		while (true)
		{
			const std::size_t sc = codec::FindAnnexBStartCode(data, size, cursor);
			if (sc == size)
			{
				break;
			}
			std::size_t nal_start = sc + 3;
			if (sc + 3 < size && data[sc + 2] == 0 && data[sc + 3] == 1)
			{
				nal_start = sc + 4;
			}
			if (nal_start >= size)
			{
				break;
			}

			const std::uint8_t nal_type = static_cast<std::uint8_t>((data[nal_start] >> 1) & 0x3F);
			if (nal_type == 32)
			{
				saw_vps = true;
			}
			else if (nal_type == 33)
			{
				saw_sps = true;
			}
			else if (nal_type == 34)
			{
				saw_pps = true;
			}
			else if (codec::IsHevcIrapNalType(nal_type))
			{
				saw_irap = true;
				if (saw_vps && saw_sps && saw_pps)
				{
					return {true, true, true, "hevc-irap"};
				}
			}
			cursor = nal_start + 1;
		}

		return {saw_irap, saw_vps && saw_sps && saw_pps, false, saw_irap ? "hevc-irap-without-vps-sps-pps" : "not-keyframe"};
	}

	inline RecoveryUnitInfo InspectHevcLengthPrefixed(const std::uint8_t *data, std::size_t size)
	{
		bool saw_vps = false;
		bool saw_sps = false;
		bool saw_pps = false;
		bool saw_irap = false;
		std::size_t offset = 0;
		while (offset + 4 <= size)
		{
			const std::uint32_t nal_size = codec::ReadBe32(data + offset);
			offset += 4;
			if (nal_size == 0 || offset + nal_size > size)
			{
				break;
			}
			const std::uint8_t nal_type = static_cast<std::uint8_t>((data[offset] >> 1) & 0x3F);
			if (nal_type == 32)
			{
				saw_vps = true;
			}
			else if (nal_type == 33)
			{
				saw_sps = true;
			}
			else if (nal_type == 34)
			{
				saw_pps = true;
			}
			else if (codec::IsHevcIrapNalType(nal_type))
			{
				saw_irap = true;
				if (saw_vps && saw_sps && saw_pps)
				{
					return {true, true, true, "hevc-irap"};
				}
			}
			offset += nal_size;
		}

		return {saw_irap, saw_vps && saw_sps && saw_pps, false, saw_irap ? "hevc-irap-without-vps-sps-pps" : "not-keyframe"};
	}

	inline RecoveryUnitInfo InspectRecoveryUnit(AVCodecID codec_id, const std::uint8_t *data, std::size_t size, bool transport_keyframe)
	{
		if (data == nullptr || size < 5)
		{
			return {};
		}
		if (codec_id == AV_CODEC_ID_H264)
		{
			return codec::LooksAnnexB(data, size) ? InspectH264AnnexB(data, size) : InspectH264LengthPrefixed(data, size);
		}
		if (codec_id == AV_CODEC_ID_HEVC)
		{
			return codec::LooksAnnexB(data, size) ? InspectHevcAnnexB(data, size) : InspectHevcLengthPrefixed(data, size);
		}
		if (codec_id == AV_CODEC_ID_AV1 && transport_keyframe)
		{
			return {true, true, true, "av1-keyframe"};
		}
		return {};
	}

} // namespace streamproto::receiver
