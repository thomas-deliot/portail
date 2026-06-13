using System;
using System.Collections.Generic;
using Portail.Core;
using UnityEngine;

namespace Portail.Stream
{
	[DefaultExecutionOrder(-9500)]
	[DisallowMultipleComponent]
	public sealed class PortailStreamReceiver : MonoBehaviour
	{
		[Header("Lifecycle")]
		[Tooltip("Controls which native receiver streaming plugin logs are forwarded to the Unity console. Native logging stays registered so errors can still be forwarded when this is set to Errors.")]
		public PortailStreamNativeConsoleLogLevel nativeConsoleLogLevel = PortailStreamNativeConsoleLogLevel.All;
		public bool allowReceiving = true;
		public bool autoReceiveAllRemotePlayers = true;

		[Header("Receiver Settings")]
		public uint appId = 480;
		public bool disableIce;

		[Header("Manual Routing")]
		public List<ulong> manualRemoteSenderSteamIds = new List<ulong>();

		[Header("Textures")]
		public int fallbackTextureWidth = 1280;
		public int fallbackTextureHeight = 720;

		[Header("Timing")]
		public float playerRefreshSeconds = 0.5f;
		[Min(0.05f)]
		public float statsRefreshSeconds = 0.5f;

		public static PortailStreamReceiver Instance { get; private set; }

		readonly Dictionary<int, PortailStreamParticipant> _players = new Dictionary<int, PortailStreamParticipant>();
		readonly Dictionary<ulong, RenderTexture> _remoteTextures = new Dictionary<ulong, RenderTexture>();
		readonly Dictionary<ulong, RenderTexture> _remoteDisplayTextures = new Dictionary<ulong, RenderTexture>();
		readonly Dictionary<ulong, int> _pendingDisplayTextureSwapFrames = new Dictionary<ulong, int>();
		readonly Dictionary<ulong, PortailStreamRateSample> _remoteRateSamples = new Dictionary<ulong, PortailStreamRateSample>();
		readonly Dictionary<ulong, PortailStreamRateSample> _remoteVideoRateSamples = new Dictionary<ulong, PortailStreamRateSample>();
		readonly Dictionary<ulong, PortailStreamRateSample> _remoteAudioRateSamples = new Dictionary<ulong, PortailStreamRateSample>();
		readonly Dictionary<ulong, PortailStreamReceiverPeerStats> _peerStatsBySender = new Dictionary<ulong, PortailStreamReceiverPeerStats>();
		readonly List<ulong> _lastAppliedRemoteSenders = new List<ulong>();
		readonly HashSet<int> _livePlayerIds = new HashSet<int>();
		readonly List<int> _stalePlayerIds = new List<int>();
		readonly HashSet<ulong> _steamIdSeen = new HashSet<ulong>();
		readonly Dictionary<ulong, int> _maxAcceptedRemoteVideoLods = new Dictionary<ulong, int>();
		readonly Dictionary<ulong, int> _maxAcceptedRemoteAudioLods = new Dictionary<ulong, int>();
		readonly Dictionary<ulong, int> _lastAppliedMaxAcceptedVideoLods = new Dictionary<ulong, int>();
		readonly Dictionary<ulong, int> _lastAppliedMaxAcceptedAudioLods = new Dictionary<ulong, int>();
		readonly List<ulong> _desiredRemoteSendersScratch = new List<ulong>();
		readonly HashSet<ulong> _desiredRemoteSenderSetScratch = new HashSet<ulong>();
		readonly List<ulong> _staleRemoteSendersScratch = new List<ulong>();
		readonly float[] _audioScratch = new float[16384];
		PortailStreamReceiverPeerStats[] _peerStatsScratch = Array.Empty<PortailStreamReceiverPeerStats>();

		PortailStreamReceiverPlugin _receiver;
		PortailStreamParticipant _localSource;
		float _nextPlayerRefreshAt;
		float _nextStatsRefreshAt;
		bool _lastAppliedDisableIce;
		bool _steamUnavailableLogged;
		bool _initialized;

