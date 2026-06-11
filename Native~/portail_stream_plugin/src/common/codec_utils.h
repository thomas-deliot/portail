#pragma once

#include <cstddef>
#include <cstdint>

namespace streamproto::codec
{

	inline bool IsHevcIrapNalType(std::uint8_t nal_type)
	{
		return nal_type >= 16 && nal_type <= 21;
	}

	inline std::uint32_t ReadBe32(const std::uint8_t *p)
	{
		return (static_cast<std::uint32_t>(p[0]) << 24U) |
			   (static_cast<std::uint32_t>(p[1]) << 16U) |
			   (static_cast<std::uint32_t>(p[2]) << 8U) |
			   static_cast<std::uint32_t>(p[3]);
	}

	inline std::size_t FindAnnexBStartCode(const std::uint8_t *data, std::size_t size, std::size_t from)
	{
		if (data == nullptr || size < 3 || from >= size)
		{
			return size;
		}
		for (std::size_t i = from; i + 3 <= size; ++i)
		{
			if (data[i] != 0 || data[i + 1] != 0)
			{
				continue;
			}
			if (data[i + 2] == 1)
			{
				return i;
			}
			if (i + 3 < size && data[i + 2] == 0 && data[i + 3] == 1)
			{
				return i;
			}
		}
		return size;
	}

	inline bool LooksAnnexB(const std::uint8_t *data, std::size_t size)
	{
		return data != nullptr && size >= 4 &&
			   data[0] == 0 && data[1] == 0 &&
			   (data[2] == 1 || (data[2] == 0 && data[3] == 1));
	}

} // namespace streamproto::codec
