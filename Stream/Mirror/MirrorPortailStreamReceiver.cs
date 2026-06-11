using System;
using System.Collections.Generic;
using Portail.Core;
using Portail.Stream;
using UnityEngine;

namespace Portail.Stream.Mirror
{
	[DefaultExecutionOrder(-9500)]
	[DisallowMultipleComponent]
	public sealed class MirrorPortailStreamReceiver : MonoBehaviour
	{
		[Header("Lifecycle")]
		[Tooltip("Controls which native client streaming plugin logs are forwarded to the Unity console. Native logging stays registered so errors can still be forwarded when this is set to Errors.")]
		public PortailStreamNativeConsoleLogLevel nativeConsoleLogLevel = PortailStreamNativeConsoleLogLevel.All;
		public bool allowReceiving = true;
		public bool autoReceiveAllRemotePlayers = true;

		[Header("Client Settings")]
		public uint appId = 480;
		public bool disableIce;

		[Header("Manual Routing")]
		public List<ulong> manualRemoteHostSteamIds = new List<ulong>();

		[Header("Textures")]
		public int fallbackTextureWidth = 1280;
		public int fallbackTextureHeight = 720;

		[Header("Timing")]
		public float playerRefreshSeconds = 0.5f;
		[Min(0.05f)]
		public float statsRefreshSeconds = 0.5f;

		public static MirrorPortailStreamReceiver Instance { get; private set; }

		readonly Dictionary<int, MirrorPortailParticipant> _players = new Dictionary<int, MirrorPortailParticipant>();
		readonly Dictionary<ulong, RenderTexture> _remoteTextures = new Dictionary<ulong, RenderTexture>();
		readonly Dictionary<ulong, RenderTexture> _remoteDisplayTextures = new Dictionary<ulong, RenderTexture>();
		readonly Dictionary<ulong, int> _pendingDisplayTextureSwapFrames = new Dictionary<ulong, int>();
		readonly Dictionary<ulong, PortailStreamRateSample> _remoteRateSamples = new Dictionary<ulong, PortailStreamRateSample>();
		readonly Dictionary<ulong, PortailStreamRateSample> _remoteVideoRateSamples = new Dictionary<ulong, PortailStreamRateSample>();
		readonly Dictionary<ulong, PortailStreamRateSample> _remoteAudioRateSamples = new Dictionary<ulong, PortailStreamRateSample>();
		readonly Dictionary<ulong, PortailStreamClientPeerStats> _peerStatsByHost = new Dictionary<ulong, PortailStreamClientPeerStats>();
		readonly List<ulong> _lastAppliedRemoteHosts = new List<ulong>();
		readonly HashSet<int> _livePlayerIds = new HashSet<int>();
		readonly List<int> _stalePlayerIds = new List<int>();
		readonly HashSet<ulong> _steamIdSeen = new HashSet<ulong>();
		readonly Dictionary<ulong, int> _maxAcceptedRemoteVideoLods = new Dictionary<ulong, int>();
		readonly Dictionary<ulong, int> _maxAcceptedRemoteAudioLods = new Dictionary<ulong, int>();
		readonly Dictionary<ulong, int> _lastAppliedMaxAcceptedVideoLods = new Dictionary<ulong, int>();
		readonly Dictionary<ulong, int> _lastAppliedMaxAcceptedAudioLods = new Dictionary<ulong, int>();
		readonly List<ulong> _desiredRemoteHostsScratch = new List<ulong>();
		readonly HashSet<ulong> _desiredRemoteHostSetScratch = new HashSet<ulong>();
		readonly List<ulong> _staleRemoteHostsScratch = new List<ulong>();
		readonly float[] _audioScratch = new float[16384];
		PortailStreamClientPeerStats[] _peerStatsScratch = Array.Empty<PortailStreamClientPeerStats>();

		PortailStreamClientPlugin _client;
		MirrorPortailParticipant _localSource;
		float _nextPlayerRefreshAt;
		float _nextStatsRefreshAt;
		bool _lastAppliedDisableIce;
		bool _steamUnavailableLogged;
		bool _initialized;

