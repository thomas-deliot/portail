#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <limits>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>
#include <cwctype>
#include <iterator>
#include <cstdio>
#include <memory>

#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

#include <Windows.Graphics.Capture.Interop.h>
#include <winrt/base.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Metadata.h>
#include <winrt/Windows.Graphics.Capture.h>
#include <winrt/Windows.Graphics.DirectX.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>

#include "common/audio_stream.h"
#include "common/time_utils.h"
#include "IUnityGraphics.h"
#include "IUnityGraphicsD3D11.h"
#include "IUnityInterface.h"

namespace
{

	using Microsoft::WRL::ComPtr;

	namespace winrt_capture
	{
		using namespace winrt::Windows::Foundation;
		using namespace winrt::Windows::Foundation::Metadata;
		using namespace winrt::Windows::Graphics::Capture;
		using namespace winrt::Windows::Graphics::DirectX;
		using namespace winrt::Windows::Graphics::DirectX::Direct3D11;

		struct __declspec(uuid("A9B3D012-3DF2-4EE3-B8D1-8695F457D3C1")) IDirect3DDxgiInterfaceAccess : ::IUnknown
		{
			virtual HRESULT __stdcall GetInterface(REFIID id, void **object) = 0;
		};

		extern "C" HRESULT __stdcall CreateDirect3D11DeviceFromDXGIDevice(::IDXGIDevice *dxgi_device, ::IInspectable **graphics_device);
	} // namespace winrt_capture

	constexpr std::uint64_t kNoPendingWindowHandle = std::numeric_limits<std::uint64_t>::max();
	constexpr int kCaptureRenderEventId = 0x1001;

	enum class CaptureMode : int
	{
		Window = 0,
		Display = 1,
	};

	enum class WgcTargetKind
	{
		Window,
		Monitor,
	};

	std::atomic<std::uint64_t> g_preview_shared_handle{0};
	std::atomic<int> g_preview_width{0};
	std::atomic<int> g_preview_height{0};
	std::mutex g_stats_mutex;
	std::uint64_t g_stats_capture_frames = 0;
	double g_stats_last_capture_ms = 0.0;
	std::atomic<int> g_stats_audio_capture_state{0};
	std::atomic<int> g_stats_audio_target_pid{0};
	std::atomic<std::uint64_t> g_stats_local_audio_samples_read{0};
	std::shared_ptr<streamproto::audio::PcmRingBuffer> g_host_local_audio_ring =
		std::make_shared<streamproto::audio::PcmRingBuffer>(
			static_cast<std::size_t>((streamproto::audio::kSampleRate * streamproto::audio::kChannels * 250) / 1000));
	std::mutex g_error_mutex;
	std::string g_last_error;
	std::mutex g_audio_error_mutex;
	std::string g_last_audio_error;
	using HostLogCallback = void(__cdecl *)(int level, const char *message);
	std::mutex g_log_callback_mutex;
	HostLogCallback g_log_callback = nullptr;
	std::atomic<bool> g_host_logging_enabled{true};

	IUnityInterfaces *g_unity_interfaces = nullptr;
	IUnityGraphics *g_unity_graphics = nullptr;
	ID3D11Device *g_unity_device = nullptr;
	ID3D11Texture2D *g_unity_target_texture = nullptr;
	HANDLE g_unity_opened_source_handle = nullptr;
	ComPtr<ID3D11Texture2D> g_unity_opened_source_texture;
	std::mutex g_unity_mutex;

	void SetLastErrorString(const std::string &error)
	{
		std::lock_guard<std::mutex> lock(g_error_mutex);
		g_last_error = error;
	}

	void SetLastAudioErrorString(const std::string &error)
	{
		std::lock_guard<std::mutex> lock(g_audio_error_mutex);
		g_last_audio_error = error;
	}

	enum class UnityLogLevel : int
	{
		Info = 0,
		Warning = 1,
		Error = 2,
	};

	void SetHostLogCallback(HostLogCallback callback)
	{
		std::lock_guard<std::mutex> lock(g_log_callback_mutex);
		g_log_callback = callback;
	}

	HostLogCallback GetHostLogCallback()
	{
		std::lock_guard<std::mutex> lock(g_log_callback_mutex);
		return g_log_callback;
	}

	void SetHostLoggingEnabled(bool enabled)
	{
		g_host_logging_enabled.store(enabled);
	}

	void WriteNativeLog(std::ostream &fallback, UnityLogLevel level, std::string message)
	{
		if (!g_host_logging_enabled.load())
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
		if (message.empty())
		{
			return;
		}

		if (HostLogCallback callback = GetHostLogCallback(); callback != nullptr)
		{
			callback(static_cast<int>(level), message.c_str());
			return;
		}

		if (transient_update)
		{
			fallback << '\r' << message << "   " << std::flush;
			return;
		}

		fallback << message << "\n" << std::flush;
	}

	class PluginLogStream
	{
	public:
		PluginLogStream(std::ostream &fallback, UnityLogLevel level)
			: fallback_(fallback), level_(level) {}

		PluginLogStream(const PluginLogStream &) = delete;
		PluginLogStream &operator=(const PluginLogStream &) = delete;

		PluginLogStream(PluginLogStream &&other) noexcept
			: fallback_(other.fallback_), level_(other.level_), buffer_(std::move(other.buffer_)), active_(other.active_)
		{
			other.active_ = false;
		}

		~PluginLogStream()
		{
			if (active_)
			{
				WriteNativeLog(fallback_, level_, buffer_.str());
			}
		}

		template <typename T>
		PluginLogStream &operator<<(const T &value)
		{
			buffer_ << value;
			return *this;
		}

		PluginLogStream &operator<<(std::ostream &(*manip)(std::ostream &))
		{
			buffer_ << manip;
			return *this;
		}

	private:
		std::ostream &fallback_;
		UnityLogLevel level_;
		std::ostringstream buffer_;
		bool active_ = true;
	};

	PluginLogStream LogInfoStream()
	{
		return PluginLogStream(std::cout, UnityLogLevel::Info);
	}

	PluginLogStream LogWarningStream()
	{
		return PluginLogStream(std::cerr, UnityLogLevel::Warning);
	}

	PluginLogStream LogErrorStream()
	{
		return PluginLogStream(std::cerr, UnityLogLevel::Error);
	}
	class WgcWindowCapture
	{
	public:
		explicit WgcWindowCapture(bool capture_cursor)
			: capture_cursor_(capture_cursor)
		{
			InitializeSRWLock(&frame_lock_);
			InitializeConditionVariable(&frame_cv_);
		}
		~WgcWindowCapture() { Shutdown(); }

		bool Init(HWND hwnd, std::string &error)
		{
			Shutdown();

			if (hwnd == nullptr)
			{
				error = "Target window handle is null.";
				return false;
			}

			hwnd_ = hwnd;
			monitor_ = nullptr;
			target_kind_ = WgcTargetKind::Window;

			if (!CreateDevice(error))
			{
				return false;
			}
			if (!CreateCaptureSession(error))
			{
				return false;
			}
			return true;
		}

