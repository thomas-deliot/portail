#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <deque>
#include <functional>
#include <iostream>
#include <limits>
#include <mutex>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include <windows.h>
#include <d3d11.h>
#include <d3d11_4.h>
#include <dxgi.h>
#include <dxgi1_2.h>
#include <share.h>
#include <wrl/client.h>

extern "C"
{
#include <libavcodec/avcodec.h>
}

#include "common/audio_stream.h"
#include "common/codec_utils.h"
#include "common/connection_path.h"
#include "common/d3d11_unity_utils.h"
#include "common/lod_utils.h"
#include "common/plugin_log.h"
#include "common/protocol.h"
#include "common/protocol_validation.h"
#include "common/steam_sdr.h"
#include "common/time_utils.h"
#include "common/unity_texture_copy.h"
#include "client/client_audio_pipeline.h"
#include "client/client_connection_state.h"
#include "client/client_peer_state.h"
#include "client/client_protocol_sender.h"
#include "client/client_quality_state.h"
#include "client/client_stats.h"
#include "client/ffmpeg_decoder.h"
#include "client/recovery_unit.h"
#include "client/client_video_decode_queue.h"
#include "client/client_video_state.h"
#include "plugin/client_plugin_api.h"
#include "IUnityGraphics.h"
#include "IUnityGraphicsD3D11.h"
#include "IUnityInterface.h"

namespace
{

	using Microsoft::WRL::ComPtr;
	using streamproto::SdrConfig;
	using streamproto::SdrRole;
	using streamproto::SteamSdr;
	using streamproto::ConnectionPath;
	using streamproto::ConnectionPathName;
	using streamproto::ExportConnectionPath;
	using streamproto::GetConnectionPath;
	using streamproto::client::ClientMaxAcceptedQuality;
	using streamproto::client::ClientAudioPipeline;
	using streamproto::client::ClientConnectionState;
	using streamproto::client::PeerExportState;
	using streamproto::client::UnityOutputBinding;
	using streamproto::client::ClientPeerQualityState;
	using streamproto::client::BuildClientStatsLine;
	using streamproto::client::ClientStats;
	using streamproto::client::ConfiguredVideoLod;
	using streamproto::client::FfmpegDecoder;
	using streamproto::client::ClientVideoDecodeEnqueueResult;
	using streamproto::client::ClientVideoDecodeEvent;
	using streamproto::client::ClientVideoState;
	using streamproto::client::VideoDecoderConfigKey;
	using streamproto::client::InspectRecoveryUnit;
	using streamproto::client::RecoveryUnitInfo;
	using streamproto::client::SendClientAppliedQuality;
	using streamproto::client::SendClientMaxAcceptedQuality;
	using streamproto::client::SendKeyframeRequest;
	using streamproto::client::SendPing;
	using streamproto::client::SendStreamControl;
	using streamproto::client::StreamConfigKey;
	namespace proto = streamproto::proto;

	constexpr int kClientRenderEventId = 0x1002;

	std::mutex g_error_mutex;
	std::string g_last_error;
	using ClientLogCallback = streamproto::log::Callback;
	std::mutex g_log_callback_mutex;
	ClientLogCallback g_log_callback = nullptr;
	std::atomic<bool> g_client_logging_enabled{true};

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

	void SetClientLogCallback(ClientLogCallback callback)
	{
		std::lock_guard<std::mutex> lock(g_log_callback_mutex);
		g_log_callback = callback;
	}

	ClientLogCallback GetClientLogCallback()
	{
		std::lock_guard<std::mutex> lock(g_log_callback_mutex);
		return g_log_callback;
	}

	void SetClientLoggingEnabled(bool enabled)
	{
		g_client_logging_enabled.store(enabled);
	}

	streamproto::log::Stream LogInfoStream()
	{
		return streamproto::log::Stream(std::cout, streamproto::log::Level::kInfo, GetClientLogCallback(), g_client_logging_enabled.load());
	}

	streamproto::log::Stream LogWarningStream()
	{
		return streamproto::log::Stream(std::cerr, streamproto::log::Level::kWarning, GetClientLogCallback(), g_client_logging_enabled.load());
	}

	streamproto::log::Stream LogErrorStream()
	{
		return streamproto::log::Stream(std::cerr, streamproto::log::Level::kError, GetClientLogCallback(), g_client_logging_enabled.load());
	}

	AVCodecID CodecFromProto(std::uint16_t codec)
	{
		if (codec == static_cast<std::uint16_t>(proto::Codec::kHEVC))
		{
			return AV_CODEC_ID_HEVC;
		}
		if (codec == static_cast<std::uint16_t>(proto::Codec::kAV1))
		{
			return AV_CODEC_ID_AV1;
		}
		return AV_CODEC_ID_H264;
	}

	bool SameVideoDecoderConfig(const proto::StreamConfigMessage &lhs, const proto::StreamConfigMessage &rhs)
	{
		return lhs.codec == rhs.codec &&
			   lhs.width == rhs.width &&
			   lhs.height == rhs.height &&
			   lhs.fps == rhs.fps;
	}

	VideoDecoderConfigKey DecoderKeyFromStreamConfig(const proto::StreamConfigMessage &cfg)
	{
		VideoDecoderConfigKey key{};
		key.codec = cfg.codec;
		key.width = cfg.width;
		key.height = cfg.height;
		key.fps = cfg.fps;
		return key;
	}

	VideoDecoderConfigKey DecoderKeyFromConfiguredLod(const ConfiguredVideoLod &lod)
	{
		VideoDecoderConfigKey key{};
		key.codec = lod.codec;
		key.width = lod.width;
		key.height = lod.height;
		key.fps = lod.fps;
		return key;
	}

	proto::StreamConfigMessage StreamConfigFromConfiguredLod(const ConfiguredVideoLod &lod, std::int32_t lod_index, std::uint32_t generation)
	{
		proto::StreamConfigMessage cfg{};
		proto::InitMessageHeader(cfg, proto::MessageType::kStreamConfig);
		cfg.codec = lod.codec;
		cfg.width = lod.width;
		cfg.height = lod.height;
		cfg.fps = lod.fps;
		cfg.bitrate_kbps = 100;
		cfg.chunk_payload_bytes = 1200;
		cfg.parity_shards = 0;
		cfg.reserved = static_cast<std::uint8_t>(lod_index >= 0 && lod_index < 255 ? lod_index + 1 : 0);
		cfg.lod_index = lod_index;
		cfg.stream_generation = generation;
		return cfg;
	}

	std::string DecoderConfigKeyLabel(const VideoDecoderConfigKey &key)
	{
		std::ostringstream s;
		s << "codec" << key.codec << "/" << key.width << "x" << key.height << "@" << key.fps;
		return s.str();
	}

	void PrintStatsLine(
		const ClientStats &stats,
		const std::string &decoder_name,
		const char *path_name,
		bool connected,
		std::uint64_t delta_ms,
		ClientStats &last_snapshot,
		FILE *stats_log_file)
	{
		const std::string text = BuildClientStatsLine(stats, decoder_name, path_name, connected, delta_ms, last_snapshot);

		LogInfoStream() << '\r' << text;
		if (stats_log_file != nullptr)
		{
			std::fputs(text.c_str(), stats_log_file);
			std::fputc('\n', stats_log_file);
			std::fflush(stats_log_file);
		}

		last_snapshot = stats;
	}

} // namespace

namespace
{

	std::thread g_client_thread;
	std::mutex g_client_thread_mutex;
	std::atomic<bool> g_client_thread_running{false};
	std::atomic<bool> g_client_stop_requested{false};
	std::atomic<bool> g_client_pause_requested{false};
	std::atomic<std::uint64_t> g_client_local_steam_id{0};
	std::mutex g_desired_hosts_mutex;
	std::vector<std::uint64_t> g_desired_hosts;
	std::atomic<bool> g_desired_hosts_dirty{false};
	std::mutex g_max_accepted_qualities_mutex;
	std::unordered_map<std::uint64_t, ClientMaxAcceptedQuality> g_max_accepted_qualities;
	std::atomic<bool> g_max_accepted_qualities_dirty{false};
	std::mutex g_client_video_lod_configs_mutex;
	std::vector<ConfiguredVideoLod> g_client_video_lod_configs;
	std::atomic<bool> g_client_video_lod_configs_dirty{false};
	std::mutex g_peer_exports_mutex;
	std::unordered_map<std::uint64_t, PeerExportState> g_peer_exports;
	std::mutex g_peer_audio_mutex;
	std::unordered_map<std::uint64_t, std::shared_ptr<streamproto::audio::PcmRingBuffer>> g_peer_audio_rings;
	std::unordered_map<std::uint64_t, UnityOutputBinding> g_unity_output_bindings;

	std::vector<std::uint64_t> CopyDesiredHosts()
	{
		std::lock_guard<std::mutex> lock(g_desired_hosts_mutex);
		return g_desired_hosts;
	}

	std::unordered_map<std::uint64_t, ClientMaxAcceptedQuality> CopyMaxAcceptedQualities()
	{
		std::lock_guard<std::mutex> lock(g_max_accepted_qualities_mutex);
		return g_max_accepted_qualities;
	}

	std::vector<ConfiguredVideoLod> CopyClientVideoLodConfigs()
	{
		std::lock_guard<std::mutex> lock(g_client_video_lod_configs_mutex);
		return g_client_video_lod_configs;
	}

	constexpr std::int32_t kStreamLodOff = streamproto::lod::kStreamLodOff;
	constexpr std::int32_t kInvalidSentLod = streamproto::lod::kInvalidSentLod;

	inline constexpr auto LodEnabled = streamproto::lod::Enabled;
	inline constexpr auto NormalizeLod = streamproto::lod::Normalize;
	inline constexpr auto BestAvailableLod = streamproto::lod::BestAvailable;

	std::uint64_t FirstDesiredHost()
	{
		std::lock_guard<std::mutex> lock(g_desired_hosts_mutex);
		return g_desired_hosts.empty() ? 0 : g_desired_hosts.front();
	}

	class ClientPeerRunner
	{
	public:
		ClientPeerRunner(
			std::uint32_t app_id,
			std::uint64_t host_id,
			bool disable_ice,
			std::vector<ConfiguredVideoLod> video_lod_configs) : app_id_(app_id), host_id_(host_id), disable_ice_(disable_ice)
		{
			video_.configured_lods = std::move(video_lod_configs);
		}
		~ClientPeerRunner() { Shutdown(); }