		public bool IsStreaming => _receiver != null && _receiver.IsRunning();
		public bool AllowReceiving => allowReceiving;
		public int ActiveRemoteSenderCount => _lastAppliedRemoteSenders.Count;
		public IReadOnlyList<ulong> ActiveRemoteSenderSteamIds => _lastAppliedRemoteSenders;
		public Component ReceiverPluginComponent => _receiver;

		void Awake()
		{
			if (Instance != null && Instance != this)
			{
				Destroy(this);
				return;
			}
		}

		void OnEnable()
		{
			if (Instance != null && Instance != this)
			{
				enabled = false;
				return;
			}

			Instance = this;
			if (_initialized)
				return;

			RefreshPlayerViews();
			_initialized = true;
		}

		void OnDisable()
		{
			if (Instance == this)
				Instance = null;

			StopReceiverStream();
			ClearRemoteTextures();
			ClearRemoteVisuals();
			if (_receiver != null)
				_receiver.enabled = false;
		}

		void OnDestroy()
		{
			if (Instance == this)
				Instance = null;

			StopReceiverStream();
			ClearRemoteTextures();
			ClearRemoteVisuals();
			if (_receiver != null)
				_receiver.enabled = false;

			_players.Clear();
		}

		void Update()
		{
			EnsureReceiverPlugin();

			float now = Time.unscaledTime;
			if (now >= _nextPlayerRefreshAt)
			{
				_nextPlayerRefreshAt = now + Mathf.Max(0.1f, playerRefreshSeconds);
				RefreshPlayerViews();
			}

			PublishLocalSteamIdentity();
			EnsureReceiverStreamRunning();

			if (_receiver != null && _receiver.IsRunning() && now >= _nextStatsRefreshAt)
			{
				_nextStatsRefreshAt = now + Mathf.Max(0.05f, statsRefreshSeconds);
				PullStats(now, true);
			}

			PumpRemoteAudio();
			ApplyRemoteSourceOutputs();
		}

		public void SetManualRemoteSenders(IReadOnlyList<ulong> senderSteamIds)
		{
			autoReceiveAllRemotePlayers = false;
			PortailStreamNetworkUtility.SanitizeSteamIds(senderSteamIds, manualRemoteSenderSteamIds, _steamIdSeen);
		}

		public void ResetAutomaticPlayerRouting()
		{
			autoReceiveAllRemotePlayers = true;
		}

		void EnsureReceiverPlugin()
		{
			if (_receiver == null)
			{
				_receiver = GetComponent<PortailStreamReceiverPlugin>();
				if (_receiver == null)
					_receiver = gameObject.AddComponent<PortailStreamReceiverPlugin>();

				_receiver.autoStart = false;
			}

			_receiver.enabled = true;
			ApplyNativeConsoleLoggingSettings();
		}


		void ApplyNativeConsoleLoggingSettings()
		{
			PortailStreamNativeLogBridge.ReceiverConsoleLogLevel = nativeConsoleLogLevel;

			if (_receiver != null)
			{
				// Keep the native callback registered for every filter mode.
				// The bridge decides which native messages reach the Unity console.
				_receiver.enableNativeLogging = true;
			}
		}

		void PublishLocalSteamIdentity()
		{
			if (_localSource == null)
				return;

			ulong localSteamId = PortailStreamNetworkUtility.ResolveLocalSteamId(_localSource, null, _receiver);
			if (localSteamId != 0)
				_localSource.TryPublishOwnerStreamId(localSteamId);
		}