		bool InitMonitor(HMONITOR monitor, std::string &error)
		{
			Shutdown();

			if (monitor == nullptr)
			{
				error = "Target monitor handle is null.";
				return false;
			}

			hwnd_ = nullptr;
			monitor_ = monitor;
			target_kind_ = WgcTargetKind::Monitor;

			if (!CreateDevice(error))
			{
				return false;
			}
			if (!CreateCaptureSession(error))
			{
				return false;
			}
			return true;
		}

		bool SwitchWindow(HWND hwnd, std::string &error)
		{
			if (hwnd == nullptr)
			{
				error = "Target window handle is null.";
				return false;
			}

			if (device_ == nullptr || context_ == nullptr)
			{
				return Init(hwnd, error);
			}

			AcquireSRWLockExclusive(&frame_lock_);
			if (pending_frame_ != nullptr)
			{
				pending_frame_.Close();
				pending_frame_ = nullptr;
			}
			ReleaseSRWLockExclusive(&frame_lock_);

			if (frame_pool_ != nullptr && frame_arrived_token_.value != 0)
			{
				frame_pool_.FrameArrived(frame_arrived_token_);
				frame_arrived_token_.value = 0;
			}

			if (capture_session_ != nullptr)
			{
				capture_session_.Close();
				capture_session_ = nullptr;
			}
			if (frame_pool_ != nullptr)
			{
				frame_pool_.Close();
				frame_pool_ = nullptr;
			}

			capture_item_ = nullptr;
			uwp_device_ = nullptr;
			if (active_gpu_frame_ != nullptr)
			{
				active_gpu_frame_.Close();
				active_gpu_frame_ = nullptr;
			}
			latest_gpu_texture_.Reset();
			latest_gpu_content_width_ = 0;
			latest_gpu_content_height_ = 0;
			capture_size_ = {0, 0};
			content_size_ = {0, 0};
			has_last_gpu_frame_ = false;
			hwnd_ = hwnd;
			monitor_ = nullptr;
			target_kind_ = WgcTargetKind::Window;

			return CreateCaptureSession(error);
		}

		[[nodiscard]] ID3D11Device *Device() const { return device_.Get(); }
		[[nodiscard]] ID3D11DeviceContext *Context() const { return context_.Get(); }

		bool CaptureGpu(ID3D11Texture2D *&texture, int &width, int &height, bool wait_for_new_frame = false)
		{
			texture = nullptr;
			width = 0;
			height = 0;

			if (capture_session_ == nullptr || frame_pool_ == nullptr)
			{
				return false;
			}

			winrt_capture::Direct3D11CaptureFrame frame{nullptr};
			std::uint64_t frame_generation = 0;
			const DWORD wait_ms = wait_for_new_frame ? 16U : (has_last_gpu_frame_ ? 0U : 16U);
			if (!AcquireFrame(frame, wait_ms, &frame_generation))
			{
				if (!wait_for_new_frame && has_last_gpu_frame_ && latest_gpu_texture_ != nullptr)
				{
					D3D11_TEXTURE2D_DESC desc{};
					latest_gpu_texture_->GetDesc(&desc);
					texture = latest_gpu_texture_.Get();
					width = latest_gpu_content_width_ > 0 ? latest_gpu_content_width_ : static_cast<int>(desc.Width);
					height = latest_gpu_content_height_ > 0 ? latest_gpu_content_height_ : static_cast<int>(desc.Height);
					return true;
				}
				return false;
			}

			ComPtr<ID3D11Texture2D> source_texture;
			UINT copy_width = 0;
			UINT copy_height = 0;
			if (!ExtractFrameTexture(frame, source_texture, copy_width, copy_height))
			{
				frame.Close();
				return false;
			}

			if (active_gpu_frame_ != nullptr)
			{
				active_gpu_frame_.Close();
				active_gpu_frame_ = nullptr;
			}
			active_gpu_frame_ = frame;
			latest_gpu_texture_ = source_texture;
			latest_gpu_content_width_ = static_cast<int>(copy_width);
			latest_gpu_content_height_ = static_cast<int>(copy_height);
			latest_gpu_generation_ = frame_generation;
			has_last_gpu_frame_ = true;
			texture = latest_gpu_texture_.Get();
			width = static_cast<int>(copy_width);
			height = static_cast<int>(copy_height);
			return true;
		}

		bool RefreshLatestGpuFromPending()
		{
			winrt_capture::Direct3D11CaptureFrame frame{nullptr};
			std::uint64_t frame_generation = 0;
			AcquireSRWLockShared(&frame_lock_);
			if (pending_frame_ != nullptr && pending_frame_generation_ != latest_gpu_generation_)
			{
				frame = pending_frame_;
				frame_generation = pending_frame_generation_;
			}
			ReleaseSRWLockShared(&frame_lock_);

			if (frame == nullptr)
			{
				return false;
			}

			ComPtr<ID3D11Texture2D> source_texture;
			UINT copy_width = 0;
			UINT copy_height = 0;
			if (!ExtractFrameTexture(frame, source_texture, copy_width, copy_height))
			{
				return false;
			}

			if (active_gpu_frame_ != nullptr)
			{
				active_gpu_frame_.Close();
				active_gpu_frame_ = nullptr;
			}
			active_gpu_frame_ = frame;
			latest_gpu_texture_ = source_texture;
			latest_gpu_content_width_ = static_cast<int>(copy_width);
			latest_gpu_content_height_ = static_cast<int>(copy_height);
			latest_gpu_generation_ = frame_generation;
			has_last_gpu_frame_ = true;
			return true;
		}

		bool GetLatestGpu(ID3D11Texture2D *&texture, int &width, int &height) const
		{
			texture = nullptr;
			width = 0;
			height = 0;
			if (!has_last_gpu_frame_ || latest_gpu_texture_ == nullptr)
			{
				return false;
			}
			D3D11_TEXTURE2D_DESC desc{};
			latest_gpu_texture_->GetDesc(&desc);
			texture = latest_gpu_texture_.Get();
			width = latest_gpu_content_width_ > 0 ? latest_gpu_content_width_ : static_cast<int>(desc.Width);
			height = latest_gpu_content_height_ > 0 ? latest_gpu_content_height_ : static_cast<int>(desc.Height);
			return true;
		}

		void Shutdown()
		{
			AcquireSRWLockExclusive(&frame_lock_);
			if (pending_frame_ != nullptr)
			{
				pending_frame_.Close();
				pending_frame_ = nullptr;
			}
			ReleaseSRWLockExclusive(&frame_lock_);

			if (frame_pool_ != nullptr && frame_arrived_token_.value != 0)
			{
				frame_pool_.FrameArrived(frame_arrived_token_);
				frame_arrived_token_.value = 0;
			}

			if (capture_session_ != nullptr)
			{
				capture_session_.Close();
				capture_session_ = nullptr;
			}
			if (frame_pool_ != nullptr)
			{
				frame_pool_.Close();
				frame_pool_ = nullptr;
			}

			capture_item_ = nullptr;
			uwp_device_ = nullptr;
			if (active_gpu_frame_ != nullptr)
			{
				active_gpu_frame_.Close();
				active_gpu_frame_ = nullptr;
			}
			latest_gpu_texture_.Reset();
			latest_gpu_content_width_ = 0;
			latest_gpu_content_height_ = 0;

			context_.Reset();
			device_.Reset();

			has_last_gpu_frame_ = false;
			capture_size_ = {0, 0};
			content_size_ = {0, 0};
			hwnd_ = nullptr;
			monitor_ = nullptr;
			target_kind_ = WgcTargetKind::Window;
		}

