using System;
using System.Collections.Generic;
using Portail.Core;
using Portail.Stream;
using UnityEngine;

namespace Portail.Stream.Mirror
{
	[DefaultExecutionOrder(-9500)]
	[DisallowMultipleComponent]
	public sealed class MirrorPortailStreamSender : MonoBehaviour
	{
		[Header("Lifecycle")]
		[Tooltip("Controls which native host streaming plugin logs are forwarded to the Unity console. Native logging stays registered so errors can still be forwarded when this is set to Errors.")]
		public PortailStreamNativeConsoleLogLevel nativeConsoleLogLevel = PortailStreamNativeConsoleLogLevel.All;
		public bool allowSending = true;

		[Header("Source")]
		[SerializeField] PortailFeed sourceFeed;

		[Header("Host Settings")]
		public uint appId = 480;
		public List<PortailStreamVideoLodConfig> videoLods = PortailStreamHostPlugin.CreateDefaultVideoLods();
		public List<PortailStreamAudioLodConfig> audioLods = PortailStreamHostPlugin.CreateDefaultAudioLods();
		public bool disableIce;
		public int chunkPayloadBytes = 24000;
		public int parityShards;
		public bool reliableVideo;
		public bool reliableKeyframes;
		public int maxQueueMs = 120;
		public string codec = "h264";
		public string encoder = "auto";

		[Header("Routing")]
		public bool autoTargetAllRemotePlayers = true;
		public List<ulong> manualHostTargetSteamIds = new List<ulong>();

		[Header("Textures")]
		public int fallbackTextureWidth = 1280;
		public int fallbackTextureHeight = 720;

		[Header("Timing")]
		public float playerRefreshSeconds = 0.5f;
		[Min(0.05f)]
		public float statsRefreshSeconds = 0.5f;

		public static MirrorPortailStreamSender Instance { get; private set; }

		readonly Dictionary<int, MirrorPortailParticipant> _players = new Dictionary<int, MirrorPortailParticipant>();
		readonly Dictionary<ulong, PortailStreamHostPeerStats> _peerStatsByClient = new Dictionary<ulong, PortailStreamHostPeerStats>();
		readonly Dictionary<ulong, PortailStreamRateSample> _clientRateSamples = new Dictionary<ulong, PortailStreamRateSample>();
		readonly List<ulong> _lastAppliedHostTargets = new List<ulong>();
		readonly HashSet<int> _livePlayerIds = new HashSet<int>();
		readonly List<int> _stalePlayerIds = new List<int>();
		readonly HashSet<ulong> _steamIdSeen = new HashSet<ulong>();
		readonly Dictionary<ulong, int> _maxAllowedClientVideoLods = new Dictionary<ulong, int>();
		readonly Dictionary<ulong, int> _maxAllowedClientAudioLods = new Dictionary<ulong, int>();
		readonly Dictionary<ulong, int> _lastAppliedMaxAllowedClientVideoLods = new Dictionary<ulong, int>();
		readonly Dictionary<ulong, int> _lastAppliedMaxAllowedClientAudioLods = new Dictionary<ulong, int>();
		readonly List<ulong> _desiredHostTargetsScratch = new List<ulong>();
		readonly List<ulong> _staleClientSteamIds = new List<ulong>();
		readonly List<PortailStreamVideoLodConfig> _lastAppliedVideoLods = new List<PortailStreamVideoLodConfig>();
		readonly List<PortailStreamAudioLodConfig> _lastAppliedAudioLods = new List<PortailStreamAudioLodConfig>();
		readonly List<bool> _lastAppliedVideoLodEnabled = new List<bool>();
		readonly List<bool> _lastAppliedAudioLodEnabled = new List<bool>();

		PortailStreamHostPlugin _host;
		MirrorPortailParticipant _localSource;
		float _nextPlayerRefreshAt;
		float _nextStatsRefreshAt;
		PortailStreamHostStats _hostStats;
		PortailStreamHostVideoLodStats[] _videoLodStats = Array.Empty<PortailStreamHostVideoLodStats>();
		PortailStreamHostAudioLodStats[] _audioLodStats = Array.Empty<PortailStreamHostAudioLodStats>();
		PortailStreamRateSample _hostRateSample;
		PortailStreamRateSample _hostEncodedVideoRateSample;
		PortailStreamRateSample _hostEncodedAudioRateSample;
		PortailStreamRateSample[] _videoLodRateSamples = Array.Empty<PortailStreamRateSample>();
		PortailStreamRateSample[] _audioLodRateSamples = Array.Empty<PortailStreamRateSample>();
		bool _lastAppliedDisableIce;
		string _lastAppliedCodec;
		string _lastAppliedEncoder;
		int _lastAppliedChunkBytes;
		int _lastAppliedParityShards;
		bool _lastAppliedReliableVideo;
		bool _lastAppliedReliableKeyframes;
		int _lastAppliedMaxQueueMs;
		bool _steamUnavailableLogged;
		bool _initialized;

