#pragma once

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include <windows.h>

namespace streamproto
{

	inline std::uint64_t NowSteadyMicros()
	{
		using namespace std::chrono;
		return duration_cast<microseconds>(steady_clock::now().time_since_epoch()).count();
	}

	inline std::uint64_t NowSteadyMillis()
	{
		using namespace std::chrono;
		return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
	}

	inline std::uint64_t NowUnixMicros()
	{
		using namespace std::chrono;
		return duration_cast<microseconds>(system_clock::now().time_since_epoch()).count();
	}

	inline std::string ToLowerAscii(std::string value)
	{
		std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c)
					   {
    if (c >= 'A' && c <= 'Z') {
      return static_cast<char>(c + ('a' - 'A'));
    }
    return static_cast<char>(c); });
		return value;
	}

	inline std::wstring Utf8ToWide(const std::string &value)
	{
		if (value.empty())
		{
			return {};
		}
		int len = MultiByteToWideChar(CP_UTF8, 0, value.c_str(), static_cast<int>(value.size()), nullptr, 0);
		if (len <= 0)
		{
			return {};
		}
		std::wstring out(static_cast<size_t>(len), L'\0');
		MultiByteToWideChar(CP_UTF8, 0, value.c_str(), static_cast<int>(value.size()), out.data(), len);
		return out;
	}

	inline std::string WideToUtf8(const std::wstring &value)
	{
		if (value.empty())
		{
			return {};
		}
		int len = WideCharToMultiByte(CP_UTF8, 0, value.c_str(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
		if (len <= 0)
		{
			return {};
		}
		std::string out(static_cast<size_t>(len), '\0');
		WideCharToMultiByte(CP_UTF8, 0, value.c_str(), static_cast<int>(value.size()), out.data(), len, nullptr, nullptr);
		return out;
	}

	inline std::string Trim(std::string value)
	{
		auto not_space = [](unsigned char c)
		{
			return c != ' ' && c != '\n' && c != '\r' && c != '\t';
		};
		auto begin = std::find_if(value.begin(), value.end(), not_space);
		auto end = std::find_if(value.rbegin(), value.rend(), not_space).base();
		if (begin >= end)
		{
			return {};
		}
		return std::string(begin, end);
	}

	inline bool ContainsCaseInsensitiveAscii(std::string_view haystack, std::string_view needle)
	{
		if (needle.empty())
		{
			return true;
		}
		std::string lhs = ToLowerAscii(std::string(haystack));
		std::string rhs = ToLowerAscii(std::string(needle));
		return lhs.find(rhs) != std::string::npos;
	}

} // namespace streamproto
