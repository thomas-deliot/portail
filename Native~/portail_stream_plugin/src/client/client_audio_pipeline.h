#pragma once

#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "common/audio_stream.h"
#include "common/lod_utils.h"
#include "common/protocol.h"
#include "common/protocol_validation.h"
#include "common/time_utils.h"
#include "client/client_stats.h"

namespace streamproto::client
{

	struct ClientAudioEvent
	{
		std::vector<std::uint8_t> bytes;
	};

	class ClientAudioPipeline
	{
	public:
		ClientAudioPipeline() = default;
		~ClientAudioPipeline() { Shutdown(); }

		ClientAudioPipeline(const ClientAudioPipeline &) = delete;
		ClientAudioPipeline &operator=(const ClientAudioPipeline &) = delete;

		std::shared_ptr<streamproto::audio::PcmRingBuffer> EnsureRing()
		{
			if (audio_ring_ == nullptr)
			{
				audio_ring_ = std::make_shared<streamproto::audio::PcmRingBuffer>();
			}
			return audio_ring_;
		}

		std::shared_ptr<streamproto::audio::PcmRingBuffer> Ring() const
		{
			return audio_ring_;
		}

		void SetClockSync(bool valid, double offset_us)
		{
			std::lock_guard<std::mutex> lock(audio_clock_mutex_);
			clock_synced_ = valid;
			clock_offset_us_ = valid ? offset_us : 0.0;
		}

		void ResetStats()
		{
			audio_packets_.store(0, std::memory_order_relaxed);
			audio_bytes_.store(0, std::memory_order_relaxed);
			audio_frames_.store(0, std::memory_order_relaxed);
			audio_decode_fail_.store(0, std::memory_order_relaxed);
			audio_recv_bytes_.store(0, std::memory_order_relaxed);
			last_audio_decode_ms_.store(0.0, std::memory_order_relaxed);
			last_audio_capture_to_push_ms_.store(0.0, std::memory_order_relaxed);
			last_audio_capture_ts_us_.store(0, std::memory_order_relaxed);
		}

		void Start()
		{
			{
				std::lock_guard<std::mutex> lock(audio_events_mutex_);
				audio_worker_stop_ = false;
				audio_events_.clear();
			}
			if (!audio_worker_.joinable())
			{
				audio_worker_ = std::thread([this]()
											{ AudioWorkerLoop(); });
			}
		}

		void Stop()
		{
			{
				std::lock_guard<std::mutex> lock(audio_events_mutex_);
				audio_worker_stop_ = true;
				audio_events_.clear();
			}
			audio_events_cv_.notify_all();
			if (audio_worker_.joinable())
			{
				audio_worker_.join();
			}
		}

		void Shutdown()
		{
			Stop();
			Deactivate();
			{
				std::lock_guard<std::mutex> decode_lock(audio_decode_mutex_);
				audio_decoder_.Shutdown();
				has_audio_config_ = false;
				current_audio_cfg_ = {};
			}
			if (audio_ring_ != nullptr)
			{
				audio_ring_->Clear();
				audio_ring_.reset();
			}
		}

		void EnqueueFrame(const std::uint8_t *data, std::size_t bytes)
		{
			if (data == nullptr || bytes == 0)
			{
				return;
			}
			ClientAudioEvent event{};
			event.bytes.assign(data, data + bytes);
			{
				std::lock_guard<std::mutex> lock(audio_events_mutex_);
				if (audio_events_.size() >= 256)
				{
					audio_events_.pop_front();
					audio_decode_fail_.fetch_add(1, std::memory_order_relaxed);
				}
				audio_events_.push_back(std::move(event));
			}
			audio_events_cv_.notify_one();
		}

		void ClearPending()
		{
			std::lock_guard<std::mutex> lock(audio_events_mutex_);
			audio_events_.clear();
		}

