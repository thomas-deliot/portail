#include "common/steam_sdr.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>
#include <fstream>
#include <iostream>
#include <mutex>
#include <string>
#include <unordered_set>
#include <vector>

#include <windows.h>

namespace streamproto
{

	namespace
	{
		std::mutex g_instances_mutex;
		std::mutex g_steam_callbacks_mutex;
		std::unordered_set<SteamSdr *> g_instances;
		std::atomic<int> g_steam_init_refcount{0};
		std::atomic<bool> g_steam_api_owned{false};
		constexpr int kStreamingVirtualPortBase = 2000;
		constexpr int kStreamingVirtualPortSpan = 50000;

		void SteamDebugLog(ESteamNetworkingSocketsDebugOutputType level, const char *msg)
		{
			if (msg == nullptr)
			{
				return;
			}
			OutputDebugStringA(msg);
			OutputDebugStringA("\n");
			std::cerr << "[SDR][" << static_cast<int>(level) << "] " << msg << "\n";
		}

		const char *AvailabilityName(ESteamNetworkingAvailability value)
		{
			switch (value)
			{
			case k_ESteamNetworkingAvailability_CannotTry:
				return "CannotTry";
			case k_ESteamNetworkingAvailability_Failed:
				return "Failed";
			case k_ESteamNetworkingAvailability_Previously:
				return "Previously";
			case k_ESteamNetworkingAvailability_Retrying:
				return "Retrying";
			case k_ESteamNetworkingAvailability_NeverTried:
				return "NeverTried";
			case k_ESteamNetworkingAvailability_Waiting:
				return "Waiting";
			case k_ESteamNetworkingAvailability_Attempting:
				return "Attempting";
			case k_ESteamNetworkingAvailability_Current:
				return "Current";
			case k_ESteamNetworkingAvailability_Unknown:
			default:
				return "Unknown";
			}
		}

		const char *ResultName(EResult result)
		{
			switch (result)
			{
			case k_EResultOK:
				return "OK";
			case k_EResultLimitExceeded:
				return "LimitExceeded";
			case k_EResultIgnored:
				return "Ignored";
			case k_EResultNoConnection:
				return "NoConnection";
			case k_EResultInvalidState:
				return "InvalidState";
			case k_EResultInvalidParam:
				return "InvalidParam";
			default:
				return "Other";
			}
		}

		const char *ConnectionPathNameFromFlags(int flags)
		{
			return (flags & k_nSteamNetworkConnectionInfoFlags_Relayed) != 0 ? "sdr" : "ice";
		}

		bool SteamApiInitViaExport(bool &out_owned, std::string &error)
		{
			out_owned = false;
			HMODULE steam_api_module = GetModuleHandleA("steam_api64.dll");
			if (steam_api_module == nullptr)
			{
				steam_api_module = LoadLibraryA("steam_api64.dll");
				if (steam_api_module == nullptr)
				{
					error = "Failed to load steam_api64.dll.";
					return false;
				}
			}

			using SteamApiGetHSteamUserFn = HSteamUser(S_CALLTYPE *)();
			const auto steam_api_get_hsteam_user = reinterpret_cast<SteamApiGetHSteamUserFn>(
				GetProcAddress(steam_api_module, "SteamAPI_GetHSteamUser"));
			if (steam_api_get_hsteam_user != nullptr && steam_api_get_hsteam_user() != 0)
			{
				return true;
			}

			using SteamApiInitFn = bool(S_CALLTYPE *)();
			const auto steam_api_init = reinterpret_cast<SteamApiInitFn>(GetProcAddress(steam_api_module, "SteamAPI_Init"));
			if (steam_api_init == nullptr)
			{
				error = "steam_api64.dll does not export SteamAPI_Init.";
				return false;
			}

			if (!steam_api_init())
			{
				error = "SteamAPI_Init failed. Make sure Steam is running and the app ID is valid.";
				return false;
			}

			out_owned = true;
			return true;
		}

		int ResolveVirtualPort(const SdrConfig &config)
		{
			if (config.virtual_port >= 0)
			{
				return config.virtual_port;
			}

			std::uint64_t steam_id_seed = 0;
			if (config.role == SdrRole::kHost)
			{
				if (SteamUser() != nullptr)
				{
					steam_id_seed = SteamUser()->GetSteamID().ConvertToUint64();
				}
			}
			else
			{
				steam_id_seed = config.remote_steam_id;
			}

			if (steam_id_seed == 0)
			{
				return kDefaultStreamingVirtualPort;
			}

			return kStreamingVirtualPortBase + static_cast<int>(steam_id_seed % static_cast<std::uint64_t>(kStreamingVirtualPortSpan));
		}
	} // namespace

	SteamSdr::SteamSdr(SdrConfig config) : config_(config) {}

	SteamSdr::~SteamSdr()
	{
		Shutdown();
	}

