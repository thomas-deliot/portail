#pragma once

#include <cstdint>
#include <limits>

namespace streamproto::lod
{

	constexpr std::int32_t kStreamLodOff = -1;
	constexpr std::int32_t kInvalidSentLod = std::numeric_limits<std::int32_t>::min();

	inline std::int32_t Normalize(std::int32_t lod)
	{
		return lod < 0 ? kStreamLodOff : lod;
	}

	inline bool Enabled(std::int32_t lod)
	{
		return lod >= 0;
	}

	inline std::uint64_t Bit(std::int32_t lod)
	{
		return lod >= 0 && lod < 63 ? (1ULL << static_cast<unsigned>(lod)) : 0;
	}

	inline std::int32_t BestAvailable(std::uint64_t mask)
	{
		for (std::int32_t i = 0; i < 63; ++i)
		{
			if ((mask & (1ULL << static_cast<unsigned>(i))) != 0)
			{
				return i;
			}
		}
		return kStreamLodOff;
	}

} // namespace streamproto::lod