		void EnsureReceiverStreamRunning()
		{
			if (_receiver == null)
				return;

			if (!allowReceiving)
			{
				_steamUnavailableLogged = false;
				StopReceiverStream();
				ClearRemoteTextures();
				ClearRemoteVisuals();
				return;
			}

			List<ulong> desiredRemoteSenders = BuildDesiredRemoteSenders();
			bool shouldRun = global::Mirror.NetworkClient.active && _localSource != null;

			bool restartRequired = _receiver.IsRunning() &&
				(_receiver.appId != appId || _lastAppliedDisableIce != disableIce);
			if (restartRequired)
				StopReceiverStream();

			if (!shouldRun)
			{
				_steamUnavailableLogged = false;
				StopReceiverStream();
				ClearRemoteTextures();
				ClearRemoteVisuals();
				return;
			}

			if (!PortailStreamNetworkUtility.EnsureSteamAvailable("PortailStreamReceiver", ref _steamUnavailableLogged))
			{
				StopReceiverStream();
				ClearRemoteTextures();
				ClearRemoteVisuals();
				return;
			}

			_receiver.appId = appId;
			_receiver.disableIce = disableIce;
			_receiver.codec = ResolveSharedVideoCodec();
			_receiver.videoLods = ResolveSharedVideoLods();
			_receiver.ConfigureVideoLods();
			ApplyNativeConsoleLoggingSettings();

			if (!_receiver.IsRunning())
			{
				if (!_receiver.StartStreaming())
					return;

				_lastAppliedDisableIce = disableIce;
				_lastAppliedRemoteSenders.Clear();
			}

			SyncRemoteTextures(desiredRemoteSenders);
			ApplyRemoteSendersIfChanged(desiredRemoteSenders);
			ApplyMaxAcceptedRemoteQualities();
		}

		List<PortailStreamVideoLodConfig> ResolveSharedVideoLods()
		{
			PortailStreamSender sender = PortailStreamSender.Instance;
			if (sender != null && sender.videoLods != null && sender.videoLods.Count > 0)
				return sender.videoLods;

			return PortailStreamSenderPlugin.CreateDefaultVideoLods();
		}

		string ResolveSharedVideoCodec()
		{
			PortailStreamSender sender = PortailStreamSender.Instance;
			if (sender != null && !string.IsNullOrWhiteSpace(sender.codec))
				return sender.codec.Trim();

			return "h264";
		}

		void PullStats(float now, bool updateRateSamples)
		{
			_peerStatsBySender.Clear();
			if (_receiver == null || !_receiver.IsRunning())
				return;

			int count = GetPeerStatsNonAlloc();
			for (int i = 0; i < count; ++i)
			{
				PortailStreamReceiverPeerStats stats = _peerStatsScratch[i];
				if (stats.sender_steam_id == 0)
					continue;

				_peerStatsBySender[stats.sender_steam_id] = stats;
				if (updateRateSamples)
				{
					if (!_remoteRateSamples.TryGetValue(stats.sender_steam_id, out PortailStreamRateSample sample))
						sample = default;
					if (!_remoteVideoRateSamples.TryGetValue(stats.sender_steam_id, out PortailStreamRateSample videoSample))
						videoSample = default;
					if (!_remoteAudioRateSamples.TryGetValue(stats.sender_steam_id, out PortailStreamRateSample audioSample))
						audioSample = default;

					PortailStreamNetworkUtility.UpdateRateSample(ref sample, stats.recv_bytes, stats.decoded_frames, now);
					PortailStreamNetworkUtility.UpdateRateSample(ref videoSample, stats.video_bytes, stats.decoded_frames, now);
					PortailStreamNetworkUtility.UpdateRateSample(ref audioSample, stats.audio_bytes, stats.audio_frames, now);
					_remoteRateSamples[stats.sender_steam_id] = sample;
					_remoteVideoRateSamples[stats.sender_steam_id] = videoSample;
					_remoteAudioRateSamples[stats.sender_steam_id] = audioSample;
				}
			}

			CompleteReadyDisplayTextureSwaps();
			ResizeRemoteTexturesToMatchStats();
		}

		int GetPeerStatsNonAlloc()
		{
			int desiredCapacity = Mathf.Max(16, _remoteTextures.Count + 8, _lastAppliedRemoteSenders.Count + 8);
			if (_peerStatsScratch == null || _peerStatsScratch.Length < desiredCapacity)
				_peerStatsScratch = new PortailStreamReceiverPeerStats[desiredCapacity];

			int count = _receiver.GetPeerStatsNonAlloc(_peerStatsScratch);
			if (count < _peerStatsScratch.Length)
				return count;

			_peerStatsScratch = new PortailStreamReceiverPeerStats[_peerStatsScratch.Length * 2];
			return _receiver.GetPeerStatsNonAlloc(_peerStatsScratch);
		}