	bool SteamSdr::Initialize(std::string &error)
	{
		if (!InitSteam(error))
		{
			return false;
		}

		{
			std::lock_guard<std::mutex> lock(g_instances_mutex);
			g_instances.insert(this);
		}

		const int debug_output_level = std::clamp(
			config_.debug_output_level,
			static_cast<int>(k_ESteamNetworkingSocketsDebugOutputType_None),
			static_cast<int>(k_ESteamNetworkingSocketsDebugOutputType_Everything));
		const int p2p_log_level = std::clamp(
			config_.p2p_log_level,
			static_cast<int>(k_ESteamNetworkingSocketsDebugOutputType_None),
			static_cast<int>(k_ESteamNetworkingSocketsDebugOutputType_Everything));

		SteamNetworkingUtils()->SetGlobalCallback_SteamNetConnectionStatusChanged(&SteamSdr::SteamNetConnectionStatusChangedCallback);
		SteamNetworkingUtils()->SetDebugOutputFunction(static_cast<ESteamNetworkingSocketsDebugOutputType>(debug_output_level), &SteamDebugLog);

		const int send_buffer = std::max(config_.send_buffer_size, 64 * 1024);
		const int recv_buffer = std::max(config_.recv_buffer_size, 64 * 1024);
		const int send_rate = std::clamp(config_.send_rate_bytes_per_sec, 64 * 1024, 256 * 1024 * 1024);
		const int nagle_usec = std::max(config_.nagle_time_usec, 0);
		const int resolved_virtual_port = ResolveVirtualPort(config_);
		const int p2p_ice_enable = config_.p2p_ice_enable;
		const int p2p_ice_penalty = std::max(config_.p2p_ice_penalty, 0);
		const int p2p_sdr_penalty = std::max(config_.p2p_sdr_penalty, 0);

		std::cerr
			<< "[SDR] Tunables:"
			<< " send_buffer=" << send_buffer
			<< " recv_buffer=" << recv_buffer
			<< " send_rate=" << send_rate
			<< "Bps nagle=" << nagle_usec
			<< "us ice_enable=" << p2p_ice_enable
			<< " ice_penalty=" << p2p_ice_penalty
			<< " sdr_penalty=" << p2p_sdr_penalty
			<< " debug_level=" << debug_output_level
			<< " p2p_log_level=" << p2p_log_level
			<< "\n";

		SteamNetworkingUtils()->InitRelayNetworkAccess();

		SteamRelayNetworkStatus_t relay_status{};
		ESteamNetworkingAvailability relay_availability = SteamNetworkingUtils()->GetRelayNetworkStatus(&relay_status);
		std::cerr << "[SDR] Relay status=" << AvailabilityName(relay_availability)
				  << " ping_location=" << relay_status.m_bPingMeasurementInProgress
				  << " relay_network=" << relay_status.m_eAvail << "\n";

		HSteamNetPollGroup pg = SteamNetworkingSockets()->CreatePollGroup();
		if (pg == k_HSteamNetPollGroup_Invalid)
		{
			error = "Failed to create Steam networking poll group.";
			Shutdown();
			return false;
		}
		poll_group_.store(pg);

		std::array<SteamNetworkingConfigValue_t, 16> config_values{};
		const int config_count = BuildConfigOptions(config_values.data(), static_cast<int>(config_values.size()));

		if (config_.role == SdrRole::kHost)
		{
			HSteamListenSocket listen = SteamNetworkingSockets()->CreateListenSocketP2P(
				resolved_virtual_port,
				config_count,
				config_count > 0 ? config_values.data() : nullptr);
			if (listen == k_HSteamListenSocket_Invalid)
			{
				error = "Failed to create Steam P2P listen socket.";
				Shutdown();
				return false;
			}
			listen_socket_.store(listen);
			std::cerr << "[SDR] Host listen socket created on virtual_port=" << resolved_virtual_port << "\n";
		}
		else
		{
			if (config_.remote_steam_id == 0)
			{
				error = "Remote Steam ID is required for client role.";
				return false;
			}

			SteamNetworkingIdentity remote_identity{};
			remote_identity.SetSteamID(CSteamID(config_.remote_steam_id));

			std::cerr << "[SDR] Client connecting to SteamID=" << config_.remote_steam_id
					  << " virtual_port=" << resolved_virtual_port << "\n";

			HSteamNetConnection conn = SteamNetworkingSockets()->ConnectP2P(
				remote_identity,
				resolved_virtual_port,
				config_count,
				config_count > 0 ? config_values.data() : nullptr);
			if (conn == k_HSteamNetConnection_Invalid)
			{
				error = "Failed to initiate Steam P2P connection.";
				Shutdown();
				return false;
			}
			connection_.store(conn);
			std::cerr << "[SDR] Client connection handle=" << conn << "\n";
		}

		return true;
	}

	void SteamSdr::PumpCallbacks()
	{
		if (!steam_initialized_.load())
		{
			return;
		}
		PumpGlobalCallbacks();
	}

	void SteamSdr::PumpGlobalCallbacks()
	{
		if (g_steam_init_refcount.load(std::memory_order_acquire) <= 0)
		{
			return;
		}

		std::lock_guard<std::mutex> lock(g_steam_callbacks_mutex);
		SteamAPI_RunCallbacks();

		if (SteamNetworkingSockets() != nullptr)
		{
			SteamNetworkingSockets()->RunCallbacks();
		}
	}

	int SteamSdr::Receive(const std::function<void(const std::uint8_t *data, std::size_t bytes)> &handler)
	{
		return ReceiveWithPeer([&](std::uint64_t, const std::uint8_t *data, std::size_t bytes)
							   { handler(data, bytes); });
	}

	int SteamSdr::ReceiveWithPeer(const std::function<void(std::uint64_t remote_steam_id, const std::uint8_t *data, std::size_t bytes)> &handler)
	{
		return ReceiveWithPeerAndLane(
			[&](std::uint64_t remote_steam_id, SdrLane, const std::uint8_t *data, std::size_t bytes)
			{
				handler(remote_steam_id, data, bytes);
			});
	}