		public bool IsStreaming => _host != null && _host.IsRunning();
		public PortailStreamHostStats CurrentStats => _hostStats;
		public IReadOnlyList<PortailStreamHostVideoLodStats> CurrentVideoLodStats => _videoLodStats;
		public IReadOnlyList<PortailStreamHostAudioLodStats> CurrentAudioLodStats => _audioLodStats;
		public float CurrentBitrateKbps => _hostRateSample.bitrateKbps;
		public float CurrentEncodedVideoBitrateKbps => _hostEncodedVideoRateSample.bitrateKbps;
		public float CurrentEncodedAudioBitrateKbps => _hostEncodedAudioRateSample.bitrateKbps;
		public float CurrentFps => _hostEncodedVideoRateSample.fps;
		public int CurrentEncodedWidth => _hostStats.encoded_width > 0 ? _hostStats.encoded_width : GetVideoLodWidth(0);
		public int CurrentEncodedHeight => _hostStats.encoded_height > 0 ? _hostStats.encoded_height : GetVideoLodHeight(0);
		public int CurrentEncodedFpsTarget => _hostStats.encoded_fps > 0 ? _hostStats.encoded_fps : GetVideoLodFpsTarget(0);
		public bool AllowSending => allowSending;
		public int ActiveClientCount => _lastAppliedHostTargets.Count;
		public IReadOnlyList<ulong> ActiveClientSteamIds => _lastAppliedHostTargets;
		public Component HostPluginComponent => _host;
		public object CurrentStatsBoxed => _hostStats;
		public PortailFeed SourceFeed => sourceFeed != null ? sourceFeed : _localSource != null ? _localSource.Feed : null;

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

			EnsureLodDefaults();
			RefreshPlayerViews();
			_initialized = true;
		}

		void OnDisable()
		{
			if (Instance == this)
				Instance = null;

			StopHostStream();
			if (_host != null)
				_host.enabled = false;
		}

		void OnDestroy()
		{
			if (Instance == this)
				Instance = null;

			StopHostStream();
			if (_host != null)
				_host.enabled = false;

			_players.Clear();
		}

		void Update()
		{
			EnsureHostPlugin();

			float now = Time.unscaledTime;
			if (now >= _nextPlayerRefreshAt)
			{
				_nextPlayerRefreshAt = now + Mathf.Max(0.1f, playerRefreshSeconds);
				RefreshPlayerViews();
			}

			PublishLocalSteamIdentity();
			EnsureHostStreamRunning();

			if ((_host != null && _host.IsRunning()) ||
				SourceFeed != null)
			{
				if (now >= _nextStatsRefreshAt)
				{
					_nextStatsRefreshAt = now + Mathf.Max(0.05f, statsRefreshSeconds);
					PullStats(now);
				}
			}

		}

		public void SetManualHostTargets(IReadOnlyList<ulong> clientSteamIds)
		{
			autoTargetAllRemotePlayers = false;
			PortailStreamMirrorUtility.SanitizeSteamIds(clientSteamIds, manualHostTargetSteamIds, _steamIdSeen);
		}

		public void ResetAutomaticPlayerRouting()
		{
			autoTargetAllRemotePlayers = true;
		}

		public void SetSourceFeed(PortailFeed feed)
		{
			sourceFeed = feed;
		}

		public void ClearSourceFeed(PortailFeed feed)
		{
			if (sourceFeed == feed)
				sourceFeed = null;
		}

		void EnsureHostPlugin()
		{
			if (_host == null)
			{
				_host = GetComponent<PortailStreamHostPlugin>();
				if (_host == null)
					_host = gameObject.AddComponent<PortailStreamHostPlugin>();

				_host.autoStart = false;
			}

			_host.enabled = true;
			ApplyNativeConsoleLoggingSettings();
		}


		void ApplyNativeConsoleLoggingSettings()
		{
			PortailStreamNativeLogBridge.HostConsoleLogLevel = nativeConsoleLogLevel;

			if (_host != null)
			{
				// Keep the native callback registered for every filter mode.
				// The bridge decides which native messages reach the Unity console.
				_host.enableNativeLogging = true;
			}
		}

