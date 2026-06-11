#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <deque>
#include <iostream>
#include <limits>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>
#include <cwctype>
#include <iterator>
#include <cstdio>
#include <limits>
#include <memory>

#include <windows.h>
#include <d3d11.h>
#include <d3d11_4.h>
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
#include "common/connection_path.h"
#include "common/d3d11_unity_utils.h"
#include "common/lod_utils.h"
#include "common/plugin_log.h"
#include "common/protocol.h"
#include "common/protocol_validation.h"
#include "common/steam_sdr.h"
#include "common/time_utils.h"
#include "common/unity_texture_copy.h"
#include "host/encoded_packet.h"
#include "host/ffmpeg_encoder.h"
#include "host/host_lod_config.h"
#include "host/host_media_sender.h"
#include "host/host_peer_sender.h"
#include "host/host_options.h"
#include "host/host_route_state.h"
#include "host/host_runtime_state.h"
#include "host/host_stats.h"
#include "host/sdr_queue_status.h"
#include "host/shared_texture_mirror.h"
#include "host/virtual_desktop_capture_bridge.h"
#include "plugin/host_plugin_api.h"
#include "IUnityGraphics.h"
#include "IUnityGraphicsD3D11.h"
#include "IUnityInterface.h"

namespace
{

	using Microsoft::WRL::ComPtr;
	using streamproto::SdrConfig;
	using streamproto::SdrLane;
	using streamproto::SdrRole;
	using streamproto::SteamSdr;
	using streamproto::ConnectionPath;
	using streamproto::ConnectionPathName;
	using streamproto::GetConnectionPath;
	using streamproto::host::EncodedPacket;
	using streamproto::host::DefaultAudioLodConfigs;
	using streamproto::host::DefaultVideoLodConfigs;
	using streamproto::host::FfmpegEncoder;
	using streamproto::host::HasKeyframePacket;
	using streamproto::host::HostAudioLodConfigState;
	using streamproto::host::HostAudioLodExportState;
	using streamproto::host::HostMediaSendResult;
	using streamproto::host::HostMediaTarget;
	using streamproto::host::HostPeerStreamState;
	using streamproto::host::HostPeerExportState;
	using streamproto::host::HostPeerLodAssignment;
	using streamproto::host::HostPeerSendDelta;
	using streamproto::host::HostPeerSender;
	using streamproto::host::HostPeerSenderMap;
	using streamproto::host::HostRouteSnapshot;
	using streamproto::host::HostOptions;
	using streamproto::host::BuildHostStatsLine;
	using streamproto::host::HostStats;
	using streamproto::host::HostVideoLodConfigState;
	using streamproto::host::HostVideoLodExportState;
	using streamproto::host::VideoLodRuntime;
	using streamproto::host::AudioLodRuntime;
	using streamproto::host::VideoLodCanProduce;
	using streamproto::host::VideoLodCanWarmEncoder;
	using streamproto::host::AudioLodCanProduce;
	using streamproto::host::BeginVideoGenerationTransition;
	using streamproto::host::BeginAudioGenerationTransition;
	using streamproto::host::HasPendingVideoGeneration;
	using streamproto::host::HasPendingAudioGeneration;
	using streamproto::host::kMaxConfiguredLods;
	using streamproto::host::NormalizeVideoRateControl;
	using streamproto::host::RawVideoFrame;
	using streamproto::host::SanitizeAudioLodConfig;
	using streamproto::host::SanitizeVideoLodConfig;
	using streamproto::host::SendAudioFrame;
	using streamproto::host::SendEncodedPacket;
	using streamproto::host::SetFfmpegEncoderLogCallbacks;
	using streamproto::host::SharedRawVideoFrame;
	using streamproto::host::HostVideoFrameHub;
	using streamproto::host::HostAudioFrameHub;
	using streamproto::host::SharedTextureMirror;
	using streamproto::host::SdrQueueStatus;
	using streamproto::host::ApplyConnectionSendRateLimit;
	using streamproto::host::ComputeSendRateBytesPerSecForBitrate;
	using streamproto::host::ComputeUnreliableChunkPayloadLimit;
	using streamproto::host::GetSdrQueueStatusForSteamId;
	using streamproto::host::ShouldRealtimeDrop;
	using streamproto::host::VirtualDesktopCaptureBridge;
	using streamproto::host::VirtualDesktopCaptureStats;
	using streamproto::host::VideoRateControl;
	namespace proto = streamproto::proto;

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

	std::atomic<bool> g_running{true};
	constexpr int kHostRenderEventId = 0x1001;

	std::atomic<bool> g_pause_stream{false};
	std::mutex g_target_clients_mutex;
	std::vector<std::uint64_t> g_target_clients;
	std::atomic<bool> g_target_clients_dirty{false};
	std::atomic<std::uint64_t> g_preview_shared_handle{0};
	std::atomic<int> g_preview_width{0};
	std::atomic<int> g_preview_height{0};

	constexpr std::int32_t kStreamLodOff = streamproto::lod::kStreamLodOff;
	constexpr std::int32_t kInvalidSentLod = streamproto::lod::kInvalidSentLod;
	constexpr std::uint64_t kStreamSwitchGraceMs = 350;
	constexpr std::uint64_t kStreamQualityResendMs = 100;

	std::mutex g_assigned_peer_lods_mutex;
	std::unordered_map<std::uint64_t, HostPeerLodAssignment> g_assigned_peer_lods;
	std::atomic<bool> g_assigned_peer_lods_dirty{false};
	std::mutex g_lod_config_mutex;
	std::vector<HostVideoLodConfigState> g_video_lod_configs;
	std::vector<HostAudioLodConfigState> g_audio_lod_configs;
	std::atomic<bool> g_lod_enabled_dirty{false};
	std::mutex g_stats_mutex;
	std::uint64_t g_stats_capture_frames = 0;
	std::uint64_t g_stats_encoded_frames = 0;
	std::uint64_t g_stats_encoded_video_bytes = 0;
	std::uint64_t g_stats_sent_chunks = 0;
	std::uint64_t g_stats_sent_bytes = 0;
	std::uint64_t g_stats_sent_video_bytes = 0;
	std::uint64_t g_stats_sent_audio_frames = 0;
	std::uint64_t g_stats_sent_audio_bytes = 0;
	std::uint64_t g_stats_encoded_audio_frames = 0;
	std::uint64_t g_stats_encoded_audio_bytes = 0;
	std::atomic<int> g_stats_encoded_width{0};
	std::atomic<int> g_stats_encoded_height{0};
	std::atomic<int> g_stats_encoded_fps{0};
	double g_stats_last_capture_ms = 0.0;
	double g_stats_last_encode_ms = 0.0;
	double g_stats_last_send_ms = 0.0;
	double g_stats_last_video_encode_ms = 0.0;
	double g_stats_last_video_send_ms = 0.0;
	double g_stats_last_audio_encode_ms = 0.0;
	double g_stats_last_audio_send_ms = 0.0;
	std::mutex g_host_peer_exports_mutex;
	std::vector<HostPeerExportState> g_host_peer_exports;
	std::mutex g_host_lod_exports_mutex;
	std::vector<HostVideoLodExportState> g_host_video_lod_exports;
	std::vector<HostAudioLodExportState> g_host_audio_lod_exports;
	std::atomic<bool> g_stats_connected{false};
	std::atomic<int> g_stats_connected_ice_clients{0};
	std::atomic<int> g_stats_connected_sdr_clients{0};
	std::atomic<std::uint64_t> g_stats_local_steam_id{0};
	std::atomic<int> g_stats_audio_capture_state{0};
	std::atomic<int> g_stats_audio_target_pid{0};
	std::atomic<std::uint64_t> g_stats_local_audio_samples_read{0};
	std::mutex g_error_mutex;
	std::string g_last_error;
	using HostLogCallback = streamproto::log::Callback;
	std::mutex g_log_callback_mutex;
	HostLogCallback g_log_callback = nullptr;
	std::atomic<bool> g_host_logging_enabled{true};

	VirtualDesktopCaptureBridge g_virtual_desktop_capture;

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

	std::vector<std::uint64_t> CopyTargetClients()
	{
		std::lock_guard<std::mutex> lock(g_target_clients_mutex);
		return g_target_clients;
	}

	std::vector<HostVideoLodConfigState> CopyVideoLodConfigs()
	{
		std::lock_guard<std::mutex> lock(g_lod_config_mutex);
		if (g_video_lod_configs.empty())
		{
			return DefaultVideoLodConfigs();
		}
		return g_video_lod_configs;
	}

	std::vector<HostAudioLodConfigState> CopyAudioLodConfigs()
	{
		std::lock_guard<std::mutex> lock(g_lod_config_mutex);
		if (g_audio_lod_configs.empty())
		{
			return DefaultAudioLodConfigs();
		}
		return g_audio_lod_configs;
	}

	inline constexpr auto NormalizeLod = streamproto::lod::Normalize;
	inline constexpr auto LodEnabled = streamproto::lod::Enabled;

	inline constexpr auto LodBit = streamproto::lod::Bit;
	inline constexpr auto BestAvailableLod = streamproto::lod::BestAvailable;

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

	streamproto::log::Stream LogInfoStream()
	{
		return streamproto::log::Stream(std::cout, streamproto::log::Level::kInfo, GetHostLogCallback(), g_host_logging_enabled.load());
	}

	streamproto::log::Stream LogWarningStream()
	{
		return streamproto::log::Stream(std::cerr, streamproto::log::Level::kWarning, GetHostLogCallback(), g_host_logging_enabled.load());
	}

	streamproto::log::Stream LogErrorStream()
	{
		return streamproto::log::Stream(std::cerr, streamproto::log::Level::kError, GetHostLogCallback(), g_host_logging_enabled.load());
	}

	void LogHostEncoderInfo(const std::string &message)
	{
		LogInfoStream() << message;
	}

	void LogHostEncoderWarning(const std::string &message)
	{
		LogWarningStream() << message;
	}

	bool EnsureVirtualDesktopCaptureBridge(std::string *out_error = nullptr)
	{
		return g_virtual_desktop_capture.Ensure(out_error);
	}

	bool IsVirtualDesktopCaptureRunning()
	{
		std::string error;
		if (!g_virtual_desktop_capture.IsRunning(&error))
		{
			if (!error.empty())
			{
				SetLastErrorString(error);
			}
			return false;
		}
		return true;
	}

	void PullVirtualDesktopCaptureStats()
	{
		VirtualDesktopCaptureStats capture_stats{};
		if (!g_virtual_desktop_capture.GetStats(capture_stats))
		{
			return;
		}
		{
			std::lock_guard<std::mutex> lock(g_stats_mutex);
			g_stats_last_capture_ms = capture_stats.last_capture_ms;
		}
		g_preview_width.store(capture_stats.preview_width);
		g_preview_height.store(capture_stats.preview_height);
		g_stats_audio_capture_state.store(capture_stats.audio_capture_state);
		g_stats_audio_target_pid.store(capture_stats.audio_target_pid);
		g_stats_local_audio_samples_read.store(capture_stats.local_audio_samples_read);
	}

	void ClearVirtualDesktopNetworkAudio()
	{
		g_virtual_desktop_capture.ClearNetworkAudio();
	}

	bool PopVirtualDesktopNetworkAudioFrame(streamproto::audio::AudioFrame &out_frame)
	{
		return g_virtual_desktop_capture.PopNetworkAudioFrame(out_frame);
	}
	int ReadIntEnvironment(const char *name, int fallback, int min_value, int max_value)
	{
		if (name == nullptr)
		{
			return fallback;
		}
		const char *value = std::getenv(name);
		if (value == nullptr || value[0] == '\0')
		{
			return fallback;
		}
		char *end = nullptr;
		const long parsed = std::strtol(value, &end, 10);
		if (end == value)
		{
			return fallback;
		}
		return std::clamp(static_cast<int>(parsed), min_value, max_value);
	}

	void ApplyMediaSendResult(
		const HostMediaSendResult &result,
		HostStats &stats,
		std::unordered_map<std::uint64_t, HostPeerStreamState> &stream_states,
		double send_ms,
		bool audio_lane)
	{
		stats.encoded_video_bytes += result.encoded_video_bytes;
		stats.encoded_audio_frames += result.encoded_audio_frames;
		stats.encoded_audio_bytes += result.encoded_audio_bytes;
		stats.sent_chunks += result.sent_chunks;
		stats.sent_bytes += result.sent_bytes;
		stats.sent_video_bytes += result.sent_video_bytes;
		stats.sent_audio_frames += result.sent_audio_frames;
		stats.sent_audio_bytes += result.sent_audio_bytes;
		if (result.queue_drop)
		{
			stats.queue_drop++;
		}
		for (const auto &[steam_id, delta] : result.peer_deltas)
		{
			HostPeerStreamState &state = stream_states[steam_id];
			state.sent_video_chunks += delta.sent_video_chunks;
			state.sent_video_bytes += delta.sent_video_bytes;
			state.sent_audio_frames += delta.sent_audio_frames;
			state.sent_audio_bytes += delta.sent_audio_bytes;
			if (audio_lane && delta.sent_audio_frames > 0)
			{
				state.last_audio_send_ms = send_ms;
			}
			if (!audio_lane && delta.sent_video_chunks > 0)
			{
				state.last_video_send_ms = send_ms;
			}
		}
	}

	HostPeerSenderMap SnapshotPeerSenders(const HostPeerSenderMap &peer_senders)
	{
		return peer_senders;
	}

	bool EnqueueReliableControl(
		HostPeerSenderMap &peer_senders,
		std::uint64_t target_steam_id,
		const void *data,
		std::size_t bytes)
	{
		if (data == nullptr || bytes == 0)
		{
			return false;
		}

		auto enqueue_for = [&](std::uint64_t steam_id) -> bool
		{
			auto sender_it = peer_senders.find(steam_id);
			if (sender_it == peer_senders.end() || sender_it->second == nullptr)
			{
				return false;
			}
			const auto *begin = static_cast<const std::uint8_t *>(data);
			std::vector<std::uint8_t> packet(begin, begin + bytes);
			return sender_it->second->Enqueue(std::move(packet), true, SdrLane::kControl).queued;
		};

		if (target_steam_id != 0)
		{
			return enqueue_for(target_steam_id);
		}

		bool queued_any = false;
		for (auto &[steam_id, sender] : peer_senders)
		{
			(void)sender;
			queued_any = enqueue_for(steam_id) || queued_any;
		}
		return queued_any;
	}