		void SetMaxAcceptedQuality(const ClientMaxAcceptedQuality &quality)
		{
			std::lock_guard<std::mutex> lock(peer_mutex_);
			(void)quality_.SetMaxAcceptedQuality(quality);
		}

		void SetVideoLodConfigs(std::vector<ConfiguredVideoLod> video_lod_configs)
		{
			std::lock_guard<std::mutex> lock(peer_mutex_);
			video_.configured_lods = std::move(video_lod_configs);
			PopulateConfiguredVideoStreamConfigs();
			WarmConfiguredVideoDecoders();
		}

		bool Initialize(std::string &error)
		{
			if (app_id_ == 0 || host_id_ == 0)
			{
				error = "invalid-peer-config";
				return false;
			}
			SdrConfig c{};
			c.app_id = app_id_;
			c.role = SdrRole::kClient;
			c.remote_steam_id = host_id_;
			c.virtual_port = streamproto::kAutoStreamingVirtualPort;
			c.send_rate_bytes_per_sec = 16 * 1024 * 1024;
			c.send_buffer_size = 8 * 1024 * 1024;
			c.recv_buffer_size = 8 * 1024 * 1024;
			c.nagle_time_usec = 0;
			c.p2p_ice_enable = disable_ice_
								   ? k_nSteamNetworkingConfig_P2P_Transport_ICE_Enable_Disable
								   : k_nSteamNetworkingConfig_P2P_Transport_ICE_Enable_All;
			c.p2p_ice_penalty = 0;
			c.p2p_sdr_penalty = 0;
			c.debug_output_level = k_ESteamNetworkingSocketsDebugOutputType_Msg;
			c.p2p_log_level = k_ESteamNetworkingSocketsDebugOutputType_Msg;
			connection_.sdr = std::make_unique<SteamSdr>(c);
			if (!connection_.sdr->Initialize(error))
			{
				connection_.sdr.reset();
				return false;
			}
			audio_pipeline_.ResetStats();
			std::shared_ptr<streamproto::audio::PcmRingBuffer> audio_ring = audio_pipeline_.EnsureRing();
			{
				std::lock_guard<std::mutex> lock(g_peer_audio_mutex);
				g_peer_audio_rings[host_id_] = audio_ring;
			}
			audio_pipeline_.Start();
			StartVideoDecodeWorker();
			PopulateConfiguredVideoStreamConfigs();
			WarmConfiguredVideoDecoders();
			last_stats_ms_ = streamproto::NowSteadyMillis();
			return true;
		}

		bool Start()
		{
			std::lock_guard<std::mutex> lock(peer_worker_mutex_);
			if (peer_worker_.joinable())
			{
				return true;
			}
			if (connection_.sdr == nullptr)
			{
				return false;
			}
			peer_worker_stop_.store(false, std::memory_order_release);
			peer_worker_ = std::thread([this]()
									   { PeerWorkerLoop(); });
			return true;
		}

		void Shutdown()
		{
			StopPeerWorker();
			StopVideoDecodeWorker();
			audio_pipeline_.Stop();
			std::lock_guard<std::mutex> lock(peer_mutex_);
			{
				std::lock_guard<std::mutex> audio_map_lock(g_peer_audio_mutex);
				g_peer_audio_rings.erase(host_id_);
			}
			if (connection_.sdr != nullptr)
			{
				connection_.sdr->Close("client-peer-shutdown");
				connection_.sdr.reset();
			}
			ShutdownVideoDecoders();
			audio_pipeline_.Shutdown();
			video_.assembler.Reset();
			video_.has_stream_config = false;
			video_.has_cfg = false;
			connection_.host_paused = false;
			connection_.client_pause_sent = false;
			video_.stream_configs_by_key.clear();
			audio_configs_by_key_.clear();
			quality_.Reset();
		}

		void Tick(bool pause_now)
		{
			if (connection_.sdr == nullptr)
			{
				return;
			}
			const bool connected = connection_.sdr->IsConnected();
			if (connected)
			{
				std::uint64_t now_ms = streamproto::NowSteadyMillis();
				if (now_ms - connection_.last_path_probe_ms >= 500)
				{
					connection_.last_path_probe_ms = now_ms;
					ConnectionPath p = connection_.active_path;
					if (GetConnectionPath(*connection_.sdr, p))
					{
						connection_.active_path = p;
					}
				}
				if (pause_now != connection_.client_pause_sent)
				{
					SendStreamControl(*connection_.sdr,
									  pause_now ? proto::StreamControlCommand::kClientPause : proto::StreamControlCommand::kClientResume,
									  connection_.sdr->LocalSteamId());
					connection_.client_pause_sent = pause_now;
				}
				if (quality_.NeedsMaxAcceptedSend())
				{
					if (SendClientMaxAcceptedQuality(*connection_.sdr, quality_.max_accepted, connection_.sdr->LocalSteamId()))
					{
						quality_.MarkMaxAcceptedSent();
					}
				}
				const bool video_ack_pending = quality_.VideoAckPending();
				const bool audio_ack_pending = quality_.AudioAckPending();
				if (video_ack_pending || audio_ack_pending)
				{
					if (SendClientAppliedQuality(
							*connection_.sdr,
							connection_.sdr->LocalSteamId(),
							quality_.effective_video_lod,
							quality_.effective_audio_lod,
							quality_.ack_pending_video_generation != 0 ? quality_.ack_pending_video_generation : quality_.active_video_generation,
							quality_.ack_pending_audio_generation != 0 ? quality_.ack_pending_audio_generation : quality_.active_audio_generation))
					{
						quality_.MarkVideoAckSent();
						quality_.MarkAudioAckSent();
					}
				}
				if (now_ms - connection_.clock_sync.last_ping_ms >= 500)
				{
					std::uint64_t t = streamproto::NowUnixMicros();
					if (SendPing(*connection_.sdr, connection_.clock_sync.next_ping_sequence++, t))
					{
						connection_.clock_sync.last_ping_ms = now_ms;
					}
				}
			}
			else
			{
				connection_.active_path = ConnectionPath::kSdr;
				connection_.last_path_probe_ms = 0;
				connection_.client_pause_sent = false;
				connection_.host_paused = false;
				quality_.MarkDisconnected();
				audio_pipeline_.SetClockSync(false, 0.0);
			}

			connection_.sdr->Receive([&](const std::uint8_t *data, std::size_t bytes)
						  { HandlePacket(data, bytes, pause_now); });

			std::optional<std::uint32_t> dropped_first{};
			std::uint64_t dropped = video_.assembler.DropTimedOut(streamproto::NowSteadyMillis(), 250, dropped_first);
			if (dropped > 0)
			{
				stats_.dropped_frames += dropped;
				if (dropped_first.has_value())
				{
					EnterKeyframeWait(*dropped_first, video_.last_assembled_frame != 0);
				}
				if (video_.last_assembled_frame == 0)
				{
					video_.waiting_for_keyframe = true;
					video_.skip_next_recovery_flush = false;
					video_.no_output_packets = 0;
					video_.assembler.Reset();
					FlushActiveVideoDecoder();
				}
			}
			if (connected && video_.has_stream_config && video_.waiting_for_keyframe && !pause_now && !connection_.host_paused)
			{
				std::uint64_t now_ms = streamproto::NowSteadyMillis();
				if (!video_.deferred_loss_recovery || now_ms - video_.deferred_loss_start_ms >= 500)
				{
					video_.deferred_loss_recovery = false;
					RequestKeyframe(video_.last_assembled_frame + 1);
				}
			}
			PumpPendingKeyframeRequest();

			std::uint64_t now_ms = streamproto::NowSteadyMillis();
			if (now_ms - last_stats_ms_ >= 1000)
			{
				std::string d = video_.active_decoder + "@" + std::to_string(host_id_);
				ClientStats stats_snapshot = SnapshotStats();
				PrintStatsLine(stats_snapshot, d, ConnectionPathName(connection_.active_path), connected, now_ms - last_stats_ms_, snapshot_, nullptr);
				last_stats_ms_ = now_ms;
			}
		}

		PeerExportState Export() const
		{
			std::lock_guard<std::mutex> lock(peer_mutex_);
			PeerExportState s{};
			const bool connected = (connection_.sdr != nullptr && connection_.sdr->IsConnected());
			s.host_steam_id = host_id_;
			{
				std::lock_guard<std::mutex> video_lock(video_.decode_mutex);
				s.shared_handle = video_.gpu_bridge.Ready() ? video_.gpu_bridge.SharedHandle() : 0;
				s.preview_width = video_.gpu_bridge.Ready() ? video_.gpu_bridge.Width() : 0;
				s.preview_height = video_.gpu_bridge.Ready() ? video_.gpu_bridge.Height() : 0;
			}
			const ClientStats snapshot = SnapshotStats();
			s.last_video_capture_ts_us = video_.last_gpu_capture_ts_us;
			s.recv_chunks = snapshot.recv_chunks;
			s.recv_bytes = snapshot.recv_bytes;
			s.video_bytes = snapshot.video_bytes;
			s.decoded_frames = snapshot.decoded_frames;
			s.audio_packets = snapshot.audio_packets;
			s.audio_bytes = snapshot.audio_bytes;
			s.audio_frames = snapshot.audio_frames;
			s.last_video_reassemble_ms = snapshot.last_video_reassemble_ms;
			s.last_decode_ms = snapshot.last_decode_core_ms;
			s.last_post_decode_ms = snapshot.last_post_decode_ms;
			s.last_video_texture_copy_ms = snapshot.last_video_texture_copy_ms;
			s.last_total_latency_ms = snapshot.last_total_latency_ms;
			s.last_video_capture_to_texture_ms = snapshot.last_video_capture_to_texture_ms;
			s.last_audio_decode_ms = snapshot.last_audio_decode_ms;
			s.last_audio_capture_to_push_ms = snapshot.last_audio_capture_to_push_ms;
			s.last_audio_capture_to_unity_push_ms = snapshot.last_audio_capture_to_unity_push_ms;
			s.last_audio_capture_ts_us = snapshot.last_audio_capture_ts_us;
			s.clock_offset_ms = snapshot.clock_offset_ms;
			s.clock_synced = snapshot.clock_synced ? 1 : 0;
			s.connected = connected ? 1 : 0;
			s.connection_path = ExportConnectionPath(connection_.active_path, connected);
			s.paused_by_host = connection_.host_paused ? 1 : 0;
			s.assigned_video_lod = quality_.assigned_video_lod;
			s.assigned_audio_lod = quality_.assigned_audio_lod;
			s.max_video_lod = quality_.max_accepted.video_lod;
			s.max_audio_lod = quality_.max_accepted.audio_lod;
			s.available_video_lod = quality_.available_video_lod;
			s.available_audio_lod = quality_.available_audio_lod;
			s.effective_video_lod = quality_.effective_video_lod;
			s.effective_audio_lod = quality_.effective_audio_lod;
			return s;
		}