		void PumpRemoteAudio()
		{
			if (_receiver == null || !_receiver.IsRunning())
				return;

			foreach (KeyValuePair<int, PortailStreamParticipant> pair in _players)
			{
				PortailStreamParticipant source = pair.Value;
				if (source == null || source.isLocalPlayer || source.OwnerStreamId == 0)
					continue;

				if (GetRemoteSenderAudioLod(source.OwnerStreamId) < 0)
					continue;

				PumpRemoteAudio(source);
			}
		}

		void PumpRemoteAudio(PortailStreamParticipant source)
		{
			ulong senderSteamId = source.OwnerStreamId;
			while (true)
			{
				int sampleCount = _receiver.ReadAudioForSender(senderSteamId, _audioScratch);
				if (sampleCount <= 0)
					break;

				source.Feed.PushAudioSamples(_audioScratch, sampleCount);
				_receiver.MarkAudioPushedForSender(senderSteamId);
				if (sampleCount < _audioScratch.Length)
					break;
			}
		}

		void ApplyRemoteSourceOutputs()
		{
			foreach (KeyValuePair<int, PortailStreamParticipant> pair in _players)
			{
				PortailStreamParticipant source = pair.Value;
				if (source == null || source.isLocalPlayer)
					continue;

				ulong remoteSteamId = source.OwnerStreamId;
				RenderTexture remoteTexture = null;
				if (remoteSteamId != 0 && GetRemoteSenderVideoLod(remoteSteamId) >= 0)
					_remoteDisplayTextures.TryGetValue(remoteSteamId, out remoteTexture);

				source.Feed.SetDisplayOutput(
					remoteTexture,
					BuildRemoteDisplayInfo(remoteSteamId),
					string.Empty);
			}
		}

		PortailFeedInfo BuildRemoteDisplayInfo(ulong remoteSteamId)
		{
			PortailFeedInfo info = default;
			if (remoteSteamId == 0)
				return info;

			if (!_peerStatsBySender.TryGetValue(remoteSteamId, out PortailStreamReceiverPeerStats stats) || stats.connected == 0)
				return info;

			_remoteRateSamples.TryGetValue(remoteSteamId, out PortailStreamRateSample sample);
			info.hasSignal = stats.effective_video_lod >= 0;
			info.isLocalSource = false;
			info.pathLabel = stats.connection_path == 1 ? "ice" : stats.connection_path == 2 ? "sdr" : "none";
			info.bitrateKbps = sample.bitrateKbps;
			info.width = stats.preview_width > 0 ? stats.preview_width : Mathf.Max(16, fallbackTextureWidth);
			info.height = stats.preview_height > 0 ? stats.preview_height : Mathf.Max(16, fallbackTextureHeight);
			info.fps = sample.fps;
			return info;
		}

		string BuildRemoteOverlayText(ulong remoteSteamId)
		{
			if (remoteSteamId == 0)
				return "Waiting for stream id";

			if (!_peerStatsBySender.TryGetValue(remoteSteamId, out PortailStreamReceiverPeerStats stats) || stats.connected == 0)
				return "Waiting for stream";

			_remoteRateSamples.TryGetValue(remoteSteamId, out PortailStreamRateSample sample);
			string path = stats.connection_path == 1 ? "ice" : stats.connection_path == 2 ? "sdr" : "none";
			int streamWidth = stats.preview_width > 0 ? stats.preview_width : Mathf.Max(16, fallbackTextureWidth);
			int streamHeight = stats.preview_height > 0 ? stats.preview_height : Mathf.Max(16, fallbackTextureHeight);
			int videoLod = PortailStreamLodUtility.NormalizeLod(stats.effective_video_lod);
			int audioLod = PortailStreamLodUtility.NormalizeLod(stats.effective_audio_lod);
			return
				$"{PortailStreamLodUtility.ToVideoLabel(videoLod)} {PortailStreamLodUtility.ToAudioLabel(audioLod)} {path}\n" +
				$"{sample.bitrateKbps:0} kbps {streamWidth}x{streamHeight} {sample.fps:0.#} fps\n" +
				$"audio {(stats.audio_frames > 0 ? "on" : "waiting")}";
		}

