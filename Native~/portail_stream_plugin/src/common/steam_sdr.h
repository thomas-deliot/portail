#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <steam/steam_api.h>

#if defined(_WIN32) && defined(STREAM_COMMON_SHARED)
#if defined(STREAM_COMMON_BUILD)
#define STREAM_COMMON_API __declspec(dllexport)
#else
#define STREAM_COMMON_API __declspec(dllimport)
#endif
#else
#define STREAM_COMMON_API
#endif

namespace streamproto
{

	constexpr int kDefaultStreamingVirtualPort = 27;
	constexpr int kAutoStreamingVirtualPort = -1;

	enum class SdrRole
	{
		kHost,
		kClient,
	};

	enum class SdrLane : std::uint16_t
	{
		kVideo = 0,
		kAudio = 1,
		kControl = 2,
	};

	constexpr int kSdrLaneCount = 3;

	struct SdrConfig
	{
		std::uint32_t app_id = 0;
		SdrRole role = SdrRole::kHost;
		std::uint64_t remote_steam_id = 0;
		int virtual_port = kAutoStreamingVirtualPort;
		int send_buffer_size = 8 * 1024 * 1024;
		int recv_buffer_size = 8 * 1024 * 1024;
		int send_rate_bytes_per_sec = 8 * 1024 * 1024;
		int nagle_time_usec = 0;
		int p2p_ice_enable = k_nSteamNetworkingConfig_P2P_Transport_ICE_Enable_All;
		int p2p_ice_penalty = 0;
		int p2p_sdr_penalty = 0;
		int debug_output_level = k_ESteamNetworkingSocketsDebugOutputType_Msg;
		int p2p_log_level = k_ESteamNetworkingSocketsDebugOutputType_Msg;
	};

	struct HostPeerStatus
	{
		std::uint64_t steam_id = 0;
		bool relayed = true;
		bool paused = false;
	};

	class STREAM_COMMON_API SteamSdr
	{
	public:
		explicit SteamSdr(SdrConfig config);
		~SteamSdr();

		SteamSdr(const SteamSdr &) = delete;
		SteamSdr &operator=(const SteamSdr &) = delete;

		bool Initialize(std::string &error);
		void PumpCallbacks();
		static void PumpGlobalCallbacks();
		int Receive(const std::function<void(const std::uint8_t *data, std::size_t bytes)> &handler);
		int ReceiveWithPeer(const std::function<void(std::uint64_t remote_steam_id, const std::uint8_t *data, std::size_t bytes)> &handler);
		int ReceiveWithPeerAndLane(const std::function<void(std::uint64_t remote_steam_id, SdrLane lane, const std::uint8_t *data, std::size_t bytes)> &handler);
		bool Send(const void *data, std::size_t bytes, bool reliable, SdrLane lane = SdrLane::kControl, EResult *out_result = nullptr);
		bool SendToSteamId(std::uint64_t steam_id, const void *data, std::size_t bytes, bool reliable, SdrLane lane = SdrLane::kControl, EResult *out_result = nullptr);
		void Close(const char *reason);
		void SetHostAllowedSteamIds(const std::vector<std::uint64_t> &steam_ids);
		void SetHostPeerPaused(std::uint64_t steam_id, bool paused);
		[[nodiscard]] std::vector<std::uint64_t> HostConnectedSteamIds() const;
		[[nodiscard]] std::vector<HostPeerStatus> HostPeerStatuses() const;
		[[nodiscard]] HSteamNetConnection ConnectionHandleForSteamId(std::uint64_t steam_id) const;
		[[nodiscard]] std::vector<HSteamNetConnection> ConnectionHandles() const;

		[[nodiscard]] bool IsConnected() const { return connected_.load(); }
		[[nodiscard]] std::uint64_t LocalSteamId() const;
		[[nodiscard]] HSteamNetConnection ConnectionHandle() const { return connection_.load(); }

	private:
		void EnsureSteamAppIdFile() const;
		bool InitSteam(std::string &error);
		void Shutdown();
		void ApplyConnectionTuning(HSteamNetConnection conn) const;
		void ConfigureConnectionLanes(HSteamNetConnection conn) const;
		bool SendToConnection(HSteamNetConnection conn, const void *data, std::size_t bytes, bool reliable, SdrLane lane, EResult *out_result) const;
		int BuildConfigOptions(SteamNetworkingConfigValue_t *out_options, int max_options) const;
		void LogDetailedConnectionStatus(HSteamNetConnection conn, const char *phase) const;
		void LogConnectionEvent(const char *phase, const SteamNetConnectionStatusChangedCallback_t *info) const;
		bool MatchesConnectionEvent(const SteamNetConnectionStatusChangedCallback_t *info) const;
		static const char *ConnectionStateName(ESteamNetworkingConnectionState state);

		static void SteamNetConnectionStatusChangedCallback(SteamNetConnectionStatusChangedCallback_t *info);
		void OnConnectionStatusChanged(SteamNetConnectionStatusChangedCallback_t *info);

		SdrConfig config_;
		std::atomic<bool> steam_initialized_{false};
		std::atomic<bool> connected_{false};
		std::atomic<HSteamListenSocket> listen_socket_{k_HSteamListenSocket_Invalid};
		std::atomic<HSteamNetPollGroup> poll_group_{k_HSteamNetPollGroup_Invalid};
		std::atomic<HSteamNetConnection> connection_{k_HSteamNetConnection_Invalid};
		std::unordered_set<HSteamNetConnection> host_connections_;
		std::unordered_map<HSteamNetConnection, std::uint64_t> host_connection_remote_ids_;
		std::unordered_set<std::uint64_t> host_allowed_ids_;
		std::unordered_set<std::uint64_t> host_paused_ids_;
		mutable std::mutex callback_mutex_;
	};

} // namespace streamproto