	int SteamSdr::ReceiveWithPeerAndLane(const std::function<void(std::uint64_t remote_steam_id, SdrLane lane, const std::uint8_t *data, std::size_t bytes)> &handler)
	{
		HSteamNetPollGroup pg = poll_group_.load();
		if (pg == k_HSteamNetPollGroup_Invalid)
		{
			return 0;
		}

		ISteamNetworkingMessage *messages[64]{};
		int received = SteamNetworkingSockets()->ReceiveMessagesOnPollGroup(pg, messages, 64);
		if (received <= 0)
		{
			return 0;
		}

		for (int i = 0; i < received; ++i)
		{
			ISteamNetworkingMessage *message = messages[i];
			if (message != nullptr && message->GetSize() > 0 && message->GetData() != nullptr)
			{
				std::uint64_t remote_steam_id = 0;
				if (config_.role == SdrRole::kHost)
				{
					std::lock_guard<std::mutex> lock(callback_mutex_);
					const auto it = host_connection_remote_ids_.find(message->m_conn);
					if (it != host_connection_remote_ids_.end())
					{
						remote_steam_id = it->second;
					}
				}
				else
				{
					remote_steam_id = config_.remote_steam_id;
				}
				SdrLane lane = SdrLane::kControl;
				if (message->m_idxLane == static_cast<std::uint16_t>(SdrLane::kVideo))
				{
					lane = SdrLane::kVideo;
				}
				else if (message->m_idxLane == static_cast<std::uint16_t>(SdrLane::kAudio))
				{
					lane = SdrLane::kAudio;
				}
				handler(remote_steam_id, lane, static_cast<const std::uint8_t *>(message->GetData()), static_cast<std::size_t>(message->GetSize()));
			}
			if (message != nullptr)
			{
				message->Release();
			}
		}
		return received;
	}

	void SteamSdr::SetHostAllowedSteamIds(const std::vector<std::uint64_t> &steam_ids)
	{
		std::vector<HSteamNetConnection> disallowed_connections;
		{
			std::lock_guard<std::mutex> lock(callback_mutex_);
			host_allowed_ids_.clear();
			for (std::uint64_t steam_id : steam_ids)
			{
				if (steam_id != 0)
				{
					host_allowed_ids_.insert(steam_id);
				}
			}

			if (config_.role != SdrRole::kHost || host_allowed_ids_.empty())
			{
				return;
			}

			for (const auto &[conn, remote_id] : host_connection_remote_ids_)
			{
				if (conn == k_HSteamNetConnection_Invalid)
				{
					continue;
				}
				if (remote_id != 0 && host_allowed_ids_.find(remote_id) == host_allowed_ids_.end())
				{
					disallowed_connections.push_back(conn);
				}
			}
		}

		for (HSteamNetConnection conn : disallowed_connections)
		{
			SteamNetworkingSockets()->CloseConnection(conn, 0, "not-allowed", false);
		}
	}

	void SteamSdr::SetHostPeerPaused(std::uint64_t steam_id, bool paused)
	{
		if (steam_id == 0)
		{
			return;
		}
		std::lock_guard<std::mutex> lock(callback_mutex_);
		if (paused)
		{
			host_paused_ids_.insert(steam_id);
		}
		else
		{
			host_paused_ids_.erase(steam_id);
		}
	}

	std::vector<std::uint64_t> SteamSdr::HostConnectedSteamIds() const
	{
		std::vector<std::uint64_t> out;
		std::lock_guard<std::mutex> lock(callback_mutex_);
		out.reserve(host_connection_remote_ids_.size());
		for (const auto &[_, steam_id] : host_connection_remote_ids_)
		{
			if (steam_id != 0)
			{
				out.push_back(steam_id);
			}
		}
		std::sort(out.begin(), out.end());
		out.erase(std::unique(out.begin(), out.end()), out.end());
		return out;
	}

	std::vector<HostPeerStatus> SteamSdr::HostPeerStatuses() const
	{
		std::vector<std::pair<HSteamNetConnection, std::uint64_t>> snapshot;
		{
			std::lock_guard<std::mutex> lock(callback_mutex_);
			snapshot.reserve(host_connection_remote_ids_.size());
			for (const auto &[conn, steam_id] : host_connection_remote_ids_)
			{
				if (conn == k_HSteamNetConnection_Invalid || steam_id == 0)
				{
					continue;
				}
				snapshot.emplace_back(conn, steam_id);
			}
		}

		std::vector<HostPeerStatus> out;
		out.reserve(snapshot.size());
		for (const auto &[conn, steam_id] : snapshot)
		{
			SteamNetConnectionInfo_t info{};
			if (!SteamNetworkingSockets()->GetConnectionInfo(conn, &info))
			{
				continue;
			}

			HostPeerStatus status{};
			status.steam_id = steam_id;
			status.relayed = (info.m_nFlags & k_nSteamNetworkConnectionInfoFlags_Relayed) != 0;
			{
				std::lock_guard<std::mutex> lock(callback_mutex_);
				status.paused = host_paused_ids_.find(steam_id) != host_paused_ids_.end();
			}
			out.push_back(status);
		}

		std::sort(out.begin(), out.end(), [](const HostPeerStatus &a, const HostPeerStatus &b)
				  { return a.steam_id < b.steam_id; });
		out.erase(
			std::unique(out.begin(), out.end(), [](const HostPeerStatus &a, const HostPeerStatus &b)
						{ return a.steam_id == b.steam_id; }),
			out.end());
		return out;
	}

	HSteamNetConnection SteamSdr::ConnectionHandleForSteamId(std::uint64_t steam_id) const
	{
		if (steam_id == 0)
		{
			return k_HSteamNetConnection_Invalid;
		}

		std::lock_guard<std::mutex> lock(callback_mutex_);
		if (config_.role == SdrRole::kHost)
		{
			for (const auto &[conn, remote_id] : host_connection_remote_ids_)
			{
				if (remote_id == steam_id && host_connections_.find(conn) != host_connections_.end())
				{
					return conn;
				}
			}
			return k_HSteamNetConnection_Invalid;
		}

		if (config_.remote_steam_id == steam_id)
		{
			return connection_.load();
		}
		return k_HSteamNetConnection_Invalid;
	}