		void PublishLocalSteamIdentity()
		{
			if (_localSource == null)
				return;

			ulong localSteamId = PortailStreamMirrorUtility.ResolveLocalSteamId(_localSource, _host, null);
			if (localSteamId != 0)
				_localSource.TryPublishOwnerStreamId(localSteamId);
		}

		void EnsureHostStreamRunning()
		{
			if (_host == null)
				return;

			if (!allowSending)
			{
				_steamUnavailableLogged = false;
				StopHostStream();
				return;
			}

			List<ulong> desiredTargets = BuildDesiredHostTargets();
			bool shouldRun = global::Mirror.NetworkClient.active && _localSource != null && desiredTargets.Count > 0;
			bool configChanged = HasHostRestartRequired();

			if (!shouldRun)
			{
				_steamUnavailableLogged = false;
				StopHostStream();
				return;
			}

			if (!PortailStreamMirrorUtility.EnsureSteamAvailable("MirrorPortailStreamSender", ref _steamUnavailableLogged))
			{
				StopHostStream();
				return;
			}

			if (_host.IsRunning() && configChanged)
				StopHostStream();

			ApplyHostSettings();

			if (!_host.IsRunning())
			{
				if (!_host.StartStreaming())
					return;

				RememberAppliedHostSettings();
				_lastAppliedHostTargets.Clear();
			}

			ApplyLodEnabledStates();
			ApplyHostTargetsIfChanged(desiredTargets);
			ApplyMaxAllowedClientLods();
		}

		void ApplyHostSettings()
		{
			EnsureLodDefaults();
			_host.appId = appId;
			_host.videoLods = videoLods;
			_host.audioLods = audioLods;
			_host.disableIce = disableIce;
			_host.chunkPayloadBytes = Mathf.Clamp(chunkPayloadBytes, 512, 60000);
			_host.parityShards = Mathf.Clamp(parityShards, 0, 1);
			_host.reliableVideo = reliableVideo;
			_host.reliableKeyframes = reliableKeyframes;
			_host.maxQueueMs = Mathf.Clamp(maxQueueMs, 0, 5000);
			_host.codec = string.IsNullOrWhiteSpace(codec) ? "h264" : codec.Trim();
			_host.encoder = string.IsNullOrWhiteSpace(encoder) ? "auto" : encoder.Trim();
			_host.previewTexture = null;
			ApplyNativeConsoleLoggingSettings();
		}

		void PullStats(float now)
		{
			if (_host != null && _host.IsRunning())
			{
				_hostStats = _host.GetStats();
				ulong frameCounter = _hostStats.encoded_frames > 0 ? _hostStats.encoded_frames : _hostStats.capture_frames;
				PortailStreamMirrorUtility.UpdateRateSample(ref _hostRateSample, _hostStats.sent_video_bytes + _hostStats.sent_audio_bytes, frameCounter, now);
				PortailStreamMirrorUtility.UpdateRateSample(ref _hostEncodedVideoRateSample, _hostStats.encoded_video_bytes, frameCounter, now);
				PortailStreamMirrorUtility.UpdateRateSample(ref _hostEncodedAudioRateSample, _hostStats.encoded_audio_bytes, _hostStats.encoded_audio_frames, now);
				PullLodStats(now);
				PullPeerStats(now);
				return;
			}

			_hostStats = default;
			_videoLodStats = Array.Empty<PortailStreamHostVideoLodStats>();
			_audioLodStats = Array.Empty<PortailStreamHostAudioLodStats>();
			_peerStatsByClient.Clear();
			_clientRateSamples.Clear();
			PortailStreamMirrorUtility.UpdateRateSample(ref _hostRateSample, 0, 0, now);
			PortailStreamMirrorUtility.UpdateRateSample(ref _hostEncodedVideoRateSample, 0, 0, now);
			PortailStreamMirrorUtility.UpdateRateSample(ref _hostEncodedAudioRateSample, 0, 0, now);
		}

