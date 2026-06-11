#pragma once

#include <algorithm>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

#include "receiver/recovery_unit.h"

namespace streamproto::receiver
{

	struct ReceiverVideoDecodeEvent
	{
		std::uint32_t frame_id = 0;
		std::int32_t lod_index = -1;
		std::uint32_t stream_generation = 0;
		std::vector<std::uint8_t> bytes;
		bool keyframe = false;
		std::uint64_t capture_ts_us = 0;
		RecoveryUnitInfo recovery{};
		double reassembly_ms = 0.0;
		double reassembly_cpu_ms = 0.0;
	};

	struct ReceiverVideoDecodeEnqueueResult
	{
		bool queued = false;
		bool dropped_completed_frames = false;
		std::uint64_t dropped_frame_count = 0;
		bool request_keyframe = false;
		std::uint32_t request_frame_id = 1;
	};

	class ReceiverVideoDecodeQueue
	{
	public:
		using Processor = std::function<void(ReceiverVideoDecodeEvent)>;

		ReceiverVideoDecodeQueue() = default;
		~ReceiverVideoDecodeQueue() { Stop(); }

		ReceiverVideoDecodeQueue(const ReceiverVideoDecodeQueue &) = delete;
		ReceiverVideoDecodeQueue &operator=(const ReceiverVideoDecodeQueue &) = delete;

		void Start(Processor processor)
		{
			Stop();
			{
				std::lock_guard<std::mutex> lock(events_mutex_);
				processor_ = std::move(processor);
				worker_stop_ = false;
				events_.clear();
			}
			worker_ = std::thread([this]()
								  { WorkerLoop(); });
		}

		void Stop()
		{
			{
				std::lock_guard<std::mutex> lock(events_mutex_);
				worker_stop_ = true;
				events_.clear();
			}
			events_cv_.notify_all();
			if (worker_.joinable())
			{
				worker_.join();
			}
			std::lock_guard<std::mutex> lock(events_mutex_);
			processor_ = {};
		}

		void Clear()
		{
			std::lock_guard<std::mutex> lock(events_mutex_);
			events_.clear();
		}

		ReceiverVideoDecodeEnqueueResult Enqueue(ReceiverVideoDecodeEvent event)
		{
			ReceiverVideoDecodeEnqueueResult result{};
			if (event.bytes.empty())
			{
				return result;
			}

			bool enqueue_event = true;
			{
				std::lock_guard<std::mutex> lock(events_mutex_);
				if (events_.size() >= 8)
				{
					result.dropped_completed_frames = true;
					result.dropped_frame_count = static_cast<std::uint64_t>(events_.size());
					events_.clear();
					enqueue_event = event.recovery.valid;
				}
				if (enqueue_event)
				{
					events_.push_back(std::move(event));
					result.queued = true;
				}
				else
				{
					result.request_keyframe = true;
					result.request_frame_id = std::max<std::uint32_t>(event.frame_id, 1);
				}
			}

			if (result.queued)
			{
				events_cv_.notify_one();
			}
			return result;
		}

	private:
		void WorkerLoop()
		{
			while (true)
			{
				ReceiverVideoDecodeEvent event{};
				Processor processor;
				{
					std::unique_lock<std::mutex> lock(events_mutex_);
					events_cv_.wait(lock, [this]()
									 { return worker_stop_ || !events_.empty(); });
					if (worker_stop_ && events_.empty())
					{
						break;
					}
					event = std::move(events_.front());
					events_.pop_front();
					processor = processor_;
				}
				if (processor)
				{
					processor(std::move(event));
				}
			}
		}

		std::thread worker_;
		mutable std::mutex events_mutex_;
		std::condition_variable events_cv_;
		std::deque<ReceiverVideoDecodeEvent> events_;
		bool worker_stop_ = true;
		Processor processor_;
	};

} // namespace streamproto::receiver
