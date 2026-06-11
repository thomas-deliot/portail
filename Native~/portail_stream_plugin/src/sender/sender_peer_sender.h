#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include <steam/steam_api.h>

#include "common/lod_utils.h"
#include "common/protocol.h"
#include "common/protocol_validation.h"
#include "common/steam_sdr.h"

namespace streamproto::sender
{

	struct SenderSendEnqueueResult
	{
		bool queued = false;
		bool dropped_old = false;
		bool dropped_new = false;
	};

	class SenderPeerSender
	{
	public:
		SenderPeerSender(SteamSdr &sdr, std::uint64_t steam_id)
			: sdr_(sdr), steam_id_(steam_id) {}

		~SenderPeerSender()
		{
			Stop();
		}

		SenderPeerSender(const SenderPeerSender &) = delete;
		SenderPeerSender &operator=(const SenderPeerSender &) = delete;

		void Start()
		{
			std::lock_guard<std::mutex> lock(worker_mutex_);
			if (worker_.joinable())
			{
				return;
			}
			stop_.store(false, std::memory_order_release);
			worker_ = std::thread([this]()
								  { WorkerLoop(); });
		}

		void Stop()
		{
			{
				std::lock_guard<std::mutex> lock(worker_mutex_);
				stop_.store(true, std::memory_order_release);
			}
			cv_.notify_all();
			if (worker_.joinable())
			{
				worker_.join();
			}
			ClearQueues();
		}

		SenderSendEnqueueResult Enqueue(std::vector<std::uint8_t> bytes, bool reliable, SdrLane lane)
		{
			SenderSendEnqueueResult result{};
			if (bytes.empty())
			{
				return result;
			}

			{
				std::lock_guard<std::mutex> lock(queue_mutex_);
				QueueState &queue = QueueForLane(lane);
				const std::size_t max_bytes = MaxQueuedBytes(lane);
				while (max_bytes > 0 &&
					   queue.queued_bytes + bytes.size() > max_bytes &&
					   !queue.messages.empty())
				{
					bool dropped = false;
					if (lane == SdrLane::kVideo)
					{
						dropped = DropOldestVideoFrame(queue);
					}
					else
					{
						queue.queued_bytes -= queue.messages.front().bytes.size();
						queue.messages.pop_front();
						dropped = true;
					}
					if (!dropped)
					{
						break;
					}
					result.dropped_old = true;
					if (lane == SdrLane::kVideo)
					{
						video_queue_drops_.fetch_add(1, std::memory_order_relaxed);
					}
					else if (lane == SdrLane::kAudio)
					{
						audio_queue_drops_.fetch_add(1, std::memory_order_relaxed);
					}
				}

				if (max_bytes > 0 && bytes.size() > max_bytes)
				{
					result.dropped_new = true;
					return result;
				}

				QueuedMessage message{};
				message.bytes = std::move(bytes);
				message.reliable = reliable;
				message.lane = lane;
				AnnotateVideoMessage(message);
				queue.queued_bytes += message.bytes.size();
				queue.messages.push_back(std::move(message));
				result.queued = true;
			}
			cv_.notify_one();
			return result;
		}

	private:
		struct QueuedMessage
		{
			std::vector<std::uint8_t> bytes;
			bool reliable = false;
			SdrLane lane = SdrLane::kControl;
			bool has_video_frame = false;
			std::int32_t video_lod = lod::kStreamLodOff;
			std::uint32_t video_generation = 0;
			std::uint32_t video_frame_id = 0;
			bool video_keyframe = false;
		};

		struct QueueState
		{
			std::deque<QueuedMessage> messages;
			std::size_t queued_bytes = 0;
		};

		QueueState &QueueForLane(SdrLane lane)
		{
			if (lane == SdrLane::kVideo)
			{
				return video_;
			}
			if (lane == SdrLane::kAudio)
			{
				return audio_;
			}
			return control_;
		}

		static std::size_t MaxQueuedBytes(SdrLane lane)
		{
			if (lane == SdrLane::kVideo)
			{
				return 4U * 1024U * 1024U;
			}
			if (lane == SdrLane::kAudio)
			{
				return 256U * 1024U;
			}
			return 0;
		}

		static void AnnotateVideoMessage(QueuedMessage &message)
		{
			if (message.lane != SdrLane::kVideo ||
				message.bytes.size() < sizeof(proto::VideoChunkHeader))
			{
				return;
			}
			const auto *header = reinterpret_cast<const proto::VideoChunkHeader *>(message.bytes.data());
			const std::size_t payload_bytes = message.bytes.size() - sizeof(proto::VideoChunkHeader);
			if (!proto::ValidateVideoChunkHeader(*header, payload_bytes))
			{
				return;
			}
			message.has_video_frame = true;
			message.video_lod = header->lod_index;
			message.video_generation = header->stream_generation;
			message.video_frame_id = header->frame_id;
			message.video_keyframe = (header->flags & proto::kChunkFlagKeyframe) != 0;
		}

