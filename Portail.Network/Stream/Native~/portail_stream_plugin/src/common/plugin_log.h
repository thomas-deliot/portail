#pragma once

#include <ios>
#include <iosfwd>
#include <ostream>
#include <sstream>
#include <string>
#include <utility>

namespace streamproto::log
{

	enum class Level : int
	{
		kInfo = 0,
		kWarning = 1,
		kError = 2,
	};

	using Callback = void(__cdecl *)(int level, const char *message);

	inline void Write(std::ostream &fallback, Callback callback, bool enabled, Level level, std::string message)
	{
		if (!enabled)
		{
			return;
		}

		const bool transient_update = !message.empty() && message.front() == '\r';
		while (!message.empty() && (message.front() == '\n' || message.front() == '\r'))
		{
			message.erase(message.begin());
		}
		while (!message.empty() && (message.back() == '\n' || message.back() == '\r'))
		{
			message.pop_back();
		}
		if (transient_update)
		{
			while (!message.empty() && (message.back() == ' ' || message.back() == '\t'))
			{
				message.pop_back();
			}
		}
		if (message.empty())
		{
			return;
		}

		if (callback != nullptr)
		{
			callback(static_cast<int>(level), message.c_str());
			return;
		}

		if (transient_update)
		{
			fallback << '\r' << message << "   " << std::flush;
			return;
		}

		fallback << message << "\n"
				 << std::flush;
	}

	class Stream
	{
	public:
		Stream(std::ostream &fallback, Level level, Callback callback, bool enabled)
			: fallback_(fallback), level_(level), callback_(callback), enabled_(enabled) {}

		Stream(const Stream &) = delete;
		Stream &operator=(const Stream &) = delete;

		Stream(Stream &&other) noexcept
			: fallback_(other.fallback_),
			  level_(other.level_),
			  callback_(other.callback_),
			  enabled_(other.enabled_),
			  buffer_(std::move(other.buffer_)),
			  active_(other.active_)
		{
			other.active_ = false;
		}

		~Stream()
		{
			if (active_)
			{
				Write(fallback_, callback_, enabled_, level_, buffer_.str());
			}
		}

		template <typename T>
		Stream &operator<<(const T &value)
		{
			buffer_ << value;
			return *this;
		}

		Stream &operator<<(std::ostream &(*manip)(std::ostream &))
		{
			manip(buffer_);
			return *this;
		}

		Stream &operator<<(std::ios &(*manip)(std::ios &))
		{
			manip(buffer_);
			return *this;
		}

		Stream &operator<<(std::ios_base &(*manip)(std::ios_base &))
		{
			manip(buffer_);
			return *this;
		}

	private:
		std::ostream &fallback_;
		Level level_;
		Callback callback_ = nullptr;
		bool enabled_ = true;
		std::ostringstream buffer_;
		bool active_ = true;
	};

} // namespace streamproto::log