	std::vector<HSteamNetConnection> SteamSdr::ConnectionHandles() const
	{
		std::vector<HSteamNetConnection> out;
		std::lock_guard<std::mutex> lock(callback_mutex_);
		if (config_.role == SdrRole::kHost)
		{
			out.reserve(host_connections_.size());
			for (HSteamNetConnection conn : host_connections_)
			{
				if (conn != k_HSteamNetConnection_Invalid)
				{
					out.push_back(conn);
				}
			}
			return out;
		}

		HSteamNetConnection conn = connection_.load();
		if (conn != k_HSteamNetConnection_Invalid)
		{
			out.push_back(conn);
		}
		return out;
	}

	bool SteamSdr::Send(const void *data, std::size_t bytes, bool reliable, SdrLane lane, EResult *out_result)
	{
		if (data == nullptr || bytes == 0 || bytes > UINT32_MAX)
		{
			if (out_result != nullptr)
			{
				*out_result = k_EResultInvalidParam;
			}
			return false;
		}

		if (config_.role == SdrRole::kHost)
		{
			std::vector<HSteamNetConnection> connections;
			std::unordered_map<HSteamNetConnection, std::uint64_t> remote_ids;
			std::unordered_set<std::uint64_t> allowed_ids;
			std::unordered_set<std::uint64_t> paused_ids;
			{
				std::lock_guard<std::mutex> lock(callback_mutex_);
				if (host_connections_.empty())
				{
					if (out_result != nullptr)
					{
						*out_result = k_EResultNoConnection;
					}
					return false;
				}
				connections.assign(host_connections_.begin(), host_connections_.end());
				remote_ids = host_connection_remote_ids_;
				allowed_ids = host_allowed_ids_;
				paused_ids = host_paused_ids_;
			}

			HSteamNetConnection primary = k_HSteamNetConnection_Invalid;
			for (HSteamNetConnection conn : connections)
			{
				const auto it = remote_ids.find(conn);
				const std::uint64_t remote_id = (it != remote_ids.end()) ? it->second : 0;
				const bool filtered_by_allowlist =
					!allowed_ids.empty() &&
					(remote_id == 0 || allowed_ids.find(remote_id) == allowed_ids.end());
				const bool filtered_by_pause = (remote_id != 0 && paused_ids.find(remote_id) != paused_ids.end());
				if (!filtered_by_allowlist && !filtered_by_pause)
				{
					primary = conn;
					break;
				}
			}
			if (primary == k_HSteamNetConnection_Invalid)
			{
				if (out_result != nullptr)
				{
					*out_result = k_EResultNoConnection;
				}
				return false;
			}

			bool any_success = false;
			EResult primary_result = k_EResultNoConnection;
			bool primary_seen = false;
			for (HSteamNetConnection conn : connections)
			{
				const auto it = remote_ids.find(conn);
				const std::uint64_t remote_id = (it != remote_ids.end()) ? it->second : 0;
				const bool filtered_by_allowlist =
					!allowed_ids.empty() &&
					(remote_id == 0 || allowed_ids.find(remote_id) == allowed_ids.end());
				const bool filtered_by_pause = (remote_id != 0 && paused_ids.find(remote_id) != paused_ids.end());
				if (filtered_by_allowlist || filtered_by_pause)
				{
					continue;
				}

				EResult result = k_EResultOK;
				(void)SendToConnection(conn, data, bytes, reliable, lane, &result);
				if (conn == primary)
				{
					primary_result = result;
					primary_seen = true;
				}
				if (result == k_EResultOK)
				{
					any_success = true;
					continue;
				}

				if (conn != primary &&
					result != k_EResultIgnored &&
					result != k_EResultLimitExceeded &&
					result != k_EResultNoConnection &&
					result != k_EResultInvalidState)
				{
					std::cerr << "[SDR] SendMessageToConnection failed on secondary conn=" << conn
							  << " result=" << static_cast<int>(result)
							  << " (" << ResultName(result) << ")\n";
				}
			}

			if (!primary_seen)
			{
				primary_result = any_success ? k_EResultOK : k_EResultNoConnection;
			}
			if (out_result != nullptr)
			{
				*out_result = any_success ? k_EResultOK : primary_result;
			}
			if (any_success)
			{
				return true;
			}

			if (primary_result == k_EResultIgnored || primary_result == k_EResultLimitExceeded)
			{
				return false;
			}

			if (primary_result != k_EResultNoConnection && primary_result != k_EResultInvalidState)
			{
				std::cerr << "[SDR] SendMessageToConnection failed: "
						  << static_cast<int>(primary_result)
						  << " (" << ResultName(primary_result) << ")\n";
			}
			return false;
		}

		HSteamNetConnection conn = connection_.load();
		if (conn == k_HSteamNetConnection_Invalid)
		{
			if (out_result != nullptr)
			{
				*out_result = k_EResultNoConnection;
			}
			return false;
		}
		EResult result = k_EResultOK;
		const bool sent = SendToConnection(conn, data, bytes, reliable, lane, &result);
		if (out_result != nullptr)
		{
			*out_result = result;
		}

		if (sent)
		{
			return true;
		}

		// Congestion drops are expected and handled by caller backpressure logic.
		if (result == k_EResultIgnored || result == k_EResultLimitExceeded)
		{
			return false;
		}

		std::cerr << "[SDR] SendMessageToConnection failed: "
				  << static_cast<int>(result)
				  << " (" << ResultName(result) << ")\n";
		return false;
	}