		static bool DropFrontVideoFrame(QueueState &queue, bool &dropped_keyframe)
		{
			dropped_keyframe = false;
			if (queue.messages.empty())
			{
				return false;
			}
			const QueuedMessage &first = queue.messages.front();
			if (!first.has_video_frame)
			{
				queue.queued_bytes -= queue.messages.front().bytes.size();
				queue.messages.pop_front();
				return true;
			}
			const std::int32_t video_lod = first.video_lod;
			const std::uint32_t video_generation = first.video_generation;
			const std::uint32_t video_frame_id = first.video_frame_id;
			dropped_keyframe = first.video_keyframe;
			do
			{
				queue.queued_bytes -= queue.messages.front().bytes.size();
				queue.messages.pop_front();
			} while (!queue.messages.empty() &&
					 queue.messages.front().has_video_frame &&
					 queue.messages.front().video_lod == video_lod &&
					 queue.messages.front().video_generation == video_generation &&
					 queue.messages.front().video_frame_id == video_frame_id);
			return true;
		}

		static bool DropOldestVideoFrame(QueueState &queue)
		{
			bool dropped_keyframe = false;
			if (!DropFrontVideoFrame(queue, dropped_keyframe))
			{
				return false;
			}
			while (!queue.messages.empty())
			{
				if (queue.messages.front().has_video_frame &&
					queue.messages.front().video_keyframe)
				{
					break;
				}
				bool ignored_keyframe = false;
				if (!DropFrontVideoFrame(queue, ignored_keyframe))
				{
					break;
				}
			}
			return true;
		}

		bool HasQueuedMessages() const
		{
			return !control_.messages.empty() ||
				   !audio_.messages.empty() ||
				   !video_.messages.empty();
		}

		bool PopNext(QueuedMessage &message)
		{
			auto pop_from = [&](QueueState &queue) -> bool
			{
				if (queue.messages.empty())
				{
					return false;
				}
				message = std::move(queue.messages.front());
				queue.queued_bytes -= message.bytes.size();
				queue.messages.pop_front();
				return true;
			};

			return pop_from(control_) || pop_from(audio_) || pop_from(video_);
		}

		void RequeueFront(QueuedMessage message)
		{
			QueueState &queue = QueueForLane(message.lane);
			queue.queued_bytes += message.bytes.size();
			queue.messages.push_front(std::move(message));
		}

		void WorkerLoop()
		{
			while (true)
			{
				QueuedMessage message{};
				{
					std::unique_lock<std::mutex> lock(queue_mutex_);
					cv_.wait(lock, [this]()
							 { return stop_.load(std::memory_order_acquire) || HasQueuedMessages(); });
					if (stop_.load(std::memory_order_acquire))
					{
						return;
					}
					if (!PopNext(message))
					{
						continue;
					}
				}

				EResult send_result = k_EResultOK;
				if (!sdr_.SendToSteamId(
						steam_id_,
						message.bytes.data(),
						message.bytes.size(),
						message.reliable,
						message.lane,
						&send_result))
				{
					if (message.reliable && message.lane == SdrLane::kControl)
					{
						{
							std::lock_guard<std::mutex> lock(queue_mutex_);
							if (!stop_.load(std::memory_order_acquire))
							{
								RequeueFront(std::move(message));
							}
						}
						std::this_thread::sleep_for(std::chrono::milliseconds(2));
						continue;
					}
					if (message.lane == SdrLane::kVideo &&
						(send_result == k_EResultIgnored || send_result == k_EResultLimitExceeded))
					{
						video_queue_drops_.fetch_add(1, std::memory_order_relaxed);
					}
				}
			}
		}

		void ClearQueues()
		{
			std::lock_guard<std::mutex> lock(queue_mutex_);
			control_ = {};
			audio_ = {};
			video_ = {};
		}

		SteamSdr &sdr_;
		std::uint64_t steam_id_ = 0;
		std::thread worker_;
		std::mutex worker_mutex_;
		std::atomic<bool> stop_{true};
		std::mutex queue_mutex_;
		std::condition_variable cv_;
		QueueState control_;
		QueueState audio_;
		QueueState video_;
		std::atomic<std::uint64_t> video_queue_drops_{0};
		std::atomic<std::uint64_t> audio_queue_drops_{0};
	};

	using SenderPeerSenderMap = std::unordered_map<std::uint64_t, std::shared_ptr<SenderPeerSender>>;

} // namespace streamproto::sender