	HostOptions MakeVideoLodOptions(const HostOptions &base, const HostVideoLodConfigState &config)
	{
		HostOptions options = base;
		options.width = config.width;
		options.height = config.height;
		options.fps = config.fps;
		options.ice_bitrate_kbps = config.target_bitrate_kbps;
		options.sdr_bitrate_kbps = config.target_bitrate_kbps;
		options.video_rate_control = NormalizeVideoRateControl(config.video_rate_control);
		return options;
	}

	std::uint64_t BuildVideoAvailabilityMask(
		const std::vector<std::unique_ptr<VideoLodRuntime>> &video_lods,
		bool source_available,
		std::uint64_t now_ms)
	{
		if (!source_available)
		{
			return 0;
		}
		std::uint64_t mask = 0;
		for (std::size_t i = 0; i < video_lods.size() && i < 63; ++i)
		{
			const VideoLodRuntime &lod = *video_lods[i];
			if (lod.config.enabled && (!lod.encoder_failed || now_ms >= lod.next_encoder_retry_ms))
			{
				mask |= (1ULL << static_cast<unsigned>(i));
			}
		}
		return mask;
	}

	std::uint64_t BuildAudioAvailabilityMask(
		const std::vector<std::unique_ptr<AudioLodRuntime>> &audio_lods,
		bool source_available,
		std::uint64_t now_ms)
	{
		if (!source_available)
		{
			return 0;
		}
		std::uint64_t mask = 0;
		for (std::size_t i = 0; i < audio_lods.size() && i < 63; ++i)
		{
			const AudioLodRuntime &lod = *audio_lods[i];
			if (lod.config.enabled && (!lod.encoder_failed || now_ms >= lod.next_encoder_retry_ms))
			{
				mask |= (1ULL << static_cast<unsigned>(i));
			}
		}
		return mask;
	}

	std::int32_t ResolveEffectiveLod(std::int32_t assigned, std::int32_t max_accepted, std::uint64_t available_mask)
	{
		assigned = NormalizeLod(assigned);
		max_accepted = NormalizeLod(max_accepted);
		if (!LodEnabled(assigned) || !LodEnabled(max_accepted) || available_mask == 0)
		{
			return kStreamLodOff;
		}
		const std::int32_t first_allowed = std::max(assigned, max_accepted);
		for (std::int32_t i = first_allowed; i < 63; ++i)
		{
			if ((available_mask & (1ULL << static_cast<unsigned>(i))) != 0)
			{
				return i;
			}
		}
		return kStreamLodOff;
	}

	bool SendStreamQualityMessage(
		HostPeerSenderMap &peer_senders,
		std::uint64_t client_steam_id,
		proto::StreamQualityCommand command,
		const HostPeerStreamState &state,
		std::uint64_t available_video_lod_mask,
		std::uint64_t available_audio_lod_mask,
		std::uint64_t sender_steam_id)
	{
		proto::StreamQualityMessage msg{};
		proto::InitMessageHeader(msg, proto::MessageType::kStreamQuality);
		msg.command = static_cast<std::uint8_t>(command);
		msg.max_video_lod = state.max_video_lod;
		msg.max_audio_lod = state.max_audio_lod;
		msg.assigned_video_lod = state.assigned_lod.video_lod;
		msg.assigned_audio_lod = state.assigned_lod.audio_lod;
		msg.effective_video_lod = state.effective_video_lod;
		msg.effective_audio_lod = state.effective_audio_lod;
		msg.available_video_lod_mask = available_video_lod_mask;
		msg.available_audio_lod_mask = available_audio_lod_mask;
		msg.sender_steam_id = sender_steam_id;
		msg.sequence = state.target_video_generation;
		msg.profile_id = state.target_audio_generation;
		return EnqueueReliableControl(peer_senders, client_steam_id, &msg, sizeof(msg));
	}

	std::unordered_map<std::uint64_t, HostPeerLodAssignment> CopyAssignedPeerLods()
	{
		std::lock_guard<std::mutex> lock(g_assigned_peer_lods_mutex);
		return g_assigned_peer_lods;
	}

	void PublishHostPeerStats(
		const SteamSdr &sdr,
		const HostStats &stats,
		const std::unordered_map<std::uint64_t, HostPeerStreamState> &stream_states,
		std::uint64_t available_video_lod_mask,
		std::uint64_t available_audio_lod_mask)
	{
		std::vector<HostPeerExportState> exports;
		const std::vector<streamproto::HostPeerStatus> peers = sdr.HostPeerStatuses();
		exports.reserve(peers.size());
		for (const streamproto::HostPeerStatus &peer : peers)
		{
			if (peer.steam_id == 0)
			{
				continue;
			}

			HostPeerExportState export_state{};
			export_state.client_steam_id = peer.steam_id;
			export_state.encoded_frames = stats.encoded_frames;
			export_state.encoded_video_bytes = stats.encoded_video_bytes;
			export_state.sent_chunks = stats.sent_chunks;
			export_state.encoded_audio_frames = stats.encoded_audio_frames;
			export_state.encoded_audio_bytes = stats.encoded_audio_bytes;
			export_state.connected = 1;
			export_state.connection_path = peer.relayed ? 2 : 1;
			export_state.preview_width = g_stats_encoded_width.load();
			export_state.preview_height = g_stats_encoded_height.load();
			export_state.paused_by_client = peer.paused ? 1 : 0;
			auto state_it = stream_states.find(peer.steam_id);
			if (state_it != stream_states.end())
			{
				const HostPeerStreamState &state = state_it->second;
				export_state.assigned_video_lod = state.assigned_lod.video_lod;
				export_state.assigned_audio_lod = state.assigned_lod.audio_lod;
				export_state.max_video_lod = state.max_video_lod;
				export_state.max_audio_lod = state.max_audio_lod;
				export_state.effective_video_lod = state.effective_video_lod;
				export_state.effective_audio_lod = state.effective_audio_lod;
				export_state.video_lod_used = state.media_video_lod;
				export_state.audio_lod_used = state.media_audio_lod;
				export_state.sent_video_bytes = state.sent_video_bytes;
				export_state.sent_audio_bytes = state.sent_audio_bytes;
				export_state.sent_audio_frames = state.sent_audio_frames;
				export_state.sent_chunks = state.sent_video_chunks;
				export_state.sent_bytes = state.sent_video_bytes + state.sent_audio_bytes;
				export_state.last_video_send_ms = state.last_video_send_ms;
				export_state.last_audio_send_ms = state.last_audio_send_ms;
			}
			export_state.available_video_lod = BestAvailableLod(available_video_lod_mask);
			export_state.available_audio_lod = BestAvailableLod(available_audio_lod_mask);
			exports.push_back(export_state);
		}

		std::lock_guard<std::mutex> lock(g_host_peer_exports_mutex);
		g_host_peer_exports.swap(exports);
	}

	void PublishHostLodStats(
		const std::vector<std::unique_ptr<VideoLodRuntime>> &video_lods,
		const std::vector<std::unique_ptr<AudioLodRuntime>> &audio_lods)
	{
		std::vector<HostVideoLodExportState> video_exports;
		video_exports.reserve(video_lods.size());
		for (std::size_t i = 0; i < video_lods.size(); ++i)
		{
			const VideoLodRuntime &lod = *video_lods[i];
			HostVideoLodExportState export_state{};
			export_state.index = static_cast<std::int32_t>(i);
			export_state.enabled = lod.config.enabled ? 1 : 0;
			export_state.width = lod.config.width;
			export_state.height = lod.config.height;
			export_state.fps = lod.config.fps;
			export_state.target_bitrate_kbps = lod.config.target_bitrate_kbps;
			export_state.client_count = lod.client_count;
			export_state.encoded_frames = lod.encoded_frames;
			export_state.encoded_video_bytes = lod.encoded_video_bytes;
			export_state.last_encode_ms = lod.last_encode_ms;
			export_state.last_send_ms = lod.last_send_ms;
			video_exports.push_back(export_state);
		}

		std::vector<HostAudioLodExportState> audio_exports;
		audio_exports.reserve(audio_lods.size());
		for (std::size_t i = 0; i < audio_lods.size(); ++i)
		{
			const AudioLodRuntime &lod = *audio_lods[i];
			HostAudioLodExportState export_state{};
			export_state.index = static_cast<std::int32_t>(i);
			export_state.enabled = lod.config.enabled ? 1 : 0;
			export_state.bitrate_kbps = lod.config.bitrate_kbps;
			export_state.client_count = lod.client_count;
			export_state.encoded_audio_frames = lod.encoded_audio_frames;
			export_state.encoded_audio_bytes = lod.encoded_audio_bytes;
			export_state.last_encode_ms = lod.last_encode_ms;
			export_state.last_send_ms = lod.last_send_ms;
			audio_exports.push_back(export_state);
		}

		std::lock_guard<std::mutex> lock(g_host_lod_exports_mutex);
		g_host_video_lod_exports.swap(video_exports);
		g_host_audio_lod_exports.swap(audio_exports);
	}

	bool SendStreamConfig(
		HostPeerSenderMap &peer_senders,
		const HostOptions &options,
		const FfmpegEncoder &encoder,
		std::uint64_t target_steam_id = 0,
		int bitrate_kbps_override = -1,
		int chunk_payload_bytes_override = -1,
		int lod_index = -1,
		std::uint32_t stream_generation = 0)
	{
		proto::StreamConfigMessage msg{};
		proto::InitMessageHeader(msg, proto::MessageType::kStreamConfig);
		msg.width = static_cast<std::uint16_t>(options.width);
		msg.height = static_cast<std::uint16_t>(options.height);
		msg.fps = static_cast<std::uint16_t>(options.fps);
		msg.codec = encoder.ProtocolCodec();
		const int bitrate_kbps = bitrate_kbps_override > 0 ? bitrate_kbps_override : encoder.CurrentBitrateKbps();
		msg.bitrate_kbps = static_cast<std::uint32_t>(std::max(bitrate_kbps, 100));
		const int chunk_payload_bytes = chunk_payload_bytes_override > 0
											? chunk_payload_bytes_override
											: options.chunk_payload_bytes;
		msg.chunk_payload_bytes = static_cast<std::uint16_t>(std::clamp(chunk_payload_bytes, 512, 60000));
		msg.parity_shards = static_cast<std::uint8_t>(options.parity_shards);
		msg.reserved = static_cast<std::uint8_t>(lod_index >= 0 && lod_index < 255 ? lod_index + 1 : 0);
		msg.lod_index = lod_index;
		msg.stream_generation = stream_generation;
		return EnqueueReliableControl(peer_senders, target_steam_id, &msg, sizeof(msg));
	}

	bool SendAudioConfig(
		HostPeerSenderMap &peer_senders,
		const streamproto::audio::FfmpegOpusEncoder &encoder,
		std::uint64_t target_steam_id = 0,
		int lod_index = -1,
		std::uint32_t stream_generation = 0)
	{
		if (!encoder.Ready())
		{
			return false;
		}

		proto::AudioConfigMessage msg{};
		proto::InitMessageHeader(msg, proto::MessageType::kAudioConfig);
		msg.sample_rate = static_cast<std::uint16_t>(streamproto::audio::kSampleRate);
		msg.channels = static_cast<std::uint16_t>(streamproto::audio::kChannels);
		msg.samples_per_frame = static_cast<std::uint16_t>(encoder.FrameSamples());
		msg.codec = static_cast<std::uint16_t>(proto::AudioCodec::kOpus);
		msg.bitrate_kbps = static_cast<std::uint32_t>(encoder.BitrateKbps());
		msg.reserved = 0;
		msg.lod_index = lod_index;
		msg.stream_generation = stream_generation;
		return EnqueueReliableControl(peer_senders, target_steam_id, &msg, sizeof(msg));
	}

	bool SendHello(HostPeerSenderMap &peer_senders, std::uint32_t app_id, std::uint64_t steam_id, std::uint64_t target_steam_id = 0)
	{
		proto::HelloMessage msg{};
		proto::InitMessageHeader(msg, proto::MessageType::kHello);
		msg.app_id = app_id;
		msg.sender_steam_id = steam_id;
		return EnqueueReliableControl(peer_senders, target_steam_id, &msg, sizeof(msg));
	}

	bool SendPong(HostPeerSenderMap &peer_senders, std::uint64_t target_steam_id, const proto::PingMessage &ping, std::uint64_t host_recv_ts_us)
	{
		proto::PongMessage msg{};
		proto::InitMessageHeader(msg, proto::MessageType::kPong);
		msg.sequence = ping.sequence;
		msg.reserved = 0;
		msg.client_send_timestamp_us = ping.client_send_timestamp_us;
		msg.host_recv_timestamp_us = host_recv_ts_us;
		msg.host_send_timestamp_us = streamproto::NowUnixMicros();
		return EnqueueReliableControl(peer_senders, target_steam_id, &msg, sizeof(msg));
	}

	bool SendStreamControl(
		HostPeerSenderMap &peer_senders,
		proto::StreamControlCommand command,
		std::uint64_t sender_steam_id,
		std::uint64_t target_steam_id = 0)
	{
		proto::StreamControlMessage msg{};
		proto::InitMessageHeader(msg, proto::MessageType::kStreamControl);
		msg.command = static_cast<std::uint8_t>(command);
		msg.reserved[0] = 0;
		msg.reserved[1] = 0;
		msg.reserved[2] = 0;
		msg.sender_steam_id = sender_steam_id;
		return EnqueueReliableControl(peer_senders, target_steam_id, &msg, sizeof(msg));
	}

	constexpr std::uint64_t kHostStatsExportIntervalMs = 50;