		public bool IsStreaming => _client != null && _client.IsRunning();
		public bool AllowReceiving => allowReceiving;
		public int ActiveRemoteHostCount => _lastAppliedRemoteHosts.Count;
		public IReadOnlyList<ulong> ActiveRemoteHostSteamIds => _lastAppliedRemoteHosts;
		public Component ClientPluginComponent => _client;

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

			StopClientStream();
			ClearRemoteTextures();
			ClearRemoteVisuals();
			if (_client != null)
				_client.enabled = false;
		}

		void OnDestroy()
		{
			if (Instance == this)
				Instance = null;

			StopClientStream();
			ClearRemoteTextures();
			ClearRemoteVisuals();
			if (_client != null)
				_client.enabled = false;

			_players.Clear();
		}

		void Update()
		{
			EnsureClientPlugin();

			float now = Time.unscaledTime;
			if (now >= _nextPlayerRefreshAt)
			{
				_nextPlayerRefreshAt = now + Mathf.Max(0.1f, playerRefreshSeconds);
				RefreshPlayerViews();
			}

			PublishLocalSteamIdentity();
			EnsureClientStreamRunning();

			if (_client != null && _client.IsRunning() && now >= _nextStatsRefreshAt)
			{
				_nextStatsRefreshAt = now + Mathf.Max(0.05f, statsRefreshSeconds);
				PullStats(now, true);
			}

			PumpRemoteAudio();
			ApplyRemoteSourceOutputs();
		}

		public void SetManualRemoteHosts(IReadOnlyList<ulong> hostSteamIds)
		{
			autoReceiveAllRemotePlayers = false;
			PortailStreamMirrorUtility.SanitizeSteamIds(hostSteamIds, manualRemoteHostSteamIds, _steamIdSeen);
		}

		public void ResetAutomaticPlayerRouting()
		{
			autoReceiveAllRemotePlayers = true;
		}

		void EnsureClientPlugin()
		{
			if (_client == null)
			{
				_client = GetComponent<PortailStreamClientPlugin>();
				if (_client == null)
					_client = gameObject.AddComponent<PortailStreamClientPlugin>();

				_client.autoStart = false;
			}

			_client.enabled = true;
			ApplyNativeConsoleLoggingSettings();
		}


		void ApplyNativeConsoleLoggingSettings()
		{
			PortailStreamNativeLogBridge.ClientConsoleLogLevel = nativeConsoleLogLevel;

			if (_client != null)
			{
				// Keep the native callback registered for every filter mode.
				// The bridge decides which native messages reach the Unity console.
				_client.enableNativeLogging = true;
			}
		}

		void PublishLocalSteamIdentity()
		{
			if (_localSource == null)
				return;

			ulong localSteamId = PortailStreamMirrorUtility.ResolveLocalSteamId(_localSource, null, _client);
			if (localSteamId != 0)
				_localSource.TryPublishOwnerStreamId(localSteamId);
		}

		void EnsureClientStreamRunning()
		{
			if (_client == null)
				return;

			if (!allowReceiving)
			{
				_steamUnavailableLogged = false;
				StopClientStream();
				ClearRemoteTextures();
				ClearRemoteVisuals();
				return;
			}

			List<ulong> desiredRemoteHosts = BuildDesiredRemoteHosts();
			bool shouldRun = global::Mirror.NetworkClient.active && _localSource != null;

			bool restartRequired = _client.IsRunning() &&
				(_client.appId != appId || _lastAppliedDisableIce != disableIce);
			if (restartRequired)
				StopClientStream();

			if (!shouldRun)
			{
				_steamUnavailableLogged = false;
				StopClientStream();
				ClearRemoteTextures();
				ClearRemoteVisuals();
				return;
			}

			if (!PortailStreamMirrorUtility.EnsureSteamAvailable("MirrorPortailStreamReceiver", ref _steamUnavailableLogged))
			{
				StopClientStream();
				ClearRemoteTextures();
				ClearRemoteVisuals();
				return;
			}

			_client.appId = appId;
			_client.disableIce = disableIce;
			_client.codec = ResolveSharedVideoCodec();
			_client.videoLods = ResolveSharedVideoLods();
			_client.ConfigureVideoLods();
			ApplyNativeConsoleLoggingSettings();

			if (!_client.IsRunning())
			{
				if (!_client.StartStreaming())
					return;

				_lastAppliedDisableIce = disableIce;
				_lastAppliedRemoteHosts.Clear();
			}

			SyncRemoteTextures(desiredRemoteHosts);
			ApplyRemoteHostsIfChanged(desiredRemoteHosts);
			ApplyMaxAcceptedRemoteQualities();
		}