		bool Activate(
			std::int32_t lod,
			std::uint32_t generation,
			const streamproto::proto::AudioConfigMessage &cfg,
			std::string &error)
		{
			lod = streamproto::lod::Normalize(lod);
			if (!streamproto::lod::Enabled(lod) || generation == 0)
			{
				error = "invalid-audio-generation";
				return false;
			}

			const bool audio_lod_changed = active_audio_lod_ != lod;
			{
				std::lock_guard<std::mutex> decode_lock(audio_decode_mutex_);
				const bool config_changed = !has_audio_config_ || !SameAudioConfig(current_audio_cfg_, cfg);
				if (config_changed || !audio_decoder_.Ready())
				{
					if (!audio_decoder_.Init(cfg, error))
					{
						has_audio_config_ = false;
						audio_decode_fail_.fetch_add(1, std::memory_order_relaxed);
						return false;
					}
					current_audio_cfg_ = cfg;
					has_audio_config_ = true;
				}
				active_audio_lod_ = lod;
				active_audio_generation_ = generation;
				audio_decode_enabled_.store(true, std::memory_order_relaxed);
			}
			if (audio_lod_changed && audio_ring_ != nullptr)
			{
				audio_ring_->Clear();
			}
			return true;
		}

		void Deactivate()
		{
			ClearPending();
			audio_decode_enabled_.store(false, std::memory_order_relaxed);
			{
				std::lock_guard<std::mutex> decode_lock(audio_decode_mutex_);
				active_audio_lod_ = streamproto::lod::kStreamLodOff;
				active_audio_generation_ = 0;
			}
			if (audio_ring_ != nullptr)
			{
				audio_ring_->Clear();
			}
		}

		void AppendStats(ClientStats &stats) const
		{
			stats.audio_packets = audio_packets_.load(std::memory_order_relaxed);
			stats.audio_bytes = audio_bytes_.load(std::memory_order_relaxed);
			stats.audio_frames = audio_frames_.load(std::memory_order_relaxed);
			stats.audio_decode_fail = audio_decode_fail_.load(std::memory_order_relaxed);
			stats.recv_bytes += audio_recv_bytes_.load(std::memory_order_relaxed);
			stats.last_audio_decode_ms = last_audio_decode_ms_.load(std::memory_order_relaxed);
			stats.last_audio_capture_to_push_ms = last_audio_capture_to_push_ms_.load(std::memory_order_relaxed);
			stats.last_audio_capture_ts_us = last_audio_capture_ts_us_.load(std::memory_order_relaxed);
		}

	private:
		static bool SameAudioConfig(
			const streamproto::proto::AudioConfigMessage &a,
			const streamproto::proto::AudioConfigMessage &b)
		{
			return a.sample_rate == b.sample_rate &&
				   a.channels == b.channels &&
				   a.samples_per_frame == b.samples_per_frame &&
				   a.codec == b.codec &&
				   a.bitrate_kbps == b.bitrate_kbps;
		}

		void AudioWorkerLoop()
		{
			while (true)
			{
				ClientAudioEvent event{};
				{
					std::unique_lock<std::mutex> lock(audio_events_mutex_);
					audio_events_cv_.wait(lock, [this]()
										  { return audio_worker_stop_ || !audio_events_.empty(); });
					if (audio_worker_stop_ && audio_events_.empty())
					{
						break;
					}
					event = std::move(audio_events_.front());
					audio_events_.pop_front();
				}
				ProcessAudioFrameEvent(event.bytes.data(), event.bytes.size());
			}
		}

