#pragma once

#include <cstdint>

#include "common/steam_sdr.h"

namespace streamproto
{

	enum class ConnectionPath
	{
		kIce,
		kSdr,
	};

	inline const char *ConnectionPathName(ConnectionPath path)
	{
		return path == ConnectionPath::kIce ? "ice" : "sdr";
	}

	inline std::int32_t ExportConnectionPath(ConnectionPath path, bool connected)
	{
		if (!connected)
		{
			return 0;
		}
		return path == ConnectionPath::kIce ? 1 : 2;
	}

	inline bool GetConnectionPath(const SteamSdr &sdr, ConnectionPath &out_path)
	{
		out_path = ConnectionPath::kSdr;
		for (HSteamNetConnection conn : sdr.ConnectionHandles())
		{
			if (conn == k_HSteamNetConnection_Invalid)
			{
				continue;
			}
			SteamNetConnectionInfo_t info{};
			if (!SteamNetworkingSockets()->GetConnectionInfo(conn, &info))
			{
				continue;
			}
			const bool is_relayed = (info.m_nFlags & k_nSteamNetworkConnectionInfoFlags_Relayed) != 0;
			out_path = is_relayed ? ConnectionPath::kSdr : ConnectionPath::kIce;
			return true;
		}
		return false;
	}

} // namespace streamproto