		void PullLodStats(float now)
		{
			int videoCount = Mathf.Max(1, videoLods != null ? videoLods.Count : 0);
			int audioCount = Mathf.Max(1, audioLods != null ? audioLods.Count : 0);
			_videoLodStats = _host.GetVideoLodStats(videoCount);
			_audioLodStats = _host.GetAudioLodStats(audioCount);
			EnsureRateSampleSize(ref _videoLodRateSamples, _videoLodStats.Length);
			EnsureRateSampleSize(ref _audioLodRateSamples, _audioLodStats.Length);

			for (int i = 0; i < _videoLodStats.Length; ++i)
			{
				PortailStreamHostVideoLodStats stats = _videoLodStats[i];
				int index = stats.index >= 0 && stats.index < _videoLodRateSamples.Length ? stats.index : i;
				PortailStreamRateSample sample = _videoLodRateSamples[index];
				PortailStreamMirrorUtility.UpdateRateSample(ref sample, stats.encoded_video_bytes, stats.encoded_frames, now);
				_videoLodRateSamples[index] = sample;
			}

			for (int i = 0; i < _audioLodStats.Length; ++i)
			{
				PortailStreamHostAudioLodStats stats = _audioLodStats[i];
				int index = stats.index >= 0 && stats.index < _audioLodRateSamples.Length ? stats.index : i;
				PortailStreamRateSample sample = _audioLodRateSamples[index];
				PortailStreamMirrorUtility.UpdateRateSample(ref sample, stats.encoded_audio_bytes, stats.encoded_audio_frames, now);
				_audioLodRateSamples[index] = sample;
			}
		}

		void PullPeerStats(float now)
		{
			_peerStatsByClient.Clear();
			if (_host == null || !_host.IsRunning())
				return;

			PortailStreamHostPeerStats[] peerStats = _host.GetPeerStats(Mathf.Max(16, _lastAppliedHostTargets.Count + 8));
			for (int i = 0; i < peerStats.Length; ++i)
			{
				PortailStreamHostPeerStats stats = peerStats[i];
				if (stats.client_steam_id == 0)
					continue;

				_peerStatsByClient[stats.client_steam_id] = stats;
				if (!_clientRateSamples.TryGetValue(stats.client_steam_id, out PortailStreamRateSample sample))
					sample = default;

				PortailStreamMirrorUtility.UpdateRateSample(ref sample, stats.sent_video_bytes + stats.sent_audio_bytes, stats.encoded_frames, now);
				_clientRateSamples[stats.client_steam_id] = sample;
			}

			_staleClientSteamIds.Clear();
			foreach (ulong clientSteamId in _clientRateSamples.Keys)
			{
				if (!_peerStatsByClient.ContainsKey(clientSteamId))
					_staleClientSteamIds.Add(clientSteamId);
			}

			for (int i = 0; i < _staleClientSteamIds.Count; ++i)
				_clientRateSamples.Remove(_staleClientSteamIds[i]);
		}

		void RefreshPlayerViews()
		{
			PortailStreamMirrorUtility.RefreshPlayerViews(_players, _livePlayerIds, _stalePlayerIds, out _localSource);
		}

		List<ulong> BuildDesiredHostTargets()
		{
			List<ulong> desiredTargets = autoTargetAllRemotePlayers
				? PortailStreamMirrorUtility.BuildRemoteStreamIds(_players, _desiredHostTargetsScratch, _steamIdSeen, _localSource, requireBroadcasting: false)
				: PortailStreamMirrorUtility.SanitizeSteamIds(manualHostTargetSteamIds, _desiredHostTargetsScratch, _steamIdSeen);

			return desiredTargets;
		}

		public void SetGlobalSendingEnabled(bool enabled)
		{
			allowSending = enabled;
			if (!enabled)
				StopHostStream();
		}

		public bool IsClientSendingEnabled(ulong clientSteamId)
		{
			return clientSteamId != 0 &&
				   allowSending &&
				   (GetClientVideoLod(clientSteamId) >= 0 ||
					GetClientAudioLod(clientSteamId) >= 0);
		}

		public void SetClientSendingEnabled(ulong clientSteamId, bool enabled)
		{
			if (clientSteamId == 0)
				return;

			SetClientMaxAllowedVideoLod(clientSteamId, enabled ? PortailStreamLodUtility.Highest : PortailStreamLodUtility.Off);
			SetClientMaxAllowedAudioLod(clientSteamId, enabled ? PortailStreamLodUtility.Highest : PortailStreamLodUtility.Off);
			if (enabled && !autoTargetAllRemotePlayers && !manualHostTargetSteamIds.Contains(clientSteamId))
			{
				manualHostTargetSteamIds.Add(clientSteamId);
				manualHostTargetSteamIds.Sort();
			}
		}

		public int GetClientVideoLod(ulong clientSteamId)
		{
			return GetClientMaxAllowedVideoLod(clientSteamId);
		}

		public int GetClientAudioLod(ulong clientSteamId)
		{
			return GetClientMaxAllowedAudioLod(clientSteamId);
		}

		public int GetClientMaxAllowedVideoLod(ulong clientSteamId)
		{
			if (clientSteamId == 0)
				return PortailStreamLodUtility.Off;

			if (_peerStatsByClient.TryGetValue(clientSteamId, out PortailStreamHostPeerStats stats))
				return PortailStreamLodUtility.NormalizeLod(stats.assigned_video_lod);

			return _maxAllowedClientVideoLods.TryGetValue(clientSteamId, out int lod)
				? PortailStreamLodUtility.NormalizeLod(lod)
				: PortailStreamLodUtility.Highest;
		}

