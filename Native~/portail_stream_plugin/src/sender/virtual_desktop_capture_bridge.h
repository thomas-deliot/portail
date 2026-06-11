#pragma once

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <iterator>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>

#include <windows.h>
#include <d3d11.h>
#include <d3d11_4.h>
#include <wrl/client.h>

#include "common/audio_stream.h"

namespace streamproto::sender
{

	struct RawVideoFrame
	{
		void *texture = nullptr;
		std::int32_t width = 0;
		std::int32_t height = 0;
		std::uint64_t capture_timestamp_us = 0;
	};

	struct VirtualDesktopCaptureStats
	{
		std::uint64_t capture_frames = 0;
		double last_capture_ms = 0.0;
		std::int32_t preview_width = 0;
		std::int32_t preview_height = 0;
		std::int32_t audio_capture_state = 0;
		std::int32_t audio_target_pid = 0;
		std::uint64_t local_audio_samples_read = 0;
	};

	class VirtualDesktopCaptureBridge
	{
	public:
		bool Ensure(std::string *out_error = nullptr)
		{
			std::lock_guard<std::mutex> lock(mutex_);
			if (module_ != nullptr)
			{
				return true;
			}

			std::wstring module_dir = ModuleDirectoryFromAddress(reinterpret_cast<const void *>(&ModuleDirectoryFromAddress));
			std::filesystem::path plugin_path = module_dir.empty()
													? std::filesystem::path(L"portail_capture_plugin.dll")
													: std::filesystem::path(module_dir) / L"portail_capture_plugin.dll";

			HMODULE module = LoadLibraryW(plugin_path.c_str());
			if (module == nullptr)
			{
				module = LoadLibraryW(L"portail_capture_plugin.dll");
			}
			if (module == nullptr)
			{
				std::string error = "Portail Stream sender requires portail_capture_plugin.dll to be loaded beside the Portail Stream native plugins: " + Win32ErrorString(GetLastError());
				if (out_error != nullptr)
				{
					*out_error = error;
				}
				return false;
			}

			Api api{};
			std::string error;
			if (!ResolveProc(module, "VDP_IsRunning", api.is_running, error) ||
				!ResolveProc(module, "VDP_AcquireCaptureDevice", api.acquire_device, error) ||
				!ResolveProc(module, "VDP_AcquireCaptureGpuFrame", api.acquire_gpu_frame, error) ||
				!ResolveProc(module, "VDP_ReleaseCaptureGpuFrame", api.release_gpu_frame, error) ||
				!ResolveProc(module, "VDP_PopNetworkAudioFrameData", api.pop_network_audio_frame_data, error) ||
				!ResolveProc(module, "VDP_ClearNetworkAudio", api.clear_network_audio, error) ||
				!ResolveProc(module, "VDP_GetCaptureStats", api.get_stats, error))
			{
				FreeLibrary(module);
				if (out_error != nullptr)
				{
					*out_error = error;
				}
				return false;
			}

			api_ = api;
			module_ = module;
			return true;
		}

		bool IsRunning(std::string *out_error = nullptr)
		{
			if (!Ensure(out_error))
			{
				return false;
			}
			return api_.is_running != nullptr && api_.is_running();
		}

		bool GetStats(VirtualDesktopCaptureStats &out_stats)
		{
			out_stats = {};
			if (!Ensure() || api_.get_stats == nullptr)
			{
				return false;
			}
			api_.get_stats(&out_stats);
			return true;
		}

		void ClearNetworkAudio()
		{
			if (Ensure() && api_.clear_network_audio != nullptr)
			{
				api_.clear_network_audio();
			}
		}

		bool PopNetworkAudioFrame(streamproto::audio::AudioFrame &out_frame)
		{
			if (!Ensure() || api_.pop_network_audio_frame_data == nullptr)
			{
				return false;
			}

			constexpr int kMaxNetworkAudioSampleFrames = 2880;
			constexpr int kMaxNetworkAudioSamples = kMaxNetworkAudioSampleFrames * streamproto::audio::kChannels;
			out_frame.sample_frames = 0;
			out_frame.sequence = 0;
			out_frame.capture_timestamp_us = 0;
			out_frame.samples.resize(kMaxNetworkAudioSamples);

			std::uint32_t sample_frames = 0;
			std::uint32_t sequence = 0;
			std::uint64_t capture_timestamp_us = 0;
			if (!api_.pop_network_audio_frame_data(
					out_frame.samples.data(),
					static_cast<std::int32_t>(out_frame.samples.size()),
					&sample_frames,
					&sequence,
					&capture_timestamp_us))
			{
				out_frame.samples.clear();
				return false;
			}

			const std::size_t sample_count = static_cast<std::size_t>(sample_frames) * streamproto::audio::kChannels;
			if (sample_frames == 0 || sample_count == 0 || sample_count > out_frame.samples.size())
			{
				out_frame.samples.clear();
				return false;
			}

			out_frame.samples.resize(sample_count);
			out_frame.sample_frames = sample_frames;
			out_frame.sequence = sequence;
			out_frame.capture_timestamp_us = capture_timestamp_us;
			return true;
		}

