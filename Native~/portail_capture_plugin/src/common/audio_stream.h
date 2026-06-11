#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "common/protocol.h"

namespace streamproto::audio
{

	constexpr int kSampleRate = 48000;
	constexpr int kChannels = 2;
	constexpr int kFrameMs = 5;
	constexpr int kFrameSamples = (kSampleRate * kFrameMs) / 1000;
	constexpr int kDefaultBitrateKbps = 96;

	int ClampAudioBitrateKbps(int bitrate_kbps);

	class PcmRingBuffer
	{
	public:
		explicit PcmRingBuffer(std::size_t capacity_samples = static_cast<std::size_t>(kSampleRate * kChannels * 2));

		void Push(const float *samples, std::size_t sample_count);
		std::size_t Read(float *out_samples, std::size_t max_samples);
		std::size_t ReadLatest(float *out_samples, std::size_t max_samples, std::size_t max_buffered_samples);
		void Clear();
		[[nodiscard]] std::size_t Available() const;

	private:
		mutable std::mutex mutex_;
		std::vector<float> data_;
		std::size_t read_pos_ = 0;
		std::size_t write_pos_ = 0;
		std::size_t size_ = 0;
	};

	struct AudioFrame
	{
		std::vector<float> samples;
		std::uint32_t sample_frames = 0;
		std::uint32_t sequence = 0;
		std::uint64_t capture_timestamp_us = 0;
	};

	class AudioFrameQueue
	{
	public:
		explicit AudioFrameQueue(std::size_t max_frames = 256);

		void Push(AudioFrame frame);
		bool Pop(AudioFrame &out_frame);
		void Clear();

	private:
		std::mutex mutex_;
		std::vector<AudioFrame> frames_;
		std::size_t max_frames_ = 256;
	};

	class FfmpegOpusEncoder
	{
	public:
		FfmpegOpusEncoder();
		~FfmpegOpusEncoder();

		bool Init(int bitrate_kbps, int frame_samples, std::string &error);
		bool Encode(const float *interleaved_samples, int sample_frames, std::vector<std::uint8_t> &out_packet);
		void Shutdown();

		[[nodiscard]] bool Ready() const;
		[[nodiscard]] int BitrateKbps() const;
		[[nodiscard]] int FrameSamples() const;
		[[nodiscard]] const std::string &ActiveEncoder() const;

	private:
		struct Impl;
		std::unique_ptr<Impl> impl_;
	};

	class FfmpegOpusDecoder
	{
	public:
		FfmpegOpusDecoder();
		~FfmpegOpusDecoder();

		bool Init(const proto::AudioConfigMessage &config, std::string &error);
		bool Decode(const std::uint8_t *data, std::size_t bytes, std::vector<float> &out_interleaved);
		void Shutdown();
		[[nodiscard]] bool Ready() const;

	private:
		struct Impl;
		std::unique_ptr<Impl> impl_;
	};

	class ProcessLoopbackCapture
	{
	public:
		ProcessLoopbackCapture();
		~ProcessLoopbackCapture();

		bool Start(
			std::uint32_t target_pid,
			int frame_samples,
			std::shared_ptr<PcmRingBuffer> local_ring,
			AudioFrameQueue *network_queue,
			std::string &error);
		bool StartSteamStreamingSpeakers(
			int frame_samples,
			std::shared_ptr<PcmRingBuffer> local_ring,
			AudioFrameQueue *network_queue,
			std::string &error);
		void Stop();
		[[nodiscard]] bool Running() const;

	private:
		struct Impl;
		std::unique_ptr<Impl> impl_;
	};

	std::uint32_t GetWindowProcessIdFromHandle(std::uint64_t window_handle);

} // namespace streamproto::audio