		public int GetClientMaxAllowedAudioLod(ulong clientSteamId)
		{
			if (clientSteamId == 0)
				return PortailStreamLodUtility.Off;

			if (_peerStatsByClient.TryGetValue(clientSteamId, out PortailStreamHostPeerStats stats))
				return PortailStreamLodUtility.NormalizeLod(stats.assigned_audio_lod);

			return _maxAllowedClientAudioLods.TryGetValue(clientSteamId, out int lod)
				? PortailStreamLodUtility.NormalizeLod(lod)
				: PortailStreamLodUtility.Highest;
		}

		public int GetAvailableVideoLodForClient(ulong clientSteamId)
		{
			if (clientSteamId != 0 && _peerStatsByClient.TryGetValue(clientSteamId, out PortailStreamHostPeerStats stats))
				return PortailStreamLodUtility.NormalizeLod(stats.available_video_lod);

			return PortailStreamLodUtility.Off;
		}

		public int GetAvailableAudioLodForClient(ulong clientSteamId)
		{
			if (clientSteamId != 0 && _peerStatsByClient.TryGetValue(clientSteamId, out PortailStreamHostPeerStats stats))
				return PortailStreamLodUtility.NormalizeLod(stats.available_audio_lod);

			return PortailStreamLodUtility.Off;
		}

		public void SetClientVideoLod(ulong clientSteamId, int lod)
		{
			SetClientMaxAllowedVideoLod(clientSteamId, lod);
		}

		public void SetClientAudioLod(ulong clientSteamId, int lod)
		{
			SetClientMaxAllowedAudioLod(clientSteamId, lod);
		}

		public void SetClientMaxAllowedVideoLod(ulong clientSteamId, int lod)
		{
			if (clientSteamId == 0)
				return;

			lod = PortailStreamLodUtility.NormalizeLod(lod);
			_maxAllowedClientVideoLods[clientSteamId] = lod;
			if (_host != null && _host.IsRunning())
				_host.SetClientMaxAllowedVideoLod(clientSteamId, lod);
		}

		public void SetClientMaxAllowedAudioLod(ulong clientSteamId, int lod)
		{
			if (clientSteamId == 0)
				return;

			lod = PortailStreamLodUtility.NormalizeLod(lod);
			_maxAllowedClientAudioLods[clientSteamId] = lod;
			if (_host != null && _host.IsRunning())
				_host.SetClientMaxAllowedAudioLod(clientSteamId, lod);
		}

		public List<ulong> GetKnownRemoteClientSteamIds(List<ulong> destination)
		{
			if (destination == null)
				destination = new List<ulong>();

			destination.Clear();
			PortailStreamMirrorUtility.BuildRemoteStreamIds(_players, destination, _steamIdSeen, _localSource, requireBroadcasting: false);
			return destination;
		}

		public bool IsClientCurrentlyActive(ulong clientSteamId)
		{
			return clientSteamId != 0 && _lastAppliedHostTargets.Contains(clientSteamId);
		}

		public bool TryGetRateForClient(ulong clientSteamId, out float bitrateKbps, out float fps)
		{
			bitrateKbps = 0f;
			fps = 0f;
			if (clientSteamId == 0 || !_lastAppliedHostTargets.Contains(clientSteamId))
				return false;

			if (_clientRateSamples.TryGetValue(clientSteamId, out PortailStreamRateSample sample))
			{
				bitrateKbps = sample.bitrateKbps;
				fps = sample.fps;
				return true;
			}

			bitrateKbps = _lastAppliedHostTargets.Count > 0 ? _hostRateSample.bitrateKbps / Mathf.Max(1, _lastAppliedHostTargets.Count) : 0f;
			fps = _hostRateSample.fps;
			return true;
		}

		public bool TryGetPeerStatsObject(ulong clientSteamId, out object stats)
		{
			if (_peerStatsByClient.TryGetValue(clientSteamId, out PortailStreamHostPeerStats typedStats))
			{
				stats = typedStats;
				return true;
			}

			stats = null;
			return false;
		}

		public bool TryGetPeerStats(ulong clientSteamId, out PortailStreamHostPeerStats stats)
		{
			return _peerStatsByClient.TryGetValue(clientSteamId, out stats);
		}

