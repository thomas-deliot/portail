#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

#include "common/steam_sdr.h"

namespace streamproto::host
{

	struct SdrQueueStatus
	{
		bool valid = false;
		int pending_unreliable_bytes = 0;
		int pending_reliable_bytes = 0;
		int queue_time_usec = 0;
		int send_rate_bps = 0;
	};

	inline bool TryGetConnectionConfigInt32(
		const streamproto::SteamSdr &sdr,
		ESteamNetworkingConfigValue config_value,
		int &out_value)
	{
		out_value = 0;
		const HSteamNetConnection conn = sdr.ConnectionHandle();
		if (conn == k_HSteamNetConnection_Invalid)
		{
			return false;
		}

		ESteamNetworkingConfigDataType data_type = k_ESteamNetworkingConfig_Int32;
		int value = 0;
		std::size_t value_bytes = sizeof(value);
		const ESteamNetworkingGetConfigValueResult result = SteamNetworkingUtils()->GetConfigValue(
			config_value,
			k_ESteamNetworkingConfig_Connection,
			conn,
			&data_type,
			&value,
			&value_bytes);
		if (result != k_ESteamNetworkingGetConfigValue_OK ||
			data_type != k_ESteamNetworkingConfig_Int32 ||
			value_bytes != sizeof(value))
		{
			return false;
		}

		out_value = value;
		return true;
	}

	inline int ComputeUnreliableChunkPayloadLimit(const streamproto::SteamSdr &sdr, int fallback_limit, int &out_mtu_data_size)
	{
		out_mtu_data_size = 0;
		int mtu_data_size = 0;
		if (!TryGetConnectionConfigInt32(sdr, k_ESteamNetworkingConfig_MTU_DataSize, mtu_data_size) || mtu_data_size <= 0)
		{
			return std::max(512, fallback_limit);
		}
		out_mtu_data_size = mtu_data_size;
		const int mtu_scaled_limit = std::clamp(mtu_data_size * 8, 1200, fallback_limit);
		return std::max(512, mtu_scaled_limit);
	}

	inline int ComputeSendRateBytesPerSecForBitrate(int bitrate_kbps)
	{
		constexpr double kHeadroom = 1.25;
		constexpr int kMinSendRateBps = 512 * 1024;
		constexpr int kMaxSendRateBps = 16 * 1024 * 1024;
		const double payload_bps = static_cast<double>(std::max(bitrate_kbps, 500)) * 1000.0 / 8.0;
		const int target_bps = static_cast<int>(std::llround(payload_bps * kHeadroom));
		return std::clamp(target_bps, kMinSendRateBps, kMaxSendRateBps);
	}

	inline bool ApplyConnectionSendRateLimit(const streamproto::SteamSdr &sdr, int send_rate_bytes_per_sec)
	{
		const std::vector<HSteamNetConnection> connections = sdr.ConnectionHandles();
		if (connections.empty())
		{
			return false;
		}
		const int clamped_rate = std::clamp(send_rate_bytes_per_sec, 64 * 1024, 256 * 1024 * 1024);
		bool all_ok = true;
		for (HSteamNetConnection conn : connections)
		{
			const bool min_ok = SteamNetworkingUtils()->SetConnectionConfigValueInt32(
				conn,
				k_ESteamNetworkingConfig_SendRateMin,
				clamped_rate);
			const bool max_ok = SteamNetworkingUtils()->SetConnectionConfigValueInt32(
				conn,
				k_ESteamNetworkingConfig_SendRateMax,
				clamped_rate);
			all_ok = all_ok && min_ok && max_ok;
		}
		return all_ok;
	}

	inline bool GetSdrConnectionQueueStatus(HSteamNetConnection conn, SdrQueueStatus &status)
	{
		status = {};
		if (conn == k_HSteamNetConnection_Invalid)
		{
			return false;
		}

		SteamNetConnectionRealTimeStatus_t rt{};
		SteamNetConnectionRealTimeLaneStatus_t lanes[streamproto::kSdrLaneCount]{};
		EResult result = SteamNetworkingSockets()->GetConnectionRealTimeStatus(
			conn,
			&rt,
			streamproto::kSdrLaneCount,
			lanes);
		if (result != k_EResultOK)
		{
			return false;
		}

		const SteamNetConnectionRealTimeLaneStatus_t &video_lane = lanes[static_cast<int>(streamproto::SdrLane::kVideo)];
		status.valid = true;
		status.pending_unreliable_bytes = std::max(0, video_lane.m_cbPendingUnreliable);
		status.pending_reliable_bytes = std::max(0, video_lane.m_cbPendingReliable);
		status.queue_time_usec = std::max(0, static_cast<int>(video_lane.m_usecQueueTime));
		status.send_rate_bps = std::max(rt.m_nSendRateBytesPerSecond, 64 * 1024);
		return true;
	}

	inline bool GetSdrQueueStatus(const streamproto::SteamSdr &sdr, SdrQueueStatus &status)
	{
		return GetSdrConnectionQueueStatus(sdr.ConnectionHandle(), status);
	}

	inline bool GetSdrQueueStatusForSteamId(const streamproto::SteamSdr &sdr, std::uint64_t steam_id, SdrQueueStatus &status)
	{
		return GetSdrConnectionQueueStatus(sdr.ConnectionHandleForSteamId(steam_id), status);
	}

	inline bool ShouldRealtimeDrop(const SdrQueueStatus &status, int max_queue_ms)
	{
		if (!status.valid || max_queue_ms <= 0)
		{
			return false;
		}

		const int pending_total = std::max(0, status.pending_unreliable_bytes + status.pending_reliable_bytes);
		const int send_rate = std::max(status.send_rate_bps, 64 * 1024);
		const int pending_limit = std::clamp(send_rate / 2, 96 * 1024, 1024 * 1024);
		const bool pending_over_limit = pending_total > pending_limit;
		const bool queue_delay_over_limit =
			status.queue_time_usec > max_queue_ms * 1000 &&
			pending_total > std::max(64 * 1024, pending_limit / 4);
		return pending_over_limit || queue_delay_over_limit;
	}

} // namespace streamproto::host