		std::uint64_t LocalSteamId() const
		{
			std::lock_guard<std::mutex> lock(peer_mutex_);
			return connection_.sdr != nullptr ? connection_.sdr->LocalSteamId() : 0;
		}
		bool NeedsReconnect() const
		{
			std::lock_guard<std::mutex> lock(peer_mutex_);
			return connection_.sdr != nullptr &&
				   !connection_.sdr->IsConnected() &&
				   connection_.sdr->ConnectionHandle() == k_HSteamNetConnection_Invalid;
		}

	private:
		void PeerWorkerLoop()
		{
			while (!peer_worker_stop_.load(std::memory_order_acquire) &&
				   !g_client_stop_requested.load(std::memory_order_acquire))
			{
				{
					std::lock_guard<std::mutex> lock(peer_mutex_);
					Tick(g_client_pause_requested.load(std::memory_order_relaxed));
				}
				std::this_thread::sleep_for(std::chrono::milliseconds(1));
			}
		}

		void StopPeerWorker()
		{
			std::lock_guard<std::mutex> lock(peer_worker_mutex_);
			peer_worker_stop_.store(true, std::memory_order_release);
			if (peer_worker_.joinable())
			{
				peer_worker_.join();
			}
		}

		void StartVideoDecodeWorker()
		{
			video_.decode_queue.Start([this](ClientVideoDecodeEvent event)
									  { ProcessVideoDecodeEvent(std::move(event)); });
		}

		void StopVideoDecodeWorker()
		{
			video_.decode_queue.Stop();
		}

		void ClearPendingVideoDecodeEvents()
		{
			video_.decode_queue.Clear();
		}

		void EnqueueVideoDecodeEvent(ClientVideoDecodeEvent event)
		{
			const ClientVideoDecodeEnqueueResult result = video_.decode_queue.Enqueue(std::move(event));
			if (result.dropped_completed_frames)
			{
				stats_.dropped_frames += result.dropped_frame_count;
				video_.waiting_for_keyframe = true;
				video_.skip_next_recovery_flush = false;
				video_.no_output_packets = 0;
				FlushActiveVideoDecoder();
				if (result.request_keyframe)
				{
					RequestKeyframe(result.request_frame_id);
				}
			}
		}

		void ProcessVideoDecodeEvent(ClientVideoDecodeEvent event)
		{
			if (event.bytes.empty())
			{
				return;
			}
			{
				std::lock_guard<std::mutex> peer_lock(peer_mutex_);
				if (event.lod_index != quality_.effective_video_lod ||
					event.stream_generation != quality_.active_video_generation)
				{
					return;
				}
			}

			double dec_ms = 0.0;
			double post_ms = 0.0;
			std::uint64_t decoded = 0;
			std::uint64_t gpu_frames = 0;
			std::uint64_t gpu_copy_fail = 0;
			bool had_decoder = false;
			bool ok = false;
			{
				std::lock_guard<std::mutex> video_lock(video_.decode_mutex);
				FfmpegDecoder *active_decoder = ActiveVideoDecoder();
				if (active_decoder != nullptr)
				{
					had_decoder = true;
					ok = active_decoder->Decode(
						event.bytes.data(),
						event.bytes.size(),
						event.recovery.valid,
						dec_ms,
						post_ms,
						decoded,
						[&](const AVFrame *frame)
						{
							if (frame == nullptr)
							{
								return;
							}
							if (active_decoder->IsHardwareFrame(frame) &&
								video_.gpu_bridge.Device() != nullptr &&
								video_.gpu_bridge.Context() != nullptr)
							{
								if (video_.gpu_bridge.UpdateFromFrame(frame))
								{
									++gpu_frames;
								}
								else
								{
									++gpu_copy_fail;
								}
							}
						});
				}
			}

			std::lock_guard<std::mutex> peer_lock(peer_mutex_);
			if (!had_decoder)
			{
				stats_.decode_fail++;
				EnterKeyframeWait(video_.last_assembled_frame + 1, false);
				return;
			}

			stats_.last_video_reassemble_ms = event.reassembly_cpu_ms;
			stats_.last_decode_core_ms = dec_ms;
			stats_.last_post_decode_ms = post_ms;
			stats_.gpu_frames += gpu_frames;
			stats_.gpu_copy_fail += gpu_copy_fail;
			if (!ok)
			{
				stats_.decode_fail++;
				video_.consecutive_decode_failures++;
				video_.no_output_packets = 0;
				video_.assembler.Reset();
				EnterKeyframeWait(video_.last_assembled_frame + 1, false);
				return;
			}

			video_.consecutive_decode_failures = 0;
			stats_.decoded_frames += decoded;
			if (decoded == 0)
			{
				++video_.no_output_packets;
				if (!video_.waiting_for_keyframe && video_.no_output_packets >= 120)
				{
					video_.no_output_packets = 0;
					EnterKeyframeWait(video_.last_assembled_frame + 1, false);
				}
				return;
			}
			if (gpu_frames > 0)
			{
				video_.last_gpu_frame = event.frame_id;
				video_.last_gpu_capture_ts_us = event.capture_ts_us;
			}
			if (event.capture_ts_us != 0)
			{
				std::int64_t host_done_us = static_cast<std::int64_t>(streamproto::NowUnixMicros());
				if (connection_.clock_sync.valid)
				{
					host_done_us += static_cast<std::int64_t>(std::llround(connection_.clock_sync.offset_us));
				}
				if (host_done_us >= 0 && static_cast<std::uint64_t>(host_done_us) >= event.capture_ts_us)
				{
					double total_ms = static_cast<double>(static_cast<std::uint64_t>(host_done_us) - event.capture_ts_us) / 1000.0;
					stats_.last_net_latency_ms = event.reassembly_ms;
					stats_.last_total_latency_ms = total_ms;
					stats_.max_total_latency_ms = std::max(stats_.max_total_latency_ms, total_ms);
					if (stats_.latency_samples == 0)
					{
						stats_.avg_net_latency_ms = event.reassembly_ms;
						stats_.avg_total_latency_ms = total_ms;
					}
					else
					{
						constexpr double a = 0.1;
						stats_.avg_net_latency_ms += (event.reassembly_ms - stats_.avg_net_latency_ms) * a;
						stats_.avg_total_latency_ms += (total_ms - stats_.avg_total_latency_ms) * a;
					}
					stats_.latency_samples++;
				}
			}
			video_.no_output_packets = 0;
			video_.last_completed_frame = event.frame_id;
		}

		ClientStats SnapshotStats() const
		{
			ClientStats s = stats_;
			audio_pipeline_.AppendStats(s);
			return s;
		}

		FfmpegDecoder *ActiveVideoDecoder()
		{
			if (!video_.has_active_decoder_key)
			{
				return nullptr;
			}
			auto it = video_.decoders_by_config.find(video_.active_decoder_key);
			if (it == video_.decoders_by_config.end() || it->second == nullptr || !it->second->Ready())
			{
				return nullptr;
			}
			return it->second.get();
		}

		const FfmpegDecoder *ActiveVideoDecoder() const
		{
			if (!video_.has_active_decoder_key)
			{
				return nullptr;
			}
			auto it = video_.decoders_by_config.find(video_.active_decoder_key);
			if (it == video_.decoders_by_config.end() || it->second == nullptr || !it->second->Ready())
			{
				return nullptr;
			}
			return it->second.get();
		}

		bool CanUseActiveVideoDecoderForConfig(AVCodecID codec_id, const proto::StreamConfigMessage &cfg) const
		{
			const VideoDecoderConfigKey cfg_key = DecoderKeyFromStreamConfig(cfg);
			if (!video_.has_cfg || !video_.has_active_decoder_key || video_.active_decoder_key != cfg_key)
			{
				return false;
			}
			const FfmpegDecoder *active_decoder = ActiveVideoDecoder();
			return active_decoder != nullptr && active_decoder->CodecId() == codec_id;
		}

		bool EnsureVideoDecoderForKey(const VideoDecoderConfigKey &key, std::string &error)
		{
			std::lock_guard<std::mutex> lock(video_.decode_mutex);
			if (!key.Valid())
			{
				error = "Cannot create decoder for invalid video config.";
				return false;
			}
			const AVCodecID codec_id = CodecFromProto(key.codec);
			auto &decoder = video_.decoders_by_config[key];
			if (decoder != nullptr && decoder->Ready() && decoder->CodecId() == codec_id)
			{
				return true;
			}
			auto next = std::make_unique<FfmpegDecoder>();
			if (!next->Init(codec_id, key.width, key.height, key.fps, video_.shared_decoder_hw_device_ctx, error))
			{
				return false;
			}
			if (video_.shared_decoder_hw_device_ctx == nullptr && next->HardwareDeviceContext() != nullptr)
			{
				video_.shared_decoder_hw_device_ctx = av_buffer_ref(next->HardwareDeviceContext());
				if (video_.shared_decoder_hw_device_ctx == nullptr)
				{
					error = "Failed to retain shared decoder hardware device context.";
					return false;
				}
			}
			decoder = std::move(next);
			return true;
		}

		bool TryGetConfiguredVideoLod(std::int32_t lod, ConfiguredVideoLod &out) const
		{
			lod = NormalizeLod(lod);
			if (!LodEnabled(lod))
			{
				return false;
			}
			const std::size_t index = static_cast<std::size_t>(lod);
			if (index >= video_.configured_lods.size())
			{
				return false;
			}
			out = video_.configured_lods[index];
			return out.enabled;
		}

		bool TryGetVideoConfigForGeneration(std::int32_t lod, std::uint32_t generation, proto::StreamConfigMessage &out) const
		{
			lod = NormalizeLod(lod);
			if (!LodEnabled(lod))
			{
				return false;
			}
			if (generation != 0)
			{
				const auto cfg_it = video_.stream_configs_by_key.find(StreamConfigKey(lod, generation));
				if (cfg_it != video_.stream_configs_by_key.end())
				{
					out = cfg_it->second;
					return true;
				}
			}

			ConfiguredVideoLod configured{};
			if (!TryGetConfiguredVideoLod(lod, configured))
			{
				return false;
			}
			out = StreamConfigFromConfiguredLod(configured, lod, generation);
			return true;
		}