		bool HasHostRestartRequired()
		{
			if (_host == null || !_host.IsRunning())
				return false;

			return _lastAppliedDisableIce != disableIce ||
				   !VideoLodConfigsMatchIgnoringEnabled(_lastAppliedVideoLods, videoLods) ||
				   !AudioLodConfigsMatchIgnoringEnabled(_lastAppliedAudioLods, audioLods) ||
				   _lastAppliedChunkBytes != Mathf.Clamp(chunkPayloadBytes, 512, 60000) ||
				   _lastAppliedParityShards != Mathf.Clamp(parityShards, 0, 1) ||
				   _lastAppliedReliableVideo != reliableVideo ||
				   _lastAppliedReliableKeyframes != reliableKeyframes ||
				   _lastAppliedMaxQueueMs != Mathf.Clamp(maxQueueMs, 0, 5000) ||
				   !string.Equals(_lastAppliedCodec, string.IsNullOrWhiteSpace(codec) ? "h264" : codec.Trim(), StringComparison.Ordinal) ||
				   !string.Equals(_lastAppliedEncoder, string.IsNullOrWhiteSpace(encoder) ? "auto" : encoder.Trim(), StringComparison.Ordinal) ||
				   _host.appId != appId;
		}

		void RememberAppliedHostSettings()
		{
			EnsureLodDefaults();
			_lastAppliedDisableIce = disableIce;
			CopyVideoLodConfigs(videoLods, _lastAppliedVideoLods);
			CopyAudioLodConfigs(audioLods, _lastAppliedAudioLods);
			CopyVideoLodEnabledStates(videoLods, _lastAppliedVideoLodEnabled);
			CopyAudioLodEnabledStates(audioLods, _lastAppliedAudioLodEnabled);
			_lastAppliedChunkBytes = Mathf.Clamp(chunkPayloadBytes, 512, 60000);
			_lastAppliedParityShards = Mathf.Clamp(parityShards, 0, 1);
			_lastAppliedReliableVideo = reliableVideo;
			_lastAppliedReliableKeyframes = reliableKeyframes;
			_lastAppliedMaxQueueMs = Mathf.Clamp(maxQueueMs, 0, 5000);
			_lastAppliedCodec = string.IsNullOrWhiteSpace(codec) ? "h264" : codec.Trim();
			_lastAppliedEncoder = string.IsNullOrWhiteSpace(encoder) ? "auto" : encoder.Trim();
		}

		void StopHostStream()
		{
			if (_host != null && _host.IsRunning())
				_host.StopStreaming();

			_hostStats = default;
			_videoLodStats = Array.Empty<PortailStreamHostVideoLodStats>();
			_audioLodStats = Array.Empty<PortailStreamHostAudioLodStats>();
			_hostRateSample = default;
			_hostEncodedVideoRateSample = default;
			_hostEncodedAudioRateSample = default;
			_videoLodRateSamples = Array.Empty<PortailStreamRateSample>();
			_audioLodRateSamples = Array.Empty<PortailStreamRateSample>();
			_lastAppliedHostTargets.Clear();
			_lastAppliedMaxAllowedClientVideoLods.Clear();
			_lastAppliedMaxAllowedClientAudioLods.Clear();
			_lastAppliedVideoLodEnabled.Clear();
			_lastAppliedAudioLodEnabled.Clear();
			_peerStatsByClient.Clear();
			_clientRateSamples.Clear();
		}

		void ApplyLodEnabledStates()
		{
			if (_host == null || !_host.IsRunning())
				return;

			EnsureLodDefaults();
			EnsureBoolListSize(_lastAppliedVideoLodEnabled, videoLods.Count, false);
			for (int i = 0; i < videoLods.Count; ++i)
			{
				bool enabled = videoLods[i] != null && videoLods[i].enabled;
				if (_lastAppliedVideoLodEnabled[i] == enabled)
					continue;
				_host.SetVideoLodEnabled(i, enabled);
				_lastAppliedVideoLodEnabled[i] = enabled;
			}

			EnsureBoolListSize(_lastAppliedAudioLodEnabled, audioLods.Count, false);
			for (int i = 0; i < audioLods.Count; ++i)
			{
				bool enabled = audioLods[i] != null && audioLods[i].enabled;
				if (_lastAppliedAudioLodEnabled[i] == enabled)
					continue;
				_host.SetAudioLodEnabled(i, enabled);
				_lastAppliedAudioLodEnabled[i] = enabled;
			}
		}

		void ApplyHostTargetsIfChanged(IReadOnlyList<ulong> desiredTargets)
		{
			if (_host == null || !_host.IsRunning())
				return;

			if (PortailStreamMirrorUtility.SteamIdListsEqual(_lastAppliedHostTargets, desiredTargets))
				return;

			_host.SetClientSteamIds(desiredTargets);
			PortailStreamMirrorUtility.CopySteamIds(desiredTargets, _lastAppliedHostTargets);
		}