		void ProcessAudioFrameEvent(const std::uint8_t *data, std::size_t bytes)
		{
			namespace proto = streamproto::proto;

			if (data == nullptr || bytes < sizeof(proto::AudioFrameHeader))
			{
				return;
			}
			const auto *frame_header = reinterpret_cast<const proto::AudioFrameHeader *>(data);
			const std::size_t payload_offset = sizeof(proto::AudioFrameHeader);
			const std::size_t payload_bytes = bytes - payload_offset;
			if (!proto::ValidateAudioFrameHeader(*frame_header, payload_bytes))
			{
				return;
			}
			if (frame_header->channels != streamproto::audio::kChannels ||
				payload_bytes == 0)
			{
				return;
			}

			std::vector<float> decoded_audio;
			std::shared_ptr<streamproto::audio::PcmRingBuffer> ring;
			double decode_ms = 0.0;
			{
				std::lock_guard<std::mutex> decode_lock(audio_decode_mutex_);
				if (!has_audio_config_ ||
					!audio_decode_enabled_.load(std::memory_order_relaxed) ||
					streamproto::lod::Normalize(frame_header->lod_index) != active_audio_lod_ ||
					frame_header->stream_generation != active_audio_generation_)
				{
					return;
				}
				ring = audio_ring_;
				const auto decode_start = std::chrono::steady_clock::now();
				if (!audio_decoder_.Decode(data + payload_offset, payload_bytes, decoded_audio))
				{
					audio_decode_fail_.fetch_add(1, std::memory_order_relaxed);
					last_audio_decode_ms_.store(0.0, std::memory_order_relaxed);
					return;
				}
				const auto decode_end = std::chrono::steady_clock::now();
				decode_ms = std::chrono::duration<double, std::milli>(decode_end - decode_start).count();
			}

			if (!decoded_audio.empty() && ring != nullptr)
			{
				ring->Push(decoded_audio.data(), decoded_audio.size());
				last_audio_decode_ms_.store(decode_ms, std::memory_order_relaxed);
				last_audio_capture_ts_us_.store(frame_header->capture_timestamp_us, std::memory_order_relaxed);
				if (frame_header->capture_timestamp_us != 0)
				{
					double offset_us = 0.0;
					bool synced = false;
					{
						std::lock_guard<std::mutex> clock_lock(audio_clock_mutex_);
						synced = clock_synced_;
						offset_us = clock_offset_us_;
					}
					if (synced)
					{
						std::int64_t host_now_us = static_cast<std::int64_t>(streamproto::NowUnixMicros());
						host_now_us += static_cast<std::int64_t>(std::llround(offset_us));
						if (host_now_us >= 0 && static_cast<std::uint64_t>(host_now_us) >= frame_header->capture_timestamp_us)
						{
							last_audio_capture_to_push_ms_.store(
								static_cast<double>(static_cast<std::uint64_t>(host_now_us) - frame_header->capture_timestamp_us) / 1000.0,
								std::memory_order_relaxed);
						}
					}
				}
			}
			audio_packets_.fetch_add(1, std::memory_order_relaxed);
			audio_bytes_.fetch_add(bytes, std::memory_order_relaxed);
			audio_recv_bytes_.fetch_add(bytes, std::memory_order_relaxed);
			audio_frames_.fetch_add(decoded_audio.size() / streamproto::audio::kChannels, std::memory_order_relaxed);
		}

		streamproto::audio::FfmpegOpusDecoder audio_decoder_{};
		mutable std::mutex audio_decode_mutex_;
		std::shared_ptr<streamproto::audio::PcmRingBuffer> audio_ring_;
		std::thread audio_worker_;
		mutable std::mutex audio_events_mutex_;
		std::condition_variable audio_events_cv_;
		std::deque<ClientAudioEvent> audio_events_;
		bool audio_worker_stop_ = true;
		std::atomic<bool> audio_decode_enabled_{false};
		std::atomic<std::uint64_t> audio_packets_{0};
		std::atomic<std::uint64_t> audio_bytes_{0};
		std::atomic<std::uint64_t> audio_frames_{0};
		std::atomic<std::uint64_t> audio_decode_fail_{0};
		std::atomic<std::uint64_t> audio_recv_bytes_{0};
		std::atomic<double> last_audio_decode_ms_{0.0};
		std::atomic<double> last_audio_capture_to_push_ms_{0.0};
		std::atomic<std::uint64_t> last_audio_capture_ts_us_{0};
		mutable std::mutex audio_clock_mutex_;
		bool clock_synced_ = false;
		double clock_offset_us_ = 0.0;
		bool has_audio_config_ = false;
		streamproto::proto::AudioConfigMessage current_audio_cfg_{};
		std::int32_t active_audio_lod_ = streamproto::lod::kStreamLodOff;
		std::uint32_t active_audio_generation_ = 0;
	};

} // namespace streamproto::client