	bool SteamSdr::SendToSteamId(std::uint64_t steam_id, const void *data, std::size_t bytes, bool reliable, SdrLane lane, EResult *out_result)
	{
		if (data == nullptr || bytes == 0 || bytes > UINT32_MAX || steam_id == 0)
		{
			if (out_result != nullptr)
			{
				*out_result = k_EResultInvalidParam;
			}
			return false;
		}

		HSteamNetConnection target = k_HSteamNetConnection_Invalid;
		{
			std::lock_guard<std::mutex> lock(callback_mutex_);
			if (config_.role == SdrRole::kHost)
			{
				if (!host_allowed_ids_.empty() && host_allowed_ids_.find(steam_id) == host_allowed_ids_.end())
				{
					if (out_result != nullptr)
					{
						*out_result = k_EResultNoConnection;
					}
					return false;
				}

				for (const auto &[conn, remote_id] : host_connection_remote_ids_)
				{
					if (remote_id == steam_id && host_connections_.find(conn) != host_connections_.end())
					{
						target = conn;
						break;
					}
				}
			}
			else if (config_.remote_steam_id == steam_id)
			{
				target = connection_.load();
			}
		}

		if (target == k_HSteamNetConnection_Invalid)
		{
			if (out_result != nullptr)
			{
				*out_result = k_EResultNoConnection;
			}
			return false;
		}

		EResult result = k_EResultOK;
		const bool sent = SendToConnection(target, data, bytes, reliable, lane, &result);
		if (out_result != nullptr)
		{
			*out_result = result;
		}
		if (sent)
		{
			return true;
		}
		if (result == k_EResultIgnored || result == k_EResultLimitExceeded)
		{
			return false;
		}

		std::cerr << "[SDR] SendMessageToConnection target=" << steam_id
				  << " failed: " << static_cast<int>(result)
				  << " (" << ResultName(result) << ")\n";
		return false;
	}

	void SteamSdr::Close(const char *reason)
	{
		std::vector<HSteamNetConnection> connections_to_close;
		{
			std::lock_guard<std::mutex> lock(callback_mutex_);
			if (config_.role == SdrRole::kHost)
			{
				for (HSteamNetConnection conn : host_connections_)
				{
					if (conn != k_HSteamNetConnection_Invalid)
					{
						connections_to_close.push_back(conn);
					}
				}
				host_connections_.clear();
				host_connection_remote_ids_.clear();
				host_paused_ids_.clear();
				connection_.store(k_HSteamNetConnection_Invalid);
				connected_.store(false);
			}
			else
			{
				HSteamNetConnection conn = connection_.exchange(k_HSteamNetConnection_Invalid);
				if (conn != k_HSteamNetConnection_Invalid)
				{
					connections_to_close.push_back(conn);
				}
				connected_.store(false);
			}
		}

		for (HSteamNetConnection conn : connections_to_close)
		{
			SteamNetworkingSockets()->CloseConnection(conn, 0, reason, false);
		}
	}

	std::uint64_t SteamSdr::LocalSteamId() const
	{
		if (!steam_initialized_.load())
		{
			return 0;
		}
		return SteamUser()->GetSteamID().ConvertToUint64();
	}

	void SteamSdr::EnsureSteamAppIdFile() const
	{
		std::ofstream out("steam_appid.txt", std::ios::trunc);
		out << config_.app_id;
	}

	bool SteamSdr::InitSteam(std::string &error)
	{
		EnsureSteamAppIdFile();
		const int previous_refcount = g_steam_init_refcount.fetch_add(1);
		if (previous_refcount == 0)
		{
			bool steam_api_owned = false;
			if (!SteamApiInitViaExport(steam_api_owned, error))
			{
				g_steam_api_owned.store(false);
				g_steam_init_refcount.fetch_sub(1);
				return false;
			}
			g_steam_api_owned.store(steam_api_owned);
		}
		steam_initialized_.store(true);
		return true;
	}

	void SteamSdr::Shutdown()
	{
		if (!steam_initialized_.load())
		{
			return;
		}

		Close("shutdown");

		HSteamListenSocket listen = listen_socket_.exchange(k_HSteamListenSocket_Invalid);
		if (listen != k_HSteamListenSocket_Invalid)
		{
			SteamNetworkingSockets()->CloseListenSocket(listen);
		}

		HSteamNetPollGroup pg = poll_group_.exchange(k_HSteamNetPollGroup_Invalid);
		if (pg != k_HSteamNetPollGroup_Invalid)
		{
			SteamNetworkingSockets()->DestroyPollGroup(pg);
		}

		{
			std::lock_guard<std::mutex> lock(g_instances_mutex);
			g_instances.erase(this);
		}

		const int previous_refcount = g_steam_init_refcount.fetch_sub(1);
		if (previous_refcount == 1 && g_steam_api_owned.exchange(false))
		{
			SteamAPI_Shutdown();
		}
		steam_initialized_.store(false);
	}