		void PopulateConfiguredVideoStreamConfigs()
		{
			if (quality_.desired_video_generation != 0 && LodEnabled(quality_.desired_video_lod))
			{
				ConfiguredVideoLod configured{};
				if (TryGetConfiguredVideoLod(quality_.desired_video_lod, configured))
				{
					video_.stream_configs_by_key.emplace(
						StreamConfigKey(quality_.desired_video_lod, quality_.desired_video_generation),
						StreamConfigFromConfiguredLod(configured, quality_.desired_video_lod, quality_.desired_video_generation));
				}
			}
		}

		void WarmConfiguredVideoDecoders()
		{
			for (const ConfiguredVideoLod &lod : video_.configured_lods)
			{
				if (!lod.enabled)
				{
					continue;
				}
				const VideoDecoderConfigKey key = DecoderKeyFromConfiguredLod(lod);
				std::string error;
				if (!EnsureVideoDecoderForKey(key, error))
				{
					LogWarningStream() << "path=" << ConnectionPathName(connection_.active_path)
									   << " peer=" << host_id_
									   << " " << DecoderConfigKeyLabel(key)
									   << " decoder warmup failed: " << error;
				}
			}
		}

		void WarmRelevantVideoDecoders()
		{
			std::array<std::int32_t, 2> warm_lods{
				quality_.effective_video_lod,
				quality_.desired_video_lod};
			for (std::int32_t lod : warm_lods)
			{
				if (!LodEnabled(lod))
				{
					continue;
				}
				if ((quality_.available_video_lod_mask & (1ULL << static_cast<unsigned>(lod))) == 0)
				{
					continue;
				}
				if (lod == quality_.desired_video_lod && quality_.desired_video_generation != 0)
				{
					const auto cfg_it = video_.stream_configs_by_key.find(StreamConfigKey(lod, quality_.desired_video_generation));
					if (cfg_it != video_.stream_configs_by_key.end())
					{
						const AVCodecID cfg_codec = CodecFromProto(cfg_it->second.codec);
						std::lock_guard<std::mutex> lock(video_.decode_mutex);
						if (CanUseActiveVideoDecoderForConfig(cfg_codec, cfg_it->second))
						{
							continue;
						}
					}
				}
				proto::StreamConfigMessage cfg{};
				if (!TryGetVideoConfigForGeneration(lod, quality_.desired_video_generation, cfg))
				{
					continue;
				}
				std::string error;
				const VideoDecoderConfigKey key = DecoderKeyFromStreamConfig(cfg);
				if (!EnsureVideoDecoderForKey(key, error))
				{
					LogWarningStream() << "path=" << ConnectionPathName(connection_.active_path)
									   << " peer=" << host_id_
									   << " V_lod" << lod
					<< " decoder warmup failed: " << error;
				}
			}
		}

		void FlushActiveVideoDecoder()
		{
			std::lock_guard<std::mutex> lock(video_.decode_mutex);
			if (FfmpegDecoder *decoder = ActiveVideoDecoder())
			{
				decoder->Flush();
			}
		}

		void ShutdownVideoDecoders()
		{
			std::lock_guard<std::mutex> lock(video_.decode_mutex);
			video_.gpu_bridge.Shutdown();
			video_.decoders_by_config.clear();
			if (video_.shared_decoder_hw_device_ctx != nullptr)
			{
				av_buffer_unref(&video_.shared_decoder_hw_device_ctx);
				video_.shared_decoder_hw_device_ctx = nullptr;
			}
			video_.stream_configs_by_key.clear();
			video_.active_decoder_lod = kStreamLodOff;
			video_.active_decoder_key = {};
			video_.has_active_decoder_key = false;
			video_.codec_id = AV_CODEC_ID_NONE;
			video_.active_decoder = "n/a";
			video_.has_stream_config = false;
			video_.has_cfg = false;
			video_.current_cfg = {};
			video_.skip_next_recovery_flush = false;
		}

		bool ActivateStreamConfigForGeneration(
			std::int32_t cfg_lod,
			std::uint32_t stream_generation,
			const proto::StreamConfigMessage &cfg)
		{
			if (!LodEnabled(cfg_lod) || stream_generation == 0)
			{
				return false;
			}

			const AVCodecID cfg_codec = CodecFromProto(cfg.codec);
			bool use_active_decoder = false;
			{
				std::lock_guard<std::mutex> video_lock(video_.decode_mutex);
				use_active_decoder = CanUseActiveVideoDecoderForConfig(cfg_codec, cfg);
			}
			if (!use_active_decoder)
			{
				std::string warmup_error;
				if (!EnsureVideoDecoderForKey(DecoderKeyFromStreamConfig(cfg), warmup_error))
				{
					LogErrorStream() << "path=" << ConnectionPathName(connection_.active_path)
									 << " peer=" << host_id_
									 << " Decoder init failed: " << warmup_error;
					return false;
				}
			}

			const bool decoder_must_change = !use_active_decoder;
			const bool generation_changed = quality_.active_video_generation != stream_generation;
			const bool stream_state_changed = decoder_must_change || generation_changed;
			if (stream_state_changed)
			{
				ClearPendingVideoDecodeEvents();
				std::string e;
				{
					std::lock_guard<std::mutex> video_lock(video_.decode_mutex);
					const VideoDecoderConfigKey decoder_key = DecoderKeyFromStreamConfig(cfg);
					const std::int32_t previous_decoder_lod = video_.active_decoder_lod;
					FfmpegDecoder *decoder = nullptr;
					auto decoder_it = video_.decoders_by_config.find(decoder_key);
					if (decoder_it != video_.decoders_by_config.end() && decoder_it->second != nullptr)
					{
						decoder = decoder_it->second.get();
					}
					if (decoder != nullptr && decoder->UsingHardwareDecode())
					{
						if (!video_.gpu_bridge.Init(decoder->HardwareDeviceContext(), e))
						{
							LogWarningStream() << "path=" << ConnectionPathName(connection_.active_path)
											   << " peer=" << host_id_
											   << " GPU bridge warning: " << e;
						}
					}
					if (decoder != nullptr)
					{
						video_.active_decoder = decoder->ActiveDecoder() + "/lod" + std::to_string(cfg_lod);
						if (use_active_decoder && previous_decoder_lod != cfg_lod)
						{
							video_.active_decoder += " (shared with lod" + std::to_string(previous_decoder_lod) + ")";
						}
					}
					else
					{
						video_.active_decoder = "n/a";
					}
					if (decoder != nullptr && decoder_must_change)
					{
						decoder->Flush();
					}
					video_.active_decoder_lod = cfg_lod;
					video_.active_decoder_key = decoder_key;
					video_.has_active_decoder_key = true;
				}
				video_.has_stream_config = true;
				video_.waiting_for_keyframe = true;
				video_.skip_next_recovery_flush = use_active_decoder && generation_changed && !decoder_must_change;
				video_.no_output_packets = 0;
				video_.consecutive_decode_failures = 0;
				video_.last_completed_frame = 0;
				video_.last_assembled_frame = 0;
				video_.last_keyreq_ms = 0;
				video_.pending_keyframe_request = false;
				video_.pending_keyframe_first_missing = 1;
				video_.deferred_loss_recovery = false;
				video_.assembler.Reset();
			}
			video_.current_cfg = cfg;
			video_.has_cfg = true;
			video_.has_stream_config = true;
			return true;
		}

		bool ActivateAudioConfigForGeneration(
			std::int32_t cfg_lod,
			std::uint32_t stream_generation,
			const proto::AudioConfigMessage &cfg)
		{
			if (!LodEnabled(cfg_lod) || stream_generation == 0)
			{
				return false;
			}
			std::string error;
			if (!audio_pipeline_.Activate(cfg_lod, stream_generation, cfg, error))
			{
				LogWarningStream() << "path=audio peer=" << host_id_
								   << " Audio decoder init failed: " << error;
				return false;
			}
			return true;
		}

		void DeactivateVideo()
		{
			ClearPendingVideoDecodeEvents();
			video_.assembler.Reset();
			FlushActiveVideoDecoder();
			video_.has_stream_config = false;
			video_.waiting_for_keyframe = true;
			video_.skip_next_recovery_flush = false;
			video_.no_output_packets = 0;
			video_.last_completed_frame = 0;
			video_.last_assembled_frame = 0;
		}

		void DeactivateAudio()
		{
			audio_pipeline_.Deactivate();
		}

		bool TryActivatePendingVideoGeneration()
		{
			if (quality_.desired_video_generation == 0 || quality_.desired_video_generation == quality_.active_video_generation)
			{
				return false;
			}

			const proto::StreamConfigMessage *video_cfg = nullptr;
			proto::StreamConfigMessage local_video_cfg{};
			if (LodEnabled(quality_.desired_video_lod))
			{
				if (!TryGetVideoConfigForGeneration(quality_.desired_video_lod, quality_.desired_video_generation, local_video_cfg))
				{
					return false;
				}
				video_cfg = &local_video_cfg;
			}

			if (video_cfg != nullptr)
			{
				if (!ActivateStreamConfigForGeneration(quality_.desired_video_lod, quality_.desired_video_generation, *video_cfg))
				{
					return false;
				}
			}
			else
			{
				DeactivateVideo();
			}

			quality_.effective_video_lod = quality_.desired_video_lod;
			quality_.active_video_generation = quality_.desired_video_generation;
			quality_.ack_pending_video_generation = quality_.active_video_generation;
			return true;
		}

		bool TryActivatePendingAudioGeneration()
		{
			if (quality_.desired_audio_generation == 0 || quality_.desired_audio_generation == quality_.active_audio_generation)
			{
				return false;
			}

			const proto::AudioConfigMessage *audio_cfg = nullptr;
			if (LodEnabled(quality_.desired_audio_lod))
			{
				const auto cfg_it = audio_configs_by_key_.find(StreamConfigKey(quality_.desired_audio_lod, quality_.desired_audio_generation));
				if (cfg_it == audio_configs_by_key_.end())
				{
					return false;
				}
				audio_cfg = &cfg_it->second;
			}

			if (audio_cfg != nullptr)
			{
				if (!ActivateAudioConfigForGeneration(quality_.desired_audio_lod, quality_.desired_audio_generation, *audio_cfg))
				{
					return false;
				}
			}
			else
			{
				DeactivateAudio();
			}

			quality_.effective_audio_lod = quality_.desired_audio_lod;
			quality_.active_audio_generation = quality_.desired_audio_generation;
			quality_.ack_pending_audio_generation = quality_.active_audio_generation;
			return true;
		}