	private:
		static std::string HResultHex(HRESULT hr)
		{
			char buffer[16]{};
			std::snprintf(buffer, sizeof(buffer), "0x%08X", static_cast<unsigned int>(hr));
			return std::string(buffer);
		}

		bool CreateDevice(std::string &error)
		{
			UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_VIDEO_SUPPORT;
#if defined(_DEBUG)
			flags |= D3D11_CREATE_DEVICE_DEBUG;
#endif
			const D3D_FEATURE_LEVEL levels[] = {
				D3D_FEATURE_LEVEL_11_1,
				D3D_FEATURE_LEVEL_11_0,
				D3D_FEATURE_LEVEL_10_1,
			};
			D3D_FEATURE_LEVEL feature_level = D3D_FEATURE_LEVEL_11_0;
			HRESULT hr = D3D11CreateDevice(
				nullptr,
				D3D_DRIVER_TYPE_HARDWARE,
				nullptr,
				flags,
				levels,
				static_cast<UINT>(std::size(levels)),
				D3D11_SDK_VERSION,
				device_.GetAddressOf(),
				&feature_level,
				context_.GetAddressOf());
			if (FAILED(hr) || device_ == nullptr || context_ == nullptr)
			{
				error = "D3D11CreateDevice failed for WGC capture hardware device.";
				return false;
			}

			ComPtr<IDXGIDevice1> dxgi1;
			if (SUCCEEDED(device_.As(&dxgi1)) && dxgi1 != nullptr)
			{
				dxgi1->SetMaximumFrameLatency(1);
			}

			ComPtr<IDXGIDevice> dxgi;
			if (SUCCEEDED(device_.As(&dxgi)) && dxgi != nullptr)
			{
				dxgi->SetGPUThreadPriority(7);
			}

			return true;
		}

		bool CreateCaptureSession(std::string &error)
		{
			if (!winrt_capture::GraphicsCaptureSession::IsSupported())
			{
				error = "Windows Graphics Capture is not supported on this system.";
				return false;
			}

			ComPtr<IDXGIDevice> dxgi_device;
			if (FAILED(device_.As(&dxgi_device)) || dxgi_device == nullptr)
			{
				error = "Failed to query IDXGIDevice from D3D11 device.";
				return false;
			}

			winrt::com_ptr<IInspectable> d3d_com;
			HRESULT hr = winrt_capture::CreateDirect3D11DeviceFromDXGIDevice(dxgi_device.Get(), d3d_com.put());
			if (FAILED(hr) || d3d_com == nullptr)
			{
				error = "CreateDirect3D11DeviceFromDXGIDevice failed: " + HResultHex(hr);
				return false;
			}

			uwp_device_ = d3d_com.as<winrt_capture::IDirect3DDevice>();
			if (uwp_device_ == nullptr)
			{
				error = "Failed to convert WinRT D3D11 device.";
				return false;
			}

			auto factory = winrt::get_activation_factory<winrt_capture::GraphicsCaptureItem, IGraphicsCaptureItemInterop>();
			if (factory == nullptr)
			{
				error = "Failed to get GraphicsCaptureItem interop factory.";
				return false;
			}

			if (target_kind_ == WgcTargetKind::Monitor)
			{
				hr = factory->CreateForMonitor(
					monitor_,
					winrt::guid_of<winrt_capture::IGraphicsCaptureItem>(),
					winrt::put_abi(capture_item_));
			}
			else
			{
				hr = factory->CreateForWindow(
					hwnd_,
					winrt::guid_of<winrt_capture::IGraphicsCaptureItem>(),
					winrt::put_abi(capture_item_));
			}
			if (FAILED(hr) || capture_item_ == nullptr)
			{
				error = std::string(target_kind_ == WgcTargetKind::Monitor ? "CreateForMonitor failed: " : "CreateForWindow failed: ") + HResultHex(hr);
				return false;
			}

			content_size_ = capture_item_.Size();
			if (content_size_.Width <= 0 || content_size_.Height <= 0)
			{
				error = "Captured window has invalid size.";
				return false;
			}
			// Keep WGC frame pool at native captured window size. Downscale happens in
			// GPU encoder path, which avoids top-left cropping when stream resolution
			// is lower than source window resolution.
			capture_size_ = content_size_;

			try
			{
				frame_pool_ = winrt_capture::Direct3D11CaptureFramePool::CreateFreeThreaded(
					uwp_device_,
					winrt_capture::DirectXPixelFormat::B8G8R8A8UIntNormalized,
					2,
					capture_size_);
				capture_session_ = frame_pool_.CreateCaptureSession(capture_item_);
				frame_arrived_token_ = frame_pool_.FrameArrived({this, &WgcWindowCapture::OnFrameArrived});

				if (target_kind_ == WgcTargetKind::Window &&
					winrt_capture::ApiInformation::IsPropertyPresent(
						L"Windows.Graphics.Capture.GraphicsCaptureSession",
						L"IncludeSecondaryWindows"))
				{
					try
					{
						capture_session_.IncludeSecondaryWindows(true);
					}
					catch (const winrt::hresult_error &e)
					{
						LogWarningStream()
							<< "[CAPTURE] IncludeSecondaryWindows unavailable: "
							<< HResultHex(static_cast<HRESULT>(e.code()));
					}
				}
				
				if (winrt_capture::ApiInformation::IsPropertyPresent(
						L"Windows.Graphics.Capture.GraphicsCaptureSession",
						L"IsBorderRequired"))
				{
					capture_session_.IsBorderRequired(false);
				}

				if (winrt_capture::ApiInformation::IsPropertyPresent(
						L"Windows.Graphics.Capture.GraphicsCaptureSession",
						L"IsCursorCaptureEnabled"))
				{
					capture_session_.IsCursorCaptureEnabled(capture_cursor_);
				}

				if (winrt_capture::ApiInformation::IsPropertyPresent(
						L"Windows.Graphics.Capture.GraphicsCaptureSession",
						L"MinUpdateInterval"))
				{
					capture_session_.MinUpdateInterval(winrt::Windows::Foundation::TimeSpan{0});
				}

				capture_session_.StartCapture();
			}
			catch (const winrt::hresult_error &e)
			{
				error = "Failed to start WGC capture session: " + HResultHex(static_cast<HRESULT>(e.code()));
				return false;
			}

			return true;
		}

		bool ExtractFrameTexture(
			winrt_capture::Direct3D11CaptureFrame const &frame,
			ComPtr<ID3D11Texture2D> &source_texture,
			UINT &copy_width,
			UINT &copy_height)
		{
			source_texture.Reset();
			copy_width = 0;
			copy_height = 0;

			auto content_size = frame.ContentSize();
			if (content_size.Width <= 0 || content_size.Height <= 0)
			{
				return false;
			}
			content_size_ = content_size;

			auto surface = frame.Surface();
			auto access = surface.as<winrt_capture::IDirect3DDxgiInterfaceAccess>();
			if (access == nullptr)
			{
				return false;
			}

			if (FAILED(access->GetInterface(IID_PPV_ARGS(source_texture.GetAddressOf()))) || source_texture == nullptr)
			{
				return false;
			}

			D3D11_TEXTURE2D_DESC source_desc{};
			source_texture->GetDesc(&source_desc);

			copy_width = std::min<UINT>(source_desc.Width, static_cast<UINT>(content_size.Width));
			copy_height = std::min<UINT>(source_desc.Height, static_cast<UINT>(content_size.Height));
			return copy_width > 0 && copy_height > 0;
		}