	void SteamSdr::ApplyConnectionTuning(HSteamNetConnection conn) const
	{
		if (conn == k_HSteamNetConnection_Invalid)
		{
			return;
		}

		const int send_buffer = std::max(config_.send_buffer_size, 64 * 1024);
		const int recv_buffer = std::max(config_.recv_buffer_size, 64 * 1024);
		const int send_rate = std::clamp(config_.send_rate_bytes_per_sec, 64 * 1024, 256 * 1024 * 1024);
		const int nagle_usec = std::max(config_.nagle_time_usec, 0);
		auto set_int32 = [conn](ESteamNetworkingConfigValue config_key, int value, const char *label)
		{
			if (!SteamNetworkingUtils()->SetConnectionConfigValueInt32(conn, config_key, value))
			{
				std::cerr << "[SDR] Failed to set conn " << conn << " " << label << "=" << value << "\n";
			}
		};

		set_int32(k_ESteamNetworkingConfig_SendBufferSize, send_buffer, "SendBufferSize");
		set_int32(k_ESteamNetworkingConfig_RecvBufferSize, recv_buffer, "RecvBufferSize");
		// Steam docs note this should currently be explicitly fixed by setting min=max.
		set_int32(k_ESteamNetworkingConfig_SendRateMin, send_rate, "SendRateMin");
		set_int32(k_ESteamNetworkingConfig_SendRateMax, send_rate, "SendRateMax");
		set_int32(k_ESteamNetworkingConfig_NagleTime, nagle_usec, "NagleTime");
		ConfigureConnectionLanes(conn);
	}

	void SteamSdr::ConfigureConnectionLanes(HSteamNetConnection conn) const
	{
		if (conn == k_HSteamNetConnection_Invalid)
		{
			return;
		}

		// Lower numeric priority wins in SteamNetworkingSockets. Lane 0 stays video
		// for wire efficiency, but control and audio are scheduled ahead of it.
		const int priorities[kSdrLaneCount] = {20, 10, 0};
		const uint16 weights[kSdrLaneCount] = {8, 2, 1};
		const EResult result = SteamNetworkingSockets()->ConfigureConnectionLanes(
			conn,
			kSdrLaneCount,
			priorities,
			weights);
		if (result != k_EResultOK)
		{
			std::cerr << "[SDR] ConfigureConnectionLanes failed conn=" << conn
					  << " result=" << static_cast<int>(result)
					  << " (" << ResultName(result) << ")\n";
		}
	}

	bool SteamSdr::SendToConnection(
		HSteamNetConnection conn,
		const void *data,
		std::size_t bytes,
		bool reliable,
		SdrLane lane,
		EResult *out_result) const
	{
		if (conn == k_HSteamNetConnection_Invalid || data == nullptr || bytes == 0 || bytes > UINT32_MAX)
		{
			if (out_result != nullptr)
			{
				*out_result = k_EResultInvalidParam;
			}
			return false;
		}

		auto send_once = [&](SdrLane send_lane) -> EResult
		{
			SteamNetworkingMessage_t *message = SteamNetworkingUtils()->AllocateMessage(static_cast<int>(bytes));
			if (message == nullptr || message->m_pData == nullptr)
			{
				if (message != nullptr)
				{
					message->Release();
				}
				return k_EResultLimitExceeded;
			}

			std::memcpy(message->m_pData, data, bytes);
			message->m_cbSize = static_cast<int>(bytes);
			message->m_conn = conn;
			message->m_nFlags = reliable ? k_nSteamNetworkingSend_ReliableNoNagle
										 : k_nSteamNetworkingSend_UnreliableNoDelay;
			message->m_idxLane = static_cast<std::uint16_t>(send_lane);

			SteamNetworkingMessage_t *messages[1] = {message};
			int64 result_value = 0;
			SteamNetworkingSockets()->SendMessages(1, messages, &result_value);
			if (result_value < 0)
			{
				return static_cast<EResult>(-result_value);
			}
			return k_EResultOK;
		};

		EResult result = send_once(lane);
		if (result != k_EResultOK &&
			lane != SdrLane::kVideo &&
			(result == k_EResultInvalidParam || result == k_EResultInvalidState))
		{
			// If lane configuration is not accepted yet, do not lose reliable
			// control/config traffic. Lane 0 always exists, so it is the safe
			// fallback for correctness; normal priority is restored once lanes work.
			result = send_once(SdrLane::kVideo);
		}

		if (out_result != nullptr)
		{
			*out_result = result;
		}
		return result == k_EResultOK;
	}

	int SteamSdr::BuildConfigOptions(SteamNetworkingConfigValue_t *out_options, int max_options) const
	{
		if (out_options == nullptr || max_options <= 0)
		{
			return 0;
		}

		int count = 0;
		auto add_int32 = [&](ESteamNetworkingConfigValue config_key, int value)
		{
			if (count >= max_options)
			{
				return;
			}
			out_options[count].SetInt32(config_key, value);
			++count;
		};

		const int send_buffer = std::max(config_.send_buffer_size, 64 * 1024);
		const int recv_buffer = std::max(config_.recv_buffer_size, 64 * 1024);
		const int send_rate = std::clamp(config_.send_rate_bytes_per_sec, 64 * 1024, 256 * 1024 * 1024);
		const int nagle_usec = std::max(config_.nagle_time_usec, 0);
		const int p2p_ice_enable = config_.p2p_ice_enable;
		const int p2p_ice_penalty = std::max(config_.p2p_ice_penalty, 0);
		const int p2p_sdr_penalty = std::max(config_.p2p_sdr_penalty, 0);

		add_int32(k_ESteamNetworkingConfig_SendBufferSize, send_buffer);
		add_int32(k_ESteamNetworkingConfig_RecvBufferSize, recv_buffer);
		// Steam docs note this should currently be explicitly fixed by setting min=max.
		add_int32(k_ESteamNetworkingConfig_SendRateMin, send_rate);
		add_int32(k_ESteamNetworkingConfig_SendRateMax, send_rate);
		add_int32(k_ESteamNetworkingConfig_NagleTime, nagle_usec);
		add_int32(k_ESteamNetworkingConfig_P2P_Transport_ICE_Enable, p2p_ice_enable);
		add_int32(k_ESteamNetworkingConfig_P2P_Transport_ICE_Penalty, p2p_ice_penalty);
		add_int32(k_ESteamNetworkingConfig_P2P_Transport_SDR_Penalty, p2p_sdr_penalty);
		return count;
	}