		void RequestKeyframe(std::uint32_t first_missing)
		{
			if (connection_.sdr == nullptr)
				return;
			std::uint64_t now_ms = streamproto::NowSteadyMillis();
			if (now_ms - video_.last_keyreq_ms < 1000)
			{
				first_missing = std::max<std::uint32_t>(first_missing, 1);
				video_.pending_keyframe_first_missing = video_.pending_keyframe_request
												  ? std::min(video_.pending_keyframe_first_missing, first_missing)
												  : first_missing;
				video_.pending_keyframe_request = true;
				return;
			}
			first_missing = std::max<std::uint32_t>(first_missing, 1);
			if (LodEnabled(quality_.effective_video_lod) &&
				quality_.active_video_generation != 0 &&
				SendKeyframeRequest(*connection_.sdr, first_missing, video_.last_assembled_frame, quality_.effective_video_lod, quality_.active_video_generation))
			{
				stats_.keyframe_requests_sent++;
				video_.last_keyreq_ms = now_ms;
				video_.pending_keyframe_request = false;
				video_.pending_keyframe_first_missing = 1;
			}
		}

		void PumpPendingKeyframeRequest()
		{
			if (video_.pending_keyframe_request)
			{
				RequestKeyframe(video_.pending_keyframe_first_missing);
			}
		}

		void EnterKeyframeWait(std::uint32_t first_missing, bool defer_until_complete_frame)
		{
			video_.waiting_for_keyframe = true;
			video_.skip_next_recovery_flush = false;
			video_.no_output_packets = 0;
			FlushActiveVideoDecoder();
			if (defer_until_complete_frame)
			{
				video_.deferred_loss_recovery = true;
				video_.deferred_loss_after_frame = video_.last_assembled_frame;
				video_.deferred_loss_first_missing = std::max<std::uint32_t>(first_missing, 1);
				video_.deferred_loss_start_ms = streamproto::NowSteadyMillis();
			}
			else
			{
				video_.deferred_loss_recovery = false;
				RequestKeyframe(first_missing);
			}
		}

		void HandlePacket(const std::uint8_t *data, std::size_t bytes, bool pause_now)
		{
			if (bytes < sizeof(proto::PacketHeader))
				return;
			const auto *h = reinterpret_cast<const proto::PacketHeader *>(data);
			if (h->magic != proto::kMagic || h->version != proto::kProtocolVersion)
				return;
			proto::MessageType type = static_cast<proto::MessageType>(h->type);

			if (type == proto::MessageType::kPong)
			{
				if (!proto::ValidateExactMessageSize(data, bytes, proto::MessageType::kPong, sizeof(proto::PongMessage)))
					return;
				const auto *m = reinterpret_cast<const proto::PongMessage *>(data);
				std::uint64_t t3 = streamproto::NowUnixMicros();
				std::uint64_t t0 = m->client_send_timestamp_us;
				std::uint64_t t1 = m->host_recv_timestamp_us;
				std::uint64_t t2 = m->host_send_timestamp_us;
				if (t3 < t0 || t2 < t1)
					return;
				std::int64_t rtt_us = static_cast<std::int64_t>(t3 - t0) - static_cast<std::int64_t>(t2 - t1);
				if (rtt_us < 0)
					rtt_us = 0;
				double rtt_ms = static_cast<double>(rtt_us) / 1000.0;
				if (rtt_ms > 3000.0)
					return;
				std::int64_t off = (static_cast<std::int64_t>(t1) - static_cast<std::int64_t>(t0) +
									static_cast<std::int64_t>(t2) - static_cast<std::int64_t>(t3)) /
								   2;
				if (!connection_.clock_sync.valid)
				{
					connection_.clock_sync.valid = true;
					connection_.clock_sync.offset_us = static_cast<double>(off);
					connection_.clock_sync.min_rtt_ms = rtt_ms;
				}
				else
				{
					double a = (rtt_ms <= connection_.clock_sync.min_rtt_ms + 2.0) ? 0.25 : 0.08;
					connection_.clock_sync.offset_us += (static_cast<double>(off) - connection_.clock_sync.offset_us) * a;
					connection_.clock_sync.min_rtt_ms = std::min(connection_.clock_sync.min_rtt_ms, rtt_ms);
				}
				connection_.clock_sync.last_rtt_ms = rtt_ms;
				connection_.clock_sync.samples++;
				stats_.clock_synced = connection_.clock_sync.valid;
				stats_.clock_offset_ms = connection_.clock_sync.offset_us / 1000.0;
				stats_.clock_rtt_ms = connection_.clock_sync.last_rtt_ms;
				stats_.clock_rtt_min_ms = std::isfinite(connection_.clock_sync.min_rtt_ms) ? connection_.clock_sync.min_rtt_ms : 0.0;
				audio_pipeline_.SetClockSync(connection_.clock_sync.valid, connection_.clock_sync.offset_us);
				return;
			}

			if (type == proto::MessageType::kStreamControl)
			{
				if (!proto::ValidateExactMessageSize(data, bytes, proto::MessageType::kStreamControl, sizeof(proto::StreamControlMessage)))
					return;
				const auto *m = reinterpret_cast<const proto::StreamControlMessage *>(data);
				auto cmd = static_cast<proto::StreamControlCommand>(m->command);
				if (cmd == proto::StreamControlCommand::kHostPause)
					connection_.host_paused = true;
				if (cmd == proto::StreamControlCommand::kHostResume)
					connection_.host_paused = false;
				return;
			}

			if (type == proto::MessageType::kStreamQuality)
			{
				if (!proto::ValidateExactMessageSize(data, bytes, proto::MessageType::kStreamQuality, sizeof(proto::StreamQualityMessage)))
					return;
				const auto *m = reinterpret_cast<const proto::StreamQualityMessage *>(data);
				const auto command = static_cast<proto::StreamQualityCommand>(m->command);
				if (command == proto::StreamQualityCommand::kHostAssigned)
				{
					const std::uint32_t video_generation = m->sequence;
					const std::uint32_t audio_generation = m->profile_id;
					quality_.assigned_video_lod = NormalizeLod(m->assigned_video_lod);
					quality_.assigned_audio_lod = NormalizeLod(m->assigned_audio_lod);
					quality_.available_video_lod = BestAvailableLod(m->available_video_lod_mask);
					quality_.available_audio_lod = BestAvailableLod(m->available_audio_lod_mask);
					quality_.available_video_lod_mask = m->available_video_lod_mask;

					if (video_generation != 0)
					{
						if (quality_.desired_video_generation != 0 &&
							video_generation < quality_.desired_video_generation &&
							video_generation == quality_.active_video_generation)
						{
							quality_.last_acked_video_generation = 0;
							quality_.ack_pending_video_generation = video_generation;
						}
						else if (quality_.desired_video_generation == 0 ||
								 video_generation >= quality_.desired_video_generation)
						{
							quality_.desired_video_lod = NormalizeLod(m->effective_video_lod);
							quality_.desired_video_generation = video_generation;
						}
					}

					if (audio_generation != 0)
					{
						if (quality_.desired_audio_generation != 0 &&
							audio_generation < quality_.desired_audio_generation &&
							audio_generation == quality_.active_audio_generation)
						{
							quality_.last_acked_audio_generation = 0;
							quality_.ack_pending_audio_generation = audio_generation;
						}
						else if (quality_.desired_audio_generation == 0 ||
								 audio_generation >= quality_.desired_audio_generation)
						{
							quality_.desired_audio_lod = NormalizeLod(m->effective_audio_lod);
							quality_.desired_audio_generation = audio_generation;
						}
					}

					WarmRelevantVideoDecoders();
					if (quality_.desired_video_generation == quality_.active_video_generation &&
						quality_.desired_video_lod == quality_.effective_video_lod)
					{
						quality_.last_acked_video_generation = 0;
						quality_.ack_pending_video_generation = quality_.active_video_generation;
					}
					else
					{
						(void)TryActivatePendingVideoGeneration();
					}
					if (quality_.desired_audio_generation == quality_.active_audio_generation &&
						quality_.desired_audio_lod == quality_.effective_audio_lod)
					{
						quality_.last_acked_audio_generation = 0;
						quality_.ack_pending_audio_generation = quality_.active_audio_generation;
					}
					else
					{
						(void)TryActivatePendingAudioGeneration();
					}
				}
				return;
			}

			if (type == proto::MessageType::kAudioConfig)
			{
				if (!proto::ValidateExactMessageSize(data, bytes, proto::MessageType::kAudioConfig, sizeof(proto::AudioConfigMessage)))
					return;
				const auto *cfg = reinterpret_cast<const proto::AudioConfigMessage *>(data);
				if (!proto::ValidateAudioConfigMessage(*cfg))
					return;
				const std::int32_t cfg_lod = LodEnabled(cfg->lod_index) ? NormalizeLod(cfg->lod_index) : quality_.desired_audio_lod;
				const std::uint32_t generation = cfg->stream_generation != 0 ? cfg->stream_generation : quality_.desired_audio_generation;
				if (!LodEnabled(cfg_lod) || generation == 0)
				{
					return;
				}
				audio_configs_by_key_[StreamConfigKey(cfg_lod, generation)] = *cfg;
				(void)TryActivatePendingAudioGeneration();
				return;
			}

			if (type == proto::MessageType::kAudioFrame)
			{
				if (bytes < sizeof(proto::AudioFrameHeader) ||
					pause_now ||
					connection_.host_paused ||
					!LodEnabled(quality_.effective_audio_lod))
					return;
				const auto *frame_header = reinterpret_cast<const proto::AudioFrameHeader *>(data);
				if (!proto::ValidateAudioFrameHeader(*frame_header, bytes - sizeof(proto::AudioFrameHeader)))
					return;
				if (NormalizeLod(frame_header->lod_index) != quality_.effective_audio_lod ||
					frame_header->stream_generation != quality_.active_audio_generation)
				{
					return;
				}
				audio_pipeline_.EnqueueFrame(data, bytes);
				return;
			}

			if (type == proto::MessageType::kStreamConfig)
			{
				if (!proto::ValidateExactMessageSize(data, bytes, proto::MessageType::kStreamConfig, sizeof(proto::StreamConfigMessage)))
					return;
				const auto *cfg = reinterpret_cast<const proto::StreamConfigMessage *>(data);
				if (!proto::ValidateStreamConfigMessage(*cfg))
					return;
				const AVCodecID cfg_codec = CodecFromProto(cfg->codec);
				const std::int32_t cfg_lod = LodEnabled(cfg->lod_index)
												 ? NormalizeLod(cfg->lod_index)
												 : (cfg->reserved > 0
														? static_cast<std::int32_t>(cfg->reserved) - 1
														: (LodEnabled(quality_.desired_video_lod) ? quality_.desired_video_lod : 0));
				const std::uint32_t generation = cfg->stream_generation != 0 ? cfg->stream_generation : quality_.desired_video_generation;
				if (!LodEnabled(cfg_lod) || generation == 0)
				{
					return;
				}
				if (video_.codec_id != AV_CODEC_ID_NONE && video_.codec_id != cfg_codec)
				{
					ShutdownVideoDecoders();
				}
				video_.codec_id = cfg_codec;
				video_.stream_configs_by_key[StreamConfigKey(cfg_lod, generation)] = *cfg;
				WarmRelevantVideoDecoders();
				(void)TryActivatePendingVideoGeneration();
				return;
			}

			if (type != proto::MessageType::kVideoChunk ||
				!video_.has_stream_config ||
				pause_now ||
				connection_.host_paused ||
				!LodEnabled(quality_.effective_video_lod))
				return;
			if (bytes < sizeof(proto::VideoChunkHeader))
				return;
			const auto *ch = reinterpret_cast<const proto::VideoChunkHeader *>(data);
			const std::size_t payload_offset = sizeof(proto::VideoChunkHeader);
			const std::size_t payload_bytes = bytes - payload_offset;
			if (!proto::ValidateVideoChunkHeader(*ch, payload_bytes))
				return;
			stats_.recv_chunks++;
			stats_.video_bytes += bytes;
			stats_.recv_bytes += bytes;
			if (NormalizeLod(ch->lod_index) != quality_.effective_video_lod ||
				ch->stream_generation != quality_.active_video_generation)
			{
				return;
			}
			const auto reassembly_cpu_start = std::chrono::steady_clock::now();
			auto assembled = video_.assembler.PushChunk(*ch, data + payload_offset, payload_bytes, streamproto::NowSteadyMillis());
			if (!assembled.has_value())
				return;
			stats_.assembled_frames++;
			std::uint32_t fid = assembled->frame_id;
			if (video_.last_assembled_frame != 0 && fid <= video_.last_assembled_frame)
				return;
			if (!video_.waiting_for_keyframe && video_.last_assembled_frame != 0 && fid > video_.last_assembled_frame + 1)
			{
				const std::uint32_t first_missing = video_.last_assembled_frame + 1;
				stats_.dropped_frames += (fid - video_.last_assembled_frame - 1);
				EnterKeyframeWait(first_missing, true);
			}
			video_.last_assembled_frame = fid;

			const RecoveryUnitInfo recovery = InspectRecoveryUnit(
				CodecFromProto(video_.current_cfg.codec),
				assembled->bytes.data(),
				assembled->bytes.size(),
				assembled->keyframe);

			if (video_.waiting_for_keyframe)
			{
				if (!recovery.valid)
				{
					if (video_.deferred_loss_recovery && fid > video_.deferred_loss_after_frame)
					{
						RequestKeyframe(video_.deferred_loss_first_missing);
						video_.deferred_loss_recovery = false;
					}
					if (assembled->keyframe && recovery.keyframe && !recovery.headers)
					{
						LogWarningStream() << "path=" << ConnectionPathName(connection_.active_path)
										   << " peer=" << host_id_
										   << " Ignoring incomplete recovery frame " << fid
										   << ": " << recovery.reason;
					}
					return;
				}
				if (video_.skip_next_recovery_flush)
				{
					video_.skip_next_recovery_flush = false;
				}
				else
				{
					FlushActiveVideoDecoder();
				}
				video_.waiting_for_keyframe = false;
				video_.deferred_loss_recovery = false;
				video_.no_output_packets = 0;
			}
			double reassembly_ms = 0.0;
			if (assembled->capture_ts_us != 0)
			{
				std::int64_t host_us = static_cast<std::int64_t>(streamproto::NowUnixMicros());
				if (connection_.clock_sync.valid)
					host_us += static_cast<std::int64_t>(std::llround(connection_.clock_sync.offset_us));
				if (host_us >= 0 && static_cast<std::uint64_t>(host_us) >= assembled->capture_ts_us)
				{
					reassembly_ms = static_cast<double>(static_cast<std::uint64_t>(host_us) - assembled->capture_ts_us) / 1000.0;
				}
			}
			const auto reassembly_cpu_end = std::chrono::steady_clock::now();
			const double reassembly_cpu_ms = std::chrono::duration<double, std::milli>(reassembly_cpu_end - reassembly_cpu_start).count();
			ClientVideoDecodeEvent decode_event{};
			decode_event.frame_id = fid;
			decode_event.lod_index = quality_.effective_video_lod;
			decode_event.stream_generation = quality_.active_video_generation;
			decode_event.bytes = std::move(assembled->bytes);
			decode_event.keyframe = assembled->keyframe;
			decode_event.capture_ts_us = assembled->capture_ts_us;
			decode_event.recovery = recovery;
			decode_event.reassembly_ms = reassembly_ms;
			decode_event.reassembly_cpu_ms = reassembly_cpu_ms;
			EnqueueVideoDecodeEvent(std::move(decode_event));
		}