		void RefreshPlayerViews()
		{
			PortailStreamNetworkUtility.RefreshPlayerViews(_players, _livePlayerIds, _stalePlayerIds, out _localSource);
		}

		List<ulong> BuildDesiredRemoteSenders()
		{
			List<ulong> desiredRemoteSenders = autoReceiveAllRemotePlayers
				? PortailStreamNetworkUtility.BuildRemoteStreamIds(_players, _desiredRemoteSendersScratch, _steamIdSeen, _localSource, requireBroadcasting: false)
				: PortailStreamNetworkUtility.SanitizeSteamIds(manualRemoteSenderSteamIds, _desiredRemoteSendersScratch, _steamIdSeen);

			return desiredRemoteSenders;
		}

		public void SetGlobalReceivingEnabled(bool enabled)
		{
			allowReceiving = enabled;
			if (!enabled)
			{
				StopReceiverStream();
				ClearRemoteTextures();
				ClearRemoteVisuals();
			}
		}

		public bool IsRemoteSenderReceivingEnabled(ulong senderSteamId)
		{
			return senderSteamId != 0 &&
				   allowReceiving &&
				   (GetRemoteSenderVideoLod(senderSteamId) >= 0 ||
					GetRemoteSenderAudioLod(senderSteamId) >= 0);
		}

		public void SetRemoteSenderReceivingEnabled(ulong senderSteamId, bool enabled)
		{
			if (senderSteamId == 0)
				return;

			SetMaxAcceptedVideoLod(senderSteamId, enabled ? PortailStreamLodUtility.Highest : PortailStreamLodUtility.Off);
			SetMaxAcceptedAudioLod(senderSteamId, enabled ? PortailStreamLodUtility.Highest : PortailStreamLodUtility.Off);
			if (enabled && !autoReceiveAllRemotePlayers && !manualRemoteSenderSteamIds.Contains(senderSteamId))
			{
				manualRemoteSenderSteamIds.Add(senderSteamId);
				manualRemoteSenderSteamIds.Sort();
			}
		}

		public int GetRemoteSenderVideoLod(ulong senderSteamId)
		{
			if (senderSteamId == 0)
				return PortailStreamLodUtility.Off;

			if (_peerStatsBySender.TryGetValue(senderSteamId, out PortailStreamReceiverPeerStats stats))
				return PortailStreamLodUtility.NormalizeLod(stats.effective_video_lod);

			return _maxAcceptedRemoteVideoLods.TryGetValue(senderSteamId, out int lod)
				? PortailStreamLodUtility.NormalizeLod(lod)
				: PortailStreamLodUtility.Highest;
		}

		public int GetRemoteSenderAudioLod(ulong senderSteamId)
		{
			if (senderSteamId == 0)
				return PortailStreamLodUtility.Off;

			if (_peerStatsBySender.TryGetValue(senderSteamId, out PortailStreamReceiverPeerStats stats))
				return PortailStreamLodUtility.NormalizeLod(stats.effective_audio_lod);

			return _maxAcceptedRemoteAudioLods.TryGetValue(senderSteamId, out int lod)
				? PortailStreamLodUtility.NormalizeLod(lod)
				: PortailStreamLodUtility.Highest;
		}

		public int GetMaxAcceptedVideoLod(ulong senderSteamId)
		{
			if (senderSteamId == 0)
				return PortailStreamLodUtility.Off;

			return _maxAcceptedRemoteVideoLods.TryGetValue(senderSteamId, out int lod)
				? PortailStreamLodUtility.NormalizeLod(lod)
				: PortailStreamLodUtility.Highest;
		}

		public int GetMaxAcceptedAudioLod(ulong senderSteamId)
		{
			if (senderSteamId == 0)
				return PortailStreamLodUtility.Off;

			return _maxAcceptedRemoteAudioLods.TryGetValue(senderSteamId, out int lod)
				? PortailStreamLodUtility.NormalizeLod(lod)
				: PortailStreamLodUtility.Highest;
		}