		bool AcquireFrame(winrt_capture::Direct3D11CaptureFrame &out_frame, DWORD timeout_ms, std::uint64_t *out_generation = nullptr)
		{
			out_frame = nullptr;
			if (out_generation != nullptr)
			{
				*out_generation = 0;
			}

			AcquireSRWLockExclusive(&frame_lock_);
			if (pending_frame_ == nullptr)
			{
				if (SleepConditionVariableSRW(&frame_cv_, &frame_lock_, timeout_ms, 0) == 0)
				{
					ReleaseSRWLockExclusive(&frame_lock_);
					return false;
				}
			}

			if (pending_frame_ != nullptr)
			{
				out_frame = pending_frame_;
				if (out_generation != nullptr)
				{
					*out_generation = pending_frame_generation_;
				}
				pending_frame_ = nullptr;
			}
			ReleaseSRWLockExclusive(&frame_lock_);
			return out_frame != nullptr;
		}

		void OnFrameArrived(
			winrt_capture::Direct3D11CaptureFramePool const &sender,
			winrt::Windows::Foundation::IInspectable const &)
		{
			winrt_capture::Direct3D11CaptureFrame frame{nullptr};
			try
			{
				frame = sender.TryGetNextFrame();
			}
			catch (const winrt::hresult_error &)
			{
				return;
			}

			if (frame == nullptr)
			{
				return;
			}

			auto content_size = frame.ContentSize();
			if (content_size.Width > 0 && content_size.Height > 0)
			{
				content_size_ = content_size;
				if (capture_size_.Width != content_size.Width || capture_size_.Height != content_size.Height)
				{
					RecreateFramePool(content_size);
				}
			}

			AcquireSRWLockExclusive(&frame_lock_);
			if (pending_frame_ != nullptr)
			{
				pending_frame_.Close();
			}
			pending_frame_ = frame;
			++pending_frame_generation_;
			ReleaseSRWLockExclusive(&frame_lock_);
			WakeConditionVariable(&frame_cv_);
		}

		void RecreateFramePool(winrt::Windows::Graphics::SizeInt32 size)
		{
			if (frame_pool_ == nullptr || size.Width <= 0 || size.Height <= 0)
			{
				return;
			}

			try
			{
				frame_pool_.Recreate(
					uwp_device_,
					winrt_capture::DirectXPixelFormat::B8G8R8A8UIntNormalized,
					2,
					size);
				capture_size_ = size;
			}
			catch (const winrt::hresult_error &)
			{
				// Keep current pool if dynamic resize fails.
			}
		}

		HWND hwnd_ = nullptr;
		HMONITOR monitor_ = nullptr;
		WgcTargetKind target_kind_ = WgcTargetKind::Window;
		bool capture_cursor_ = true;
		bool has_last_gpu_frame_ = false;

		ComPtr<ID3D11Device> device_;
		ComPtr<ID3D11DeviceContext> context_;

		winrt_capture::IDirect3DDevice uwp_device_{nullptr};
		winrt_capture::GraphicsCaptureItem capture_item_{nullptr};
		winrt_capture::Direct3D11CaptureFramePool frame_pool_{nullptr};
		winrt_capture::GraphicsCaptureSession capture_session_{nullptr};
		winrt_capture::Direct3D11CaptureFrame pending_frame_{nullptr};
		winrt_capture::Direct3D11CaptureFrame active_gpu_frame_{nullptr};
		ComPtr<ID3D11Texture2D> latest_gpu_texture_;
		int latest_gpu_content_width_ = 0;
		int latest_gpu_content_height_ = 0;
		std::uint64_t pending_frame_generation_ = 0;
		std::uint64_t latest_gpu_generation_ = 0;
		winrt::event_token frame_arrived_token_{};
		winrt::Windows::Graphics::SizeInt32 capture_size_{0, 0};
		winrt::Windows::Graphics::SizeInt32 content_size_{0, 0};

		SRWLOCK frame_lock_{};
		CONDITION_VARIABLE frame_cv_{};
	};

	class SharedTextureMirror
	{
	public:
		bool Init(ID3D11Device *device, ID3D11DeviceContext *context)
		{
			texture_.Reset();
			shared_handle_ = nullptr;
			width_ = 0;
			height_ = 0;
			format_ = DXGI_FORMAT_UNKNOWN;
			device_ = device;
			context_ = context;
			return device_ != nullptr && context_ != nullptr;
		}

		bool UpdateFromTexture(ID3D11Texture2D *source, int visible_width, int visible_height)
		{
			if (source == nullptr || device_ == nullptr || context_ == nullptr)
			{
				return false;
			}

			D3D11_TEXTURE2D_DESC source_desc{};
			source->GetDesc(&source_desc);
			const UINT copy_width = std::clamp<UINT>(
				static_cast<UINT>(std::max(visible_width, 1)),
				1,
				source_desc.Width);
			const UINT copy_height = std::clamp<UINT>(
				static_cast<UINT>(std::max(visible_height, 1)),
				1,
				source_desc.Height);
			if (!EnsureTexture(source_desc, static_cast<int>(copy_width), static_cast<int>(copy_height)))
			{
				return false;
			}

			D3D11_BOX src_box{};
			src_box.left = 0;
			src_box.top = 0;
			src_box.front = 0;
			src_box.right = copy_width;
			src_box.bottom = copy_height;
			src_box.back = 1;
			context_->CopySubresourceRegion(texture_.Get(), 0, 0, 0, 0, source, 0, &src_box);
			// Without an explicit flush here, the shared preview texture can appear to
			// update only sporadically until the encoder starts issuing its own GPU work.
			context_->Flush();
			return true;
		}

		[[nodiscard]] HANDLE SharedHandle() const { return shared_handle_; }
		[[nodiscard]] ID3D11Texture2D *Texture() const { return texture_.Get(); }
		[[nodiscard]] int Width() const { return width_; }
		[[nodiscard]] int Height() const { return height_; }
		[[nodiscard]] DXGI_FORMAT Format() const { return format_; }

	private:
		bool EnsureTexture(const D3D11_TEXTURE2D_DESC &source_desc, int width, int height)
		{
			if (texture_ != nullptr &&
				width_ == width &&
				height_ == height &&
				format_ == source_desc.Format)
			{
				return true;
			}

			texture_.Reset();
			shared_handle_ = nullptr;

			D3D11_TEXTURE2D_DESC out_desc{};
			out_desc.Width = static_cast<UINT>(std::max(width, 1));
			out_desc.Height = static_cast<UINT>(std::max(height, 1));
			out_desc.MipLevels = 1;
			out_desc.ArraySize = 1;
			out_desc.Format = source_desc.Format;
			out_desc.SampleDesc.Count = 1;
			out_desc.Usage = D3D11_USAGE_DEFAULT;
			out_desc.CPUAccessFlags = 0;

			UINT bind_flags = source_desc.BindFlags;
			if ((bind_flags & D3D11_BIND_SHADER_RESOURCE) == 0)
			{
				bind_flags |= D3D11_BIND_SHADER_RESOURCE;
			}
			out_desc.BindFlags = bind_flags;
			out_desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED;
			HRESULT hr = device_->CreateTexture2D(&out_desc, nullptr, texture_.GetAddressOf());
			if (FAILED(hr) || texture_ == nullptr)
			{
				return false;
			}

			ComPtr<IDXGIResource> resource;
			if (FAILED(texture_.As(&resource)) || resource == nullptr)
			{
				texture_.Reset();
				return false;
			}
			if (FAILED(resource->GetSharedHandle(&shared_handle_)))
			{
				texture_.Reset();
				shared_handle_ = nullptr;
				return false;
			}

			width_ = width;
			height_ = height;
			format_ = source_desc.Format;
			return true;
		}

