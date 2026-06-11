#include "common/audio_stream.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <cwchar>
#include <limits>
#include <mutex>
#include <optional>
#include <sstream>
#include <thread>
#include <utility>

#include <windows.h>
#include <audioclient.h>
#include <audioclientactivationparams.h>
#include <avrt.h>
#include <ksmedia.h>
#include <mmdeviceapi.h>
#include <propidl.h>
#include <propsys.h>
#include <wrl/client.h>

#include "common/time_utils.h"

#ifndef VIRTUAL_AUDIO_DEVICE_PROCESS_LOOPBACK
#define VIRTUAL_AUDIO_DEVICE_PROCESS_LOOPBACK L"VAD\\Process_Loopback"
#endif

namespace streamproto::audio
{
	namespace
	{

		using Microsoft::WRL::ComPtr;

		constexpr wchar_t kSteamStreamingSpeakersName[] = L"Steam Streaming Speakers";
		const PROPERTYKEY kPKeyDeviceFriendlyName = {
			{0xA45C254E, 0xDF1C, 0x4EFD, {0x80, 0x20, 0x67, 0xD1, 0x46, 0xA8, 0x50, 0xE0}},
			14};
		const PROPERTYKEY kPKeyDeviceInterfaceFriendlyName = {
			{0x026E516E, 0xB814, 0x414B, {0x83, 0xCD, 0x85, 0x6D, 0x6F, 0xEF, 0x48, 0x22}},
			2};
		const PROPERTYKEY kPKeyDeviceDeviceDesc = {
			{0xA45C254E, 0xDF1C, 0x4EFD, {0x80, 0x20, 0x67, 0xD1, 0x46, 0xA8, 0x50, 0xE0}},
			2};

		std::string HResultString(HRESULT hr)
		{
			std::ostringstream ss;
			ss << "0x" << std::hex << static_cast<unsigned long>(hr);
			return ss.str();
		}

		class AudioActivationHandler final : public IActivateAudioInterfaceCompletionHandler, public IAgileObject
		{
		public:
			AudioActivationHandler() : event_(CreateEventW(nullptr, TRUE, FALSE, nullptr))
			{
				IUnknown *outer = static_cast<IUnknown *>(static_cast<IActivateAudioInterfaceCompletionHandler *>(this));
				CoCreateFreeThreadedMarshaler(outer, free_threaded_marshaler_.GetAddressOf());
			}

			~AudioActivationHandler()
			{
				if (event_ != nullptr)
				{
					CloseHandle(event_);
					event_ = nullptr;
				}
			}

			HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **object) override
			{
				if (object == nullptr)
				{
					return E_POINTER;
				}
				*object = nullptr;
				if (riid == __uuidof(IUnknown) || riid == __uuidof(IActivateAudioInterfaceCompletionHandler))
				{
					*object = static_cast<IActivateAudioInterfaceCompletionHandler *>(this);
					AddRef();
					return S_OK;
				}
				if (riid == __uuidof(IAgileObject))
				{
					*object = static_cast<IAgileObject *>(this);
					AddRef();
					return S_OK;
				}
				if (riid == __uuidof(IMarshal) && free_threaded_marshaler_ != nullptr)
				{
					return free_threaded_marshaler_->QueryInterface(riid, object);
				}
				return E_NOINTERFACE;
			}

			ULONG STDMETHODCALLTYPE AddRef() override
			{
				return ref_count_.fetch_add(1, std::memory_order_relaxed) + 1;
			}

			ULONG STDMETHODCALLTYPE Release() override
			{
				ULONG value = ref_count_.fetch_sub(1, std::memory_order_acq_rel) - 1;
				if (value == 0)
				{
					delete this;
				}
				return value;
			}

			HRESULT STDMETHODCALLTYPE ActivateCompleted(IActivateAudioInterfaceAsyncOperation *operation) override
			{
				HRESULT activate_result = E_FAIL;
				ComPtr<IUnknown> activated;
				if (operation != nullptr)
				{
					operation->GetActivateResult(&activate_result, activated.GetAddressOf());
				}

				{
					std::lock_guard<std::mutex> lock(mutex_);
					result_ = activate_result;
					if (SUCCEEDED(activate_result) && activated != nullptr)
					{
						activate_result = activated.As(&client_);
						result_ = activate_result;
					}
				}

				if (event_ != nullptr)
				{
					SetEvent(event_);
				}
				return S_OK;
			}

			HANDLE event() const { return event_; }

			HRESULT GetResult(ComPtr<IAudioClient> &out_client)
			{
				std::lock_guard<std::mutex> lock(mutex_);
				out_client = client_;
				return result_;
			}

		private:
			std::atomic<ULONG> ref_count_{1};
			HANDLE event_ = nullptr;
			std::mutex mutex_;
			HRESULT result_ = E_PENDING;
			ComPtr<IAudioClient> client_;
			ComPtr<IUnknown> free_threaded_marshaler_;
		};

