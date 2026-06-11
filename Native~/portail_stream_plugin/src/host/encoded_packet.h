#pragma once

#include <algorithm>
#include <cstdint>
#include <vector>

namespace streamproto::host
{

	struct EncodedPacket
	{
		std::vector<std::uint8_t> bytes;
		bool keyframe = false;
		std::uint64_t capture_ts_us = 0;
	};

	inline bool HasKeyframePacket(const std::vector<EncodedPacket> &packets)
	{
		return std::any_of(packets.begin(), packets.end(), [](const EncodedPacket &packet)
						   { return packet.keyframe; });
	}

} // namespace streamproto::host