		public void SetMaxAcceptedVideoLod(ulong senderSteamId, int lod)
		{
			if (senderSteamId == 0)
				return;

			lod = PortailStreamLodUtility.NormalizeLod(lod);
			_maxAcceptedRemoteVideoLods[senderSteamId] = lod;
			if (_receiver != null && _receiver.IsRunning())
				_receiver.SetMaxAcceptedVideoLod(senderSteamId, lod);
		}

		public void SetMaxAcceptedAudioLod(ulong senderSteamId, int lod)
		{
			if (senderSteamId == 0)
				return;

			lod = PortailStreamLodUtility.NormalizeLod(lod);
			_maxAcceptedRemoteAudioLods[senderSteamId] = lod;
			if (_receiver != null && _receiver.IsRunning())
				_receiver.SetMaxAcceptedAudioLod(senderSteamId, lod);
		}

		public List<ulong> GetKnownRemoteSenderSteamIds(List<ulong> destination)
		{
			if (destination == null)
				destination = new List<ulong>();

			destination.Clear();
			PortailStreamNetworkUtility.BuildRemoteStreamIds(_players, destination, _steamIdSeen, _localSource, requireBroadcasting: false);
			return destination;
		}

		public bool IsRemoteSenderCurrentlyActive(ulong senderSteamId)
		{
			return senderSteamId != 0 && _lastAppliedRemoteSenders.Contains(senderSteamId);
		}

		public bool TryGetPeerStatsObject(ulong senderSteamId, out object stats)
		{
			if (_peerStatsBySender.TryGetValue(senderSteamId, out PortailStreamReceiverPeerStats typedStats))
			{
				stats = typedStats;
				return true;
			}

			stats = null;
			return false;
		}

		public bool TryGetRemoteRate(ulong senderSteamId, out float bitrateKbps, out float fps)
		{
			bitrateKbps = 0f;
			fps = 0f;
			if (!_remoteRateSamples.TryGetValue(senderSteamId, out PortailStreamRateSample sample))
				return false;

			bitrateKbps = sample.bitrateKbps;
			fps = sample.fps;
			return true;
		}

		public bool TryGetRemoteVideoRate(ulong senderSteamId, out float bitrateKbps, out float fps)
		{
			bitrateKbps = 0f;
			fps = 0f;
			if (!_remoteVideoRateSamples.TryGetValue(senderSteamId, out PortailStreamRateSample sample))
				return false;

			bitrateKbps = sample.bitrateKbps;
			fps = sample.fps;
			return true;
		}

		public bool TryGetRemoteAudioRate(ulong senderSteamId, out float bitrateKbps)
		{
			bitrateKbps = 0f;
			if (!_remoteAudioRateSamples.TryGetValue(senderSteamId, out PortailStreamRateSample sample))
				return false;

			bitrateKbps = sample.bitrateKbps;
			return true;
		}