	void PublishHostStatsSnapshot(const HostStats &stats, bool preserve_capture_timing)
	{
		std::lock_guard<std::mutex> lock(g_stats_mutex);
		g_stats_capture_frames = stats.capture_frames;
		g_stats_encoded_frames = stats.encoded_frames;
		g_stats_encoded_video_bytes = stats.encoded_video_bytes;
		g_stats_sent_chunks = stats.sent_chunks;
		g_stats_sent_bytes = stats.sent_bytes;
		g_stats_sent_video_bytes = stats.sent_video_bytes;
		g_stats_sent_audio_frames = stats.sent_audio_frames;
		g_stats_sent_audio_bytes = stats.sent_audio_bytes;
		g_stats_encoded_audio_frames = stats.encoded_audio_frames;
		g_stats_encoded_audio_bytes = stats.encoded_audio_bytes;
		if (!preserve_capture_timing)
		{
			g_stats_last_capture_ms = stats.last_capture_ms;
		}
		g_stats_last_encode_ms = stats.last_encode_ms;
		g_stats_last_send_ms = stats.last_send_ms;
		g_stats_last_video_encode_ms = stats.last_video_encode_ms;
		g_stats_last_video_send_ms = stats.last_video_send_ms;
		g_stats_last_audio_encode_ms = stats.last_audio_encode_ms;
		g_stats_last_audio_send_ms = stats.last_audio_send_ms;
	}

	void PrintStatsLine(
		const HostStats &stats,
		const std::string &encoder_name,
		const char *path_name,
		bool connected,
		std::uint64_t delta_ms,
		HostStats &last_snapshot)
	{
		LogInfoStream() << '\r' << BuildHostStatsLine(stats, encoder_name, path_name, connected, delta_ms, last_snapshot) << std::flush;

		last_snapshot = stats;
	}

	bool AcquireCaptureDevice(ID3D11Device *&device, ID3D11DeviceContext *&context)
	{
		std::string error;
		if (!g_virtual_desktop_capture.AcquireDevice(device, context, &error))
		{
			if (!error.empty())
			{
				SetLastErrorString(error);
			}
			return false;
		}
		return true;
	}

	bool AcquireCaptureGpuFrame(RawVideoFrame &out_frame)
	{
		std::string error;
		if (!g_virtual_desktop_capture.AcquireGpuFrame(out_frame, &error))
		{
			if (!error.empty())
			{
				SetLastErrorString(error);
			}
			return false;
		}
		return true;
	}

	void ReleaseCaptureGpuFrame(RawVideoFrame &frame)
	{
		g_virtual_desktop_capture.ReleaseGpuFrame(frame);
	}

	bool HasAnyTargets(const std::vector<std::vector<HostMediaTarget>> &targets_by_lod)
	{
		for (const auto &targets : targets_by_lod)
		{
			if (!targets.empty())
			{
				return true;
			}
		}
		return false;
	}