		List<PortailStreamVideoLodConfig> ResolveSharedVideoLods()
		{
			MirrorPortailStreamSender host = MirrorPortailStreamSender.Instance;
			if (host != null && host.videoLods != null && host.videoLods.Count > 0)
				return host.videoLods;

			return PortailStreamHostPlugin.CreateDefaultVideoLods();
		}

		string ResolveSharedVideoCodec()
		{
			MirrorPortailStreamSender host = MirrorPortailStreamSender.Instance;
			if (host != null && !string.IsNullOrWhiteSpace(host.codec))
				return host.codec.Trim();

			return "h264";
		}

		void PullStats(float now, bool updateRateSamples)
		{
			_peerStatsByHost.Clear();
			if (_client == null || !_client.IsRunning())
				return;

			int count = GetPeerStatsNonAlloc();
			for (int i = 0; i < count; ++i)
			{
				PortailStreamClientPeerStats stats = _peerStatsScratch[i];
				if (stats.host_steam_id == 0)
					continue;

				_peerStatsByHost[stats.host_steam_id] = stats;
				if (updateRateSamples)
				{
					if (!_remoteRateSamples.TryGetValue(stats.host_steam_id, out PortailStreamRateSample sample))
						sample = default;
					if (!_remoteVideoRateSamples.TryGetValue(stats.host_steam_id, out PortailStreamRateSample videoSample))
						videoSample = default;
					if (!_remoteAudioRateSamples.TryGetValue(stats.host_steam_id, out PortailStreamRateSample audioSample))
						audioSample = default;

					PortailStreamMirrorUtility.UpdateRateSample(ref sample, stats.recv_bytes, stats.decoded_frames, now);
					PortailStreamMirrorUtility.UpdateRateSample(ref videoSample, stats.video_bytes, stats.decoded_frames, now);
					PortailStreamMirrorUtility.UpdateRateSample(ref audioSample, stats.audio_bytes, stats.audio_frames, now);
					_remoteRateSamples[stats.host_steam_id] = sample;
					_remoteVideoRateSamples[stats.host_steam_id] = videoSample;
					_remoteAudioRateSamples[stats.host_steam_id] = audioSample;
				}
			}

			CompleteReadyDisplayTextureSwaps();
			ResizeRemoteTexturesToMatchStats();
		}

		int GetPeerStatsNonAlloc()
		{
			int desiredCapacity = Mathf.Max(16, _remoteTextures.Count + 8, _lastAppliedRemoteHosts.Count + 8);
			if (_peerStatsScratch == null || _peerStatsScratch.Length < desiredCapacity)
				_peerStatsScratch = new PortailStreamClientPeerStats[desiredCapacity];

			int count = _client.GetPeerStatsNonAlloc(_peerStatsScratch);
			if (count < _peerStatsScratch.Length)
				return count;

			_peerStatsScratch = new PortailStreamClientPeerStats[_peerStatsScratch.Length * 2];
			return _client.GetPeerStatsNonAlloc(_peerStatsScratch);
		}

		void PumpRemoteAudio()
		{
			if (_client == null || !_client.IsRunning())
				return;

			foreach (KeyValuePair<int, MirrorPortailParticipant> pair in _players)
			{
				MirrorPortailParticipant source = pair.Value;
				if (source == null || source.isLocalPlayer || source.OwnerStreamId == 0)
					continue;

				if (GetRemoteHostAudioLod(source.OwnerStreamId) < 0)
					continue;

				PumpRemoteAudio(source);
			}
		}

		void PumpRemoteAudio(MirrorPortailParticipant source)
		{
			ulong hostSteamId = source.OwnerStreamId;
			while (true)
			{
				int sampleCount = _client.ReadAudioForHost(hostSteamId, _audioScratch);
				if (sampleCount <= 0)
					break;

				source.Feed.PushAudioSamples(_audioScratch, sampleCount);
				_client.MarkAudioPushedForHost(hostSteamId);
				if (sampleCount < _audioScratch.Length)
					break;
			}
		}

