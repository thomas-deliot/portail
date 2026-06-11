#pragma once

#include <cstdint>
#include <memory>

#include "client/client_stats.h"
#include "common/connection_path.h"
#include "common/steam_sdr.h"

namespace streamproto::client
{

	struct ClientConnectionState
	{
		std::unique_ptr<SteamSdr> sdr;
		ClockSyncState clock_sync{};
		ConnectionPath active_path = ConnectionPath::kSdr;
		std::uint64_t last_path_probe_ms = 0;
		bool client_pause_sent = false;
		bool host_paused = false;

		void ResetTransient()
		{
			clock_sync = {};
			active_path = ConnectionPath::kSdr;
			last_path_probe_ms = 0;
			client_pause_sent = false;
			host_paused = false;
		}
	};

} // namespace streamproto::client