		ComPtr<ID3D11Device> device_;
		ComPtr<ID3D11DeviceContext> context_;
		ComPtr<ID3D11Texture2D> texture_;
		HANDLE shared_handle_ = nullptr;
		int width_ = 0;
		int height_ = 0;
		DXGI_FORMAT format_ = DXGI_FORMAT_UNKNOWN;
	};

	struct HostStats
	{
		std::uint64_t capture_frames = 0;
		double last_capture_ms = 0.0;
	};
	struct CaptureOptions
	{
		std::uint64_t window_handle = 0;
		bool enable_audio = true;
		CaptureMode capture_mode = CaptureMode::Window;
		bool capture_cursor = true;
	};

	struct CaptureStats
	{
		std::uint64_t capture_frames = 0;
		double last_capture_ms = 0.0;
		std::int32_t preview_width = 0;
		std::int32_t preview_height = 0;
		std::int32_t audio_capture_state = 0;
		std::int32_t audio_target_pid = 0;
		std::uint64_t local_audio_samples_read = 0;
	};

	struct RawVideoFrame
	{
		void *texture = nullptr;
		std::int32_t width = 0;
		std::int32_t height = 0;
		std::uint64_t capture_timestamp_us = 0;
	};

	std::thread g_capture_thread;
	std::mutex g_capture_thread_mutex;
	std::mutex g_capture_mutex;
	std::atomic<bool> g_capture_running{false};
	std::atomic<std::uint64_t> g_capture_pending_window_handle{kNoPendingWindowHandle};
	std::unique_ptr<WgcWindowCapture> g_capture;
	SharedTextureMirror g_capture_preview_mirror;
	streamproto::audio::AudioFrameQueue g_capture_audio_queue;
	std::uint64_t g_capture_frame_sequence = 0;

	void ClearCaptureState()
	{
		g_preview_shared_handle.store(0);
		g_preview_width.store(0);
		g_preview_height.store(0);
		g_stats_audio_capture_state.store(0);
		g_stats_audio_target_pid.store(0);
		if (g_host_local_audio_ring != nullptr)
		{
			g_host_local_audio_ring->Clear();
		}
		g_capture_audio_queue.Clear();
	}