		void ApplyMaxAllowedClientLods()
		{
			if (_host == null || !_host.IsRunning())
				return;

			foreach (KeyValuePair<ulong, int> pair in _maxAllowedClientVideoLods)
			{
				if (_lastAppliedMaxAllowedClientVideoLods.TryGetValue(pair.Key, out int lastLod) && lastLod == pair.Value)
					continue;

				_host.SetClientMaxAllowedVideoLod(pair.Key, pair.Value);
				_lastAppliedMaxAllowedClientVideoLods[pair.Key] = pair.Value;
			}

			foreach (KeyValuePair<ulong, int> pair in _maxAllowedClientAudioLods)
			{
				if (_lastAppliedMaxAllowedClientAudioLods.TryGetValue(pair.Key, out int lastLod) && lastLod == pair.Value)
					continue;

				_host.SetClientMaxAllowedAudioLod(pair.Key, pair.Value);
				_lastAppliedMaxAllowedClientAudioLods[pair.Key] = pair.Value;
			}
		}

		public void SetVideoLodEnabled(int lodIndex, bool enabled)
		{
			EnsureLodDefaults();
			if (lodIndex < 0 || lodIndex >= videoLods.Count || videoLods[lodIndex] == null)
				return;

			videoLods[lodIndex].enabled = enabled;
			if (_host != null && _host.IsRunning())
			{
				_host.SetVideoLodEnabled(lodIndex, enabled);
				EnsureBoolListSize(_lastAppliedVideoLodEnabled, videoLods.Count, false);
				_lastAppliedVideoLodEnabled[lodIndex] = enabled;
			}
		}

		public void SetAudioLodEnabled(int lodIndex, bool enabled)
		{
			EnsureLodDefaults();
			if (lodIndex < 0 || lodIndex >= audioLods.Count || audioLods[lodIndex] == null)
				return;

			audioLods[lodIndex].enabled = enabled;
			if (_host != null && _host.IsRunning())
			{
				_host.SetAudioLodEnabled(lodIndex, enabled);
				EnsureBoolListSize(_lastAppliedAudioLodEnabled, audioLods.Count, false);
				_lastAppliedAudioLodEnabled[lodIndex] = enabled;
			}
		}

		public bool TryGetVideoLodRate(int lodIndex, out float bitrateKbps, out float fps)
		{
			bitrateKbps = 0f;
			fps = 0f;
			if (lodIndex < 0 || lodIndex >= _videoLodRateSamples.Length)
				return false;

			PortailStreamRateSample sample = _videoLodRateSamples[lodIndex];
			bitrateKbps = sample.bitrateKbps;
			fps = sample.fps;
			return true;
		}

		public bool TryGetAudioLodRate(int lodIndex, out float bitrateKbps)
		{
			bitrateKbps = 0f;
			if (lodIndex < 0 || lodIndex >= _audioLodRateSamples.Length)
				return false;

			bitrateKbps = _audioLodRateSamples[lodIndex].bitrateKbps;
			return true;
		}

		int GetVideoLodWidth(int lodIndex)
		{
			EnsureLodDefaults();
			if (lodIndex >= 0 && lodIndex < videoLods.Count && videoLods[lodIndex] != null)
				return Mathf.Max(16, videoLods[lodIndex].width);
			return 1280;
		}

		int GetVideoLodHeight(int lodIndex)
		{
			EnsureLodDefaults();
			if (lodIndex >= 0 && lodIndex < videoLods.Count && videoLods[lodIndex] != null)
				return Mathf.Max(16, videoLods[lodIndex].height);
			return 720;
		}

		int GetVideoLodFpsTarget(int lodIndex)
		{
			EnsureLodDefaults();
			if (lodIndex >= 0 && lodIndex < videoLods.Count && videoLods[lodIndex] != null)
				return Mathf.Clamp(videoLods[lodIndex].fps, 1, 240);
			return 60;
		}

		void EnsureLodDefaults()
		{
			if (videoLods == null || videoLods.Count == 0)
				videoLods = PortailStreamHostPlugin.CreateDefaultVideoLods();
			if (audioLods == null || audioLods.Count == 0)
				audioLods = PortailStreamHostPlugin.CreateDefaultAudioLods();
		}