		void SyncRemoteTextures(IReadOnlyList<ulong> desiredRemoteSenders)
		{
			if (_receiver == null)
				return;

			_desiredRemoteSenderSetScratch.Clear();
			for (int i = 0; i < desiredRemoteSenders.Count; ++i)
			{
				ulong senderId = desiredRemoteSenders[i];
				if (senderId == 0 || !_desiredRemoteSenderSetScratch.Add(senderId))
					continue;

				int targetWidth = Mathf.Max(16, fallbackTextureWidth);
				int targetHeight = Mathf.Max(16, fallbackTextureHeight);
				if (_peerStatsBySender.TryGetValue(senderId, out PortailStreamReceiverPeerStats stats))
				{
					if (stats.preview_width > 0)
						targetWidth = stats.preview_width;
					if (stats.preview_height > 0)
						targetHeight = stats.preview_height;
				}

				if (!_remoteTextures.TryGetValue(senderId, out RenderTexture texture) ||
					texture == null ||
					texture.width != targetWidth ||
					texture.height != targetHeight)
				{
					texture = RetargetRemoteTexture(senderId, targetWidth, targetHeight);
				}

				_receiver.SetOutputTextureForSender(senderId, texture);
			}

			_staleRemoteSendersScratch.Clear();
			foreach (ulong senderId in _remoteTextures.Keys)
			{
				if (!_desiredRemoteSenderSetScratch.Contains(senderId))
					_staleRemoteSendersScratch.Add(senderId);
			}

			for (int i = 0; i < _staleRemoteSendersScratch.Count; ++i)
			{
				ulong staleSender = _staleRemoteSendersScratch[i];
				_receiver.SetOutputTextureForSender(staleSender, null);

				RenderTexture nativeTexture = _remoteTextures[staleSender];
				_remoteDisplayTextures.TryGetValue(staleSender, out RenderTexture displayTexture);
				PortailStreamNetworkUtility.ReleaseTexture(nativeTexture);
				if (displayTexture != null && displayTexture != nativeTexture)
					PortailStreamNetworkUtility.ReleaseTexture(displayTexture);

				_remoteTextures.Remove(staleSender);
				_remoteDisplayTextures.Remove(staleSender);
				_pendingDisplayTextureSwapFrames.Remove(staleSender);
				_remoteRateSamples.Remove(staleSender);
				_remoteVideoRateSamples.Remove(staleSender);
				_remoteAudioRateSamples.Remove(staleSender);
				_peerStatsBySender.Remove(staleSender);
			}
		}

		RenderTexture RetargetRemoteTexture(ulong senderId, int width, int height)
		{
			_remoteTextures.TryGetValue(senderId, out RenderTexture previousNativeTexture);
			_remoteDisplayTextures.TryGetValue(senderId, out RenderTexture currentDisplayTexture);

			RenderTexture nextTexture = PortailStreamNetworkUtility.CreatePreviewTexture($"PortailStream_Remote_{senderId}", width, height);
			_remoteTextures[senderId] = nextTexture;
			_receiver.SetOutputTextureForSender(senderId, nextTexture);

			if (currentDisplayTexture == null)
			{
				_remoteDisplayTextures[senderId] = nextTexture;
				_pendingDisplayTextureSwapFrames.Remove(senderId);
				if (previousNativeTexture != null && previousNativeTexture != nextTexture)
					PortailStreamNetworkUtility.ReleaseTexture(previousNativeTexture);
				return nextTexture;
			}

			if (previousNativeTexture != null && previousNativeTexture != currentDisplayTexture)
				PortailStreamNetworkUtility.ReleaseTexture(previousNativeTexture);

			_pendingDisplayTextureSwapFrames[senderId] = Time.frameCount;
			return nextTexture;
		}

		void CompleteReadyDisplayTextureSwaps()
		{
			if (_pendingDisplayTextureSwapFrames.Count == 0)
				return;

			_staleRemoteSendersScratch.Clear();
			foreach (KeyValuePair<ulong, int> pair in _pendingDisplayTextureSwapFrames)
			{
				if (Time.frameCount <= pair.Value)
					continue;

				ulong senderId = pair.Key;
				if (!_remoteTextures.TryGetValue(senderId, out RenderTexture nextTexture) || nextTexture == null)
				{
					_staleRemoteSendersScratch.Add(senderId);
					continue;
				}

				_remoteDisplayTextures.TryGetValue(senderId, out RenderTexture previousDisplayTexture);
				_remoteDisplayTextures[senderId] = nextTexture;
				if (previousDisplayTexture != null && previousDisplayTexture != nextTexture)
					PortailStreamNetworkUtility.ReleaseTexture(previousDisplayTexture);

				_staleRemoteSendersScratch.Add(senderId);
			}

			for (int i = 0; i < _staleRemoteSendersScratch.Count; ++i)
				_pendingDisplayTextureSwapFrames.Remove(_staleRemoteSendersScratch[i]);
		}