		std::uint32_t app_id_ = 0;
		std::uint64_t host_id_ = 0;
		bool disable_ice_ = false;
		mutable std::mutex peer_mutex_;
		std::mutex peer_worker_mutex_;
		std::thread peer_worker_;
		std::atomic<bool> peer_worker_stop_{true};
		ClientConnectionState connection_{};
		ClientVideoState video_{};
		ClientAudioPipeline audio_pipeline_{};
		std::unordered_map<std::uint64_t, proto::AudioConfigMessage> audio_configs_by_key_;
		ClientStats stats_{};
		ClientStats snapshot_{};
		std::uint64_t last_stats_ms_ = 0;
		ClientPeerQualityState quality_{};
	};

	void RunClientPluginWorker(std::uint32_t app_id, bool disable_ice)
	{
		std::unordered_map<std::uint64_t, std::unique_ptr<ClientPeerRunner>> peers;
		std::unordered_map<std::uint64_t, std::uint64_t> retry_after_ms;
		std::unordered_map<std::uint64_t, ClientMaxAcceptedQuality> max_accepted_qualities = CopyMaxAcceptedQualities();
		std::vector<ConfiguredVideoLod> video_lod_configs = CopyClientVideoLodConfigs();
		std::uint64_t last_reconcile_ms = 0;
		while (!g_client_stop_requested.load())
		{
			std::uint64_t now_ms = streamproto::NowSteadyMillis();
			SteamSdr::PumpGlobalCallbacks();
			if (g_client_video_lod_configs_dirty.exchange(false))
			{
				video_lod_configs = CopyClientVideoLodConfigs();
				for (auto &[_, peer] : peers)
				{
					peer->SetVideoLodConfigs(video_lod_configs);
				}
			}
			if (g_max_accepted_qualities_dirty.exchange(false))
			{
				max_accepted_qualities = CopyMaxAcceptedQualities();
				for (auto &[host_id, peer] : peers)
				{
					auto quality_it = max_accepted_qualities.find(host_id);
					peer->SetMaxAcceptedQuality(quality_it != max_accepted_qualities.end()
													? quality_it->second
													: ClientMaxAcceptedQuality{});
				}
			}
			bool reconcile = g_desired_hosts_dirty.exchange(false) || (now_ms - last_reconcile_ms >= 1000);
			if (reconcile)
			{
				last_reconcile_ms = now_ms;
				std::vector<std::uint64_t> desired = CopyDesiredHosts();
				std::unordered_set<std::uint64_t> desired_set(desired.begin(), desired.end());
				for (auto it = peers.begin(); it != peers.end();)
				{
					if (desired_set.find(it->first) == desired_set.end())
					{
						retry_after_ms.erase(it->first);
						it = peers.erase(it);
					}
					else
					{
						++it;
					}
				}
				for (std::uint64_t host_id : desired_set)
				{
					if (host_id == 0 || peers.find(host_id) != peers.end())
						continue;
					auto rt = retry_after_ms.find(host_id);
					if (rt != retry_after_ms.end() && now_ms < rt->second)
						continue;
					auto peer = std::make_unique<ClientPeerRunner>(app_id, host_id, disable_ice, video_lod_configs);
					std::string err;
					if (!peer->Initialize(err))
					{
						retry_after_ms[host_id] = now_ms + 5000;
						SetLastErrorString("Failed to initialize client peer " + std::to_string(host_id) + ": " + err);
						continue;
					}
					auto quality_it = max_accepted_qualities.find(host_id);
					peer->SetMaxAcceptedQuality(quality_it != max_accepted_qualities.end()
													? quality_it->second
													: ClientMaxAcceptedQuality{});
					if (!peer->Start())
					{
						retry_after_ms[host_id] = now_ms + 5000;
						SetLastErrorString("Failed to start client peer worker " + std::to_string(host_id));
						continue;
					}
					if (g_client_local_steam_id.load() == 0 && peer->LocalSteamId() != 0)
						g_client_local_steam_id.store(peer->LocalSteamId());
					peers.emplace(host_id, std::move(peer));
					retry_after_ms.erase(host_id);
				}
			}
			std::vector<std::uint64_t> reconnect_hosts;
			reconnect_hosts.reserve(peers.size());
			for (auto &[host, peer] : peers)
			{
				if (peer->NeedsReconnect())
				{
					reconnect_hosts.push_back(host);
				}
			}
			if (!reconnect_hosts.empty())
			{
				for (std::uint64_t host_id : reconnect_hosts)
				{
					peers.erase(host_id);
					retry_after_ms[host_id] = now_ms + 1000;
				}
			}
			std::unordered_map<std::uint64_t, PeerExportState> exports;
			exports.reserve(peers.size());
			for (auto &[host, peer] : peers)
				exports.emplace(host, peer->Export());
			{
				std::lock_guard<std::mutex> lock(g_peer_exports_mutex);
				for (auto &[host, next] : exports)
				{
					auto old_it = g_peer_exports.find(host);
					if (old_it != g_peer_exports.end())
					{
						next.last_video_texture_copy_ms = old_it->second.last_video_texture_copy_ms;
						next.last_video_capture_to_texture_ms = old_it->second.last_video_capture_to_texture_ms;
						next.last_audio_capture_to_unity_push_ms = old_it->second.last_audio_capture_to_unity_push_ms;
					}
				}
				g_peer_exports.swap(exports);
			}
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
		}
		peers.clear();
		{
			std::lock_guard<std::mutex> lock(g_peer_exports_mutex);
			g_peer_exports.clear();
		}
		{
			std::lock_guard<std::mutex> lock(g_peer_audio_mutex);
			g_peer_audio_rings.clear();
		}
		g_client_local_steam_id.store(0);
	}