	void SteamSdr::LogDetailedConnectionStatus(HSteamNetConnection conn, const char *phase) const
	{
		if (conn == k_HSteamNetConnection_Invalid)
		{
			return;
		}
		SteamNetConnectionInfo_t info{};
		if (SteamNetworkingSockets()->GetConnectionInfo(conn, &info))
		{
			char remote_addr[128] = {};
			info.m_addrRemote.ToString(remote_addr, sizeof(remote_addr), true);
			const bool is_fast = (info.m_nFlags & k_nSteamNetworkConnectionInfoFlags_Fast) != 0;
			std::cerr << "[SDR] " << (config_.role == SdrRole::kHost ? "host" : "client")
					  << " " << (phase != nullptr ? phase : "status")
					  << " conn=" << conn
					  << " path=" << ConnectionPathNameFromFlags(info.m_nFlags)
					  << " fast=" << (is_fast ? "yes" : "no")
					  << " pop_remote=" << info.m_idPOPRemote
					  << " pop_relay=" << info.m_idPOPRelay
					  << " addr=" << remote_addr
					  << " desc=\"" << info.m_szConnectionDescription << "\""
					  << "\n";
		}

		std::array<char, 4096> text{};
		SteamNetworkingSockets()->GetDetailedConnectionStatus(conn, text.data(), static_cast<int>(text.size()));
		if (text[0] == '\0')
		{
			return;
		}
		const std::size_t text_len = std::strlen(text.data());
		std::cerr << "[SDR] " << (config_.role == SdrRole::kHost ? "host" : "client")
				  << " " << (phase != nullptr ? phase : "status")
				  << " conn=" << conn << " detailed:\n"
				  << text.data();
		if (text_len == 0 || text[text_len - 1] != '\n')
		{
			std::cerr << "\n";
		}
	}

	const char *SteamSdr::ConnectionStateName(ESteamNetworkingConnectionState state)
	{
		switch (state)
		{
		case k_ESteamNetworkingConnectionState_None:
			return "None";
		case k_ESteamNetworkingConnectionState_Connecting:
			return "Connecting";
		case k_ESteamNetworkingConnectionState_FindingRoute:
			return "FindingRoute";
		case k_ESteamNetworkingConnectionState_Connected:
			return "Connected";
		case k_ESteamNetworkingConnectionState_ClosedByPeer:
			return "ClosedByPeer";
		case k_ESteamNetworkingConnectionState_ProblemDetectedLocally:
			return "ProblemDetectedLocally";
		case k_ESteamNetworkingConnectionState_FinWait:
			return "FinWait";
		case k_ESteamNetworkingConnectionState_Linger:
			return "Linger";
		case k_ESteamNetworkingConnectionState_Dead:
			return "Dead";
		default:
			return "Unknown";
		}
	}

	void SteamSdr::LogConnectionEvent(const char *phase, const SteamNetConnectionStatusChangedCallback_t *info) const
	{
		if (info == nullptr)
		{
			return;
		}

		std::cerr << "[SDR] " << (config_.role == SdrRole::kHost ? "host" : "client")
				  << " " << (phase != nullptr ? phase : "event")
				  << " conn=" << info->m_hConn
				  << " path=" << ConnectionPathNameFromFlags(info->m_info.m_nFlags)
				  << " " << ConnectionStateName(info->m_eOldState)
				  << " -> " << ConnectionStateName(info->m_info.m_eState)
				  << " end_reason=" << info->m_info.m_eEndReason;

		if (info->m_info.m_szEndDebug != nullptr && info->m_info.m_szEndDebug[0] != '\0')
		{
			std::cerr << " debug=\"" << info->m_info.m_szEndDebug << "\"";
		}

		const std::uint64_t remote_id = info->m_info.m_identityRemote.GetSteamID64();
		if (remote_id != 0)
		{
			std::cerr << " remote=" << remote_id;
		}

		std::cerr << "\n";
	}

	bool SteamSdr::MatchesConnectionEvent(const SteamNetConnectionStatusChangedCallback_t *info) const
	{
		if (info == nullptr)
		{
			return false;
		}

		std::lock_guard<std::mutex> lock(callback_mutex_);
		if (config_.role == SdrRole::kHost)
		{
			HSteamListenSocket listen = listen_socket_.load();
			if (listen != k_HSteamListenSocket_Invalid && info->m_info.m_hListenSocket == listen)
			{
				return true;
			}
			if (host_connections_.find(info->m_hConn) != host_connections_.end())
			{
				return true;
			}
			return connection_.load() == info->m_hConn;
		}

		return connection_.load() == info->m_hConn;
	}

	void SteamSdr::SteamNetConnectionStatusChangedCallback(SteamNetConnectionStatusChangedCallback_t *info)
	{
		if (info == nullptr)
		{
			return;
		}

		SteamSdr *target = nullptr;
		{
			std::lock_guard<std::mutex> lock(g_instances_mutex);
			for (SteamSdr *instance : g_instances)
			{
				if (instance != nullptr && instance->MatchesConnectionEvent(info))
				{
					target = instance;
					break;
				}
			}
		}

		if (target != nullptr)
		{
			target->OnConnectionStatusChanged(info);
		}
	}