		bool ActivateProcessLoopbackClient(DWORD pid, ComPtr<IAudioClient> &out_client, std::string &error)
		{
			out_client.Reset();
			if (pid == 0)
			{
				error = "process-loopback target pid is zero";
				return false;
			}

			AUDIOCLIENT_ACTIVATION_PARAMS params{};
			params.ActivationType = AUDIOCLIENT_ACTIVATION_TYPE_PROCESS_LOOPBACK;
			params.ProcessLoopbackParams.TargetProcessId = pid;
			params.ProcessLoopbackParams.ProcessLoopbackMode = PROCESS_LOOPBACK_MODE_INCLUDE_TARGET_PROCESS_TREE;

			PROPVARIANT activation_params{};
			PropVariantInit(&activation_params);
			activation_params.vt = VT_BLOB;
			activation_params.blob.cbSize = sizeof(params);
			activation_params.blob.pBlobData = reinterpret_cast<BYTE *>(&params);

			AudioActivationHandler *handler = new AudioActivationHandler();
			if (handler->event() == nullptr)
			{
				handler->Release();
				error = "CreateEvent failed for process-loopback activation";
				return false;
			}

			ComPtr<IActivateAudioInterfaceAsyncOperation> operation;
			HRESULT hr = ActivateAudioInterfaceAsync(
				VIRTUAL_AUDIO_DEVICE_PROCESS_LOOPBACK,
				__uuidof(IAudioClient),
				&activation_params,
				handler,
				operation.GetAddressOf());
			if (FAILED(hr))
			{
				handler->Release();
				error = "ActivateAudioInterfaceAsync(process-loopback) failed: " + HResultString(hr);
				return false;
			}

			DWORD wait = WaitForSingleObject(handler->event(), 10000);
			if (wait != WAIT_OBJECT_0)
			{
				handler->Release();
				error = "ActivateAudioInterfaceAsync(process-loopback) timed out";
				return false;
			}

			hr = handler->GetResult(out_client);
			handler->Release();
			if (FAILED(hr) || out_client == nullptr)
			{
				error = "process-loopback activation failed: " + HResultString(hr);
				return false;
			}
			return true;
		}

		bool DevicePropertyEquals(IMMDevice *device, REFPROPERTYKEY key, const wchar_t *expected)
		{
			if (device == nullptr || expected == nullptr)
			{
				return false;
			}

			ComPtr<IPropertyStore> store;
			HRESULT hr = device->OpenPropertyStore(STGM_READ, store.GetAddressOf());
			if (FAILED(hr) || store == nullptr)
			{
				return false;
			}

			PROPVARIANT value{};
			PropVariantInit(&value);
			hr = store->GetValue(key, &value);
			if (FAILED(hr))
			{
				PropVariantClear(&value);
				return false;
			}

			const bool matches =
				value.vt == VT_LPWSTR &&
				value.pwszVal != nullptr &&
				_wcsicmp(value.pwszVal, expected) == 0;
			PropVariantClear(&value);
			return matches;
		}

		bool FindSteamStreamingSpeakersDevice(IMMDeviceEnumerator *enumerator, ComPtr<IMMDevice> &out_device, std::string &error)
		{
			out_device.Reset();
			if (enumerator == nullptr)
			{
				error = "MMDeviceEnumerator is unavailable.";
				return false;
			}

			ComPtr<IMMDeviceCollection> devices;
			HRESULT hr = enumerator->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, devices.GetAddressOf());
			if (FAILED(hr) || devices == nullptr)
			{
				error = "EnumAudioEndpoints(eRender, active) failed: " + HResultString(hr);
				return false;
			}

			UINT count = 0;
			hr = devices->GetCount(&count);
			if (FAILED(hr))
			{
				error = "IMMDeviceCollection::GetCount failed: " + HResultString(hr);
				return false;
			}

			for (UINT i = 0; i < count; ++i)
			{
				ComPtr<IMMDevice> device;
				hr = devices->Item(i, device.GetAddressOf());
				if (FAILED(hr) || device == nullptr)
				{
					continue;
				}

				if (DevicePropertyEquals(device.Get(), kPKeyDeviceInterfaceFriendlyName, kSteamStreamingSpeakersName) ||
					DevicePropertyEquals(device.Get(), kPKeyDeviceFriendlyName, kSteamStreamingSpeakersName) ||
					DevicePropertyEquals(device.Get(), kPKeyDeviceDeviceDesc, kSteamStreamingSpeakersName))
				{
					out_device = device;
					return true;
				}
			}