	int RunHostSenderWithOptions(HostOptions options)
	{
		SetFfmpegEncoderLogCallbacks(&LogHostEncoderInfo, &LogHostEncoderWarning);

		try
		{
			winrt::init_apartment(winrt::apartment_type::multi_threaded);
		}
		catch (const winrt::hresult_error &)
		{
			// COM apartment may already be initialized by host process.
		}

		std::string error;

		std::vector<HostVideoLodConfigState> configured_video_lods = CopyVideoLodConfigs();
		std::vector<HostAudioLodConfigState> configured_audio_lods = CopyAudioLodConfigs();
		if (configured_video_lods.empty())
		{
			configured_video_lods = DefaultVideoLodConfigs();
		}
		if (configured_audio_lods.empty())
		{
			configured_audio_lods = DefaultAudioLodConfigs();
		}

		std::vector<std::unique_ptr<VideoLodRuntime>> video_lods;
		video_lods.reserve(configured_video_lods.size());
		for (const HostVideoLodConfigState &config : configured_video_lods)
		{
			auto runtime = std::make_unique<VideoLodRuntime>();
			runtime->config = SanitizeVideoLodConfig(config);
			runtime->options = MakeVideoLodOptions(options, runtime->config);
			runtime->next_frame_time = std::chrono::steady_clock::now();
			video_lods.emplace_back(std::move(runtime));
		}

		std::vector<std::unique_ptr<AudioLodRuntime>> audio_lods;
		audio_lods.reserve(configured_audio_lods.size());
		for (const HostAudioLodConfigState &config : configured_audio_lods)
		{
			auto runtime = std::make_unique<AudioLodRuntime>();
			runtime->config = SanitizeAudioLodConfig(config);
			audio_lods.emplace_back(std::move(runtime));
		}

		SdrConfig sdr_config{};
		sdr_config.app_id = options.app_id;
		sdr_config.role = SdrRole::kHost;
		sdr_config.remote_steam_id = 0;
		sdr_config.virtual_port = streamproto::kAutoStreamingVirtualPort;
		int aggregate_configured_bitrate_kbps = 0;
		for (const auto &lod : video_lods)
		{
			if (lod != nullptr && lod->config.enabled)
				aggregate_configured_bitrate_kbps += std::max(100, lod->config.target_bitrate_kbps);
		}
		for (const auto &lod : audio_lods)
		{
			if (lod != nullptr && lod->config.enabled)
				aggregate_configured_bitrate_kbps += std::max(32, lod->config.bitrate_kbps);
		}
		const int configured_send_rate = ComputeSendRateBytesPerSecForBitrate(
			std::max(aggregate_configured_bitrate_kbps, std::max(options.ice_bitrate_kbps, options.sdr_bitrate_kbps)));
		sdr_config.send_rate_bytes_per_sec = configured_send_rate;
		sdr_config.send_buffer_size = std::max(8 * 1024 * 1024, configured_send_rate * 2);
		sdr_config.recv_buffer_size = 8 * 1024 * 1024;
		sdr_config.nagle_time_usec = 0;
		sdr_config.p2p_ice_enable = options.disable_ice
										? k_nSteamNetworkingConfig_P2P_Transport_ICE_Enable_Disable
										: k_nSteamNetworkingConfig_P2P_Transport_ICE_Enable_All;
		sdr_config.p2p_ice_penalty = 0;
		sdr_config.p2p_sdr_penalty = 0;
		sdr_config.debug_output_level = k_ESteamNetworkingSocketsDebugOutputType_Msg;
		sdr_config.p2p_log_level = k_ESteamNetworkingSocketsDebugOutputType_Msg;
		SteamSdr sdr(sdr_config);
		if (!sdr.Initialize(error))
		{
			LogErrorStream() << "path=sdr Steam SDR init failed: " << error;
			SetLastErrorString("Steam SDR init failed: " + error);
			return 1;
		}

		if (options.disable_ice)
		{
			LogInfoStream() << "path=sdr P2P transport: SDR relay only (ICE disabled)";
		}
		else
		{
			LogInfoStream() << "path=sdr P2P transport: ICE-preferred with automatic SDR fallback";
		}
		LogInfoStream() << "path=sdr Configured video LODs=" << video_lods.size()
						<< " audio LODs=" << audio_lods.size()
						<< " aggregate=" << aggregate_configured_bitrate_kbps << "kbps";
		LogInfoStream() << "path=sdr Local Steam ID: " << sdr.LocalSteamId();
		g_stats_local_steam_id.store(sdr.LocalSteamId());
		LogInfoStream() << "path=sdr Waiting for client connection...";
		if (g_target_clients_dirty.exchange(false))
		{
			sdr.SetHostAllowedSteamIds(CopyTargetClients());
		}

		HostStats stats{};
		HostStats snapshot{};
		std::uint64_t last_stats_ms = streamproto::NowSteadyMillis();
		std::uint64_t last_stats_export_ms = last_stats_ms;
		std::atomic<std::uint64_t> last_queue_drop_ms{0};
		ConnectionPath active_path = ConnectionPath::kSdr;
		stats.current_bitrate_kbps = aggregate_configured_bitrate_kbps;
		std::uint64_t last_route_probe_ms = 0;
		constexpr int kDefaultUnreliableChunkPayloadLimit = 12000;
		int unreliable_chunk_payload_limit = kDefaultUnreliableChunkPayloadLimit;
		int effective_unreliable_chunk_payload = std::clamp(options.chunk_payload_bytes, 512, 60000);
		bool unreliable_chunk_tuning_logged = false;
		std::unordered_map<std::uint64_t, HostPeerStreamState> peer_stream_states;
		HostPeerSenderMap peer_senders;
		std::mutex runtime_mutex;
		std::mutex peer_senders_mutex;
		std::mutex route_mutex;
		std::condition_variable route_cv;
		HostRouteSnapshot route_snapshot{};
		route_snapshot.video_targets_by_lod.resize(video_lods.size());
		route_snapshot.audio_targets_by_lod.resize(audio_lods.size());
		route_snapshot.effective_unreliable_chunk_payload = effective_unreliable_chunk_payload;
		route_snapshot.unreliable_chunk_payload_limit = unreliable_chunk_payload_limit;
		route_snapshot.chunk_payload_bytes = options.chunk_payload_bytes;
		route_snapshot.parity_shards = options.parity_shards;
		route_snapshot.reliable_video = options.reliable_video;
		route_snapshot.reliable_keyframes = options.reliable_keyframes;
		route_snapshot.max_queue_ms = options.max_queue_ms;
		HostVideoFrameHub video_frame_hub{};
		HostAudioFrameHub audio_frame_hub{};
		audio_frame_hub.queues.resize(audio_lods.size());
		std::atomic<bool> media_workers_stop{false};
		std::vector<std::thread> video_lod_workers;
		std::vector<std::thread> audio_lod_workers;
		std::thread video_capture_worker;
		std::thread audio_capture_worker;
		std::unordered_map<std::uint64_t, HostPeerLodAssignment> assigned_peer_lods = CopyAssignedPeerLods();
		std::uint64_t available_video_lod_mask = 0;
		std::uint64_t available_audio_lod_mask = 0;
		auto ensure_video_encoder = [&](VideoLodRuntime &lod, std::size_t lod_index) -> bool
		{
			{
				std::lock_guard<std::mutex> lock(runtime_mutex);
				if (lod.encoder_ready)
				{
					return true;
				}
				const std::uint64_t now_ms = streamproto::NowSteadyMillis();
				if (!VideoLodCanWarmEncoder(lod, now_ms))
				{
					return false;
				}
				if (now_ms < lod.next_encoder_retry_ms)
				{
					return false;
				}
			}

			ID3D11Device *capture_device = nullptr;
			ID3D11DeviceContext *capture_context = nullptr;
			if (!AcquireCaptureDevice(capture_device, capture_context))
			{
				std::lock_guard<std::mutex> lock(runtime_mutex);
				lod.next_encoder_retry_ms = streamproto::NowSteadyMillis() + 1000;
				return false;
			}

			std::string encoder_error;
			const bool ok = lod.encoder.Init(lod.options, capture_device, capture_context, encoder_error);
			capture_context->Release();
			capture_device->Release();
			if (!ok)
			{
				{
					std::lock_guard<std::mutex> lock(runtime_mutex);
					lod.encoder_ready = false;
					lod.encoder_failed = true;
					lod.next_encoder_retry_ms = streamproto::NowSteadyMillis() + 3000;
				}
				LogErrorStream() << "path=sdr V_lod" << lod_index << " encoder init failed: " << encoder_error;
				SetLastErrorString("V_lod" + std::to_string(lod_index) + " encoder init failed: " + encoder_error);
				return false;
			}

			{
				std::lock_guard<std::mutex> lock(runtime_mutex);
				lod.encoder_ready = true;
				lod.encoder_failed = false;
				lod.force_keyframe = true;
			}
			LogInfoStream() << "path=sdr V_lod" << lod_index << " active encoder: " << lod.encoder.ActiveEncoder()
							<< " " << lod.config.width << "x" << lod.config.height
							<< "@" << lod.config.fps << " " << lod.config.target_bitrate_kbps << "kbps";
			LogInfoStream() << "path=sdr V_lod" << lod_index << " encode input path: " << lod.encoder.EncodeInputPath();
			return true;
		};
		auto ensure_audio_encoder = [&](AudioLodRuntime &lod, std::size_t lod_index) -> bool
		{
			{
				std::lock_guard<std::mutex> lock(runtime_mutex);
				if (lod.encoder.Ready())
				{
					return true;
				}
				const std::uint64_t now_ms = streamproto::NowSteadyMillis();
				if (!AudioLodCanProduce(lod, now_ms))
				{
					return false;
				}
				if (now_ms < lod.next_encoder_retry_ms)
				{
					return false;
				}
			}
			std::string audio_error;
			if (lod.encoder.Init(lod.config.bitrate_kbps, streamproto::audio::kFrameSamples, audio_error))
			{
				{
					std::lock_guard<std::mutex> lock(runtime_mutex);
					lod.encoder_failed = false;
				}
				LogInfoStream() << "path=sdr A_lod" << lod_index << " audio encoder: " << lod.encoder.ActiveEncoder()
								<< " stereo " << streamproto::audio::kSampleRate
								<< "Hz frame=" << lod.encoder.FrameSamples()
								<< " samples bitrate=" << lod.encoder.BitrateKbps() << "kbps";
				return true;
			}
			{
				std::lock_guard<std::mutex> lock(runtime_mutex);
				lod.encoder_failed = true;
				lod.next_encoder_retry_ms = streamproto::NowSteadyMillis() + 3000;
			}
			LogWarningStream() << "path=sdr A_lod" << lod_index << " Opus encoder unavailable: " << audio_error;
			return false;
		};
		auto apply_route_profile = [&](ConnectionPath path, bool force_config_send)
		{
			int aggregate_bitrate_kbps = 0;
			for (const auto &lod : video_lods)
			{
				if (lod != nullptr && lod->config.enabled)
					aggregate_bitrate_kbps += std::max(100, lod->config.target_bitrate_kbps);
			}
			for (const auto &lod : audio_lods)
			{
				if (lod != nullptr && lod->config.enabled)
					aggregate_bitrate_kbps += std::max(32, lod->config.bitrate_kbps);
			}
			const int route_bitrate_kbps = std::max(aggregate_bitrate_kbps, 100);
			const int route_send_rate = ComputeSendRateBytesPerSecForBitrate(route_bitrate_kbps);
			const bool send_rate_ok = ApplyConnectionSendRateLimit(sdr, route_send_rate);
			stats.current_bitrate_kbps = route_bitrate_kbps;
			if (force_config_send)
			{
				for (auto &lod : video_lods)
				{
					if (lod != nullptr)
						lod->force_keyframe = true;
				}
				for (auto &[client_id, peer_state] : peer_stream_states)
				{
					(void)client_id;
					peer_state.sent_video_config_generation_by_lod.clear();
					peer_state.sent_audio_config_generation_by_lod.clear();
				}
			}

			LogInfoStream()
				<< "path=" << ConnectionPathName(path)
				<< " [NET] send_rate=" << route_send_rate << "Bps"
				<< " bitrate=" << route_bitrate_kbps << "kbps"
				<< " apply_send_rate=" << (send_rate_ok ? "ok" : "failed")
				<< "\n";

		};

		auto copy_route_snapshot = [&]() -> HostRouteSnapshot
		{
			std::lock_guard<std::mutex> lock(route_mutex);
			return route_snapshot;
		};

		auto publish_route_snapshot = [&](HostRouteSnapshot snapshot)
		{
			{
				std::lock_guard<std::mutex> lock(route_mutex);
				route_snapshot = std::move(snapshot);
			}
			route_cv.notify_all();
		};

		auto clear_audio_hub_queues = [&]()
		{
			std::lock_guard<std::mutex> lock(audio_frame_hub.mutex);
			for (auto &queue : audio_frame_hub.queues)
			{
				queue.clear();
			}
		};

		auto video_capture_loop = [&]()
		{
			while (g_running.load() && !media_workers_stop.load(std::memory_order_acquire))
			{
				const HostRouteSnapshot route = copy_route_snapshot();
				const bool should_capture =
					route.connected &&
					!route.paused &&
					route.video_source_available &&
					HasAnyTargets(route.video_targets_by_lod);
				if (!should_capture)
				{
					{
						std::lock_guard<std::mutex> lock(video_frame_hub.mutex);
						video_frame_hub.latest.reset();
					}
					std::this_thread::sleep_for(std::chrono::milliseconds(2));
					continue;
				}

				RawVideoFrame raw_frame{};
				if (AcquireCaptureGpuFrame(raw_frame))
				{
					auto shared = std::make_shared<SharedRawVideoFrame>();
					shared->frame = raw_frame;
					shared->release = &ReleaseCaptureGpuFrame;
					{
						std::lock_guard<std::mutex> lock(video_frame_hub.mutex);
						video_frame_hub.latest = std::move(shared);
						++video_frame_hub.version;
					}
					video_frame_hub.cv.notify_all();
				}
				else
				{
					std::lock_guard<std::mutex> lock(runtime_mutex);
					stats.capture_fail++;
				}
				std::this_thread::sleep_for(std::chrono::milliseconds(1));
			}

			{
				std::lock_guard<std::mutex> lock(video_frame_hub.mutex);
				video_frame_hub.latest.reset();
				++video_frame_hub.version;
			}
			video_frame_hub.cv.notify_all();
		};

		auto video_lod_loop = [&](std::size_t lod_index)
		{
			std::uint64_t consumed_frame_version = 0;
			while (g_running.load() && !media_workers_stop.load(std::memory_order_acquire))
			{
				if (lod_index >= video_lods.size())
				{
					return;
				}

				VideoLodRuntime &lod = *video_lods[lod_index];
				HostRouteSnapshot route = copy_route_snapshot();
				bool can_produce = false;
				int fps = 60;
				{
					std::lock_guard<std::mutex> lock(runtime_mutex);
					const std::uint64_t now_ms = streamproto::NowSteadyMillis();
					can_produce = VideoLodCanProduce(lod, now_ms);
					fps = std::max(lod.config.fps, 1);
					if (!can_produce)
					{
						lod.client_count = 0;
					}
				}
				if (route.video_source_available)
				{
					(void)ensure_video_encoder(lod, lod_index);
				}
				if (!can_produce)
				{
					std::this_thread::sleep_for(std::chrono::milliseconds(10));
					continue;
				}

				const std::vector<HostMediaTarget> video_targets =
					lod_index < route.video_targets_by_lod.size()
						? route.video_targets_by_lod[lod_index]
						: std::vector<HostMediaTarget>{};
				const bool has_video_targets = !video_targets.empty();
				bool has_pending_config_targets = false;
				{
					std::lock_guard<std::mutex> lock(runtime_mutex);
					for (std::uint64_t target : route.connected_peer_ids)
					{
						auto state_it = peer_stream_states.find(target);
						if (state_it == peer_stream_states.end())
						{
							continue;
						}
						const HostPeerStreamState &state = state_it->second;
						if (state.effective_video_lod != static_cast<std::int32_t>(lod_index))
						{
							continue;
						}
						auto sent_it = state.sent_video_config_generation_by_lod.find(static_cast<std::int32_t>(lod_index));
						if (sent_it == state.sent_video_config_generation_by_lod.end() ||
							sent_it->second != state.target_video_generation)
						{
							has_pending_config_targets = true;
							break;
						}
					}
				}

				bool encoder_ready = false;
				{
					std::lock_guard<std::mutex> lock(runtime_mutex);
					encoder_ready = lod.encoder_ready;
				}

				const std::uint64_t lod_bit = lod_index < 63 ? (1ULL << static_cast<unsigned>(lod_index)) : 0;
				if (encoder_ready && lod_bit != 0 && route.video_source_available && has_pending_config_targets)
				{
					std::lock_guard<std::mutex> runtime_lock(runtime_mutex);
					std::lock_guard<std::mutex> sender_lock(peer_senders_mutex);
					for (std::uint64_t target : route.connected_peer_ids)
					{
						HostPeerStreamState &state = peer_stream_states[target];
						if (state.effective_video_lod != static_cast<std::int32_t>(lod_index))
						{
							continue;
						}
						std::uint32_t &sent_generation =
							state.sent_video_config_generation_by_lod[static_cast<std::int32_t>(lod_index)];
						if (sent_generation == state.target_video_generation)
						{
							continue;
						}
						if (SendStreamConfig(
								peer_senders,
								lod.options,
								lod.encoder,
								target,
								-1,
								route.effective_unreliable_chunk_payload,
								static_cast<int>(lod_index),
								state.target_video_generation))
						{
							sent_generation = state.target_video_generation;
						}
					}
				}

				const auto now = std::chrono::steady_clock::now();
				bool due = false;
				{
					std::lock_guard<std::mutex> lock(runtime_mutex);
					const auto frame_interval = std::chrono::microseconds(static_cast<long long>(1'000'000LL / std::max(fps, 1)));
					if (now >= lod.next_frame_time)
					{
						due = true;
						lod.next_frame_time += frame_interval;
						if (now > lod.next_frame_time + frame_interval * 2)
						{
							lod.next_frame_time = now + frame_interval;
						}
					}
				}
				if (!due)
				{
					std::this_thread::sleep_for(std::chrono::milliseconds(1));
					continue;
				}

				const bool wants_video =
					route.connected &&
					!route.paused &&
					route.video_source_available &&
					encoder_ready &&
					has_video_targets;
				if (!wants_video)
				{
					continue;
				}

				bool force_keyframe_for_filter = false;
				{
					std::lock_guard<std::mutex> lock(runtime_mutex);
					force_keyframe_for_filter = lod.force_keyframe;
				}
				std::vector<HostMediaTarget> sendable_video_targets;
				sendable_video_targets.reserve(video_targets.size());
				int peer_realtime_drops = 0;
				for (const HostMediaTarget &target : video_targets)
				{
					const auto queue_it = route.video_queue_status_by_peer.find(target.steam_id);
					if (!force_keyframe_for_filter &&
						queue_it != route.video_queue_status_by_peer.end() &&
						ShouldRealtimeDrop(queue_it->second, route.max_queue_ms))
					{
						++peer_realtime_drops;
						continue;
					}
					sendable_video_targets.push_back(target);
				}
				if (peer_realtime_drops > 0)
				{
					std::lock_guard<std::mutex> lock(runtime_mutex);
					stats.realtime_drop += static_cast<std::uint64_t>(peer_realtime_drops);
				}
				if (sendable_video_targets.empty())
				{
					if (!force_keyframe_for_filter)
					{
						std::this_thread::sleep_for(std::chrono::milliseconds(1));
					}
					continue;
				}

				std::shared_ptr<SharedRawVideoFrame> shared_frame;
				{
					std::unique_lock<std::mutex> lock(video_frame_hub.mutex);
					video_frame_hub.cv.wait_for(
						lock,
						std::chrono::milliseconds(8),
						[&]()
						{
							return media_workers_stop.load(std::memory_order_acquire) ||
								   (video_frame_hub.latest != nullptr && video_frame_hub.version != consumed_frame_version);
						});
					if (media_workers_stop.load(std::memory_order_acquire))
					{
						break;
					}
					if (video_frame_hub.latest == nullptr)
					{
						continue;
					}
					shared_frame = video_frame_hub.latest;
					consumed_frame_version = video_frame_hub.version;
				}

				std::vector<EncodedPacket> packets;
				double encode_ms = 0.0;
				bool requested_keyframe = false;
				{
					std::lock_guard<std::mutex> lock(runtime_mutex);
					requested_keyframe = lod.force_keyframe;
				}

				const bool ok = lod.encoder.EncodeFrameGpu(
					static_cast<ID3D11Texture2D *>(shared_frame->frame.texture),
					shared_frame->frame.width,
					shared_frame->frame.height,
					requested_keyframe,
					shared_frame->frame.capture_timestamp_us,
					packets,
					encode_ms);
				const bool produced_keyframe = HasKeyframePacket(packets);
				{
					std::lock_guard<std::mutex> lock(runtime_mutex);
					lod.force_keyframe = requested_keyframe && (!ok || !produced_keyframe);
					stats.last_encode_ms = ok ? encode_ms : 0.0;
					stats.last_video_encode_ms = ok ? encode_ms : 0.0;
					lod.last_encode_ms = ok ? encode_ms : 0.0;
					if (!ok)
					{
						stats.encode_fail++;
					}
					else
					{
						stats.capture_frames++;
						stats.encoded_frames++;
						lod.encoded_frames++;
					}
				}
				if (!ok)
				{
					continue;
				}

				const auto send_start = std::chrono::steady_clock::now();
				bool frame_had_queue_drop = false;
				bool frame_send_error = false;
				HostPeerSenderMap peer_sender_snapshot;
				{
					std::lock_guard<std::mutex> sender_lock(peer_senders_mutex);
					peer_sender_snapshot = SnapshotPeerSenders(peer_senders);
				}
				for (const EncodedPacket &packet : packets)
				{
					std::uint32_t frame_id = 0;
					{
						std::lock_guard<std::mutex> runtime_lock(runtime_mutex);
						frame_id = lod.frame_id++;
					}

					HostMediaSendResult send_result = SendEncodedPacket(
						peer_sender_snapshot,
						sendable_video_targets,
						frame_id,
						packet,
						route.chunk_payload_bytes,
						route.unreliable_chunk_payload_limit,
						route.parity_shards,
						route.reliable_video,
						route.reliable_keyframes,
						static_cast<int>(lod_index));

					{
						std::lock_guard<std::mutex> runtime_lock(runtime_mutex);
						ApplyMediaSendResult(send_result, stats, peer_stream_states, 0.0, false);
						lod.encoded_video_bytes += send_result.encoded_video_bytes;
						if (send_result.sent)
						{
							stats.encoded_packets++;
						}
					}
					frame_had_queue_drop = frame_had_queue_drop || send_result.queue_drop;
					if (!send_result.sent)
					{
						frame_send_error = true;
						break;
					}
				}

				const auto send_end = std::chrono::steady_clock::now();
				{
					std::lock_guard<std::mutex> lock(runtime_mutex);
					const double send_ms = std::chrono::duration<double, std::milli>(send_end - send_start).count();
					stats.last_send_ms = send_ms;
					stats.last_video_send_ms = send_ms;
					lod.last_send_ms = send_ms;
					if (!frame_send_error)
					{
						for (const HostMediaTarget &target : sendable_video_targets)
						{
							auto it = peer_stream_states.find(target.steam_id);
							if (it != peer_stream_states.end())
							{
								it->second.last_video_send_ms = send_ms;
							}
						}
					}
					if (frame_send_error)
					{
						stats.encode_fail++;
					}
					if (frame_had_queue_drop)
					{
						const std::uint64_t drop_ms = streamproto::NowSteadyMillis();
						last_queue_drop_ms.store(drop_ms, std::memory_order_release);
					}
				}
			}

			if (lod_index < video_lods.size() && video_lods[lod_index] != nullptr)
			{
				video_lods[lod_index]->encoder.Shutdown();
			}
		};

		auto audio_capture_loop = [&]()
		{
			while (g_running.load() && !media_workers_stop.load(std::memory_order_acquire))
			{
				const HostRouteSnapshot route = copy_route_snapshot();
				const bool should_capture =
					route.connected &&
					!route.paused &&
					route.audio_source_available &&
					HasAnyTargets(route.audio_targets_by_lod);
				if (!should_capture)
				{
					ClearVirtualDesktopNetworkAudio();
					clear_audio_hub_queues();
					std::this_thread::sleep_for(std::chrono::milliseconds(5));
					continue;
				}

				bool popped_any = false;
				streamproto::audio::AudioFrame audio_frame{};
				int attempts = 0;
				while (attempts < 32 && PopVirtualDesktopNetworkAudioFrame(audio_frame))
				{
					++attempts;
					popped_any = true;
					{
						std::lock_guard<std::mutex> lock(audio_frame_hub.mutex);
						for (std::size_t queue_index = 0; queue_index < audio_frame_hub.queues.size(); ++queue_index)
						{
							if (queue_index >= route.audio_targets_by_lod.size() ||
								route.audio_targets_by_lod[queue_index].empty())
							{
								continue;
							}
							auto &queue = audio_frame_hub.queues[queue_index];
							while (queue.size() >= 8)
							{
								queue.pop_front();
							}
							queue.push_back(audio_frame);
						}
					}
					audio_frame_hub.cv.notify_all();
				}

				if (!popped_any)
				{
					std::this_thread::sleep_for(std::chrono::milliseconds(1));
				}
			}
			clear_audio_hub_queues();
			audio_frame_hub.cv.notify_all();
		};

		auto audio_lod_loop = [&](std::size_t lod_index)
		{
			while (g_running.load() && !media_workers_stop.load(std::memory_order_acquire))
			{
				if (lod_index >= audio_lods.size())
				{
					return;
				}

				AudioLodRuntime &lod = *audio_lods[lod_index];
				HostRouteSnapshot route = copy_route_snapshot();
				bool can_produce = false;
				bool has_pending_config_targets = false;
				{
					std::lock_guard<std::mutex> lock(runtime_mutex);
					const std::uint64_t now_ms = streamproto::NowSteadyMillis();
					can_produce = AudioLodCanProduce(lod, now_ms);
					if (!can_produce)
					{
						if (!lod.config.enabled && now_ms >= lod.disabled_grace_until_ms)
						{
							lod.encoder_failed = false;
						}
						lod.client_count = 0;
					}
					else
					{
						for (std::uint64_t target : route.connected_peer_ids)
						{
							auto state_it = peer_stream_states.find(target);
							if (state_it == peer_stream_states.end())
							{
								continue;
							}
							const HostPeerStreamState &state = state_it->second;
							if (state.effective_audio_lod != static_cast<std::int32_t>(lod_index))
							{
								continue;
							}
							auto sent_it = state.sent_audio_config_generation_by_lod.find(static_cast<std::int32_t>(lod_index));
							if (sent_it == state.sent_audio_config_generation_by_lod.end() ||
								sent_it->second != state.target_audio_generation)
							{
								has_pending_config_targets = true;
								break;
							}
						}
					}
				}
				if (!can_produce)
				{
					lod.encoder.Shutdown();
					std::this_thread::sleep_for(std::chrono::milliseconds(10));
					continue;
				}

				const std::vector<HostMediaTarget> audio_targets =
					lod_index < route.audio_targets_by_lod.size()
						? route.audio_targets_by_lod[lod_index]
						: std::vector<HostMediaTarget>{};
				const bool wants_audio =
					route.connected &&
					!route.paused &&
					route.audio_source_available &&
					!audio_targets.empty();
				if (!wants_audio && !has_pending_config_targets)
				{
					std::this_thread::sleep_for(std::chrono::milliseconds(2));
					continue;
				}

				if (!ensure_audio_encoder(lod, lod_index))
				{
					std::this_thread::sleep_for(std::chrono::milliseconds(10));
					continue;
				}

				{
					std::lock_guard<std::mutex> runtime_lock(runtime_mutex);
					std::lock_guard<std::mutex> sender_lock(peer_senders_mutex);
					for (std::uint64_t target : route.connected_peer_ids)
					{
						HostPeerStreamState &state = peer_stream_states[target];
						if (state.effective_audio_lod != static_cast<std::int32_t>(lod_index))
						{
							continue;
						}
						std::uint32_t &sent_generation =
							state.sent_audio_config_generation_by_lod[static_cast<std::int32_t>(lod_index)];
						if (sent_generation == state.target_audio_generation)
						{
							continue;
						}
						if (SendAudioConfig(
								peer_senders,
								lod.encoder,
								target,
								static_cast<int>(lod_index),
								state.target_audio_generation))
						{
							sent_generation = state.target_audio_generation;
						}
					}
				}

				if (!wants_audio)
				{
					std::this_thread::sleep_for(std::chrono::milliseconds(2));
					continue;
				}

				streamproto::audio::AudioFrame audio_frame{};
				{
					std::unique_lock<std::mutex> lock(audio_frame_hub.mutex);
					audio_frame_hub.cv.wait_for(
						lock,
						std::chrono::milliseconds(20),
						[&]()
						{
							return media_workers_stop.load(std::memory_order_acquire) ||
								   (lod_index < audio_frame_hub.queues.size() && !audio_frame_hub.queues[lod_index].empty());
						});
					if (media_workers_stop.load(std::memory_order_acquire))
					{
						break;
					}
					if (lod_index >= audio_frame_hub.queues.size() || audio_frame_hub.queues[lod_index].empty())
					{
						continue;
					}
					audio_frame = std::move(audio_frame_hub.queues[lod_index].front());
					audio_frame_hub.queues[lod_index].pop_front();
				}

				std::vector<std::uint8_t> audio_packet;
				const auto audio_encode_start = std::chrono::steady_clock::now();
				if (!lod.encoder.Encode(audio_frame.samples.data(), static_cast<int>(audio_frame.sample_frames), audio_packet))
				{
					std::lock_guard<std::mutex> lock(runtime_mutex);
					stats.audio_encode_fail++;
					lod.last_encode_ms = 0.0;
					stats.last_audio_encode_ms = 0.0;
					continue;
				}
				const auto audio_encode_end = std::chrono::steady_clock::now();
				const double audio_encode_ms = std::chrono::duration<double, std::milli>(audio_encode_end - audio_encode_start).count();

				HostPeerSenderMap peer_sender_snapshot;
				{
					std::lock_guard<std::mutex> sender_lock(peer_senders_mutex);
					peer_sender_snapshot = SnapshotPeerSenders(peer_senders);
				}
				const auto audio_send_start = std::chrono::steady_clock::now();
				HostMediaSendResult audio_result = SendAudioFrame(
					peer_sender_snapshot,
					audio_targets,
					audio_frame,
					audio_packet,
					static_cast<int>(lod_index));
				const auto audio_send_end = std::chrono::steady_clock::now();
				const double audio_send_ms = std::chrono::duration<double, std::milli>(audio_send_end - audio_send_start).count();
				{
					std::lock_guard<std::mutex> runtime_lock(runtime_mutex);
					ApplyMediaSendResult(audio_result, stats, peer_stream_states, audio_send_ms, true);
					lod.encoded_audio_frames += audio_result.encoded_audio_frames;
					lod.encoded_audio_bytes += audio_result.encoded_audio_bytes;
					lod.last_encode_ms = audio_encode_ms;
					lod.last_send_ms = audio_send_ms;
					stats.last_audio_encode_ms = audio_encode_ms;
					stats.last_audio_send_ms = audio_send_ms;
				}
			}

			if (lod_index < audio_lods.size() && audio_lods[lod_index] != nullptr)
			{
				audio_lods[lod_index]->encoder.Shutdown();
			}
		};

		video_capture_worker = std::thread(video_capture_loop);
		audio_capture_worker = std::thread(audio_capture_loop);
		video_lod_workers.reserve(video_lods.size());
		for (std::size_t lod_index = 0; lod_index < video_lods.size(); ++lod_index)
		{
			video_lod_workers.emplace_back([&, lod_index]()
										   { video_lod_loop(lod_index); });
		}
		audio_lod_workers.reserve(audio_lods.size());
		for (std::size_t lod_index = 0; lod_index < audio_lods.size(); ++lod_index)
		{
			audio_lod_workers.emplace_back([&, lod_index]()
										   { audio_lod_loop(lod_index); });
		}

		while (g_running.load())
		{
			sdr.PumpCallbacks();
			PullVirtualDesktopCaptureStats();

			const std::vector<streamproto::HostPeerStatus> peer_statuses = sdr.HostPeerStatuses();
			g_stats_connected.store(!peer_statuses.empty());
			int connected_ice_clients = 0;
			int connected_sdr_clients = 0;
			std::unordered_set<std::uint64_t> connected_peer_ids;
			connected_peer_ids.reserve(peer_statuses.size());
			for (const streamproto::HostPeerStatus &peer_status : peer_statuses)
			{
				if (peer_status.steam_id != 0)
				{
					connected_peer_ids.insert(peer_status.steam_id);
				}
				if (peer_status.relayed)
				{
					++connected_sdr_clients;
				}
				else
				{
					++connected_ice_clients;
				}
			}
			g_stats_connected_ice_clients.store(connected_ice_clients);
			g_stats_connected_sdr_clients.store(connected_sdr_clients);

			if (g_target_clients_dirty.exchange(false))
			{
				sdr.SetHostAllowedSteamIds(CopyTargetClients());
			}

			const bool assigned_lods_dirty = g_assigned_peer_lods_dirty.exchange(false);
			std::unordered_map<std::uint64_t, HostPeerLodAssignment> assigned_peer_lods_update;
			if (assigned_lods_dirty)
			{
				assigned_peer_lods_update = CopyAssignedPeerLods();
			}

			const bool lod_enabled_dirty = g_lod_enabled_dirty.exchange(false);
			std::vector<HostVideoLodConfigState> video_configs;
			std::vector<HostAudioLodConfigState> audio_configs;
			if (lod_enabled_dirty)
			{
				video_configs = CopyVideoLodConfigs();
				audio_configs = CopyAudioLodConfigs();
			}

			// Peer sender lifetime only needs peer_senders_mutex. Keeping this outside runtime_mutex
			// prevents connect/disconnect churn from blocking the media LOD workers.
			{
				std::lock_guard<std::mutex> sender_lock(peer_senders_mutex);
				for (auto it = peer_senders.begin(); it != peer_senders.end();)
				{
					if (connected_peer_ids.find(it->first) == connected_peer_ids.end())
					{
						it = peer_senders.erase(it);
					}
					else
					{
						++it;
					}
				}

				for (std::uint64_t client_id : connected_peer_ids)
				{
					if (peer_senders.find(client_id) == peer_senders.end())
					{
						auto sender = std::make_shared<HostPeerSender>(sdr, client_id);
						sender->Start();
						peer_senders.emplace(client_id, std::move(sender));
					}
				}
			}

			// Steam receive/message parsing is intentionally outside the host supervisor's
			// runtime lock. Each callback takes a small lock only for the state it mutates.
			sdr.ReceiveWithPeer([&](std::uint64_t remote_steam_id, const std::uint8_t *data, std::size_t bytes)
							{
				if (bytes < sizeof(proto::PacketHeader))
				{
					return;
				}

				const auto *header = reinterpret_cast<const proto::PacketHeader *>(data);
				if (header->magic != proto::kMagic || header->version != proto::kProtocolVersion)
				{
					return;
				}

				const proto::MessageType type = static_cast<proto::MessageType>(header->type);
				if (type == proto::MessageType::kKeyframeRequest)
				{
					if (!proto::ValidateExactMessageSize(data, bytes, proto::MessageType::kKeyframeRequest, sizeof(proto::KeyframeRequestMessage)))
					{
						return;
					}
					const auto *request = reinterpret_cast<const proto::KeyframeRequestMessage *>(data);
					std::lock_guard<std::mutex> runtime_lock(runtime_mutex);
					auto state_it = peer_stream_states.find(remote_steam_id);
					if (state_it == peer_stream_states.end())
					{
						return;
					}
					const HostPeerStreamState &state = state_it->second;
					const std::int32_t requested_lod = NormalizeLod(request->lod_index);
					const bool generation_matches =
						request->stream_generation == state.target_video_generation ||
						request->stream_generation == state.media_video_generation;
					const bool lod_matches =
						requested_lod == state.effective_video_lod ||
						requested_lod == state.media_video_lod;
					if (!generation_matches || !lod_matches || !LodEnabled(requested_lod))
					{
						return;
					}
					if (static_cast<std::size_t>(requested_lod) >= video_lods.size())
					{
						return;
					}
					VideoLodRuntime &lod = *video_lods[static_cast<std::size_t>(requested_lod)];
					const std::uint64_t now_ms = streamproto::NowSteadyMillis();
					const bool recently_congested =
						(now_ms - last_queue_drop_ms.load(std::memory_order_acquire)) < 1500;
					if (now_ms - lod.last_forced_keyframe_ms >= 1000 && !recently_congested)
					{
						lod.force_keyframe = true;
						stats.keyframe_requests++;
						lod.last_forced_keyframe_ms = now_ms;
					}
					return;
				}

				if (type == proto::MessageType::kPing)
				{
					if (!proto::ValidateExactMessageSize(data, bytes, proto::MessageType::kPing, sizeof(proto::PingMessage)))
					{
						return;
					}
					const auto *ping = reinterpret_cast<const proto::PingMessage *>(data);
					const std::uint64_t host_recv_ts_us = streamproto::NowUnixMicros();
					{
						std::lock_guard<std::mutex> sender_lock(peer_senders_mutex);
						SendPong(peer_senders, remote_steam_id, *ping, host_recv_ts_us);
					}
					return;
				}

				if (type == proto::MessageType::kStreamQuality)
				{
					if (!proto::ValidateExactMessageSize(data, bytes, proto::MessageType::kStreamQuality, sizeof(proto::StreamQualityMessage)) || remote_steam_id == 0)
					{
						return;
					}
					const auto *quality_msg = reinterpret_cast<const proto::StreamQualityMessage *>(data);
					const auto command = static_cast<proto::StreamQualityCommand>(quality_msg->command);
					std::lock_guard<std::mutex> runtime_lock(runtime_mutex);
					if (command == proto::StreamQualityCommand::kClientMaxAccepted)
					{
						HostPeerStreamState &state = peer_stream_states[remote_steam_id];
						const std::int32_t next_max_video_lod = NormalizeLod(quality_msg->max_video_lod);
						const std::int32_t next_max_audio_lod = NormalizeLod(quality_msg->max_audio_lod);
						if (state.max_video_lod != next_max_video_lod)
						{
							state.max_video_lod = next_max_video_lod;
						}
						if (state.max_audio_lod != next_max_audio_lod)
						{
							state.max_audio_lod = next_max_audio_lod;
						}
					}
					else if (command == proto::StreamQualityCommand::kClientApplied)
					{
						HostPeerStreamState &state = peer_stream_states[remote_steam_id];
						if (quality_msg->sequence == state.target_video_generation &&
							NormalizeLod(quality_msg->effective_video_lod) == state.effective_video_lod)
						{
							state.client_applied_video_generation = quality_msg->sequence;
						}
						if (quality_msg->profile_id == state.target_audio_generation &&
							NormalizeLod(quality_msg->effective_audio_lod) == state.effective_audio_lod)
						{
							state.client_applied_audio_generation = quality_msg->profile_id;
						}
					}
					return;
				}

				if (type == proto::MessageType::kStreamControl)
				{
					if (!proto::ValidateExactMessageSize(data, bytes, proto::MessageType::kStreamControl, sizeof(proto::StreamControlMessage)))
					{
						return;
					}
					const auto *control = reinterpret_cast<const proto::StreamControlMessage *>(data);
					const auto command = static_cast<proto::StreamControlCommand>(control->command);
					const std::uint64_t sender = control->sender_steam_id != 0 ? control->sender_steam_id : remote_steam_id;
					if (command == proto::StreamControlCommand::kClientPause)
					{
						sdr.SetHostPeerPaused(sender, true);
					}
					else if (command == proto::StreamControlCommand::kClientResume)
					{
						sdr.SetHostPeerPaused(sender, false);
					}
				}
			});

			const bool connected = !peer_statuses.empty();
			const bool video_source_available =
				IsVirtualDesktopCaptureRunning() &&
				g_preview_width.load() > 0 &&
				g_preview_height.load() > 0;
			const bool audio_source_available =
				g_stats_audio_capture_state.load() > 0;
			const std::uint64_t loop_now_ms = streamproto::NowSteadyMillis();
			std::vector<std::vector<HostMediaTarget>> video_targets_by_lod(video_lods.size());
			std::vector<std::vector<HostMediaTarget>> audio_targets_by_lod(audio_lods.size());
			{
				std::lock_guard<std::mutex> runtime_lock(runtime_mutex);

				if (assigned_lods_dirty)
				{
					assigned_peer_lods = std::move(assigned_peer_lods_update);
				}

				if (lod_enabled_dirty)
				{
					const std::uint64_t now_ms = streamproto::NowSteadyMillis();
					for (std::size_t i = 0; i < video_lods.size() && i < video_configs.size(); ++i)
					{
						const bool was_enabled = video_lods[i]->config.enabled;
						video_lods[i]->config.enabled = video_configs[i].enabled;
						if (video_lods[i]->config.enabled && !was_enabled)
						{
							video_lods[i]->disabled_grace_until_ms = 0;
							video_lods[i]->next_frame_time = std::chrono::steady_clock::now();
							video_lods[i]->force_keyframe = true;
						}
						else if (!video_lods[i]->config.enabled && was_enabled)
						{
							video_lods[i]->disabled_grace_until_ms = now_ms + kStreamSwitchGraceMs;
							video_lods[i]->force_keyframe = true;
						}
					}

					for (std::size_t i = 0; i < audio_lods.size() && i < audio_configs.size(); ++i)
					{
						const bool was_enabled = audio_lods[i]->config.enabled;
						audio_lods[i]->config.enabled = audio_configs[i].enabled;
						if (audio_lods[i]->config.enabled && !was_enabled)
						{
							audio_lods[i]->disabled_grace_until_ms = 0;
						}
						else if (!audio_lods[i]->config.enabled && was_enabled)
						{
							audio_lods[i]->disabled_grace_until_ms = now_ms + kStreamSwitchGraceMs;
						}
					}
					apply_route_profile(active_path, true);
				}

				for (auto it = peer_stream_states.begin(); it != peer_stream_states.end();)
				{
					if (connected_peer_ids.find(it->first) == connected_peer_ids.end())
					{
						it = peer_stream_states.erase(it);
					}
					else
					{
						++it;
					}
				}

				for (std::uint64_t client_id : connected_peer_ids)
				{
					HostPeerStreamState &state = peer_stream_states[client_id];
					auto assigned_it = assigned_peer_lods.find(client_id);
					state.assigned_lod = assigned_it != assigned_peer_lods.end() ? assigned_it->second : HostPeerLodAssignment{};
				}

				available_video_lod_mask = BuildVideoAvailabilityMask(video_lods, video_source_available, loop_now_ms);
				available_audio_lod_mask = BuildAudioAvailabilityMask(audio_lods, audio_source_available, loop_now_ms);

				for (const streamproto::HostPeerStatus &peer_status : peer_statuses)
				{
					if (peer_status.steam_id == 0)
					{
						continue;
					}

					HostPeerStreamState &state = peer_stream_states[peer_status.steam_id];
					const std::int32_t next_effective_video = ResolveEffectiveLod(
						state.assigned_lod.video_lod,
						state.max_video_lod,
						available_video_lod_mask);
					const std::int32_t next_effective_audio = ResolveEffectiveLod(
						state.assigned_lod.audio_lod,
						state.max_audio_lod,
						available_audio_lod_mask);
					if (state.effective_video_lod != next_effective_video)
					{
						state.effective_video_lod = next_effective_video;
						BeginVideoGenerationTransition(state, loop_now_ms);
						if (LodEnabled(state.effective_video_lod) &&
							static_cast<std::size_t>(state.effective_video_lod) < video_lods.size())
						{
							VideoLodRuntime &lod = *video_lods[static_cast<std::size_t>(state.effective_video_lod)];
							lod.force_keyframe = true;
							lod.last_forced_keyframe_ms = streamproto::NowSteadyMillis();
						}
					}
					if (state.effective_audio_lod != next_effective_audio)
					{
						state.effective_audio_lod = next_effective_audio;
						BeginAudioGenerationTransition(state, loop_now_ms);
					}

					if (HasPendingVideoGeneration(state))
					{
						auto can_keep_video = [&]() -> bool
						{
							if (!LodEnabled(state.media_video_lod))
							{
								return true;
							}
							const std::size_t idx = static_cast<std::size_t>(state.media_video_lod);
							return idx < video_lods.size() && VideoLodCanProduce(*video_lods[idx], loop_now_ms);
						};
						const bool client_ready = state.client_applied_video_generation == state.target_video_generation;
						const bool grace_expired =
							state.video_transition_started_ms == 0 ||
							loop_now_ms - state.video_transition_started_ms >= kStreamSwitchGraceMs;
						const bool no_media_to_keep = !LodEnabled(state.media_video_lod);
						const bool old_media_unavailable = !can_keep_video();
						if (client_ready || grace_expired || no_media_to_keep || old_media_unavailable)
						{
							state.media_video_lod = state.effective_video_lod;
							state.media_video_generation = state.target_video_generation;
							if (LodEnabled(state.media_video_lod) &&
								static_cast<std::size_t>(state.media_video_lod) < video_lods.size())
							{
								VideoLodRuntime &lod = *video_lods[static_cast<std::size_t>(state.media_video_lod)];
								lod.force_keyframe = true;
								lod.last_forced_keyframe_ms = streamproto::NowSteadyMillis();
							}
						}
					}

					if (HasPendingAudioGeneration(state))
					{
						auto can_keep_audio = [&]() -> bool
						{
							if (!LodEnabled(state.media_audio_lod))
							{
								return true;
							}
							const std::size_t idx = static_cast<std::size_t>(state.media_audio_lod);
							return idx < audio_lods.size() && AudioLodCanProduce(*audio_lods[idx], loop_now_ms);
						};
						const bool client_ready = state.client_applied_audio_generation == state.target_audio_generation;
						const bool grace_expired =
							state.audio_transition_started_ms == 0 ||
							loop_now_ms - state.audio_transition_started_ms >= kStreamSwitchGraceMs;
						const bool no_media_to_keep = !LodEnabled(state.media_audio_lod);
						const bool old_media_unavailable = !can_keep_audio();
						if (client_ready || grace_expired || no_media_to_keep || old_media_unavailable)
						{
							state.media_audio_lod = state.effective_audio_lod;
							state.media_audio_generation = state.target_audio_generation;
						}
					}

					if (!state.hello_sent)
					{
						std::lock_guard<std::mutex> sender_lock(peer_senders_mutex);
						state.hello_sent = SendHello(peer_senders, options.app_id, sdr.LocalSteamId(), peer_status.steam_id);
						LogInfoStream() << "path=" << (peer_status.relayed ? "sdr" : "ice")
										<< " Client connected: " << peer_status.steam_id;
					}

					const bool awaiting_generation_ack =
						state.client_applied_video_generation != state.target_video_generation ||
						state.client_applied_audio_generation != state.target_audio_generation;
					const bool quality_resend_due =
						awaiting_generation_ack &&
						(state.last_quality_send_ms == 0 ||
						 loop_now_ms - state.last_quality_send_ms >= kStreamQualityResendMs);
					if (state.last_sent_available_video_lod_mask != available_video_lod_mask ||
						state.last_sent_available_audio_lod_mask != available_audio_lod_mask ||
						state.last_sent_effective_video_lod != state.effective_video_lod ||
						state.last_sent_effective_audio_lod != state.effective_audio_lod ||
						state.last_sent_assigned_video_lod != state.assigned_lod.video_lod ||
						state.last_sent_assigned_audio_lod != state.assigned_lod.audio_lod ||
						state.last_sent_video_generation != state.target_video_generation ||
						state.last_sent_audio_generation != state.target_audio_generation ||
						quality_resend_due)
					{
						std::lock_guard<std::mutex> sender_lock(peer_senders_mutex);
						if (SendStreamQualityMessage(
								peer_senders,
								peer_status.steam_id,
								proto::StreamQualityCommand::kHostAssigned,
								state,
								available_video_lod_mask,
								available_audio_lod_mask,
								sdr.LocalSteamId()))
						{
							state.last_sent_available_video_lod_mask = available_video_lod_mask;
							state.last_sent_available_audio_lod_mask = available_audio_lod_mask;
							state.last_sent_effective_video_lod = state.effective_video_lod;
							state.last_sent_effective_audio_lod = state.effective_audio_lod;
							state.last_sent_assigned_video_lod = state.assigned_lod.video_lod;
							state.last_sent_assigned_audio_lod = state.assigned_lod.audio_lod;
							state.last_sent_video_generation = state.target_video_generation;
							state.last_sent_audio_generation = state.target_audio_generation;
							state.last_quality_send_ms = loop_now_ms;
						}
					}

					if (!peer_status.paused &&
						LodEnabled(state.media_video_lod) &&
						state.media_video_generation != 0 &&
						static_cast<std::size_t>(state.media_video_lod) < video_targets_by_lod.size())
					{
						video_targets_by_lod[static_cast<std::size_t>(state.media_video_lod)].push_back(
							HostMediaTarget{peer_status.steam_id, state.media_video_generation});
					}
					if (!peer_status.paused &&
						LodEnabled(state.media_audio_lod) &&
						state.media_audio_generation != 0 &&
						static_cast<std::size_t>(state.media_audio_lod) < audio_targets_by_lod.size())
					{
						audio_targets_by_lod[static_cast<std::size_t>(state.media_audio_lod)].push_back(
							HostMediaTarget{peer_status.steam_id, state.media_audio_generation});
					}
				}

				for (std::size_t i = 0; i < video_lods.size(); ++i)
				{
					video_lods[i]->client_count = static_cast<int>(video_targets_by_lod[i].size());
				}
				for (std::size_t i = 0; i < audio_lods.size(); ++i)
				{
					audio_lods[i]->client_count = static_cast<int>(audio_targets_by_lod[i].size());
				}

			}

			if (connected)
			{
				const std::uint64_t now_ms = streamproto::NowSteadyMillis();
				if (last_route_probe_ms == 0 || now_ms - last_route_probe_ms >= 500)
				{
					last_route_probe_ms = now_ms;
					ConnectionPath detected_path = active_path;
					if (GetConnectionPath(sdr, detected_path) && detected_path != active_path)
					{
						active_path = detected_path;
						LogInfoStream() << "path=" << ConnectionPathName(active_path) << " [NET] path switch detected";
						std::lock_guard<std::mutex> runtime_lock(runtime_mutex);
						apply_route_profile(active_path, true);
					}
				}

				if (!unreliable_chunk_tuning_logged)
				{
					int mtu_data_size = 0;
					unreliable_chunk_payload_limit = ComputeUnreliableChunkPayloadLimit(
						sdr,
						kDefaultUnreliableChunkPayloadLimit,
						mtu_data_size);
					effective_unreliable_chunk_payload = std::min(
						std::clamp(options.chunk_payload_bytes, 512, 60000),
						unreliable_chunk_payload_limit);
					std::ostringstream line;
					line << "path=" << ConnectionPathName(active_path)
						 << " [NET] chunk payload request=" << options.chunk_payload_bytes
						 << "B effective_unreliable=" << effective_unreliable_chunk_payload
						 << "B limit=" << unreliable_chunk_payload_limit
						 << "B";
					if (mtu_data_size > 0)
					{
						line << " mtu_data=" << mtu_data_size << "B";
					}
					LogInfoStream() << line.str();
					unreliable_chunk_tuning_logged = true;
				}
			}
			else
			{
				active_path = ConnectionPath::kSdr;
				unreliable_chunk_payload_limit = kDefaultUnreliableChunkPayloadLimit;
				effective_unreliable_chunk_payload = std::clamp(options.chunk_payload_bytes, 512, 60000);
				unreliable_chunk_tuning_logged = false;
				last_route_probe_ms = 0;
				{
					std::lock_guard<std::mutex> runtime_lock(runtime_mutex);
					for (auto &lod : video_lods)
					{
						if (lod != nullptr)
						{
							lod->next_frame_time = std::chrono::steady_clock::now();
						}
					}
					stats.current_bitrate_kbps = aggregate_configured_bitrate_kbps;
					stats.pending_unreliable_bytes = 0;
					stats.queue_time_ms = 0;
					stats.send_rate_kbps = 0;
				}
				ClearVirtualDesktopNetworkAudio();
			}

			const bool pause_now = g_pause_stream.load();
			{
				std::lock_guard<std::mutex> runtime_lock(runtime_mutex);
				std::lock_guard<std::mutex> sender_lock(peer_senders_mutex);
				for (const streamproto::HostPeerStatus &peer_status : peer_statuses)
				{
					HostPeerStreamState &state = peer_stream_states[peer_status.steam_id];
					if (!state.host_pause_state_sent || state.last_host_pause_sent != pause_now)
					{
						if (SendStreamControl(
								peer_senders,
								pause_now ? proto::StreamControlCommand::kHostPause : proto::StreamControlCommand::kHostResume,
								sdr.LocalSteamId(),
								peer_status.steam_id))
						{
							state.host_pause_state_sent = true;
							state.last_host_pause_sent = pause_now;
						}
					}
				}
			}

			SdrQueueStatus snapshot_queue_status{};
			std::unordered_map<std::uint64_t, SdrQueueStatus> snapshot_queue_status_by_peer;
			if (connected)
			{
				int pending_total = 0;
				int max_queue_time_usec = 0;
				int send_rate_total_bps = 0;
				for (std::uint64_t steam_id : connected_peer_ids)
				{
					SdrQueueStatus peer_queue_status{};
					if (GetSdrQueueStatusForSteamId(sdr, steam_id, peer_queue_status) && peer_queue_status.valid)
					{
						snapshot_queue_status_by_peer.emplace(steam_id, peer_queue_status);
						pending_total += std::max(0, peer_queue_status.pending_unreliable_bytes + peer_queue_status.pending_reliable_bytes);
						max_queue_time_usec = std::max(max_queue_time_usec, peer_queue_status.queue_time_usec);
						send_rate_total_bps += std::max(0, peer_queue_status.send_rate_bps);
					}
				}
				snapshot_queue_status.valid = !snapshot_queue_status_by_peer.empty();
				snapshot_queue_status.pending_unreliable_bytes = pending_total;
				snapshot_queue_status.pending_reliable_bytes = 0;
				snapshot_queue_status.queue_time_usec = max_queue_time_usec;
				snapshot_queue_status.send_rate_bps = send_rate_total_bps;
			}

			{
				std::lock_guard<std::mutex> runtime_lock(runtime_mutex);
				if (connected)
				{
					stats.pending_unreliable_bytes = snapshot_queue_status.pending_unreliable_bytes;
					stats.queue_time_ms = snapshot_queue_status.queue_time_usec / 1000;
					stats.send_rate_kbps = snapshot_queue_status.send_rate_bps / 1000;
				}
				else
				{
					stats.pending_unreliable_bytes = 0;
					stats.queue_time_ms = 0;
					stats.send_rate_kbps = 0;
				}

				HostRouteSnapshot next_route{};
				next_route.connected = connected;
				next_route.paused = pause_now;
				next_route.video_source_available = video_source_available;
				next_route.audio_source_available = audio_source_available;
				next_route.connected_peer_ids.assign(connected_peer_ids.begin(), connected_peer_ids.end());
				next_route.video_targets_by_lod = video_targets_by_lod;
				next_route.audio_targets_by_lod = audio_targets_by_lod;
				next_route.video_queue_status = snapshot_queue_status;
				next_route.video_queue_status_by_peer = std::move(snapshot_queue_status_by_peer);
				next_route.effective_unreliable_chunk_payload = effective_unreliable_chunk_payload;
				next_route.unreliable_chunk_payload_limit = unreliable_chunk_payload_limit;
				next_route.chunk_payload_bytes = options.chunk_payload_bytes;
				next_route.parity_shards = options.parity_shards;
				next_route.reliable_video = options.reliable_video;
				next_route.reliable_keyframes = options.reliable_keyframes;
				next_route.max_queue_ms = options.max_queue_ms;
				next_route.active_path = active_path;
				publish_route_snapshot(std::move(next_route));
			}

			const std::uint64_t publish_now_ms = streamproto::NowSteadyMillis();
			if (publish_now_ms - last_stats_export_ms >= kHostStatsExportIntervalMs)
			{
				PullVirtualDesktopCaptureStats();
				HostStats stats_export{};
				std::unordered_map<std::uint64_t, HostPeerStreamState> peer_states_export;
				std::uint64_t video_lod_mask_export = 0;
				std::uint64_t audio_lod_mask_export = 0;
				{
					std::lock_guard<std::mutex> runtime_lock(runtime_mutex);
					{
						std::lock_guard<std::mutex> lock(g_stats_mutex);
						stats.last_capture_ms = g_stats_last_capture_ms;
					}
					stats_export = stats;
					peer_states_export = peer_stream_states;
					video_lod_mask_export = available_video_lod_mask;
					audio_lod_mask_export = available_audio_lod_mask;
				}

				PublishHostStatsSnapshot(stats_export, true);
				PublishHostPeerStats(sdr, stats_export, peer_states_export, video_lod_mask_export, audio_lod_mask_export);
				{
					std::lock_guard<std::mutex> runtime_lock(runtime_mutex);
					PublishHostLodStats(video_lods, audio_lods);
				}
				last_stats_export_ms = publish_now_ms;
			}

			if (publish_now_ms - last_stats_ms >= 1000)
			{
				HostStats stats_print{};
				std::string encoder_name = "n/a";
				ConnectionPath print_path = active_path;
				{
					std::lock_guard<std::mutex> runtime_lock(runtime_mutex);
					stats_print = stats;
					for (const auto &lod : video_lods)
					{
						if (lod != nullptr && lod->encoder_ready)
						{
							encoder_name = lod->encoder.ActiveEncoder();
							break;
						}
					}
					print_path = active_path;
				}
				PrintStatsLine(
					stats_print,
					encoder_name,
					ConnectionPathName(print_path),
					connected,
					publish_now_ms - last_stats_ms,
					snapshot);
				last_stats_ms = publish_now_ms;
			}

			std::this_thread::sleep_for(std::chrono::milliseconds(1));
			continue;
		}
		media_workers_stop.store(true, std::memory_order_release);
		route_cv.notify_all();
		video_frame_hub.cv.notify_all();
		audio_frame_hub.cv.notify_all();
		if (video_capture_worker.joinable())
		{
			video_capture_worker.join();
		}
		if (audio_capture_worker.joinable())
		{
			audio_capture_worker.join();
		}
		for (std::thread &worker : video_lod_workers)
		{
			if (worker.joinable())
			{
				worker.join();
			}
		}
		for (std::thread &worker : audio_lod_workers)
		{
			if (worker.joinable())
			{
				worker.join();
			}
		}
		{
			std::lock_guard<std::mutex> sender_lock(peer_senders_mutex);
			peer_senders.clear();
		}

		LogInfoStream() << "path=" << ConnectionPathName(active_path) << " Stopping host sender.";
		sdr.Close("host-exit");
		ClearVirtualDesktopNetworkAudio();
		g_stats_connected.store(false);
		g_stats_connected_ice_clients.store(0);
		g_stats_connected_sdr_clients.store(0);
		{
			std::lock_guard<std::mutex> lock(g_host_peer_exports_mutex);
			g_host_peer_exports.clear();
		}
		{
			std::lock_guard<std::mutex> lock(g_host_lod_exports_mutex);
			g_host_video_lod_exports.clear();
			g_host_audio_lod_exports.clear();
		}
		return 0;
	}

} // namespace