		void ApplyRemoteSourceOutputs()
		{
			foreach (KeyValuePair<int, MirrorPortailParticipant> pair in _players)
			{
				MirrorPortailParticipant source = pair.Value;
				if (source == null || source.isLocalPlayer)
					continue;

				ulong remoteSteamId = source.OwnerStreamId;
				RenderTexture remoteTexture = null;
				if (remoteSteamId != 0 && GetRemoteHostVideoLod(remoteSteamId) >= 0)
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

			if (!_peerStatsByHost.TryGetValue(remoteSteamId, out PortailStreamClientPeerStats stats) || stats.connected == 0)
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

			if (!_peerStatsByHost.TryGetValue(remoteSteamId, out PortailStreamClientPeerStats stats) || stats.connected == 0)
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
			PortailStreamMirrorUtility.RefreshPlayerViews(_players, _livePlayerIds, _stalePlayerIds, out _localSource);
		}

		List<ulong> BuildDesiredRemoteHosts()
		{
			List<ulong> desiredRemoteHosts = autoReceiveAllRemotePlayers
				? PortailStreamMirrorUtility.BuildRemoteStreamIds(_players, _desiredRemoteHostsScratch, _steamIdSeen, _localSource, requireBroadcasting: false)
				: PortailStreamMirrorUtility.SanitizeSteamIds(manualRemoteHostSteamIds, _desiredRemoteHostsScratch, _steamIdSeen);

			return desiredRemoteHosts;
		}

		public void SetGlobalReceivingEnabled(bool enabled)
		{
			allowReceiving = enabled;
			if (!enabled)
			{
				StopClientStream();
				ClearRemoteTextures();
				ClearRemoteVisuals();
			}
		}

		public bool IsRemoteHostReceivingEnabled(ulong hostSteamId)
		{
			return hostSteamId != 0 &&
				   allowReceiving &&
				   (GetRemoteHostVideoLod(hostSteamId) >= 0 ||
					GetRemoteHostAudioLod(hostSteamId) >= 0);
		}

		public void SetRemoteHostReceivingEnabled(ulong hostSteamId, bool enabled)
		{
			if (hostSteamId == 0)
				return;

			SetMaxAcceptedVideoLod(hostSteamId, enabled ? PortailStreamLodUtility.Highest : PortailStreamLodUtility.Off);
			SetMaxAcceptedAudioLod(hostSteamId, enabled ? PortailStreamLodUtility.Highest : PortailStreamLodUtility.Off);
			if (enabled && !autoReceiveAllRemotePlayers && !manualRemoteHostSteamIds.Contains(hostSteamId))
			{
				manualRemoteHostSteamIds.Add(hostSteamId);
				manualRemoteHostSteamIds.Sort();
			}
		}

		public int GetRemoteHostVideoLod(ulong hostSteamId)
		{
			if (hostSteamId == 0)
				return PortailStreamLodUtility.Off;

			if (_peerStatsByHost.TryGetValue(hostSteamId, out PortailStreamClientPeerStats stats))
				return PortailStreamLodUtility.NormalizeLod(stats.effective_video_lod);

			return _maxAcceptedRemoteVideoLods.TryGetValue(hostSteamId, out int lod)
				? PortailStreamLodUtility.NormalizeLod(lod)
				: PortailStreamLodUtility.Highest;
		}

		public int GetRemoteHostAudioLod(ulong hostSteamId)
		{
			if (hostSteamId == 0)
				return PortailStreamLodUtility.Off;

			if (_peerStatsByHost.TryGetValue(hostSteamId, out PortailStreamClientPeerStats stats))
				return PortailStreamLodUtility.NormalizeLod(stats.effective_audio_lod);

			return _maxAcceptedRemoteAudioLods.TryGetValue(hostSteamId, out int lod)
				? PortailStreamLodUtility.NormalizeLod(lod)
				: PortailStreamLodUtility.Highest;
		}

		public int GetMaxAcceptedVideoLod(ulong hostSteamId)
		{
			if (hostSteamId == 0)
				return PortailStreamLodUtility.Off;

			return _maxAcceptedRemoteVideoLods.TryGetValue(hostSteamId, out int lod)
				? PortailStreamLodUtility.NormalizeLod(lod)
				: PortailStreamLodUtility.Highest;
		}

		public int GetMaxAcceptedAudioLod(ulong hostSteamId)
		{
			if (hostSteamId == 0)
				return PortailStreamLodUtility.Off;

			return _maxAcceptedRemoteAudioLods.TryGetValue(hostSteamId, out int lod)
				? PortailStreamLodUtility.NormalizeLod(lod)
				: PortailStreamLodUtility.Highest;
		}

		public void SetMaxAcceptedVideoLod(ulong hostSteamId, int lod)
		{
			if (hostSteamId == 0)
				return;

			lod = PortailStreamLodUtility.NormalizeLod(lod);
			_maxAcceptedRemoteVideoLods[hostSteamId] = lod;
			if (_client != null && _client.IsRunning())
				_client.SetMaxAcceptedVideoLod(hostSteamId, lod);
		}

		public void SetMaxAcceptedAudioLod(ulong hostSteamId, int lod)
		{
			if (hostSteamId == 0)
				return;

			lod = PortailStreamLodUtility.NormalizeLod(lod);
			_maxAcceptedRemoteAudioLods[hostSteamId] = lod;
			if (_client != null && _client.IsRunning())
				_client.SetMaxAcceptedAudioLod(hostSteamId, lod);
		}

		public List<ulong> GetKnownRemoteHostSteamIds(List<ulong> destination)
		{
			if (destination == null)
				destination = new List<ulong>();

			destination.Clear();
			PortailStreamMirrorUtility.BuildRemoteStreamIds(_players, destination, _steamIdSeen, _localSource, requireBroadcasting: false);
			return destination;
		}

		public bool IsRemoteHostCurrentlyActive(ulong hostSteamId)
		{
			return hostSteamId != 0 && _lastAppliedRemoteHosts.Contains(hostSteamId);
		}

		public bool TryGetPeerStatsObject(ulong hostSteamId, out object stats)
		{
			if (_peerStatsByHost.TryGetValue(hostSteamId, out PortailStreamClientPeerStats typedStats))
			{
				stats = typedStats;
				return true;
			}

			stats = null;
			return false;
		}

		public bool TryGetRemoteRate(ulong hostSteamId, out float bitrateKbps, out float fps)
		{
			bitrateKbps = 0f;
			fps = 0f;
			if (!_remoteRateSamples.TryGetValue(hostSteamId, out PortailStreamRateSample sample))
				return false;

			bitrateKbps = sample.bitrateKbps;
			fps = sample.fps;
			return true;
		}

		public bool TryGetRemoteVideoRate(ulong hostSteamId, out float bitrateKbps, out float fps)
		{
			bitrateKbps = 0f;
			fps = 0f;
			if (!_remoteVideoRateSamples.TryGetValue(hostSteamId, out PortailStreamRateSample sample))
				return false;

			bitrateKbps = sample.bitrateKbps;
			fps = sample.fps;
			return true;
		}

		public bool TryGetRemoteAudioRate(ulong hostSteamId, out float bitrateKbps)
		{
			bitrateKbps = 0f;
			if (!_remoteAudioRateSamples.TryGetValue(hostSteamId, out PortailStreamRateSample sample))
				return false;

			bitrateKbps = sample.bitrateKbps;
			return true;
		}

		void SyncRemoteTextures(IReadOnlyList<ulong> desiredRemoteHosts)
		{
			if (_client == null)
				return;

			_desiredRemoteHostSetScratch.Clear();
			for (int i = 0; i < desiredRemoteHosts.Count; ++i)
			{
				ulong hostId = desiredRemoteHosts[i];
				if (hostId == 0 || !_desiredRemoteHostSetScratch.Add(hostId))
					continue;

				int targetWidth = Mathf.Max(16, fallbackTextureWidth);
				int targetHeight = Mathf.Max(16, fallbackTextureHeight);
				if (_peerStatsByHost.TryGetValue(hostId, out PortailStreamClientPeerStats stats))
				{
					if (stats.preview_width > 0)
						targetWidth = stats.preview_width;
					if (stats.preview_height > 0)
						targetHeight = stats.preview_height;
				}

				if (!_remoteTextures.TryGetValue(hostId, out RenderTexture texture) ||
					texture == null ||
					texture.width != targetWidth ||
					texture.height != targetHeight)
				{
					texture = RetargetRemoteTexture(hostId, targetWidth, targetHeight);
				}

				_client.SetOutputTextureForHost(hostId, texture);
			}

			_staleRemoteHostsScratch.Clear();
			foreach (ulong hostId in _remoteTextures.Keys)
			{
				if (!_desiredRemoteHostSetScratch.Contains(hostId))
					_staleRemoteHostsScratch.Add(hostId);
			}

			for (int i = 0; i < _staleRemoteHostsScratch.Count; ++i)
			{
				ulong staleHost = _staleRemoteHostsScratch[i];
				_client.SetOutputTextureForHost(staleHost, null);

				RenderTexture nativeTexture = _remoteTextures[staleHost];
				_remoteDisplayTextures.TryGetValue(staleHost, out RenderTexture displayTexture);
				PortailStreamMirrorUtility.ReleaseTexture(nativeTexture);
				if (displayTexture != null && displayTexture != nativeTexture)
					PortailStreamMirrorUtility.ReleaseTexture(displayTexture);

				_remoteTextures.Remove(staleHost);
				_remoteDisplayTextures.Remove(staleHost);
				_pendingDisplayTextureSwapFrames.Remove(staleHost);
				_remoteRateSamples.Remove(staleHost);
				_remoteVideoRateSamples.Remove(staleHost);
				_remoteAudioRateSamples.Remove(staleHost);
				_peerStatsByHost.Remove(staleHost);
			}
		}

		RenderTexture RetargetRemoteTexture(ulong hostId, int width, int height)
		{
			_remoteTextures.TryGetValue(hostId, out RenderTexture previousNativeTexture);
			_remoteDisplayTextures.TryGetValue(hostId, out RenderTexture currentDisplayTexture);

			RenderTexture nextTexture = PortailStreamMirrorUtility.CreatePreviewTexture($"PortailStream_Remote_{hostId}", width, height);
			_remoteTextures[hostId] = nextTexture;
			_client.SetOutputTextureForHost(hostId, nextTexture);

			if (currentDisplayTexture == null)
			{
				_remoteDisplayTextures[hostId] = nextTexture;
				_pendingDisplayTextureSwapFrames.Remove(hostId);
				if (previousNativeTexture != null && previousNativeTexture != nextTexture)
					PortailStreamMirrorUtility.ReleaseTexture(previousNativeTexture);
				return nextTexture;
			}

			if (previousNativeTexture != null && previousNativeTexture != currentDisplayTexture)
				PortailStreamMirrorUtility.ReleaseTexture(previousNativeTexture);

			_pendingDisplayTextureSwapFrames[hostId] = Time.frameCount;
			return nextTexture;
		}

		void CompleteReadyDisplayTextureSwaps()
		{
			if (_pendingDisplayTextureSwapFrames.Count == 0)
				return;

			_staleRemoteHostsScratch.Clear();
			foreach (KeyValuePair<ulong, int> pair in _pendingDisplayTextureSwapFrames)
			{
				if (Time.frameCount <= pair.Value)
					continue;

				ulong hostId = pair.Key;
				if (!_remoteTextures.TryGetValue(hostId, out RenderTexture nextTexture) || nextTexture == null)
				{
					_staleRemoteHostsScratch.Add(hostId);
					continue;
				}

				_remoteDisplayTextures.TryGetValue(hostId, out RenderTexture previousDisplayTexture);
				_remoteDisplayTextures[hostId] = nextTexture;
				if (previousDisplayTexture != null && previousDisplayTexture != nextTexture)
					PortailStreamMirrorUtility.ReleaseTexture(previousDisplayTexture);

				_staleRemoteHostsScratch.Add(hostId);
			}

			for (int i = 0; i < _staleRemoteHostsScratch.Count; ++i)
				_pendingDisplayTextureSwapFrames.Remove(_staleRemoteHostsScratch[i]);
		}

		void ResizeRemoteTexturesToMatchStats()
		{
			if (_client == null || !_client.IsRunning())
				return;

			foreach (KeyValuePair<ulong, PortailStreamClientPeerStats> pair in _peerStatsByHost)
			{
				ulong hostId = pair.Key;
				PortailStreamClientPeerStats stats = pair.Value;
				if (stats.connected == 0 ||
					stats.effective_video_lod < 0 ||
					stats.preview_width <= 0 ||
					stats.preview_height <= 0)
				{
					continue;
				}

				if (_remoteTextures.TryGetValue(hostId, out RenderTexture existing) &&
					existing != null &&
					existing.width == stats.preview_width &&
					existing.height == stats.preview_height)
				{
					continue;
				}

				RetargetRemoteTexture(hostId, stats.preview_width, stats.preview_height);
			}
		}

		void StopClientStream()
		{
			if (_client != null && _client.IsRunning())
				_client.StopStreaming();

			_lastAppliedRemoteHosts.Clear();
			_lastAppliedMaxAcceptedVideoLods.Clear();
			_lastAppliedMaxAcceptedAudioLods.Clear();
		}

		void ClearRemoteTextures()
		{
			if (_client != null)
			{
				foreach (ulong hostId in _remoteTextures.Keys)
					_client.SetOutputTextureForHost(hostId, null);
			}

			foreach (RenderTexture texture in _remoteTextures.Values)
				PortailStreamMirrorUtility.ReleaseTexture(texture);
			foreach (RenderTexture texture in _remoteDisplayTextures.Values)
			{
				if (texture != null && !_remoteTextures.ContainsValue(texture))
					PortailStreamMirrorUtility.ReleaseTexture(texture);
			}

			_remoteTextures.Clear();
			_remoteDisplayTextures.Clear();
			_pendingDisplayTextureSwapFrames.Clear();
			_remoteRateSamples.Clear();
			_remoteVideoRateSamples.Clear();
			_remoteAudioRateSamples.Clear();
			_peerStatsByHost.Clear();
		}

		void ClearRemoteVisuals()
		{
			foreach (KeyValuePair<int, MirrorPortailParticipant> pair in _players)
			{
				MirrorPortailParticipant source = pair.Value;
				if (source != null && !source.isLocalPlayer)
					source.Feed.ClearDisplayOutput();
			}
		}

		void ApplyRemoteHostsIfChanged(IReadOnlyList<ulong> desiredRemoteHosts)
		{
			if (_client == null || !_client.IsRunning())
				return;

			if (PortailStreamMirrorUtility.SteamIdListsEqual(_lastAppliedRemoteHosts, desiredRemoteHosts))
				return;

			_client.SetRemoteHostSteamIds(desiredRemoteHosts);
			PortailStreamMirrorUtility.CopySteamIds(desiredRemoteHosts, _lastAppliedRemoteHosts);
		}

		void ApplyMaxAcceptedRemoteQualities()
		{
			if (_client == null || !_client.IsRunning())
				return;

			foreach (ulong hostSteamId in _lastAppliedRemoteHosts)
			{
				int videoLod = _maxAcceptedRemoteVideoLods.TryGetValue(hostSteamId, out int requestedVideo)
					? PortailStreamLodUtility.NormalizeLod(requestedVideo)
					: PortailStreamLodUtility.Highest;
				if (!_lastAppliedMaxAcceptedVideoLods.TryGetValue(hostSteamId, out int lastVideo) || lastVideo != videoLod)
				{
					_client.SetMaxAcceptedVideoLod(hostSteamId, videoLod);
					_lastAppliedMaxAcceptedVideoLods[hostSteamId] = videoLod;
				}

				int audioLod = _maxAcceptedRemoteAudioLods.TryGetValue(hostSteamId, out int requestedAudio)
					? PortailStreamLodUtility.NormalizeLod(requestedAudio)
					: PortailStreamLodUtility.Highest;
				if (!_lastAppliedMaxAcceptedAudioLods.TryGetValue(hostSteamId, out int lastAudio) || lastAudio != audioLod)
				{
					_client.SetMaxAcceptedAudioLod(hostSteamId, audioLod);
					_lastAppliedMaxAcceptedAudioLods[hostSteamId] = audioLod;
				}
			}
		}
	}
}