	void UNITY_INTERFACE_API OnClientGraphicsDeviceEvent(UnityGfxDeviceEventType event_type)
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
			for (auto &[_, b] : g_unity_output_bindings)
			{
				b.opened_source_texture.Reset();
				b.opened_source_handle = nullptr;
			}
		}
		else if (event_type == kUnityGfxDeviceEventShutdown)
		{
			g_unity_device = nullptr;
			g_unity_output_bindings.clear();
		}
	}

	struct ClientRenderSourceInfo
	{
		std::uint64_t shared_handle = 0;
		std::uint64_t capture_ts_us = 0;
		double clock_offset_ms = 0.0;
		bool clock_synced = false;
	};

	void UNITY_INTERFACE_API OnClientRenderEvent(int event_id)
	{
		if (event_id != kClientRenderEventId)
		{
			return;
		}
		std::unordered_map<std::uint64_t, ClientRenderSourceInfo> sources;
		{
			std::lock_guard<std::mutex> lock(g_peer_exports_mutex);
			sources.reserve(g_peer_exports.size());
			for (const auto &[host, s] : g_peer_exports)
			{
				if (s.shared_handle != 0)
				{
					ClientRenderSourceInfo info{};
					info.shared_handle = s.shared_handle;
					info.capture_ts_us = s.last_video_capture_ts_us;
					info.clock_offset_ms = s.clock_offset_ms;
					info.clock_synced = s.clock_synced != 0;
					sources.emplace(host, info);
				}
			}
		}

		std::lock_guard<std::mutex> lock(g_unity_mutex);
		if (g_unity_device == nullptr || sources.empty())
		{
			return;
		}

		for (auto &[host, binding] : g_unity_output_bindings)
		{
			if (binding.target_texture == nullptr)
				continue;
			auto it = sources.find(host);
			if (it == sources.end())
				continue;
			const ClientRenderSourceInfo &source_info = it->second;
			HANDLE source_handle = reinterpret_cast<HANDLE>(static_cast<std::uintptr_t>(source_info.shared_handle));
			if (source_handle == nullptr)
				continue;
			const auto copy_start = std::chrono::steady_clock::now();
			const bool copied = streamproto::unity::CopySharedTextureToTarget(
				g_unity_device,
				source_handle,
				binding.target_texture,
				binding.opened_source_handle,
				binding.opened_source_texture,
				true);
			const auto copy_end = std::chrono::steady_clock::now();
			if (copied)
			{
				const double copy_ms = std::chrono::duration<double, std::milli>(copy_end - copy_start).count();
				double capture_to_texture_ms = 0.0;
				if (source_info.capture_ts_us != 0 && source_info.clock_synced)
				{
					std::int64_t host_now_us = static_cast<std::int64_t>(streamproto::NowUnixMicros());
					host_now_us += static_cast<std::int64_t>(std::llround(source_info.clock_offset_ms * 1000.0));
					if (host_now_us >= 0 && static_cast<std::uint64_t>(host_now_us) >= source_info.capture_ts_us)
					{
						capture_to_texture_ms = static_cast<double>(static_cast<std::uint64_t>(host_now_us) - source_info.capture_ts_us) / 1000.0;
					}
				}
				std::lock_guard<std::mutex> export_lock(g_peer_exports_mutex);
				auto export_it = g_peer_exports.find(host);
				if (export_it != g_peer_exports.end())
				{
					export_it->second.last_video_texture_copy_ms = copy_ms;
					if (capture_to_texture_ms > 0.0)
					{
						export_it->second.last_video_capture_to_texture_ms = capture_to_texture_ms;
					}
				}
			}
		}
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
			g_unity_graphics->RegisterDeviceEventCallback(OnClientGraphicsDeviceEvent);
		}
		OnClientGraphicsDeviceEvent(kUnityGfxDeviceEventInitialize);
	}

	UNITY_INTERFACE_EXPORT void UNITY_INTERFACE_API UnityPluginUnload()
	{
		if (g_unity_graphics != nullptr)
		{
			g_unity_graphics->UnregisterDeviceEventCallback(OnClientGraphicsDeviceEvent);
		}
		SetClientLogCallback(nullptr);
		SetClientLoggingEnabled(true);
		OnClientGraphicsDeviceEvent(kUnityGfxDeviceEventShutdown);
		g_unity_interfaces = nullptr;
		g_unity_graphics = nullptr;
	}

	UNITY_INTERFACE_EXPORT void SSPC_SetLogCallback(ClientLogCallback callback)
	{
		SetClientLogCallback(callback);
	}

	UNITY_INTERFACE_EXPORT void SSPC_SetLoggingEnabled(bool enabled)
	{
		SetClientLoggingEnabled(enabled);
	}

	UNITY_INTERFACE_EXPORT bool SSPC_Start(const SSPC_StartParams *params)
	{
		if (params == nullptr || params->app_id == 0)
		{
			return false;
		}

		std::lock_guard<std::mutex> lock(g_client_thread_mutex);
		if (g_client_thread_running.load())
		{
			return false;
		}

		if (params->host_steam_id != 0)
		{
			std::lock_guard<std::mutex> dlock(g_desired_hosts_mutex);
			if (g_desired_hosts.empty())
				g_desired_hosts.push_back(params->host_steam_id);
		}
		g_desired_hosts_dirty.store(true);
		g_client_stop_requested.store(false);
		g_client_pause_requested.store(false);
		g_client_local_steam_id.store(0);
		SetLastErrorString("");
		{
			std::lock_guard<std::mutex> exports_lock(g_peer_exports_mutex);
			g_peer_exports.clear();
		}
		{
			std::lock_guard<std::mutex> audio_lock(g_peer_audio_mutex);
			g_peer_audio_rings.clear();
		}
		const std::uint32_t app_id = params->app_id;
		const bool disable_ice = params->disable_ice != 0;

		g_client_thread_running.store(true);
		g_client_thread = std::thread([app_id, disable_ice]()
									  {
    RunClientPluginWorker(app_id, disable_ice);
    g_client_thread_running.store(false); });
		return true;
	}

	UNITY_INTERFACE_EXPORT void SSPC_Stop()
	{
		std::lock_guard<std::mutex> lock(g_client_thread_mutex);
		g_client_stop_requested.store(true);
		if (g_client_thread.joinable())
		{
			g_client_thread.join();
		}
		g_client_stop_requested.store(false);
		g_client_thread_running.store(false);
		{
			std::lock_guard<std::mutex> ex_lock(g_peer_exports_mutex);
			g_peer_exports.clear();
		}
		{
			std::lock_guard<std::mutex> audio_lock(g_peer_audio_mutex);
			g_peer_audio_rings.clear();
		}
	}

	UNITY_INTERFACE_EXPORT bool SSPC_IsRunning()
	{
		return g_client_thread_running.load();
	}

	UNITY_INTERFACE_EXPORT void SSPC_SetPaused(bool paused)
	{
		g_client_pause_requested.store(paused);
	}

	UNITY_INTERFACE_EXPORT void SSPC_SetRemoteSteamIds(const std::uint64_t *steam_ids, int count)
	{
		std::vector<std::uint64_t> values;
		if (steam_ids != nullptr && count > 0)
		{
			values.reserve(static_cast<std::size_t>(count));
			for (int i = 0; i < count; ++i)
				if (steam_ids[i] != 0)
					values.push_back(steam_ids[i]);
			std::sort(values.begin(), values.end());
			values.erase(std::unique(values.begin(), values.end()), values.end());
		}
		{
			std::lock_guard<std::mutex> lock(g_desired_hosts_mutex);
			g_desired_hosts = std::move(values);
		}
		g_desired_hosts_dirty.store(true);
	}

	UNITY_INTERFACE_EXPORT void SSPC_ConfigureVideoLods(const SSPC_VideoLodConfig *lods, int count)
	{
		std::vector<ConfiguredVideoLod> values;
		if (lods != nullptr && count > 0)
		{
			const int safe_count = std::clamp(count, 0, 32);
			values.reserve(static_cast<std::size_t>(safe_count));
			for (int i = 0; i < safe_count; ++i)
			{
				ConfiguredVideoLod lod{};
				lod.enabled = lods[i].enabled != 0;
				lod.width = static_cast<std::uint16_t>(std::clamp(lods[i].width, 16, 8192));
				lod.height = static_cast<std::uint16_t>(std::clamp(lods[i].height, 16, 8192));
				lod.fps = static_cast<std::uint16_t>(std::clamp(lods[i].fps, 1, 240));
				if (lods[i].codec == static_cast<std::int32_t>(proto::Codec::kHEVC))
				{
					lod.codec = static_cast<std::uint16_t>(proto::Codec::kHEVC);
				}
				else if (lods[i].codec == static_cast<std::int32_t>(proto::Codec::kAV1))
				{
					lod.codec = static_cast<std::uint16_t>(proto::Codec::kAV1);
				}
				else
				{
					lod.codec = static_cast<std::uint16_t>(proto::Codec::kH264);
				}
				values.push_back(lod);
			}
		}
		{
			std::lock_guard<std::mutex> lock(g_client_video_lod_configs_mutex);
			g_client_video_lod_configs = std::move(values);
		}
		g_client_video_lod_configs_dirty.store(true);
	}

	UNITY_INTERFACE_EXPORT void SSPC_SetMaxAcceptedVideoLod(std::uint64_t host_steam_id, std::int32_t lod)
	{
		if (host_steam_id == 0)
		{
			return;
		}
		{
			std::lock_guard<std::mutex> lock(g_max_accepted_qualities_mutex);
			g_max_accepted_qualities[host_steam_id].video_lod = NormalizeLod(lod);
		}
		g_max_accepted_qualities_dirty.store(true);
	}

	UNITY_INTERFACE_EXPORT void SSPC_SetMaxAcceptedAudioLod(std::uint64_t host_steam_id, std::int32_t lod)
	{
		if (host_steam_id == 0)
		{
			return;
		}
		{
			std::lock_guard<std::mutex> lock(g_max_accepted_qualities_mutex);
			g_max_accepted_qualities[host_steam_id].audio_lod = NormalizeLod(lod);
		}
		g_max_accepted_qualities_dirty.store(true);
	}

	UNITY_INTERFACE_EXPORT void SSPC_SetOutputTextureForSteamId(std::uint64_t host_steam_id, void *native_texture_ptr)
	{
		if (host_steam_id == 0)
			return;
		std::lock_guard<std::mutex> lock(g_unity_mutex);
		if (native_texture_ptr == nullptr)
		{
			g_unity_output_bindings.erase(host_steam_id);
			return;
		}
		UnityOutputBinding &b = g_unity_output_bindings[host_steam_id];
		b.target_texture = static_cast<ID3D11Texture2D *>(native_texture_ptr);
		b.opened_source_texture.Reset();
		b.opened_source_handle = nullptr;
	}

	UNITY_INTERFACE_EXPORT void SSPC_SetOutputTexture(void *native_texture_ptr)
	{
		std::uint64_t host_id = FirstDesiredHost();
		if (host_id == 0)
			return;
		SSPC_SetOutputTextureForSteamId(host_id, native_texture_ptr);
	}

	UNITY_INTERFACE_EXPORT UnityRenderingEvent SSPC_GetRenderEventFunc()
	{
		return OnClientRenderEvent;
	}

	UNITY_INTERFACE_EXPORT int SSPC_GetRenderEventId()
	{
		return kClientRenderEventId;
	}

	UNITY_INTERFACE_EXPORT void SSPC_GetStats(SSPC_Stats *out_stats)
	{
		if (out_stats == nullptr)
		{
			return;
		}
		std::uint64_t recv_chunks = 0;
		std::uint64_t recv_bytes = 0;
		std::uint64_t video_bytes = 0;
		std::uint64_t decoded_frames = 0;
		std::uint64_t audio_packets = 0;
		std::uint64_t audio_bytes = 0;
		std::uint64_t audio_frames = 0;
		double reassemble_sum = 0.0;
		double decode_sum = 0.0;
		double post_decode_sum = 0.0;
		double texture_copy_sum = 0.0;
		double latency_sum = 0.0;
		double texture_latency_sum = 0.0;
		double audio_decode_sum = 0.0;
		double audio_push_sum = 0.0;
		double audio_unity_push_sum = 0.0;
		int connected = 0;
		int paused_by_host = 0;
		int preview_width = 0;
		int preview_height = 0;
		std::size_t count = 0;
		{
			std::lock_guard<std::mutex> lock(g_peer_exports_mutex);
			count = g_peer_exports.size();
			for (const auto &[_, s] : g_peer_exports)
			{
				recv_chunks += s.recv_chunks;
				recv_bytes += s.recv_bytes;
				video_bytes += s.video_bytes;
				decoded_frames += s.decoded_frames;
				audio_packets += s.audio_packets;
				audio_bytes += s.audio_bytes;
				audio_frames += s.audio_frames;
				reassemble_sum += s.last_video_reassemble_ms;
				decode_sum += s.last_decode_ms;
				post_decode_sum += s.last_post_decode_ms;
				texture_copy_sum += s.last_video_texture_copy_ms;
				latency_sum += s.last_total_latency_ms;
				texture_latency_sum += s.last_video_capture_to_texture_ms;
				audio_decode_sum += s.last_audio_decode_ms;
				audio_push_sum += s.last_audio_capture_to_push_ms;
				audio_unity_push_sum += s.last_audio_capture_to_unity_push_ms;
				if (s.connected != 0)
					connected++;
				if (s.paused_by_host != 0)
					paused_by_host++;
				preview_width = std::max(preview_width, s.preview_width);
				preview_height = std::max(preview_height, s.preview_height);
			}
		}
		out_stats->recv_chunks = recv_chunks;
		out_stats->recv_bytes = recv_bytes;
		out_stats->video_bytes = video_bytes;
		out_stats->decoded_frames = decoded_frames;
		out_stats->audio_packets = audio_packets;
		out_stats->audio_bytes = audio_bytes;
		out_stats->audio_frames = audio_frames;
		out_stats->last_video_reassemble_ms = count > 0 ? reassemble_sum / static_cast<double>(count) : 0.0;
		out_stats->last_decode_ms = count > 0 ? decode_sum / static_cast<double>(count) : 0.0;
		out_stats->last_post_decode_ms = count > 0 ? post_decode_sum / static_cast<double>(count) : 0.0;
		out_stats->last_video_texture_copy_ms = count > 0 ? texture_copy_sum / static_cast<double>(count) : 0.0;
		out_stats->last_total_latency_ms = count > 0 ? latency_sum / static_cast<double>(count) : 0.0;
		out_stats->last_video_capture_to_texture_ms = count > 0 ? texture_latency_sum / static_cast<double>(count) : 0.0;
		out_stats->last_audio_decode_ms = count > 0 ? audio_decode_sum / static_cast<double>(count) : 0.0;
		out_stats->last_audio_capture_to_push_ms = count > 0 ? audio_push_sum / static_cast<double>(count) : 0.0;
		out_stats->last_audio_capture_to_unity_push_ms = count > 0 ? audio_unity_push_sum / static_cast<double>(count) : 0.0;
		out_stats->local_steam_id = g_client_local_steam_id.load();
		out_stats->connected = connected > 0 ? 1 : 0;
		out_stats->preview_width = preview_width;
		out_stats->preview_height = preview_height;
		out_stats->paused_by_host = paused_by_host > 0 ? 1 : 0;
	}

	UNITY_INTERFACE_EXPORT int SSPC_GetPeerStats(SSPC_PeerStats *out_stats, int max_count)
	{
		if (out_stats == nullptr || max_count <= 0)
			return 0;
		std::vector<PeerExportState> snapshot;
		{
			std::lock_guard<std::mutex> lock(g_peer_exports_mutex);
			snapshot.reserve(g_peer_exports.size());
			for (const auto &[_, s] : g_peer_exports)
				snapshot.push_back(s);
		}
		std::sort(snapshot.begin(), snapshot.end(), [](const PeerExportState &a, const PeerExportState &b)
				  { return a.host_steam_id < b.host_steam_id; });
		int n = std::min<int>(max_count, static_cast<int>(snapshot.size()));
		for (int i = 0; i < n; ++i)
		{
			const PeerExportState &s = snapshot[static_cast<std::size_t>(i)];
			out_stats[i].host_steam_id = s.host_steam_id;
			out_stats[i].recv_chunks = s.recv_chunks;
			out_stats[i].recv_bytes = s.recv_bytes;
			out_stats[i].video_bytes = s.video_bytes;
			out_stats[i].decoded_frames = s.decoded_frames;
			out_stats[i].audio_packets = s.audio_packets;
			out_stats[i].audio_bytes = s.audio_bytes;
			out_stats[i].audio_frames = s.audio_frames;
			out_stats[i].last_video_reassemble_ms = s.last_video_reassemble_ms;
			out_stats[i].last_decode_ms = s.last_decode_ms;
			out_stats[i].last_post_decode_ms = s.last_post_decode_ms;
			out_stats[i].last_video_texture_copy_ms = s.last_video_texture_copy_ms;
			out_stats[i].last_total_latency_ms = s.last_total_latency_ms;
			out_stats[i].last_video_capture_to_texture_ms = s.last_video_capture_to_texture_ms;
			out_stats[i].last_audio_decode_ms = s.last_audio_decode_ms;
			out_stats[i].last_audio_capture_to_push_ms = s.last_audio_capture_to_push_ms;
			out_stats[i].last_audio_capture_to_unity_push_ms = s.last_audio_capture_to_unity_push_ms;
			out_stats[i].connected = s.connected;
			out_stats[i].connection_path = s.connection_path;
			out_stats[i].preview_width = s.preview_width;
			out_stats[i].preview_height = s.preview_height;
			out_stats[i].paused_by_host = s.paused_by_host;
			out_stats[i].assigned_video_lod = s.assigned_video_lod;
			out_stats[i].assigned_audio_lod = s.assigned_audio_lod;
			out_stats[i].max_video_lod = s.max_video_lod;
			out_stats[i].max_audio_lod = s.max_audio_lod;
			out_stats[i].available_video_lod = s.available_video_lod;
			out_stats[i].available_audio_lod = s.available_audio_lod;
			out_stats[i].effective_video_lod = s.effective_video_lod;
			out_stats[i].effective_audio_lod = s.effective_audio_lod;
		}
		return n;
	}

	UNITY_INTERFACE_EXPORT int SSPC_ReadAudioForSteamId(std::uint64_t host_steam_id, float *out_samples, int max_samples)
	{
		if (host_steam_id == 0 || out_samples == nullptr || max_samples <= 0)
		{
			return 0;
		}

		std::shared_ptr<streamproto::audio::PcmRingBuffer> ring;
		{
			std::lock_guard<std::mutex> lock(g_peer_audio_mutex);
			auto it = g_peer_audio_rings.find(host_steam_id);
			if (it == g_peer_audio_rings.end())
			{
				return 0;
			}
			ring = it->second;
		}
		if (ring == nullptr)
		{
			return 0;
		}
		const std::size_t read = ring->Read(out_samples, static_cast<std::size_t>(max_samples));
		return static_cast<int>(std::min<std::size_t>(read, static_cast<std::size_t>(std::numeric_limits<int>::max())));
	}

	UNITY_INTERFACE_EXPORT void SSPC_MarkAudioPushedForSteamId(std::uint64_t host_steam_id)
	{
		if (host_steam_id == 0)
		{
			return;
		}

		std::lock_guard<std::mutex> export_lock(g_peer_exports_mutex);
		auto export_it = g_peer_exports.find(host_steam_id);
		if (export_it == g_peer_exports.end() || export_it->second.last_audio_capture_ts_us == 0)
		{
			return;
		}

		if (export_it->second.clock_synced == 0)
		{
			return;
		}
		std::int64_t host_now_us = static_cast<std::int64_t>(streamproto::NowUnixMicros());
		host_now_us += static_cast<std::int64_t>(std::llround(export_it->second.clock_offset_ms * 1000.0));
		if (host_now_us >= 0 && static_cast<std::uint64_t>(host_now_us) >= export_it->second.last_audio_capture_ts_us)
		{
			export_it->second.last_audio_capture_to_unity_push_ms =
				static_cast<double>(static_cast<std::uint64_t>(host_now_us) - export_it->second.last_audio_capture_ts_us) / 1000.0;
		}
	}

	UNITY_INTERFACE_EXPORT void SSPC_GetAudioFormat(int *out_sample_rate, int *out_channels)
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

	UNITY_INTERFACE_EXPORT const char *SSPC_GetLastError()
	{
		std::lock_guard<std::mutex> lock(g_error_mutex);
		return g_last_error.c_str();
	}

} // extern "C"