namespace
{

	std::thread g_host_thread;
	std::mutex g_host_thread_mutex;
	std::atomic<bool> g_host_thread_running{false};


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
			streamproto::d3d11::EnableImmediateContextMultithreadProtection(g_unity_device);
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
		if (event_id != kHostRenderEventId)
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

		(void)streamproto::unity::CopySharedTextureToTarget(
			g_unity_device,
			source_handle,
			g_unity_target_texture,
			g_unity_opened_source_handle,
			g_unity_opened_source_texture,
			false);
	}

} // namespace

extern "C"
{

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

	UNITY_INTERFACE_EXPORT void SSPH_ConfigureVideoLods(const SSPH_VideoLodConfig *lods, int count)
	{
		std::vector<HostVideoLodConfigState> values;
		if (lods != nullptr && count > 0)
		{
			const int safe_count = std::clamp(count, 0, kMaxConfiguredLods);
			values.reserve(static_cast<std::size_t>(safe_count));
			for (int i = 0; i < safe_count; ++i)
			{
				HostVideoLodConfigState cfg{};
				cfg.enabled = lods[i].enabled != 0;
				cfg.width = lods[i].width;
				cfg.height = lods[i].height;
				cfg.fps = lods[i].fps;
				cfg.target_bitrate_kbps = lods[i].target_bitrate_kbps;
				cfg.video_rate_control = lods[i].video_rate_control;
				values.push_back(SanitizeVideoLodConfig(cfg));
			}
		}
		if (values.empty())
		{
			values = DefaultVideoLodConfigs();
		}
		{
			std::lock_guard<std::mutex> lock(g_lod_config_mutex);
			g_video_lod_configs = std::move(values);
		}
		g_lod_enabled_dirty.store(true);
	}

	UNITY_INTERFACE_EXPORT void SSPH_ConfigureAudioLods(const SSPH_AudioLodConfig *lods, int count)
	{
		std::vector<HostAudioLodConfigState> values;
		if (lods != nullptr && count > 0)
		{
			const int safe_count = std::clamp(count, 0, kMaxConfiguredLods);
			values.reserve(static_cast<std::size_t>(safe_count));
			for (int i = 0; i < safe_count; ++i)
			{
				HostAudioLodConfigState cfg{};
				cfg.enabled = lods[i].enabled != 0;
				cfg.bitrate_kbps = lods[i].bitrate_kbps;
				values.push_back(SanitizeAudioLodConfig(cfg));
			}
		}
		if (values.empty())
		{
			values = DefaultAudioLodConfigs();
		}
		{
			std::lock_guard<std::mutex> lock(g_lod_config_mutex);
			g_audio_lod_configs = std::move(values);
		}
		g_lod_enabled_dirty.store(true);
	}

	UNITY_INTERFACE_EXPORT void SSPH_SetVideoLodEnabled(int lod_index, bool enabled)
	{
		if (lod_index < 0)
			return;
		{
			std::lock_guard<std::mutex> lock(g_lod_config_mutex);
			if (static_cast<std::size_t>(lod_index) >= g_video_lod_configs.size())
				return;
			g_video_lod_configs[static_cast<std::size_t>(lod_index)].enabled = enabled;
		}
		g_lod_enabled_dirty.store(true);
	}

	UNITY_INTERFACE_EXPORT void SSPH_SetAudioLodEnabled(int lod_index, bool enabled)
	{
		if (lod_index < 0)
			return;
		{
			std::lock_guard<std::mutex> lock(g_lod_config_mutex);
			if (static_cast<std::size_t>(lod_index) >= g_audio_lod_configs.size())
				return;
			g_audio_lod_configs[static_cast<std::size_t>(lod_index)].enabled = enabled;
		}
		g_lod_enabled_dirty.store(true);
	}

	UNITY_INTERFACE_EXPORT bool SSPH_Start(const SSPH_StartParams *params)
	{
		if (params == nullptr || params->app_id == 0)
		{
			return false;
		}
		std::string virtual_desktop_error;
		if (!EnsureVirtualDesktopCaptureBridge(&virtual_desktop_error))
		{
			SetLastErrorString(virtual_desktop_error);
			return false;
		}

		std::lock_guard<std::mutex> lock(g_host_thread_mutex);
		if (g_host_thread_running.load())
		{
			return false;
		}

		g_running.store(true);
		g_pause_stream.store(false);
		g_stats_connected.store(false);
		g_stats_connected_ice_clients.store(0);
		g_stats_connected_sdr_clients.store(0);
		{
			std::lock_guard<std::mutex> stats_lock(g_stats_mutex);
			g_stats_capture_frames = 0;
			g_stats_encoded_frames = 0;
			g_stats_encoded_video_bytes = 0;
			g_stats_sent_chunks = 0;
			g_stats_sent_bytes = 0;
			g_stats_sent_video_bytes = 0;
			g_stats_sent_audio_frames = 0;
			g_stats_sent_audio_bytes = 0;
			g_stats_encoded_audio_frames = 0;
			g_stats_encoded_audio_bytes = 0;
			g_stats_encoded_width = 0;
			g_stats_encoded_height = 0;
			g_stats_encoded_fps = 0;
			g_stats_last_encode_ms = 0.0;
			g_stats_last_send_ms = 0.0;
			g_stats_last_video_encode_ms = 0.0;
			g_stats_last_video_send_ms = 0.0;
			g_stats_last_audio_encode_ms = 0.0;
			g_stats_last_audio_send_ms = 0.0;
		}
		{
			std::lock_guard<std::mutex> peer_lock(g_host_peer_exports_mutex);
			g_host_peer_exports.clear();
		}
		{
			std::lock_guard<std::mutex> lod_lock(g_host_lod_exports_mutex);
			g_host_video_lod_exports.clear();
			g_host_audio_lod_exports.clear();
		}
		{
			std::lock_guard<std::mutex> quality_lock(g_assigned_peer_lods_mutex);
			g_assigned_peer_lods.clear();
		}
		g_assigned_peer_lods_dirty.store(true);
		ClearVirtualDesktopNetworkAudio();
		SetLastErrorString("");

		HostOptions options{};
		options.app_id = params->app_id;
		options.width = std::max(1, params->width);
		options.height = std::max(1, params->height);
		options.fps = std::clamp(params->fps, 1, 240);
		options.ice_bitrate_kbps = std::max(100, params->target_bitrate_kbps);
		options.sdr_bitrate_kbps = std::max(100, params->target_bitrate_kbps);
		options.video_rate_control = NormalizeVideoRateControl(params->video_rate_control);
		options.encoder_queue_depth = ReadIntEnvironment("SSPH_ENCODER_QUEUE_DEPTH", 1, 1, 8);
		options.disable_ice = params->disable_ice != 0;
		options.chunk_payload_bytes = std::clamp(params->chunk_payload_bytes <= 0 ? 24000 : params->chunk_payload_bytes, 512, 60000);
		options.parity_shards = std::clamp(params->parity_shards, 0, 1);
		options.gop = 0;
		options.reliable_video = params->reliable_video != 0;
		options.reliable_keyframes = params->reliable_keyframes != 0;
		options.max_queue_ms = std::clamp(params->max_queue_ms <= 0 ? 120 : params->max_queue_ms, 0, 5000);
		options.enable_audio = params->enable_audio != 0;
		options.audio_bitrate_kbps = streamproto::audio::ClampAudioBitrateKbps(params->audio_bitrate_kbps);
		if (params->codec != nullptr && params->codec[0] != '\0')
		{
			options.codec = streamproto::ToLowerAscii(params->codec);
		}
		if (params->encoder != nullptr && params->encoder[0] != '\0')
		{
			options.encoder_pref = streamproto::ToLowerAscii(params->encoder);
		}
		{
			std::lock_guard<std::mutex> stats_lock(g_stats_mutex);
			g_stats_encoded_width = options.width;
			g_stats_encoded_height = options.height;
			g_stats_encoded_fps = options.fps;
		}

		g_host_thread_running.store(true);
		g_host_thread = std::thread([options]()
									{
    const int result = RunHostSenderWithOptions(options);
    if (result != 0) {
      bool has_error = false;
      {
        std::lock_guard<std::mutex> lock(g_error_mutex);
        has_error = !g_last_error.empty();
      }
      if (!has_error) {
        SetLastErrorString("Host worker exited with failure.");
      }
    }
    g_host_thread_running.store(false); });
		return true;
	}

	UNITY_INTERFACE_EXPORT void SSPH_Stop()
	{
		std::lock_guard<std::mutex> lock(g_host_thread_mutex);
		g_running.store(false);
		if (g_host_thread.joinable())
		{
			g_host_thread.join();
		}
		g_host_thread_running.store(false);
		g_stats_connected.store(false);
		g_stats_connected_ice_clients.store(0);
		g_stats_connected_sdr_clients.store(0);
		{
			std::lock_guard<std::mutex> stats_lock(g_stats_mutex);
			g_stats_capture_frames = 0;
			g_stats_encoded_frames = 0;
			g_stats_encoded_video_bytes = 0;
			g_stats_sent_chunks = 0;
			g_stats_sent_bytes = 0;
			g_stats_sent_video_bytes = 0;
			g_stats_sent_audio_frames = 0;
			g_stats_sent_audio_bytes = 0;
			g_stats_encoded_audio_frames = 0;
			g_stats_encoded_audio_bytes = 0;
			g_stats_encoded_width = 0;
			g_stats_encoded_height = 0;
			g_stats_encoded_fps = 0;
			g_stats_last_encode_ms = 0.0;
			g_stats_last_send_ms = 0.0;
			g_stats_last_video_encode_ms = 0.0;
			g_stats_last_video_send_ms = 0.0;
			g_stats_last_audio_encode_ms = 0.0;
			g_stats_last_audio_send_ms = 0.0;
		}
		{
			std::lock_guard<std::mutex> peer_lock(g_host_peer_exports_mutex);
			g_host_peer_exports.clear();
		}
		{
			std::lock_guard<std::mutex> lod_lock(g_host_lod_exports_mutex);
			g_host_video_lod_exports.clear();
			g_host_audio_lod_exports.clear();
		}
		{
			std::lock_guard<std::mutex> quality_lock(g_assigned_peer_lods_mutex);
			g_assigned_peer_lods.clear();
		}
		g_assigned_peer_lods_dirty.store(true);
		ClearVirtualDesktopNetworkAudio();
	}

	UNITY_INTERFACE_EXPORT bool SSPH_IsRunning()
	{
		return g_host_thread_running.load();
	}

	UNITY_INTERFACE_EXPORT void SSPH_SetPaused(bool paused)
	{
		g_pause_stream.store(paused);
	}

	UNITY_INTERFACE_EXPORT void SSPH_SetClientSteamIds(const std::uint64_t *steam_ids, int count)
	{
		std::vector<std::uint64_t> values;
		if (steam_ids != nullptr && count > 0)
		{
			values.reserve(static_cast<std::size_t>(count));
			for (int i = 0; i < count; ++i)
			{
				if (steam_ids[i] != 0)
				{
					values.push_back(steam_ids[i]);
				}
			}
		}
		{
			std::lock_guard<std::mutex> lock(g_target_clients_mutex);
			g_target_clients = std::move(values);
		}
		g_target_clients_dirty.store(true);
	}

	UNITY_INTERFACE_EXPORT void SSPH_SetClientVideoLod(std::uint64_t client_steam_id, std::int32_t lod)
	{
		if (client_steam_id == 0)
		{
			return;
		}

		{
			std::lock_guard<std::mutex> lock(g_assigned_peer_lods_mutex);
			g_assigned_peer_lods[client_steam_id].video_lod = NormalizeLod(lod);
		}
		g_assigned_peer_lods_dirty.store(true);
	}

	UNITY_INTERFACE_EXPORT void SSPH_SetClientAudioLod(std::uint64_t client_steam_id, std::int32_t lod)
	{
		if (client_steam_id == 0)
		{
			return;
		}
		{
			std::lock_guard<std::mutex> lock(g_assigned_peer_lods_mutex);
			g_assigned_peer_lods[client_steam_id].audio_lod = NormalizeLod(lod);
		}
		g_assigned_peer_lods_dirty.store(true);
	}

	UNITY_INTERFACE_EXPORT void SSPH_SetPreviewTexture(void *native_texture_ptr)
	{
		std::lock_guard<std::mutex> lock(g_unity_mutex);
		g_unity_target_texture = static_cast<ID3D11Texture2D *>(native_texture_ptr);
	}

	UNITY_INTERFACE_EXPORT void SSPH_SetLogCallback(HostLogCallback callback)
	{
		SetHostLogCallback(callback);
	}

	UNITY_INTERFACE_EXPORT void SSPH_SetLoggingEnabled(bool enabled)
	{
		SetHostLoggingEnabled(enabled);
	}

	UNITY_INTERFACE_EXPORT UnityRenderingEvent SSPH_GetRenderEventFunc()
	{
		return OnHostRenderEvent;
	}

	UNITY_INTERFACE_EXPORT int SSPH_GetRenderEventId()
	{
		return kHostRenderEventId;
	}

	UNITY_INTERFACE_EXPORT void SSPH_GetStats(SSPH_Stats *out_stats)
	{
		if (out_stats == nullptr)
		{
			return;
		}
		PullVirtualDesktopCaptureStats();
		std::lock_guard<std::mutex> lock(g_stats_mutex);
		out_stats->capture_frames = g_stats_capture_frames;
		out_stats->encoded_frames = g_stats_encoded_frames;
		out_stats->sent_chunks = g_stats_sent_chunks;
		out_stats->sent_bytes = g_stats_sent_bytes;
		out_stats->last_capture_ms = g_stats_last_capture_ms;
		out_stats->last_encode_ms = g_stats_last_encode_ms;
		out_stats->last_send_ms = g_stats_last_send_ms;
		out_stats->last_video_encode_ms = g_stats_last_video_encode_ms;
		out_stats->last_video_send_ms = g_stats_last_video_send_ms;
		out_stats->last_audio_encode_ms = g_stats_last_audio_encode_ms;
		out_stats->last_audio_send_ms = g_stats_last_audio_send_ms;
		out_stats->local_steam_id = g_stats_local_steam_id.load();
		out_stats->connected = g_stats_connected.load() ? 1 : 0;
		out_stats->connected_ice_clients = g_stats_connected_ice_clients.load();
		out_stats->connected_sdr_clients = g_stats_connected_sdr_clients.load();
		out_stats->preview_width = g_preview_width.load();
		out_stats->preview_height = g_preview_height.load();
		out_stats->encoded_width = g_stats_encoded_width.load();
		out_stats->encoded_height = g_stats_encoded_height.load();
		out_stats->encoded_fps = g_stats_encoded_fps.load();
		out_stats->sent_audio_frames = g_stats_sent_audio_frames;
		out_stats->sent_audio_bytes = g_stats_sent_audio_bytes;
		out_stats->audio_capture_state = g_stats_audio_capture_state.load();
		out_stats->audio_target_pid = g_stats_audio_target_pid.load();
		out_stats->reserved_audio_muted_sessions = 0;
		out_stats->local_audio_samples_read = g_stats_local_audio_samples_read.load();
		out_stats->encoded_video_bytes = g_stats_encoded_video_bytes;
		out_stats->sent_video_bytes = g_stats_sent_video_bytes;
		out_stats->encoded_audio_frames = g_stats_encoded_audio_frames;
		out_stats->encoded_audio_bytes = g_stats_encoded_audio_bytes;
	}

	UNITY_INTERFACE_EXPORT int SSPH_GetPeerStats(SSPH_PeerStats *out_stats, int max_count)
	{
		if (out_stats == nullptr || max_count <= 0)
		{
			return 0;
		}

		std::vector<HostPeerExportState> snapshot;
		{
			std::lock_guard<std::mutex> lock(g_host_peer_exports_mutex);
			snapshot = g_host_peer_exports;
		}

		std::sort(snapshot.begin(), snapshot.end(), [](const HostPeerExportState &a, const HostPeerExportState &b)
				  { return a.client_steam_id < b.client_steam_id; });
		const int count = std::min<int>(max_count, static_cast<int>(snapshot.size()));
		for (int i = 0; i < count; ++i)
		{
			const HostPeerExportState &source = snapshot[static_cast<std::size_t>(i)];
			out_stats[i].client_steam_id = source.client_steam_id;
			out_stats[i].encoded_frames = source.encoded_frames;
			out_stats[i].sent_chunks = source.sent_chunks;
			out_stats[i].sent_bytes = source.sent_bytes;
			out_stats[i].sent_audio_frames = source.sent_audio_frames;
			out_stats[i].sent_audio_bytes = source.sent_audio_bytes;
			out_stats[i].connected = source.connected;
			out_stats[i].connection_path = source.connection_path;
			out_stats[i].preview_width = source.preview_width;
			out_stats[i].preview_height = source.preview_height;
			out_stats[i].paused_by_client = source.paused_by_client;
			out_stats[i].assigned_video_lod = source.assigned_video_lod;
			out_stats[i].assigned_audio_lod = source.assigned_audio_lod;
			out_stats[i].max_video_lod = source.max_video_lod;
			out_stats[i].max_audio_lod = source.max_audio_lod;
			out_stats[i].available_video_lod = source.available_video_lod;
			out_stats[i].available_audio_lod = source.available_audio_lod;
			out_stats[i].effective_video_lod = source.effective_video_lod;
			out_stats[i].effective_audio_lod = source.effective_audio_lod;
			out_stats[i].encoded_video_bytes = source.encoded_video_bytes;
			out_stats[i].sent_video_bytes = source.sent_video_bytes;
			out_stats[i].encoded_audio_frames = source.encoded_audio_frames;
			out_stats[i].encoded_audio_bytes = source.encoded_audio_bytes;
			out_stats[i].video_lod_used = source.video_lod_used;
			out_stats[i].audio_lod_used = source.audio_lod_used;
			out_stats[i].last_video_send_ms = source.last_video_send_ms;
			out_stats[i].last_audio_send_ms = source.last_audio_send_ms;
		}
		return count;
	}

	UNITY_INTERFACE_EXPORT int SSPH_GetVideoLodStats(SSPH_VideoLodStats *out_stats, int max_count)
	{
		if (out_stats == nullptr || max_count <= 0)
		{
			return 0;
		}
		std::vector<HostVideoLodExportState> snapshot;
		{
			std::lock_guard<std::mutex> lock(g_host_lod_exports_mutex);
			snapshot = g_host_video_lod_exports;
		}
		const int count = std::min<int>(max_count, static_cast<int>(snapshot.size()));
		for (int i = 0; i < count; ++i)
		{
			const HostVideoLodExportState &source = snapshot[static_cast<std::size_t>(i)];
			out_stats[i].index = source.index;
			out_stats[i].enabled = source.enabled;
			out_stats[i].width = source.width;
			out_stats[i].height = source.height;
			out_stats[i].fps = source.fps;
			out_stats[i].target_bitrate_kbps = source.target_bitrate_kbps;
			out_stats[i].client_count = source.client_count;
			out_stats[i].encoded_frames = source.encoded_frames;
			out_stats[i].encoded_video_bytes = source.encoded_video_bytes;
			out_stats[i].last_encode_ms = source.last_encode_ms;
			out_stats[i].last_send_ms = source.last_send_ms;
		}
		return count;
	}

	UNITY_INTERFACE_EXPORT int SSPH_GetAudioLodStats(SSPH_AudioLodStats *out_stats, int max_count)
	{
		if (out_stats == nullptr || max_count <= 0)
		{
			return 0;
		}
		std::vector<HostAudioLodExportState> snapshot;
		{
			std::lock_guard<std::mutex> lock(g_host_lod_exports_mutex);
			snapshot = g_host_audio_lod_exports;
		}
		const int count = std::min<int>(max_count, static_cast<int>(snapshot.size()));
		for (int i = 0; i < count; ++i)
		{
			const HostAudioLodExportState &source = snapshot[static_cast<std::size_t>(i)];
			out_stats[i].index = source.index;
			out_stats[i].enabled = source.enabled;
			out_stats[i].bitrate_kbps = source.bitrate_kbps;
			out_stats[i].client_count = source.client_count;
			out_stats[i].encoded_audio_frames = source.encoded_audio_frames;
			out_stats[i].encoded_audio_bytes = source.encoded_audio_bytes;
			out_stats[i].last_encode_ms = source.last_encode_ms;
			out_stats[i].last_send_ms = source.last_send_ms;
		}
		return count;
	}

	UNITY_INTERFACE_EXPORT const char *SSPH_GetLastError()
	{
		std::lock_guard<std::mutex> lock(g_error_mutex);
		return g_last_error.c_str();
	}

} // extern "C"