		void ResizeRemoteTexturesToMatchStats()
		{
			if (_receiver == null || !_receiver.IsRunning())
				return;

			foreach (KeyValuePair<ulong, PortailStreamReceiverPeerStats> pair in _peerStatsBySender)
			{
				ulong senderId = pair.Key;
				PortailStreamReceiverPeerStats stats = pair.Value;
				if (stats.connected == 0 ||
					stats.effective_video_lod < 0 ||
					stats.preview_width <= 0 ||
					stats.preview_height <= 0)
				{
					continue;
				}

				if (_remoteTextures.TryGetValue(senderId, out RenderTexture existing) &&
					existing != null &&
					existing.width == stats.preview_width &&
					existing.height == stats.preview_height)
				{
					continue;
				}

				RetargetRemoteTexture(senderId, stats.preview_width, stats.preview_height);
			}
		}

		void StopReceiverStream()
		{
			if (_receiver != null && _receiver.IsRunning())
				_receiver.StopStreaming();

			_lastAppliedRemoteSenders.Clear();
			_lastAppliedMaxAcceptedVideoLods.Clear();
			_lastAppliedMaxAcceptedAudioLods.Clear();
		}

		void ClearRemoteTextures()
		{
			if (_receiver != null)
			{
				foreach (ulong senderId in _remoteTextures.Keys)
					_receiver.SetOutputTextureForSender(senderId, null);
			}

			foreach (RenderTexture texture in _remoteTextures.Values)
				PortailStreamNetworkUtility.ReleaseTexture(texture);
			foreach (RenderTexture texture in _remoteDisplayTextures.Values)
			{
				if (texture != null && !_remoteTextures.ContainsValue(texture))
					PortailStreamNetworkUtility.ReleaseTexture(texture);
			}

			_remoteTextures.Clear();
			_remoteDisplayTextures.Clear();
			_pendingDisplayTextureSwapFrames.Clear();
			_remoteRateSamples.Clear();
			_remoteVideoRateSamples.Clear();
			_remoteAudioRateSamples.Clear();
			_peerStatsBySender.Clear();
		}

		void ClearRemoteVisuals()
		{
			foreach (KeyValuePair<int, PortailStreamParticipant> pair in _players)
			{
				PortailStreamParticipant source = pair.Value;
				if (source != null && !source.isLocalPlayer)
					source.Feed.ClearDisplayOutput();
			}
		}

		void ApplyRemoteSendersIfChanged(IReadOnlyList<ulong> desiredRemoteSenders)
		{
			if (_receiver == null || !_receiver.IsRunning())
				return;

			if (PortailStreamNetworkUtility.SteamIdListsEqual(_lastAppliedRemoteSenders, desiredRemoteSenders))
				return;

			_receiver.SetRemoteSenderSteamIds(desiredRemoteSenders);
			PortailStreamNetworkUtility.CopySteamIds(desiredRemoteSenders, _lastAppliedRemoteSenders);
		}

		void ApplyMaxAcceptedRemoteQualities()
		{
			if (_receiver == null || !_receiver.IsRunning())
				return;

			foreach (ulong senderSteamId in _lastAppliedRemoteSenders)
			{
				int videoLod = _maxAcceptedRemoteVideoLods.TryGetValue(senderSteamId, out int requestedVideo)
					? PortailStreamLodUtility.NormalizeLod(requestedVideo)
					: PortailStreamLodUtility.Highest;
				if (!_lastAppliedMaxAcceptedVideoLods.TryGetValue(senderSteamId, out int lastVideo) || lastVideo != videoLod)
				{
					_receiver.SetMaxAcceptedVideoLod(senderSteamId, videoLod);
					_lastAppliedMaxAcceptedVideoLods[senderSteamId] = videoLod;
				}

				int audioLod = _maxAcceptedRemoteAudioLods.TryGetValue(senderSteamId, out int requestedAudio)
					? PortailStreamLodUtility.NormalizeLod(requestedAudio)
					: PortailStreamLodUtility.Highest;
				if (!_lastAppliedMaxAcceptedAudioLods.TryGetValue(senderSteamId, out int lastAudio) || lastAudio != audioLod)
				{
					_receiver.SetMaxAcceptedAudioLod(senderSteamId, audioLod);
					_lastAppliedMaxAcceptedAudioLods[senderSteamId] = audioLod;
				}
			}
		}
	}
}