		static void EnsureRateSampleSize(ref PortailStreamRateSample[] samples, int count)
		{
			count = Mathf.Max(0, count);
			if (samples != null && samples.Length >= count)
				return;

			PortailStreamRateSample[] next = new PortailStreamRateSample[count];
			if (samples != null)
				Array.Copy(samples, next, Mathf.Min(samples.Length, next.Length));
			samples = next;
		}

		static void EnsureBoolListSize(List<bool> values, int count, bool defaultValue)
		{
			while (values.Count < count)
				values.Add(defaultValue);
			while (values.Count > count)
				values.RemoveAt(values.Count - 1);
		}

		static void CopyVideoLodConfigs(IReadOnlyList<PortailStreamVideoLodConfig> source, List<PortailStreamVideoLodConfig> destination)
		{
			destination.Clear();
			if (source == null)
				return;

			for (int i = 0; i < source.Count; ++i)
			{
				PortailStreamVideoLodConfig lod = source[i] ?? new PortailStreamVideoLodConfig();
				destination.Add(new PortailStreamVideoLodConfig
				{
					name = lod.name,
					enabled = lod.enabled,
					width = lod.width,
					height = lod.height,
					fps = lod.fps,
					targetBitrateKbps = lod.targetBitrateKbps,
					rateControl = lod.rateControl,
				});
			}
		}

		static void CopyAudioLodConfigs(IReadOnlyList<PortailStreamAudioLodConfig> source, List<PortailStreamAudioLodConfig> destination)
		{
			destination.Clear();
			if (source == null)
				return;

			for (int i = 0; i < source.Count; ++i)
			{
				PortailStreamAudioLodConfig lod = source[i] ?? new PortailStreamAudioLodConfig();
				destination.Add(new PortailStreamAudioLodConfig
				{
					name = lod.name,
					enabled = lod.enabled,
					bitrateKbps = lod.bitrateKbps,
				});
			}
		}

		static void CopyVideoLodEnabledStates(IReadOnlyList<PortailStreamVideoLodConfig> source, List<bool> destination)
		{
			destination.Clear();
			if (source == null)
				return;
			for (int i = 0; i < source.Count; ++i)
				destination.Add(source[i] != null && source[i].enabled);
		}

		static void CopyAudioLodEnabledStates(IReadOnlyList<PortailStreamAudioLodConfig> source, List<bool> destination)
		{
			destination.Clear();
			if (source == null)
				return;
			for (int i = 0; i < source.Count; ++i)
				destination.Add(source[i] != null && source[i].enabled);
		}

		static bool VideoLodConfigsMatchIgnoringEnabled(IReadOnlyList<PortailStreamVideoLodConfig> lhs, IReadOnlyList<PortailStreamVideoLodConfig> rhs)
		{
			int lhsCount = lhs != null ? lhs.Count : 0;
			int rhsCount = rhs != null ? rhs.Count : 0;
			if (lhsCount != rhsCount)
				return false;

			for (int i = 0; i < lhsCount; ++i)
			{
				PortailStreamVideoLodConfig a = lhs[i];
				PortailStreamVideoLodConfig b = rhs[i];
				if (a == null || b == null)
					return a == b;
				if (!string.Equals(a.name ?? string.Empty, b.name ?? string.Empty, StringComparison.Ordinal) ||
					Mathf.Max(16, a.width) != Mathf.Max(16, b.width) ||
					Mathf.Max(16, a.height) != Mathf.Max(16, b.height) ||
					Mathf.Clamp(a.fps, 1, 240) != Mathf.Clamp(b.fps, 1, 240) ||
					Mathf.Max(100, a.targetBitrateKbps) != Mathf.Max(100, b.targetBitrateKbps) ||
					a.rateControl != b.rateControl)
				{
					return false;
				}
			}
			return true;
		}

		static bool AudioLodConfigsMatchIgnoringEnabled(IReadOnlyList<PortailStreamAudioLodConfig> lhs, IReadOnlyList<PortailStreamAudioLodConfig> rhs)
		{
			int lhsCount = lhs != null ? lhs.Count : 0;
			int rhsCount = rhs != null ? rhs.Count : 0;
			if (lhsCount != rhsCount)
				return false;

			for (int i = 0; i < lhsCount; ++i)
			{
				PortailStreamAudioLodConfig a = lhs[i];
				PortailStreamAudioLodConfig b = rhs[i];
				if (a == null || b == null)
					return a == b;
				if (!string.Equals(a.name ?? string.Empty, b.name ?? string.Empty, StringComparison.Ordinal) ||
					Mathf.Clamp(a.bitrateKbps, 32, 512) != Mathf.Clamp(b.bitrateKbps, 32, 512))
				{
					return false;
				}
			}
			return true;
		}
	}
}
