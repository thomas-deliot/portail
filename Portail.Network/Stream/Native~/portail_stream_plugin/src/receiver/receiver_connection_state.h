#pragma once

#include <cstdint>
#include <memory>

#include "receiver/receiver_stats.h"
#include "common/connection_path.h"
#include "common/steam_sdr.h"

namespace streamproto::receiver
{

	struct ReceiverConnectionState
	{
		std::unique_ptr<SteamSdr> sdr;
		ClockSyncState clock_sync{};
		ConnectionPath active_path = ConnectionPath::kSdr;
		std::uint64_t last_path_probe_ms = 0;
		bool receiver_pause_sent = false;
		bool sender_paused = false;

		void ResetTransient()
		{
			clock_sync = {};
			active_path = ConnectionPath::kSdr;
			last_path_probe_ms = 0;
			receiver_pause_sent = false;
			sender_paused = false;
		}
	};

} // namespace streamproto::receiver
