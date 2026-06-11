#pragma once

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

#include "common/audio_stream.h"
#include "common/connection_path.h"
#include "host/host_media_sender.h"
#include "host/sdr_queue_status.h"
#include "host/virtual_desktop_capture_bridge.h"

namespace streamproto::host
{

	struct HostRouteSnapshot
	{
		bool connected = false;
		bool paused = false;
		bool video_source_available = false;
		bool audio_source_available = false;
		std::vector<std::uint64_t> connected_peer_ids;
		std::vector<std::vector<HostMediaTarget>> video_targets_by_lod;
		std::vector<std::vector<HostMediaTarget>> audio_targets_by_lod;
		SdrQueueStatus video_queue_status{};
		std::unordered_map<std::uint64_t, SdrQueueStatus> video_queue_status_by_peer;
		int effective_unreliable_chunk_payload = 12000;
		int unreliable_chunk_payload_limit = 12000;
		int chunk_payload_bytes = 12000;
		int parity_shards = 0;
		bool reliable_video = false;
		bool reliable_keyframes = false;
		int max_queue_ms = 80;
		ConnectionPath active_path = ConnectionPath::kSdr;
	};

	struct SharedRawVideoFrame
	{
		using ReleaseFn = void (*)(RawVideoFrame &);

		RawVideoFrame frame{};
		ReleaseFn release = nullptr;

		~SharedRawVideoFrame()
		{
			if (release != nullptr)
			{
				release(frame);
			}
		}
	};

	struct HostVideoFrameHub
	{
		std::mutex mutex;
		std::condition_variable cv;
		std::shared_ptr<SharedRawVideoFrame> latest;
		std::uint64_t version = 0;
	};

	struct HostAudioFrameHub
	{
		std::mutex mutex;
		std::condition_variable cv;
		std::vector<std::deque<streamproto::audio::AudioFrame>> queues;
	};

} // namespace streamproto::host