			error = "Steam Streaming Speakers render endpoint was not found.";
			return false;
		}

		bool ActivateSteamStreamingSpeakersLoopbackClient(ComPtr<IAudioClient> &out_client, std::string &error)
		{
			out_client.Reset();

			ComPtr<IMMDeviceEnumerator> enumerator;
			HRESULT hr = CoCreateInstance(
				__uuidof(MMDeviceEnumerator),
				nullptr,
				CLSCTX_ALL,
				IID_PPV_ARGS(enumerator.GetAddressOf()));
			if (FAILED(hr) || enumerator == nullptr)
			{
				error = "CoCreateInstance(MMDeviceEnumerator) failed: " + HResultString(hr);
				return false;
			}

			ComPtr<IMMDevice> device;
			if (!FindSteamStreamingSpeakersDevice(enumerator.Get(), device, error))
			{
				return false;
			}

			hr = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, reinterpret_cast<void **>(out_client.GetAddressOf()));
			if (FAILED(hr) || out_client == nullptr)
			{
				error = "IMMDevice::Activate(IAudioClient) failed: " + HResultString(hr);
				return false;
			}
			return true;
		}

		WAVEFORMATEXTENSIBLE BuildCaptureFormat()
		{
			WAVEFORMATEXTENSIBLE format{};
			format.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
			format.Format.nChannels = kChannels;
			format.Format.nSamplesPerSec = kSampleRate;
			format.Format.wBitsPerSample = 32;
			format.Format.nBlockAlign = static_cast<WORD>(format.Format.nChannels * sizeof(float));
			format.Format.nAvgBytesPerSec = format.Format.nSamplesPerSec * format.Format.nBlockAlign;
			format.Format.cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
			format.Samples.wValidBitsPerSample = 32;
			format.dwChannelMask = SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT;
			format.SubFormat = KSDATAFORMAT_SUBTYPE_IEEE_FLOAT;
			return format;
		}

		void ApplyLowLatencyAudioClientProperties(const ComPtr<IAudioClient> &audio_client)
		{
			ComPtr<IAudioClient2> audio_client2;
			if (audio_client == nullptr || FAILED(audio_client.As(&audio_client2)) || audio_client2 == nullptr)
			{
				return;
			}

			AudioClientProperties properties{};
			properties.cbSize = sizeof(properties);
			properties.bIsOffload = FALSE;
			properties.eCategory = AudioCategory_GameMedia;
			properties.Options = AUDCLNT_STREAMOPTIONS_RAW;
			audio_client2->SetClientProperties(&properties);
		}

		HRESULT InitializeProcessLoopbackAudioClient(
			const ComPtr<IAudioClient> &audio_client,
			DWORD flags,
			const WAVEFORMATEX *capture_format)
		{
			ComPtr<IAudioClient3> audio_client3;
			if (audio_client != nullptr && SUCCEEDED(audio_client.As(&audio_client3)) && audio_client3 != nullptr)
			{
				UINT32 default_period_frames = 0;
				UINT32 fundamental_period_frames = 0;
				UINT32 min_period_frames = 0;
				UINT32 max_period_frames = 0;
				HRESULT hr = audio_client3->GetSharedModeEnginePeriod(
					capture_format,
					&default_period_frames,
					&fundamental_period_frames,
					&min_period_frames,
					&max_period_frames);
				if (SUCCEEDED(hr) && min_period_frames > 0)
				{
					hr = audio_client3->InitializeSharedAudioStream(flags, min_period_frames, capture_format, nullptr);
					if (SUCCEEDED(hr))
					{
						return hr;
					}
				}
			}

			return audio_client->Initialize(
				AUDCLNT_SHAREMODE_SHARED,
				flags,
				0,
				0,
				capture_format,
				nullptr);
		}

		HRESULT InitializeEndpointLoopbackAudioClient(
			const ComPtr<IAudioClient> &audio_client,
			DWORD flags,
			const WAVEFORMATEX *capture_format)
		{
			constexpr REFERENCE_TIME kRequestedDuration = 100000; // 10 ms
			return audio_client->Initialize(
				AUDCLNT_SHAREMODE_SHARED,
				flags,
				kRequestedDuration,
				0,
				capture_format,
				nullptr);
		}

		bool IsFloatFormat(const WAVEFORMATEX *format)
		{
			if (format == nullptr)
			{
				return false;
			}
			if (format->wFormatTag == WAVE_FORMAT_IEEE_FLOAT)
			{
				return true;
			}
			if (format->wFormatTag != WAVE_FORMAT_EXTENSIBLE || format->cbSize < sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX))
			{
				return false;
			}
			const auto *extensible = reinterpret_cast<const WAVEFORMATEXTENSIBLE *>(format);
			return extensible->SubFormat == KSDATAFORMAT_SUBTYPE_IEEE_FLOAT;
		}

		bool IsPcmFormat(const WAVEFORMATEX *format)
		{
			if (format == nullptr)
			{
				return false;
			}
			if (format->wFormatTag == WAVE_FORMAT_PCM)
			{
				return true;
			}
			if (format->wFormatTag != WAVE_FORMAT_EXTENSIBLE || format->cbSize < sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX))
			{
				return false;
			}
			const auto *extensible = reinterpret_cast<const WAVEFORMATEXTENSIBLE *>(format);
			return extensible->SubFormat == KSDATAFORMAT_SUBTYPE_PCM;
		}

	} // namespace

	int ClampAudioBitrateKbps(int bitrate_kbps)
	{
		return std::clamp(bitrate_kbps <= 0 ? kDefaultBitrateKbps : bitrate_kbps, 32, 512);
	}

	PcmRingBuffer::PcmRingBuffer(std::size_t capacity_samples) : data_(std::max<std::size_t>(capacity_samples, 1)) {}

	void PcmRingBuffer::Push(const float *samples, std::size_t sample_count)
	{
		if (samples == nullptr || sample_count == 0 || data_.empty())
		{
			return;
		}

		std::lock_guard<std::mutex> lock(mutex_);
		if (sample_count >= data_.size())
		{
			samples += sample_count - data_.size();
			sample_count = data_.size();
			read_pos_ = 0;
			write_pos_ = 0;
			size_ = 0;
		}

		const std::size_t free_samples = data_.size() - size_;
		if (sample_count > free_samples)
		{
			const std::size_t drop = sample_count - free_samples;
			read_pos_ = (read_pos_ + drop) % data_.size();
			size_ -= drop;
		}

		std::size_t remaining = sample_count;
		while (remaining > 0)
		{
			const std::size_t contiguous = std::min(remaining, data_.size() - write_pos_);
			std::memcpy(data_.data() + write_pos_, samples + (sample_count - remaining), contiguous * sizeof(float));
			write_pos_ = (write_pos_ + contiguous) % data_.size();
			size_ += contiguous;
			remaining -= contiguous;
		}
	}

	std::size_t PcmRingBuffer::Read(float *out_samples, std::size_t max_samples)
	{
		if (out_samples == nullptr || max_samples == 0 || data_.empty())
		{
			return 0;
		}

		std::lock_guard<std::mutex> lock(mutex_);
		const std::size_t to_read = std::min(max_samples, size_);
		std::size_t remaining = to_read;
		while (remaining > 0)
		{
			const std::size_t contiguous = std::min(remaining, data_.size() - read_pos_);
			std::memcpy(out_samples + (to_read - remaining), data_.data() + read_pos_, contiguous * sizeof(float));
			read_pos_ = (read_pos_ + contiguous) % data_.size();
			size_ -= contiguous;
			remaining -= contiguous;
		}
		return to_read;
	}

	std::size_t PcmRingBuffer::ReadLatest(float *out_samples, std::size_t max_samples, std::size_t max_buffered_samples)
	{
		if (out_samples == nullptr || max_samples == 0 || data_.empty())
		{
			return 0;
		}

		std::lock_guard<std::mutex> lock(mutex_);
		max_buffered_samples = std::max(max_buffered_samples, max_samples);
		if (size_ > max_buffered_samples)
		{
			const std::size_t drop = size_ - max_buffered_samples;
			read_pos_ = (read_pos_ + drop) % data_.size();
			size_ -= drop;
		}

		const std::size_t to_read = std::min(max_samples, size_);
		std::size_t remaining = to_read;
		while (remaining > 0)
		{
			const std::size_t contiguous = std::min(remaining, data_.size() - read_pos_);
			std::memcpy(out_samples + (to_read - remaining), data_.data() + read_pos_, contiguous * sizeof(float));
			read_pos_ = (read_pos_ + contiguous) % data_.size();
			size_ -= contiguous;
			remaining -= contiguous;
		}
		return to_read;
	}

	void PcmRingBuffer::Clear()
	{
		std::lock_guard<std::mutex> lock(mutex_);
		read_pos_ = 0;
		write_pos_ = 0;
		size_ = 0;
	}

	std::size_t PcmRingBuffer::Available() const
	{
		std::lock_guard<std::mutex> lock(mutex_);
		return size_;
	}

	AudioFrameQueue::AudioFrameQueue(std::size_t max_frames) : max_frames_(std::max<std::size_t>(max_frames, 1)) {}

	void AudioFrameQueue::Push(AudioFrame frame)
	{
		std::lock_guard<std::mutex> lock(mutex_);
		if (frames_.size() >= max_frames_)
		{
			frames_.erase(frames_.begin());
		}
		frames_.emplace_back(std::move(frame));
	}

	bool AudioFrameQueue::Pop(AudioFrame &out_frame)
	{
		std::lock_guard<std::mutex> lock(mutex_);
		if (frames_.empty())
		{
			return false;
		}
		out_frame = std::move(frames_.front());
		frames_.erase(frames_.begin());
		return true;
	}

	void AudioFrameQueue::Clear()
	{
		std::lock_guard<std::mutex> lock(mutex_);
		frames_.clear();
	}

	struct FfmpegOpusEncoder::Impl {};

	FfmpegOpusEncoder::FfmpegOpusEncoder() = default;
	FfmpegOpusEncoder::~FfmpegOpusEncoder() = default;

	bool FfmpegOpusEncoder::Init(int, int, std::string &error)
	{
		error = "Opus encoding is not available in virtualdesktop_plugin.";
		return false;
	}

	bool FfmpegOpusEncoder::Encode(const float *, int, std::vector<std::uint8_t> &out_packet)
	{
		out_packet.clear();
		return false;
	}

	void FfmpegOpusEncoder::Shutdown()
	{
		impl_.reset();
	}

	bool FfmpegOpusEncoder::Ready() const
	{
		return false;
	}

	int FfmpegOpusEncoder::BitrateKbps() const
	{
		return 0;
	}

	int FfmpegOpusEncoder::FrameSamples() const
	{
		return kFrameSamples;
	}

	const std::string &FfmpegOpusEncoder::ActiveEncoder() const
	{
		static const std::string kNone = "none";
		return kNone;
	}

	struct FfmpegOpusDecoder::Impl {};

	FfmpegOpusDecoder::FfmpegOpusDecoder() = default;
	FfmpegOpusDecoder::~FfmpegOpusDecoder() = default;

	bool FfmpegOpusDecoder::Init(const proto::AudioConfigMessage &, std::string &error)
	{
		error = "Opus decoding is not available in virtualdesktop_plugin.";
		return false;
	}

	bool FfmpegOpusDecoder::Decode(const std::uint8_t *, std::size_t, std::vector<float> &out_interleaved)
	{
		out_interleaved.clear();
		return false;
	}

	void FfmpegOpusDecoder::Shutdown()
	{
		impl_.reset();
	}

	bool FfmpegOpusDecoder::Ready() const
	{
		return false;
	}
	struct ProcessLoopbackCapture::Impl
	{
		enum class CaptureSource
		{
			Process,
			SteamStreamingSpeakers,
		};

		std::atomic<bool> running{false};
		std::thread thread;
		std::mutex init_mutex;
		std::condition_variable init_cv;
		bool init_done = false;
		bool init_ok = false;
		std::string init_error;
		CaptureSource source = CaptureSource::Process;
		DWORD target_pid = 0;
		int frame_samples = kFrameSamples;
		std::shared_ptr<PcmRingBuffer> local_ring;
		AudioFrameQueue *network_queue = nullptr;
		UINT32 capture_channels = kChannels;
		UINT32 capture_sample_rate = kSampleRate;
		WORD capture_bits_per_sample = 32;
		bool capture_is_float = true;
		bool capture_is_pcm = false;
		double resample_position = 0.0;

		bool Start(DWORD pid, int samples_per_frame, std::shared_ptr<PcmRingBuffer> ring, AudioFrameQueue *queue, std::string &error)
		{
			if (running.load())
			{
				error = "process-loopback capture is already running";
				return false;
			}
			if (pid == 0 || ring == nullptr || queue == nullptr)
			{
				error = "invalid process-loopback capture arguments";
				return false;
			}

			target_pid = pid;
			source = CaptureSource::Process;
			return StartInternal(samples_per_frame, std::move(ring), queue, error);
		}

		bool StartSteamStreamingSpeakers(int samples_per_frame, std::shared_ptr<PcmRingBuffer> ring, AudioFrameQueue *queue, std::string &error)
		{
			target_pid = 0;
			source = CaptureSource::SteamStreamingSpeakers;
			return StartInternal(samples_per_frame, std::move(ring), queue, error);
		}

		bool StartInternal(int samples_per_frame, std::shared_ptr<PcmRingBuffer> ring, AudioFrameQueue *queue, std::string &error)
		{
			if (running.load())
			{
				error = "audio capture is already running";
				return false;
			}
			if (ring == nullptr || queue == nullptr)
			{
				error = "invalid audio capture arguments";
				return false;
			}

			frame_samples = std::clamp(samples_per_frame <= 0 ? kFrameSamples : samples_per_frame, 120, 2880);
			local_ring = std::move(ring);
			network_queue = queue;
			init_done = false;
			init_ok = false;
			init_error.clear();
			running.store(true);
			thread = std::thread([this]()
								 { CaptureThread(); });

			std::unique_lock<std::mutex> lock(init_mutex);
			const bool signaled = init_cv.wait_for(lock, std::chrono::seconds(10), [&]()
												   { return init_done; });
			if (!signaled || !init_ok)
			{
				error = signaled ? init_error : "process-loopback capture initialization timed out";
				running.store(false);
				lock.unlock();
				if (thread.joinable())
				{
					thread.join();
				}
				return false;
			}
			return true;
		}

		void Stop()
		{
			running.store(false);
			if (thread.joinable())
			{
				thread.join();
			}
			if (local_ring != nullptr)
			{
				local_ring->Clear();
			}
			if (network_queue != nullptr)
			{
				network_queue->Clear();
			}
		}

		void PublishInit(bool ok, std::string error)
		{
			{
				std::lock_guard<std::mutex> lock(init_mutex);
				init_ok = ok;
				init_error = std::move(error);
				init_done = true;
			}
			init_cv.notify_all();
		}

		void PushSamples(std::vector<float> &accumulator, const float *samples, std::size_t sample_count, std::uint32_t &sequence)
		{
			if (samples == nullptr || sample_count == 0)
			{
				return;
			}

			if (local_ring != nullptr)
			{
				local_ring->Push(samples, sample_count);
			}

			if (network_queue == nullptr)
			{
				return;
			}

			accumulator.insert(accumulator.end(), samples, samples + sample_count);
			const std::size_t frame_sample_count = static_cast<std::size_t>(frame_samples * kChannels);
			while (accumulator.size() >= frame_sample_count)
			{
				AudioFrame frame{};
				frame.samples.assign(accumulator.begin(), accumulator.begin() + static_cast<std::ptrdiff_t>(frame_sample_count));
				frame.sample_frames = static_cast<std::uint32_t>(frame_samples);
				frame.sequence = sequence++;
				frame.capture_timestamp_us = streamproto::NowUnixMicros();
				network_queue->Push(std::move(frame));
				accumulator.erase(accumulator.begin(), accumulator.begin() + static_cast<std::ptrdiff_t>(frame_sample_count));
			}
		}

		void ConfigureCapturedFormat(const WAVEFORMATEX *format)
		{
			capture_channels = format != nullptr && format->nChannels > 0 ? format->nChannels : kChannels;
			capture_sample_rate = format != nullptr && format->nSamplesPerSec > 0 ? format->nSamplesPerSec : kSampleRate;
			capture_bits_per_sample = format != nullptr && format->wBitsPerSample > 0 ? format->wBitsPerSample : 32;
			capture_is_float = IsFloatFormat(format);
			capture_is_pcm = IsPcmFormat(format);
			resample_position = 0.0;
		}

		float ReadEndpointSample(const BYTE *data, std::size_t sample_index) const
		{
			if (data == nullptr)
			{
				return 0.0f;
			}
			if (capture_is_float && capture_bits_per_sample == 32)
			{
				return reinterpret_cast<const float *>(data)[sample_index];
			}
			if (!capture_is_pcm)
			{
				return 0.0f;
			}
			if (capture_bits_per_sample == 16)
			{
				return static_cast<float>(reinterpret_cast<const std::int16_t *>(data)[sample_index]) / 32768.0f;
			}
			if (capture_bits_per_sample == 32)
			{
				return static_cast<float>(reinterpret_cast<const std::int32_t *>(data)[sample_index]) / 2147483648.0f;
			}
			if (capture_bits_per_sample == 24)
			{
				const BYTE *sample = data + (sample_index * 3);
				std::int32_t value = static_cast<std::int32_t>(sample[0] | (sample[1] << 8) | (sample[2] << 16));
				if ((value & 0x00800000) != 0)
				{
					value |= unchecked_sign_extension_mask();
				}
				return static_cast<float>(value) / 8388608.0f;
			}
			return 0.0f;
		}

		static std::int32_t unchecked_sign_extension_mask()
		{
			return static_cast<std::int32_t>(0xFF000000u);
		}

		void ConvertEndpointFramesToStereo(const BYTE *data, UINT32 frame_count, bool silent, std::vector<float> &out_samples)
		{
			out_samples.clear();
			if (frame_count == 0)
			{
				return;
			}

			std::vector<float> source_stereo;
			source_stereo.resize(static_cast<std::size_t>(frame_count) * kChannels);
			for (UINT32 frame = 0; frame < frame_count; ++frame)
			{
				float left = 0.0f;
				float right = 0.0f;
				if (!silent && data != nullptr)
				{
					const std::size_t base = static_cast<std::size_t>(frame) * capture_channels;
					left = ReadEndpointSample(data, base);
					right = capture_channels > 1 ? ReadEndpointSample(data, base + 1) : left;
				}
				const std::size_t dst = static_cast<std::size_t>(frame) * kChannels;
				source_stereo[dst] = left;
				source_stereo[dst + 1] = right;
			}

			if (capture_sample_rate == kSampleRate)
			{
				out_samples.swap(source_stereo);
				return;
			}

			const double step = static_cast<double>(capture_sample_rate) / static_cast<double>(kSampleRate);
			while (resample_position < static_cast<double>(frame_count))
			{
				UINT32 source_frame = static_cast<UINT32>(resample_position);
				const std::size_t src = static_cast<std::size_t>(std::min<UINT32>(source_frame, frame_count - 1)) * kChannels;
				out_samples.push_back(source_stereo[src]);
				out_samples.push_back(source_stereo[src + 1]);
				resample_position += step;
			}
			resample_position -= static_cast<double>(frame_count);
			if (resample_position < 0.0)
			{
				resample_position = 0.0;
			}
		}

		void CaptureThread()
		{
			HRESULT coinit = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
			const bool should_uninit = SUCCEEDED(coinit);
			if (FAILED(coinit) && coinit != RPC_E_CHANGED_MODE)
			{
				PublishInit(false, "CoInitializeEx(MTA) failed: " + HResultString(coinit));
				return;
			}

			DWORD avrt_task_index = 0;
			HANDLE avrt_handle = AvSetMmThreadCharacteristicsW(L"Pro Audio", &avrt_task_index);

			ComPtr<IAudioClient> audio_client;
			std::string error;
			bool activated = source == CaptureSource::SteamStreamingSpeakers
				? ActivateSteamStreamingSpeakersLoopbackClient(audio_client, error)
				: ActivateProcessLoopbackClient(target_pid, audio_client, error);
			if (!activated)
			{
				if (avrt_handle != nullptr)
				{
					AvRevertMmThreadCharacteristics(avrt_handle);
				}
				if (should_uninit)
				{
					CoUninitialize();
				}
				PublishInit(false, error);
				return;
			}
			if (source == CaptureSource::Process)
			{
				ApplyLowLatencyAudioClientProperties(audio_client);
			}

			const bool use_event_callback = source == CaptureSource::Process;
			DWORD flags = AUDCLNT_STREAMFLAGS_LOOPBACK;
			if (source == CaptureSource::Process)
			{
				flags |= AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY;
			}
			if (use_event_callback)
			{
				flags |= AUDCLNT_STREAMFLAGS_EVENTCALLBACK;
			}
			WAVEFORMATEXTENSIBLE float48_format = BuildCaptureFormat();
			WAVEFORMATEX *mix_format = nullptr;
			const WAVEFORMATEX *active_format = &float48_format.Format;
			if (source == CaptureSource::SteamStreamingSpeakers)
			{
				HRESULT mix_hr = audio_client->GetMixFormat(&mix_format);
				if (FAILED(mix_hr) || mix_format == nullptr)
				{
					if (avrt_handle != nullptr)
					{
						AvRevertMmThreadCharacteristics(avrt_handle);
					}
					if (should_uninit)
					{
						CoUninitialize();
					}
					PublishInit(false, "IAudioClient::GetMixFormat(Steam Streaming Speakers) failed: " + HResultString(mix_hr));
					return;
				}
				active_format = mix_format;
			}

			ConfigureCapturedFormat(active_format);
			HRESULT hr = source == CaptureSource::SteamStreamingSpeakers
				? InitializeEndpointLoopbackAudioClient(audio_client, flags, active_format)
				: InitializeProcessLoopbackAudioClient(audio_client, flags, active_format);
			if (mix_format != nullptr)
			{
				CoTaskMemFree(mix_format);
				mix_format = nullptr;
			}
			if (FAILED(hr))
			{
				if (avrt_handle != nullptr)
				{
					AvRevertMmThreadCharacteristics(avrt_handle);
				}
				if (should_uninit)
				{
					CoUninitialize();
				}
				std::string initialize_error = "IAudioClient::Initialize(loopback float 48 kHz stereo) failed: " + HResultString(hr);
				PublishInit(false, initialize_error);
				return;
			}

			REFERENCE_TIME default_period = 0;
			audio_client->GetDevicePeriod(&default_period, nullptr);
			DWORD wait_timeout_ms = static_cast<DWORD>(std::clamp<REFERENCE_TIME>(default_period / 10000, 5, 50));

			UINT32 buffer_frames = 0;
			hr = audio_client->GetBufferSize(&buffer_frames);
			if (FAILED(hr) || buffer_frames == 0)
			{
				if (avrt_handle != nullptr)
				{
					AvRevertMmThreadCharacteristics(avrt_handle);
				}
				if (should_uninit)
				{
					CoUninitialize();
				}
				PublishInit(false, "IAudioClient::GetBufferSize(loopback) failed: " + HResultString(hr));
				return;
			}

			HANDLE event = nullptr;
			if (use_event_callback)
			{
				event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
				if (event == nullptr)
				{
					if (avrt_handle != nullptr)
					{
						AvRevertMmThreadCharacteristics(avrt_handle);
					}
					if (should_uninit)
					{
						CoUninitialize();
					}
					PublishInit(false, "CreateEvent failed for process-loopback capture");
					return;
				}

				hr = audio_client->SetEventHandle(event);
				if (FAILED(hr))
				{
					CloseHandle(event);
					if (avrt_handle != nullptr)
					{
						AvRevertMmThreadCharacteristics(avrt_handle);
					}
					if (should_uninit)
					{
						CoUninitialize();
					}
					PublishInit(false, "IAudioClient::SetEventHandle failed: " + HResultString(hr));
					return;
				}
			}

			ComPtr<IAudioCaptureClient> capture_client;
			hr = audio_client->GetService(IID_PPV_ARGS(capture_client.GetAddressOf()));
			if (FAILED(hr) || capture_client == nullptr)
			{
				if (event != nullptr)
				{
					CloseHandle(event);
				}
				if (avrt_handle != nullptr)
				{
					AvRevertMmThreadCharacteristics(avrt_handle);
				}
				if (should_uninit)
				{
					CoUninitialize();
				}
				PublishInit(false, "IAudioClient::GetService(IAudioCaptureClient) failed: " + HResultString(hr));
				return;
			}

			hr = audio_client->Start();
			if (FAILED(hr))
			{
				if (event != nullptr)
				{
					CloseHandle(event);
				}
				if (avrt_handle != nullptr)
				{
					AvRevertMmThreadCharacteristics(avrt_handle);
				}
				if (should_uninit)
				{
					CoUninitialize();
				}
				PublishInit(false, "IAudioClient::Start(loopback) failed: " + HResultString(hr));
				return;
			}

			PublishInit(true, "");

			std::vector<float> accumulator;
			accumulator.reserve(static_cast<std::size_t>(frame_samples * kChannels * 4));
			std::vector<float> packet_buffer;
			packet_buffer.reserve(static_cast<std::size_t>(std::max<UINT32>(buffer_frames, static_cast<UINT32>(frame_samples)) * kChannels));
			std::uint32_t sequence = 1;

			while (running.load())
			{
				if (event != nullptr)
				{
					DWORD wait = WaitForSingleObjectEx(event, wait_timeout_ms, FALSE);
					if (wait != WAIT_OBJECT_0 && wait != WAIT_TIMEOUT)
					{
						break;
					}
				}
				else
				{
					std::this_thread::sleep_for(std::chrono::milliseconds(wait_timeout_ms));
				}

				while (running.load())
				{
					UINT32 next_packet_frames = 0;
					hr = capture_client->GetNextPacketSize(&next_packet_frames);
					if (FAILED(hr) || next_packet_frames == 0)
					{
						break;
					}

					BYTE *data = nullptr;
					UINT32 frame_count = 0;
					DWORD buffer_flags = 0;
					hr = capture_client->GetBuffer(&data, &frame_count, &buffer_flags, nullptr, nullptr);
					if (hr == AUDCLNT_E_DEVICE_INVALIDATED)
					{
						running.store(false);
						break;
					}
					if (FAILED(hr))
					{
						break;
					}

					const bool silent = (buffer_flags & AUDCLNT_BUFFERFLAGS_SILENT) != 0 || data == nullptr;
					if (source == CaptureSource::SteamStreamingSpeakers)
					{
						ConvertEndpointFramesToStereo(data, frame_count, silent, packet_buffer);
						PushSamples(accumulator, packet_buffer.data(), packet_buffer.size(), sequence);
					}
					else
					{
						const std::size_t sample_count = static_cast<std::size_t>(frame_count) * kChannels;
						if (silent)
						{
							packet_buffer.assign(sample_count, 0.0f);
							PushSamples(accumulator, packet_buffer.data(), packet_buffer.size(), sequence);
						}
						else
						{
							PushSamples(accumulator, reinterpret_cast<const float *>(data), sample_count, sequence);
						}
					}
					capture_client->ReleaseBuffer(frame_count);
				}
			}

			audio_client->Stop();
			if (event != nullptr)
			{
				CloseHandle(event);
			}
			if (avrt_handle != nullptr)
			{
				AvRevertMmThreadCharacteristics(avrt_handle);
			}
			if (should_uninit)
			{
				CoUninitialize();
			}
		}
	};

	ProcessLoopbackCapture::ProcessLoopbackCapture() = default;

	ProcessLoopbackCapture::~ProcessLoopbackCapture()
	{
		Stop();
	}

	bool ProcessLoopbackCapture::Start(
		std::uint32_t target_pid,
		int frame_samples,
		std::shared_ptr<PcmRingBuffer> local_ring,
		AudioFrameQueue *network_queue,
		std::string &error)
	{
		if (impl_ == nullptr)
		{
			impl_ = std::make_unique<Impl>();
		}
		return impl_->Start(static_cast<DWORD>(target_pid), frame_samples, std::move(local_ring), network_queue, error);
	}

	bool ProcessLoopbackCapture::StartSteamStreamingSpeakers(
		int frame_samples,
		std::shared_ptr<PcmRingBuffer> local_ring,
		AudioFrameQueue *network_queue,
		std::string &error)
	{
		if (impl_ == nullptr)
		{
			impl_ = std::make_unique<Impl>();
		}
		return impl_->StartSteamStreamingSpeakers(frame_samples, std::move(local_ring), network_queue, error);
	}

	void ProcessLoopbackCapture::Stop()
	{
		if (impl_ != nullptr)
		{
			impl_->Stop();
		}
	}

	bool ProcessLoopbackCapture::Running() const
	{
		return impl_ != nullptr && impl_->running.load();
	}

	std::uint32_t GetWindowProcessIdFromHandle(std::uint64_t window_handle)
	{
		HWND hwnd = reinterpret_cast<HWND>(static_cast<std::uintptr_t>(window_handle));
		if (hwnd == nullptr || !IsWindow(hwnd))
		{
			return 0;
		}

		DWORD pid = 0;
		GetWindowThreadProcessId(hwnd, &pid);
		return static_cast<std::uint32_t>(pid);
	}

} // namespace streamproto::audio