	void SteamSdr::OnConnectionStatusChanged(SteamNetConnectionStatusChangedCallback_t *info)
	{
		std::lock_guard<std::mutex> lock(callback_mutex_);
		LogConnectionEvent("status", info);

		auto register_host_connection = [&]()
		{
			if (config_.role != SdrRole::kHost)
			{
				return false;
			}

			HSteamListenSocket listen = listen_socket_.load();
			const bool matches_listen_socket =
				listen != k_HSteamListenSocket_Invalid && info->m_info.m_hListenSocket == listen;
			const bool already_tracked = host_connections_.find(info->m_hConn) != host_connections_.end();
			if (!matches_listen_socket && !already_tracked)
			{
				return false;
			}

			const std::uint64_t remote_id = info->m_info.m_identityRemote.GetSteamID64();
			if (!host_allowed_ids_.empty() && (remote_id == 0 || host_allowed_ids_.find(remote_id) == host_allowed_ids_.end()))
			{
				std::cerr << "[SDR] Rejecting connection from non-allowed SteamID=" << remote_id << "\n";
				SteamNetworkingSockets()->CloseConnection(info->m_hConn, 0, "not-allowed", false);
				return false;
			}

			ApplyConnectionTuning(info->m_hConn);

			HSteamNetPollGroup pg = poll_group_.load();
			if (pg != k_HSteamNetPollGroup_Invalid)
			{
				bool set_group_ok = SteamNetworkingSockets()->SetConnectionPollGroup(info->m_hConn, pg);
				if (!set_group_ok)
				{
					std::cerr << "[SDR] SetConnectionPollGroup failed while registering host connection.\n";
				}
			}

			host_connections_.insert(info->m_hConn);
			if (remote_id != 0)
			{
				host_connection_remote_ids_[info->m_hConn] = remote_id;
			}

			HSteamNetConnection current = connection_.load();
			if (current == k_HSteamNetConnection_Invalid || host_connections_.find(current) == host_connections_.end())
			{
				connection_.store(info->m_hConn);
			}
			connected_.store(!host_connections_.empty());
			return true;
		};

		switch (info->m_info.m_eState)
		{
		case k_ESteamNetworkingConnectionState_None:
			break;

		case k_ESteamNetworkingConnectionState_Connecting:
		{
			if (config_.role == SdrRole::kHost)
			{
				HSteamListenSocket listen = listen_socket_.load();
				if (listen != k_HSteamListenSocket_Invalid && info->m_info.m_hListenSocket == listen)
				{
					EResult accepted = SteamNetworkingSockets()->AcceptConnection(info->m_hConn);
					if (accepted != k_EResultOK && accepted != k_EResultInvalidState)
					{
						std::cerr << "[SDR] AcceptConnection failed: " << static_cast<int>(accepted) << "\n";
						SteamNetworkingSockets()->CloseConnection(info->m_hConn, 0, "accept-failed", false);
						break;
					}
					if (accepted == k_EResultInvalidState)
					{
						std::cerr << "[SDR] AcceptConnection already advanced for conn=" << info->m_hConn
								  << ", treating as accepted.\n";
					}
					register_host_connection();
				}
			}
			break;
		}

		case k_ESteamNetworkingConnectionState_FindingRoute:
		{
			if (config_.role == SdrRole::kHost)
			{
				register_host_connection();
			}
			break;
		}

		case k_ESteamNetworkingConnectionState_Connected:
		{
			if (config_.role == SdrRole::kHost)
			{
				if (register_host_connection())
				{
					LogDetailedConnectionStatus(info->m_hConn, "connected");
				}
			}
			else
			{
				HSteamNetConnection current = connection_.load();
				if (current != k_HSteamNetConnection_Invalid && current != info->m_hConn)
				{
					SteamNetworkingSockets()->CloseConnection(info->m_hConn, 0, "busy", false);
					break;
				}
				connection_.store(info->m_hConn);
				ApplyConnectionTuning(info->m_hConn);
				HSteamNetPollGroup pg = poll_group_.load();
				if (pg != k_HSteamNetPollGroup_Invalid)
				{
					bool set_group_ok = SteamNetworkingSockets()->SetConnectionPollGroup(info->m_hConn, pg);
					if (!set_group_ok)
					{
						std::cerr << "[SDR] SetConnectionPollGroup failed on connected state.\n";
					}
				}
				connected_.store(true);
				LogDetailedConnectionStatus(info->m_hConn, "connected");
			}
			break;
		}

		case k_ESteamNetworkingConnectionState_ClosedByPeer:
		case k_ESteamNetworkingConnectionState_ProblemDetectedLocally:
		{
			LogDetailedConnectionStatus(info->m_hConn, "closed");
			if (config_.role == SdrRole::kHost)
			{
				host_connections_.erase(info->m_hConn);
				host_connection_remote_ids_.erase(info->m_hConn);
				HSteamNetConnection current = connection_.load();
				if (current == info->m_hConn)
				{
					if (host_connections_.empty())
					{
						connection_.store(k_HSteamNetConnection_Invalid);
					}
					else
					{
						connection_.store(*host_connections_.begin());
					}
				}
				connected_.store(!host_connections_.empty());
			}
			else
			{
				HSteamNetConnection current = connection_.load();
				if (current == info->m_hConn)
				{
					connected_.store(false);
					connection_.store(k_HSteamNetConnection_Invalid);
				}
			}
			SteamNetworkingSockets()->CloseConnection(info->m_hConn, 0, "closed", false);
			break;
		}

		default:
			break;
		}
	}

} // namespace streamproto