	void PublishCapturePreview(ID3D11Texture2D *preview_texture, int preview_w, int preview_h)
	{
		if (preview_texture == nullptr)
		{
			return;
		}
		if (!g_capture_preview_mirror.UpdateFromTexture(preview_texture, preview_w, preview_h))
		{
			return;
		}
		g_preview_shared_handle.store(
			static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(g_capture_preview_mirror.SharedHandle())));
		g_preview_width.store(g_capture_preview_mirror.Width());
		g_preview_height.store(g_capture_preview_mirror.Height());
	}

	void StartRawAudioCaptureForWindow(
		streamproto::audio::ProcessLoopbackCapture &audio_capture,
		const CaptureOptions &options,
		HWND hwnd)
	{
		audio_capture.Stop();
		g_capture_audio_queue.Clear();
		g_stats_audio_capture_state.store(0);
		g_stats_audio_target_pid.store(0);
		SetLastAudioErrorString("");
		if (g_host_local_audio_ring != nullptr)
		{
			g_host_local_audio_ring->Clear();
		}
		if (!options.enable_audio || hwnd == nullptr || !IsWindow(hwnd))
		{
			if (!options.enable_audio)
			{
				SetLastAudioErrorString("Audio capture disabled.");
			}
			return;
		}

		DWORD pid = 0;
		GetWindowThreadProcessId(hwnd, &pid);
		if (pid == 0)
		{
			const std::string message = "Audio capture skipped: target window PID is unavailable.";
			SetLastAudioErrorString(message);
			LogWarningStream() << "[CAPTURE] " << message;
			return;
		}
		g_stats_audio_target_pid.store(static_cast<int>(pid));

		std::string audio_error;
		if (audio_capture.Start(
				static_cast<std::uint32_t>(pid),
				streamproto::audio::kFrameSamples,
				g_host_local_audio_ring,
				&g_capture_audio_queue,
				audio_error))
		{
			g_stats_audio_capture_state.store(1);
			SetLastAudioErrorString("");
			LogInfoStream() << "[CAPTURE] Audio process-loopback capture started for pid=" << pid;
		}
		else
		{
			g_stats_audio_capture_state.store(-1);
			SetLastAudioErrorString(audio_error);
			LogWarningStream() << "[CAPTURE] Audio process-loopback capture unavailable for pid="
							   << pid << ": " << audio_error;
		}
	}

	void StartRawAudioCaptureForSteamStreamingSpeakers(
		streamproto::audio::ProcessLoopbackCapture &audio_capture,
		const CaptureOptions &options)
	{
		audio_capture.Stop();
		g_capture_audio_queue.Clear();
		g_stats_audio_capture_state.store(0);
		g_stats_audio_target_pid.store(0);
		SetLastAudioErrorString("");
		if (g_host_local_audio_ring != nullptr)
		{
			g_host_local_audio_ring->Clear();
		}
		if (!options.enable_audio)
		{
			SetLastAudioErrorString("Audio capture disabled.");
			return;
		}

		std::string audio_error;
		if (audio_capture.StartSteamStreamingSpeakers(
				streamproto::audio::kFrameSamples,
				g_host_local_audio_ring,
				&g_capture_audio_queue,
				audio_error))
		{
			g_stats_audio_capture_state.store(1);
			SetLastAudioErrorString("");
			LogInfoStream() << "[CAPTURE] Audio Steam Streaming Speakers loopback capture started";
		}
		else
		{
			g_stats_audio_capture_state.store(-1);
			SetLastAudioErrorString(audio_error);
			LogWarningStream() << "[CAPTURE] Audio Steam Streaming Speakers loopback capture unavailable: " << audio_error;
		}
	}

	int RunCaptureWorker(CaptureOptions options)
	{
		HWND target_hwnd = reinterpret_cast<HWND>(static_cast<std::uintptr_t>(options.window_handle));
		HMONITOR target_monitor = nullptr;
		if (options.capture_mode == CaptureMode::Display)
		{
			target_monitor = options.window_handle != 0
				? reinterpret_cast<HMONITOR>(static_cast<std::uintptr_t>(options.window_handle))
				: nullptr;
			if (target_monitor == nullptr)
			{
				POINT origin{0, 0};
				target_monitor = MonitorFromPoint(origin, MONITOR_DEFAULTTOPRIMARY);
			}
			if (target_monitor == nullptr)
			{
				SetLastErrorString("Display monitor is unavailable.");
				return 1;
			}
			MONITORINFO monitor_info{};
			monitor_info.cbSize = sizeof(monitor_info);
			if (!GetMonitorInfoW(target_monitor, &monitor_info))
			{
				SetLastErrorString("Requested display monitor handle is invalid.");
				return 1;
			}
		}
		else if (target_hwnd == nullptr || !IsWindow(target_hwnd))
		{
			SetLastErrorString("Invalid capture window handle.");
			return 1;
		}

		std::string error;
		auto capture = std::make_unique<WgcWindowCapture>(options.capture_cursor);
		const bool init_ok = options.capture_mode == CaptureMode::Display
			? capture->InitMonitor(target_monitor, error)
			: capture->Init(target_hwnd, error);
		if (!init_ok)
		{
			SetLastErrorString("Capture init failed: " + error);
			LogErrorStream() << "[CAPTURE] Init failed: " << error;
			return 1;
		}

		{
			std::lock_guard<std::mutex> lock(g_capture_mutex);
			g_capture = std::move(capture);
			g_capture_preview_mirror.Init(g_capture->Device(), g_capture->Context());
		}

		streamproto::audio::ProcessLoopbackCapture audio_capture{};
		if (options.capture_mode == CaptureMode::Display)
		{
			StartRawAudioCaptureForSteamStreamingSpeakers(audio_capture, options);
		}
		else
		{
			StartRawAudioCaptureForWindow(audio_capture, options, target_hwnd);
		}

		HostStats capture_stats{};
		std::uint64_t last_stats_ms = streamproto::NowSteadyMillis();

		while (g_capture_running.load())
		{
			const std::uint64_t pending_window_handle = g_capture_pending_window_handle.exchange(kNoPendingWindowHandle);
			if (pending_window_handle != kNoPendingWindowHandle)
			{
				if (options.capture_mode == CaptureMode::Display)
				{
					continue;
				}

				HWND new_hwnd = reinterpret_cast<HWND>(static_cast<std::uintptr_t>(pending_window_handle));
				if (new_hwnd != nullptr && IsWindow(new_hwnd))
				{
					std::string switch_error;
					bool ok = false;
					{
						std::lock_guard<std::mutex> lock(g_capture_mutex);
						ok = g_capture != nullptr && g_capture->SwitchWindow(new_hwnd, switch_error);
						if (ok)
						{
							g_capture_preview_mirror.Init(g_capture->Device(), g_capture->Context());
						}
					}
					if (ok)
					{
						target_hwnd = new_hwnd;
						StartRawAudioCaptureForWindow(audio_capture, options, target_hwnd);
					}
					else
					{
						SetLastErrorString("Capture window switch failed: " + switch_error);
					}
				}
				else if (new_hwnd == nullptr)
				{
					audio_capture.Stop();
					std::lock_guard<std::mutex> lock(g_capture_mutex);
					if (g_capture != nullptr)
					{
						g_capture->Shutdown();
					}
					ClearCaptureState();
				}
				else
				{
					SetLastErrorString("Requested capture window handle is invalid.");
				}
			}

			ID3D11Texture2D *texture = nullptr;
			int frame_width = 0;
			int frame_height = 0;
			bool have_frame = false;
			const auto capture_start = std::chrono::steady_clock::now();
			{
				std::lock_guard<std::mutex> lock(g_capture_mutex);
				have_frame =
					g_capture != nullptr &&
					g_capture->RefreshLatestGpuFromPending() &&
					g_capture->GetLatestGpu(texture, frame_width, frame_height);
				if (have_frame)
				{
					PublishCapturePreview(texture, frame_width, frame_height);
					++g_capture_frame_sequence;
				}
			}

			if (have_frame)
			{
				const auto capture_end = std::chrono::steady_clock::now();
				capture_stats.last_capture_ms = std::chrono::duration<double, std::milli>(capture_end - capture_start).count();
				capture_stats.capture_frames++;
			}
			else
			{
				std::this_thread::sleep_for(std::chrono::milliseconds(1));
			}

			const std::uint64_t now_ms = streamproto::NowSteadyMillis();
			if (now_ms - last_stats_ms >= 250)
			{
				last_stats_ms = now_ms;
				std::lock_guard<std::mutex> lock(g_stats_mutex);
				g_stats_capture_frames = capture_stats.capture_frames;
				g_stats_last_capture_ms = capture_stats.last_capture_ms;
			}
		}

		audio_capture.Stop();
		{
			std::lock_guard<std::mutex> lock(g_capture_mutex);
			if (g_capture != nullptr)
			{
				g_capture->Shutdown();
				g_capture.reset();
			}
		}
		ClearCaptureState();
		return 0;
	}

	bool AreCopyCompatibleFormats(DXGI_FORMAT lhs, DXGI_FORMAT rhs)
	{
		if (lhs == rhs)
		{
			return true;
		}

		auto format_family = [](DXGI_FORMAT format)
		{
			switch (format)
			{
			case DXGI_FORMAT_B8G8R8A8_TYPELESS:
			case DXGI_FORMAT_B8G8R8A8_UNORM:
			case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
				return 1;
			case DXGI_FORMAT_R8G8B8A8_TYPELESS:
			case DXGI_FORMAT_R8G8B8A8_UNORM:
			case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
				return 2;
			default:
				return 0;
			}
		};

		const int lhs_family = format_family(lhs);
		return lhs_family != 0 && lhs_family == format_family(rhs);
	}

	void UNITY_INTERFACE_API OnHostGraphicsDeviceEvent(UnityGfxDeviceEventType event_type)
	{
		std::lock_guard<std::mutex> lock(g_unity_mutex);
		if (event_type == kUnityGfxDeviceEventInitialize)
		{
			if (g_unity_interfaces == nullptr)
			{
				return;
			}
			g_unity_graphics = g_unity_interfaces->Get<IUnityGraphics>();
			if (g_unity_graphics == nullptr || g_unity_graphics->GetRenderer() != kUnityGfxRendererD3D11)
			{
				g_unity_device = nullptr;
				return;
			}
			IUnityGraphicsD3D11 *d3d11 = g_unity_interfaces->Get<IUnityGraphicsD3D11>();
			g_unity_device = d3d11 != nullptr ? d3d11->GetDevice() : nullptr;
			g_unity_opened_source_texture.Reset();
			g_unity_opened_source_handle = nullptr;
		}
		else if (event_type == kUnityGfxDeviceEventShutdown)
		{
			g_unity_device = nullptr;
			g_unity_target_texture = nullptr;
			g_unity_opened_source_texture.Reset();
			g_unity_opened_source_handle = nullptr;
		}
	}

	void UNITY_INTERFACE_API OnHostRenderEvent(int event_id)
	{
		if (event_id != kCaptureRenderEventId)
		{
			return;
		}

		std::lock_guard<std::mutex> lock(g_unity_mutex);
		if (g_unity_device == nullptr || g_unity_target_texture == nullptr)
		{
			return;
		}

		HANDLE source_handle = reinterpret_cast<HANDLE>(
			static_cast<std::uintptr_t>(g_preview_shared_handle.load()));
		if (source_handle == nullptr)
		{
			return;
		}

		if (source_handle != g_unity_opened_source_handle || g_unity_opened_source_texture == nullptr)
		{
			g_unity_opened_source_texture.Reset();
			HRESULT hr = g_unity_device->OpenSharedResource(
				source_handle,
				IID_PPV_ARGS(g_unity_opened_source_texture.GetAddressOf()));
			if (FAILED(hr) || g_unity_opened_source_texture == nullptr)
			{
				g_unity_opened_source_handle = nullptr;
				return;
			}
			g_unity_opened_source_handle = source_handle;
		}

		D3D11_TEXTURE2D_DESC src_desc{};
		D3D11_TEXTURE2D_DESC dst_desc{};
		g_unity_opened_source_texture->GetDesc(&src_desc);
		g_unity_target_texture->GetDesc(&dst_desc);
		if (!AreCopyCompatibleFormats(src_desc.Format, dst_desc.Format))
		{
			return;
		}

		ComPtr<ID3D11DeviceContext> context;
		g_unity_device->GetImmediateContext(context.GetAddressOf());
		if (context == nullptr)
		{
			return;
		}

		const UINT copy_width = std::min(src_desc.Width, dst_desc.Width);
		const UINT copy_height = std::min(src_desc.Height, dst_desc.Height);
		if (copy_width == 0 || copy_height == 0)
		{
			return;
		}

		D3D11_BOX src_box{};
		src_box.left = 0;
		src_box.top = 0;
		src_box.front = 0;
		src_box.right = copy_width;
		src_box.bottom = copy_height;
		src_box.back = 1;
		context->CopySubresourceRegion(
			g_unity_target_texture,
			0,
			0,
			0,
			0,
			g_unity_opened_source_texture.Get(),
			0,
			&src_box);
	}

} // namespace