		bool AcquireDevice(ID3D11Device *&device, ID3D11DeviceContext *&context, std::string *out_error = nullptr)
		{
			device = nullptr;
			context = nullptr;
			if (!Ensure(out_error))
			{
				return false;
			}

			void *raw_device = nullptr;
			void *raw_context = nullptr;
			if (api_.acquire_device == nullptr ||
				!api_.acquire_device(&raw_device, &raw_context) ||
				raw_device == nullptr ||
				raw_context == nullptr)
			{
				return false;
			}

			device = static_cast<ID3D11Device *>(raw_device);
			context = static_cast<ID3D11DeviceContext *>(raw_context);
			Microsoft::WRL::ComPtr<ID3D11Multithread> multithread;
			if (context != nullptr && SUCCEEDED(context->QueryInterface(IID_PPV_ARGS(multithread.GetAddressOf()))) && multithread != nullptr)
			{
				multithread->SetMultithreadProtected(TRUE);
			}
			return device != nullptr && context != nullptr;
		}

		bool AcquireGpuFrame(RawVideoFrame &out_frame, std::string *out_error = nullptr)
		{
			out_frame = {};
			if (!Ensure(out_error) || api_.acquire_gpu_frame == nullptr)
			{
				return false;
			}

			RawVideoFrame frame{};
			if (!api_.acquire_gpu_frame(&frame) || frame.texture == nullptr || frame.width <= 0 || frame.height <= 0)
			{
				return false;
			}

			out_frame = frame;
			return true;
		}

		void ReleaseGpuFrame(RawVideoFrame &frame)
		{
			if (frame.texture != nullptr && Ensure() && api_.release_gpu_frame != nullptr)
			{
				api_.release_gpu_frame(&frame);
			}
			frame = {};
		}

	private:
		struct Api
		{
			bool(__cdecl *is_running)() = nullptr;
			bool(__cdecl *acquire_device)(void **out_device, void **out_context) = nullptr;
			bool(__cdecl *acquire_gpu_frame)(RawVideoFrame *out_frame) = nullptr;
			void(__cdecl *release_gpu_frame)(RawVideoFrame *frame) = nullptr;
			bool(__cdecl *pop_network_audio_frame_data)(
				float *out_samples,
				std::int32_t max_samples,
				std::uint32_t *out_sample_frames,
				std::uint32_t *out_sequence,
				std::uint64_t *out_capture_timestamp_us) = nullptr;
			void(__cdecl *clear_network_audio)() = nullptr;
			void(__cdecl *get_stats)(VirtualDesktopCaptureStats *out_stats) = nullptr;
		};

		static std::string Win32ErrorString(DWORD error)
		{
			std::ostringstream ss;
			ss << "Win32 error " << error;
			return ss.str();
		}

		static std::wstring ModuleDirectoryFromAddress(const void *address)
		{
			HMODULE module = nullptr;
			if (!GetModuleHandleExW(
					GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
					reinterpret_cast<LPCWSTR>(address),
					&module))
			{
				return std::wstring();
			}

			wchar_t buffer[MAX_PATH] = {};
			const DWORD length = GetModuleFileNameW(module, buffer, static_cast<DWORD>(std::size(buffer)));
			if (length == 0 || length >= std::size(buffer))
			{
				return std::wstring();
			}

			return std::filesystem::path(buffer).parent_path().wstring();
		}

		template <typename Fn>
		static bool ResolveProc(HMODULE module, const char *name, Fn &out_fn, std::string &error)
		{
			out_fn = reinterpret_cast<Fn>(GetProcAddress(module, name));
			if (out_fn == nullptr)
			{
				error = std::string("portail_capture_plugin.dll is missing export ") + name + ".";
				return false;
			}
			return true;
		}

		std::mutex mutex_;
		HMODULE module_ = nullptr;
		Api api_{};
	};

} // namespace streamproto::sender