extern "C"
{

	struct SSPCAP_StartParams
	{
		std::uint64_t window_handle;
		std::int32_t enable_audio;
		std::int32_t capture_mode;
		std::int32_t capture_cursor;
	};

	struct SSPCAP_Stats
	{
		std::uint64_t capture_frames;
		double last_capture_ms;
		std::int32_t preview_width;
		std::int32_t preview_height;
		std::int32_t audio_capture_state;
		std::int32_t audio_target_pid;
		std::uint64_t local_audio_samples_read;
	};

	UNITY_INTERFACE_EXPORT void UNITY_INTERFACE_API UnityPluginLoad(IUnityInterfaces *unity_interfaces)
	{
		g_unity_interfaces = unity_interfaces;
		g_unity_graphics = unity_interfaces != nullptr ? unity_interfaces->Get<IUnityGraphics>() : nullptr;
		if (g_unity_graphics != nullptr)
		{
			g_unity_graphics->RegisterDeviceEventCallback(OnHostGraphicsDeviceEvent);
		}
		OnHostGraphicsDeviceEvent(kUnityGfxDeviceEventInitialize);
	}

	UNITY_INTERFACE_EXPORT void UNITY_INTERFACE_API UnityPluginUnload()
	{
		if (g_unity_graphics != nullptr)
		{
			g_unity_graphics->UnregisterDeviceEventCallback(OnHostGraphicsDeviceEvent);
		}
		OnHostGraphicsDeviceEvent(kUnityGfxDeviceEventShutdown);
		SetHostLogCallback(nullptr);
		SetHostLoggingEnabled(true);
		g_unity_interfaces = nullptr;
		g_unity_graphics = nullptr;
	}

	UNITY_INTERFACE_EXPORT bool SSPCAP_Start(const SSPCAP_StartParams *params)
	{
		if (params == nullptr)
		{
			SetLastErrorString("Capture start requires parameters.");
			return false;
		}

		const CaptureMode capture_mode = params->capture_mode == static_cast<std::int32_t>(CaptureMode::Display)
			? CaptureMode::Display
			: CaptureMode::Window;
		if (capture_mode == CaptureMode::Window && params->window_handle == 0)
		{
			SetLastErrorString("Window capture start requires a window handle.");
			return false;
		}

		std::lock_guard<std::mutex> lock(g_capture_thread_mutex);
		if (g_capture_running.load())
		{
			return false;
		}

		CaptureOptions options{};
		options.window_handle = params->window_handle;
		options.enable_audio = params->enable_audio != 0;
		options.capture_mode = capture_mode;
		options.capture_cursor = params->capture_cursor != 0;

		g_capture_running.store(true);
		g_capture_pending_window_handle.store(kNoPendingWindowHandle);
		{
			std::lock_guard<std::mutex> stats_lock(g_stats_mutex);
			g_stats_capture_frames = 0;
			g_stats_last_capture_ms = 0.0;
		}
		g_stats_audio_capture_state.store(0);
		g_stats_audio_target_pid.store(0);
		g_stats_local_audio_samples_read.store(0);
		SetLastAudioErrorString("");
		SetLastErrorString("");
		if (g_host_local_audio_ring != nullptr)
		{
			g_host_local_audio_ring->Clear();
		}
		g_capture_audio_queue.Clear();

		g_capture_thread = std::thread([options]()
			{
				const int result = RunCaptureWorker(options);
				if (result != 0)
				{
					bool has_error = false;
					{
						std::lock_guard<std::mutex> lock(g_error_mutex);
						has_error = !g_last_error.empty();
					}
					if (!has_error)
					{
						SetLastErrorString("Capture worker exited with failure.");
					}
				}
				g_capture_running.store(false);
			});

		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
		while (g_capture_running.load())
		{
			{
				std::lock_guard<std::mutex> capture_lock(g_capture_mutex);
				if (g_capture != nullptr && g_capture->Device() != nullptr && g_capture->Context() != nullptr)
				{
					return true;
				}
			}
			if (std::chrono::steady_clock::now() >= deadline)
			{
				return true;
			}
			std::this_thread::sleep_for(std::chrono::milliseconds(10));
		}

		if (g_capture_thread.joinable())
		{
			g_capture_thread.join();
		}
		return false;
	}

	UNITY_INTERFACE_EXPORT void SSPCAP_Stop()
	{
		std::lock_guard<std::mutex> lock(g_capture_thread_mutex);
		g_capture_running.store(false);
		if (g_capture_thread.joinable())
		{
			g_capture_thread.join();
		}
		ClearCaptureState();
	}

	UNITY_INTERFACE_EXPORT bool SSPCAP_IsRunning()
	{
		return g_capture_running.load();
	}

	UNITY_INTERFACE_EXPORT void SSPCAP_SetWindowHandle(std::uint64_t window_handle)
	{
		g_capture_pending_window_handle.store(window_handle);
	}

	UNITY_INTERFACE_EXPORT void SSPCAP_SetPreviewTexture(void *native_texture_ptr)
	{
		std::lock_guard<std::mutex> lock(g_unity_mutex);
		g_unity_target_texture = static_cast<ID3D11Texture2D *>(native_texture_ptr);
	}

	UNITY_INTERFACE_EXPORT UnityRenderingEvent SSPCAP_GetRenderEventFunc()
	{
		return OnHostRenderEvent;
	}

	UNITY_INTERFACE_EXPORT int SSPCAP_GetRenderEventId()
	{
		return kCaptureRenderEventId;
	}

	UNITY_INTERFACE_EXPORT void SSPCAP_GetStats(SSPCAP_Stats *out_stats)
	{
		if (out_stats == nullptr)
		{
			return;
		}
		std::lock_guard<std::mutex> lock(g_stats_mutex);
		out_stats->capture_frames = g_stats_capture_frames;
		out_stats->last_capture_ms = g_stats_last_capture_ms;
		out_stats->preview_width = g_preview_width.load();
		out_stats->preview_height = g_preview_height.load();
		out_stats->audio_capture_state = g_stats_audio_capture_state.load();
		out_stats->audio_target_pid = g_stats_audio_target_pid.load();
		out_stats->local_audio_samples_read = g_stats_local_audio_samples_read.load();
	}

	UNITY_INTERFACE_EXPORT int SSPCAP_ReadLocalAudio(float *out_samples, int max_samples)
	{
		if (out_samples == nullptr || max_samples <= 0 || g_host_local_audio_ring == nullptr)
		{
			return 0;
		}
		constexpr std::size_t kMaxLocalBufferedSamples =
			static_cast<std::size_t>((streamproto::audio::kSampleRate * streamproto::audio::kChannels * 60) / 1000);
		const std::size_t read = g_host_local_audio_ring->ReadLatest(
			out_samples,
			static_cast<std::size_t>(max_samples),
			kMaxLocalBufferedSamples);
		if (read > 0)
		{
			g_stats_local_audio_samples_read.fetch_add(read);
		}
		return static_cast<int>(std::min<std::size_t>(read, static_cast<std::size_t>(std::numeric_limits<int>::max())));
	}

	UNITY_INTERFACE_EXPORT void SSPCAP_GetLocalAudioFormat(int *out_sample_rate, int *out_channels)
	{
		if (out_sample_rate != nullptr)
		{
			*out_sample_rate = streamproto::audio::kSampleRate;
		}
		if (out_channels != nullptr)
		{
			*out_channels = streamproto::audio::kChannels;
		}
	}

	UNITY_INTERFACE_EXPORT void SSPCAP_SetLogCallback(HostLogCallback callback)
	{
		SetHostLogCallback(callback);
	}

	UNITY_INTERFACE_EXPORT void SSPCAP_SetLoggingEnabled(bool enabled)
	{
		SetHostLoggingEnabled(enabled);
	}

	UNITY_INTERFACE_EXPORT const char *SSPCAP_GetLastAudioError()
	{
		std::lock_guard<std::mutex> lock(g_audio_error_mutex);
		return g_last_audio_error.c_str();
	}

	UNITY_INTERFACE_EXPORT const char *SSPCAP_GetLastError()
	{
		std::lock_guard<std::mutex> lock(g_error_mutex);
		return g_last_error.c_str();
	}

	UNITY_INTERFACE_EXPORT bool VDP_IsRunning()
	{
		return SSPCAP_IsRunning();
	}

	UNITY_INTERFACE_EXPORT void VDP_SetWindowHandle(std::uint64_t window_handle)
	{
		SSPCAP_SetWindowHandle(window_handle);
	}

	UNITY_INTERFACE_EXPORT bool VDP_AcquireCaptureDevice(void **out_device, void **out_context)
	{
		if (out_device == nullptr || out_context == nullptr)
		{
			return false;
		}
		*out_device = nullptr;
		*out_context = nullptr;

		std::lock_guard<std::mutex> lock(g_capture_mutex);
		if (g_capture == nullptr || g_capture->Device() == nullptr || g_capture->Context() == nullptr)
		{
			return false;
		}

		ID3D11Device *device = g_capture->Device();
		ID3D11DeviceContext *context = g_capture->Context();
		device->AddRef();
		context->AddRef();
		*out_device = device;
		*out_context = context;
		return true;
	}

	UNITY_INTERFACE_EXPORT bool VDP_AcquireCaptureGpuFrame(RawVideoFrame *out_frame)
	{
		if (out_frame == nullptr)
		{
			return false;
		}
		*out_frame = {};

		std::lock_guard<std::mutex> lock(g_capture_mutex);
		if (g_capture == nullptr)
		{
			return false;
		}

		ID3D11Texture2D *texture = g_capture_preview_mirror.Texture();
		const int width = g_capture_preview_mirror.Width();
		const int height = g_capture_preview_mirror.Height();
		if (texture == nullptr || width <= 0 || height <= 0)
		{
			return false;
		}

		texture->AddRef();
		out_frame->texture = texture;
		out_frame->width = width;
		out_frame->height = height;
		out_frame->capture_timestamp_us = streamproto::NowUnixMicros();
		return true;
	}

	UNITY_INTERFACE_EXPORT void VDP_ReleaseCaptureGpuFrame(RawVideoFrame *frame)
	{
		if (frame == nullptr)
		{
			return;
		}
		if (frame->texture != nullptr)
		{
			static_cast<ID3D11Texture2D *>(frame->texture)->Release();
		}
		*frame = {};
	}

	UNITY_INTERFACE_EXPORT bool VDP_PopNetworkAudioFrame(streamproto::audio::AudioFrame *out_frame)
	{
		return out_frame != nullptr && g_capture_audio_queue.Pop(*out_frame);
	}

	UNITY_INTERFACE_EXPORT bool VDP_PopNetworkAudioFrameData(
		float *out_samples,
		std::int32_t max_samples,
		std::uint32_t *out_sample_frames,
		std::uint32_t *out_sequence,
		std::uint64_t *out_capture_timestamp_us)
	{
		if (out_samples == nullptr ||
			max_samples <= 0 ||
			out_sample_frames == nullptr ||
			out_sequence == nullptr ||
			out_capture_timestamp_us == nullptr)
		{
			return false;
		}

		streamproto::audio::AudioFrame frame{};
		if (!g_capture_audio_queue.Pop(frame))
		{
			return false;
		}

		const std::size_t sample_count =
			static_cast<std::size_t>(frame.sample_frames) * streamproto::audio::kChannels;
		if (frame.sample_frames == 0 ||
			sample_count == 0 ||
			sample_count > frame.samples.size() ||
			sample_count > static_cast<std::size_t>(max_samples))
		{
			return false;
		}

		std::memcpy(out_samples, frame.samples.data(), sample_count * sizeof(float));
		*out_sample_frames = frame.sample_frames;
		*out_sequence = frame.sequence;
		*out_capture_timestamp_us = frame.capture_timestamp_us;
		return true;
	}

	UNITY_INTERFACE_EXPORT void VDP_ClearNetworkAudio()
	{
		g_capture_audio_queue.Clear();
	}

	UNITY_INTERFACE_EXPORT void VDP_GetCaptureStats(SSPCAP_Stats *out_stats)
	{
		SSPCAP_GetStats(out_stats);
	}

	UNITY_INTERFACE_EXPORT int VDP_ReadLocalAudio(float *out_samples, int max_samples)
	{
		return SSPCAP_ReadLocalAudio(out_samples, max_samples);
	}

	UNITY_INTERFACE_EXPORT void VDP_GetLocalAudioFormat(int *out_sample_rate, int *out_channels)
	{
		SSPCAP_GetLocalAudioFormat(out_sample_rate, out_channels);
	}

	UNITY_INTERFACE_EXPORT const char *VDP_GetLastAudioError()
	{
		return SSPCAP_GetLastAudioError();
	}

} // extern "C"
