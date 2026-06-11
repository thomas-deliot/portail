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
		[Tooltip("Controls which native sender streaming plugin logs are forwarded to the Unity console. Native logging stays registered so errors can still be forwarded when this is set to Errors.")]
		public PortailStreamNativeConsoleLogLevel nativeConsoleLogLevel = PortailStreamNativeConsoleLogLevel.All;
		public bool allowSending = true;

		[Header("Source")]
		[SerializeField] PortailFeed sourceFeed;

		[Header("Sender Settings")]
		public uint appId = 480;
		public List<PortailStreamVideoLodConfig> videoLods = PortailStreamSenderPlugin.CreateDefaultVideoLods();
		public List<PortailStreamAudioLodConfig> audioLods = PortailStreamSenderPlugin.CreateDefaultAudioLods();
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
		public List<ulong> manualSenderTargetSteamIds = new List<ulong>();

		[Header("Textures")]
		public int fallbackTextureWidth = 1280;
		public int fallbackTextureHeight = 720;

		[Header("Timing")]
		public float playerRefreshSeconds = 0.5f;
		[Min(0.05f)]
		public float statsRefreshSeconds = 0.5f;

		public static MirrorPortailStreamSender Instance { get; private set; }

		readonly Dictionary<int, MirrorPortailParticipant> _players = new Dictionary<int, MirrorPortailParticipant>();
		readonly Dictionary<ulong, PortailStreamSenderPeerStats> _peerStatsByReceiver = new Dictionary<ulong, PortailStreamSenderPeerStats>();
		readonly Dictionary<ulong, PortailStreamRateSample> _receiverRateSamples = new Dictionary<ulong, PortailStreamRateSample>();
		readonly List<ulong> _lastAppliedSenderTargets = new List<ulong>();
		readonly HashSet<int> _livePlayerIds = new HashSet<int>();
		readonly List<int> _stalePlayerIds = new List<int>();
		readonly HashSet<ulong> _steamIdSeen = new HashSet<ulong>();
		readonly Dictionary<ulong, int> _maxAllowedReceiverVideoLods = new Dictionary<ulong, int>();
		readonly Dictionary<ulong, int> _maxAllowedReceiverAudioLods = new Dictionary<ulong, int>();
		readonly Dictionary<ulong, int> _lastAppliedMaxAllowedReceiverVideoLods = new Dictionary<ulong, int>();
		readonly Dictionary<ulong, int> _lastAppliedMaxAllowedReceiverAudioLods = new Dictionary<ulong, int>();
		readonly List<ulong> _desiredSenderTargetsScratch = new List<ulong>();
		readonly List<ulong> _staleReceiverSteamIds = new List<ulong>();
		readonly List<PortailStreamVideoLodConfig> _lastAppliedVideoLods = new List<PortailStreamVideoLodConfig>();
		readonly List<PortailStreamAudioLodConfig> _lastAppliedAudioLods = new List<PortailStreamAudioLodConfig>();
		readonly List<bool> _lastAppliedVideoLodEnabled = new List<bool>();
		readonly List<bool> _lastAppliedAudioLodEnabled = new List<bool>();

		PortailStreamSenderPlugin _sender;
		MirrorPortailParticipant _localSource;
		float _nextPlayerRefreshAt;
		float _nextStatsRefreshAt;
		PortailStreamSenderStats _senderStats;
		PortailStreamSenderVideoLodStats[] _videoLodStats = Array.Empty<PortailStreamSenderVideoLodStats>();
		PortailStreamSenderAudioLodStats[] _audioLodStats = Array.Empty<PortailStreamSenderAudioLodStats>();
		PortailStreamRateSample _senderRateSample;
		PortailStreamRateSample _senderEncodedVideoRateSample;
		PortailStreamRateSample _senderEncodedAudioRateSample;
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

		public bool IsStreaming => _sender != null && _sender.IsRunning();
		public PortailStreamSenderStats CurrentStats => _senderStats;
		public IReadOnlyList<PortailStreamSenderVideoLodStats> CurrentVideoLodStats => _videoLodStats;
		public IReadOnlyList<PortailStreamSenderAudioLodStats> CurrentAudioLodStats => _audioLodStats;
		public float CurrentBitrateKbps => _senderRateSample.bitrateKbps;
		public float CurrentEncodedVideoBitrateKbps => _senderEncodedVideoRateSample.bitrateKbps;
		public float CurrentEncodedAudioBitrateKbps => _senderEncodedAudioRateSample.bitrateKbps;
		public float CurrentFps => _senderEncodedVideoRateSample.fps;
		public int CurrentEncodedWidth => _senderStats.encoded_width > 0 ? _senderStats.encoded_width : GetVideoLodWidth(0);
		public int CurrentEncodedHeight => _senderStats.encoded_height > 0 ? _senderStats.encoded_height : GetVideoLodHeight(0);
		public int CurrentEncodedFpsTarget => _senderStats.encoded_fps > 0 ? _senderStats.encoded_fps : GetVideoLodFpsTarget(0);
		public bool AllowSending => allowSending;
		public int ActiveReceiverCount => _lastAppliedSenderTargets.Count;
		public IReadOnlyList<ulong> ActiveReceiverSteamIds => _lastAppliedSenderTargets;
		public Component SenderPluginComponent => _sender;
		public object CurrentStatsBoxed => _senderStats;
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

			StopSenderStream();
			if (_sender != null)
				_sender.enabled = false;
		}

		void OnDestroy()
		{
			if (Instance == this)
				Instance = null;

			StopSenderStream();
			if (_sender != null)
				_sender.enabled = false;

			_players.Clear();
		}

		void Update()
		{
			EnsureSenderPlugin();

			float now = Time.unscaledTime;
			if (now >= _nextPlayerRefreshAt)
			{
				_nextPlayerRefreshAt = now + Mathf.Max(0.1f, playerRefreshSeconds);
				RefreshPlayerViews();
			}

			PublishLocalSteamIdentity();
			EnsureSenderStreamRunning();

			if ((_sender != null && _sender.IsRunning()) ||
				SourceFeed != null)
			{
				if (now >= _nextStatsRefreshAt)
				{
					_nextStatsRefreshAt = now + Mathf.Max(0.05f, statsRefreshSeconds);
					PullStats(now);
				}
			}

		}

		public void SetManualSenderTargets(IReadOnlyList<ulong> receiverSteamIds)
		{
			autoTargetAllRemotePlayers = false;
			PortailStreamMirrorUtility.SanitizeSteamIds(receiverSteamIds, manualSenderTargetSteamIds, _steamIdSeen);
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

		void EnsureSenderPlugin()
		{
			if (_sender == null)
			{
				_sender = GetComponent<PortailStreamSenderPlugin>();
				if (_sender == null)
					_sender = gameObject.AddComponent<PortailStreamSenderPlugin>();

				_sender.autoStart = false;
			}

			_sender.enabled = true;
			ApplyNativeConsoleLoggingSettings();
		}


		void ApplyNativeConsoleLoggingSettings()
		{
			PortailStreamNativeLogBridge.SenderConsoleLogLevel = nativeConsoleLogLevel;

			if (_sender != null)
			{
				// Keep the native callback registered for every filter mode.
				// The bridge decides which native messages reach the Unity console.
				_sender.enableNativeLogging = true;
			}
		}

		void PublishLocalSteamIdentity()
		{
			if (_localSource == null)
				return;

			ulong localSteamId = PortailStreamMirrorUtility.ResolveLocalSteamId(_localSource, _sender, null);
			if (localSteamId != 0)
				_localSource.TryPublishOwnerStreamId(localSteamId);
		}

		void EnsureSenderStreamRunning()
		{
			if (_sender == null)
				return;

			if (!allowSending)
			{
				_steamUnavailableLogged = false;
				StopSenderStream();
				return;
			}

			List<ulong> desiredTargets = BuildDesiredSenderTargets();
			bool shouldRun = global::Mirror.NetworkClient.active && _localSource != null && desiredTargets.Count > 0;
			bool configChanged = HasSenderRestartRequired();

			if (!shouldRun)
			{
				_steamUnavailableLogged = false;
				StopSenderStream();
				return;
			}

			if (!PortailStreamMirrorUtility.EnsureSteamAvailable("MirrorPortailStreamSender", ref _steamUnavailableLogged))
			{
				StopSenderStream();
				return;
			}

			if (_sender.IsRunning() && configChanged)
				StopSenderStream();

			ApplySenderSettings();

			if (!_sender.IsRunning())
			{
				if (!_sender.StartStreaming())
					return;

				RememberAppliedSenderSettings();
				_lastAppliedSenderTargets.Clear();
			}

			ApplyLodEnabledStates();
			ApplySenderTargetsIfChanged(desiredTargets);
			ApplyMaxAllowedReceiverLods();
		}

		void ApplySenderSettings()
		{
			EnsureLodDefaults();
			_sender.appId = appId;
			_sender.videoLods = videoLods;
			_sender.audioLods = audioLods;
			_sender.disableIce = disableIce;
			_sender.chunkPayloadBytes = Mathf.Clamp(chunkPayloadBytes, 512, 60000);
			_sender.parityShards = Mathf.Clamp(parityShards, 0, 1);
			_sender.reliableVideo = reliableVideo;
			_sender.reliableKeyframes = reliableKeyframes;
			_sender.maxQueueMs = Mathf.Clamp(maxQueueMs, 0, 5000);
			_sender.codec = string.IsNullOrWhiteSpace(codec) ? "h264" : codec.Trim();
			_sender.encoder = string.IsNullOrWhiteSpace(encoder) ? "auto" : encoder.Trim();
			_sender.previewTexture = null;
			ApplyNativeConsoleLoggingSettings();
		}

		void PullStats(float now)
		{
			if (_sender != null && _sender.IsRunning())
			{
				_senderStats = _sender.GetStats();
				ulong frameCounter = _senderStats.encoded_frames > 0 ? _senderStats.encoded_frames : _senderStats.capture_frames;
				PortailStreamMirrorUtility.UpdateRateSample(ref _senderRateSample, _senderStats.sent_video_bytes + _senderStats.sent_audio_bytes, frameCounter, now);
				PortailStreamMirrorUtility.UpdateRateSample(ref _senderEncodedVideoRateSample, _senderStats.encoded_video_bytes, frameCounter, now);
				PortailStreamMirrorUtility.UpdateRateSample(ref _senderEncodedAudioRateSample, _senderStats.encoded_audio_bytes, _senderStats.encoded_audio_frames, now);
				PullLodStats(now);
				PullPeerStats(now);
				return;
			}

			_senderStats = default;
			_videoLodStats = Array.Empty<PortailStreamSenderVideoLodStats>();
			_audioLodStats = Array.Empty<PortailStreamSenderAudioLodStats>();
			_peerStatsByReceiver.Clear();
			_receiverRateSamples.Clear();
			PortailStreamMirrorUtility.UpdateRateSample(ref _senderRateSample, 0, 0, now);
			PortailStreamMirrorUtility.UpdateRateSample(ref _senderEncodedVideoRateSample, 0, 0, now);
			PortailStreamMirrorUtility.UpdateRateSample(ref _senderEncodedAudioRateSample, 0, 0, now);
		}

		void PullLodStats(float now)
		{
			int videoCount = Mathf.Max(1, videoLods != null ? videoLods.Count : 0);
			int audioCount = Mathf.Max(1, audioLods != null ? audioLods.Count : 0);
			_videoLodStats = _sender.GetVideoLodStats(videoCount);
			_audioLodStats = _sender.GetAudioLodStats(audioCount);
			EnsureRateSampleSize(ref _videoLodRateSamples, _videoLodStats.Length);
			EnsureRateSampleSize(ref _audioLodRateSamples, _audioLodStats.Length);

			for (int i = 0; i < _videoLodStats.Length; ++i)
			{
				PortailStreamSenderVideoLodStats stats = _videoLodStats[i];
				int index = stats.index >= 0 && stats.index < _videoLodRateSamples.Length ? stats.index : i;
				PortailStreamRateSample sample = _videoLodRateSamples[index];
				PortailStreamMirrorUtility.UpdateRateSample(ref sample, stats.encoded_video_bytes, stats.encoded_frames, now);
				_videoLodRateSamples[index] = sample;
			}

			for (int i = 0; i < _audioLodStats.Length; ++i)
			{
				PortailStreamSenderAudioLodStats stats = _audioLodStats[i];
				int index = stats.index >= 0 && stats.index < _audioLodRateSamples.Length ? stats.index : i;
				PortailStreamRateSample sample = _audioLodRateSamples[index];
				PortailStreamMirrorUtility.UpdateRateSample(ref sample, stats.encoded_audio_bytes, stats.encoded_audio_frames, now);
				_audioLodRateSamples[index] = sample;
			}
		}

		void PullPeerStats(float now)
		{
			_peerStatsByReceiver.Clear();
			if (_sender == null || !_sender.IsRunning())
				return;

			PortailStreamSenderPeerStats[] peerStats = _sender.GetPeerStats(Mathf.Max(16, _lastAppliedSenderTargets.Count + 8));
			for (int i = 0; i < peerStats.Length; ++i)
			{
				PortailStreamSenderPeerStats stats = peerStats[i];
				if (stats.receiver_steam_id == 0)
					continue;

				_peerStatsByReceiver[stats.receiver_steam_id] = stats;
				if (!_receiverRateSamples.TryGetValue(stats.receiver_steam_id, out PortailStreamRateSample sample))
					sample = default;

				PortailStreamMirrorUtility.UpdateRateSample(ref sample, stats.sent_video_bytes + stats.sent_audio_bytes, stats.encoded_frames, now);
				_receiverRateSamples[stats.receiver_steam_id] = sample;
			}

			_staleReceiverSteamIds.Clear();
			foreach (ulong receiverSteamId in _receiverRateSamples.Keys)
			{
				if (!_peerStatsByReceiver.ContainsKey(receiverSteamId))
					_staleReceiverSteamIds.Add(receiverSteamId);
			}

			for (int i = 0; i < _staleReceiverSteamIds.Count; ++i)
				_receiverRateSamples.Remove(_staleReceiverSteamIds[i]);
		}

		void RefreshPlayerViews()
		{
			PortailStreamMirrorUtility.RefreshPlayerViews(_players, _livePlayerIds, _stalePlayerIds, out _localSource);
		}

		List<ulong> BuildDesiredSenderTargets()
		{
			List<ulong> desiredTargets = autoTargetAllRemotePlayers
				? PortailStreamMirrorUtility.BuildRemoteStreamIds(_players, _desiredSenderTargetsScratch, _steamIdSeen, _localSource, requireBroadcasting: false)
				: PortailStreamMirrorUtility.SanitizeSteamIds(manualSenderTargetSteamIds, _desiredSenderTargetsScratch, _steamIdSeen);

			return desiredTargets;
		}

		public void SetGlobalSendingEnabled(bool enabled)
		{
			allowSending = enabled;
			if (!enabled)
				StopSenderStream();
		}

		public bool IsReceiverSendingEnabled(ulong receiverSteamId)
		{
			return receiverSteamId != 0 &&
				   allowSending &&
				   (GetReceiverVideoLod(receiverSteamId) >= 0 ||
					GetReceiverAudioLod(receiverSteamId) >= 0);
		}

		public void SetReceiverSendingEnabled(ulong receiverSteamId, bool enabled)
		{
			if (receiverSteamId == 0)
				return;

			SetReceiverMaxAllowedVideoLod(receiverSteamId, enabled ? PortailStreamLodUtility.Highest : PortailStreamLodUtility.Off);
			SetReceiverMaxAllowedAudioLod(receiverSteamId, enabled ? PortailStreamLodUtility.Highest : PortailStreamLodUtility.Off);
			if (enabled && !autoTargetAllRemotePlayers && !manualSenderTargetSteamIds.Contains(receiverSteamId))
			{
				manualSenderTargetSteamIds.Add(receiverSteamId);
				manualSenderTargetSteamIds.Sort();
			}
		}

		public int GetReceiverVideoLod(ulong receiverSteamId)
		{
			return GetReceiverMaxAllowedVideoLod(receiverSteamId);
		}

		public int GetReceiverAudioLod(ulong receiverSteamId)
		{
			return GetReceiverMaxAllowedAudioLod(receiverSteamId);
		}

		public int GetReceiverMaxAllowedVideoLod(ulong receiverSteamId)
		{
			if (receiverSteamId == 0)
				return PortailStreamLodUtility.Off;

			if (_peerStatsByReceiver.TryGetValue(receiverSteamId, out PortailStreamSenderPeerStats stats))
				return PortailStreamLodUtility.NormalizeLod(stats.assigned_video_lod);

			return _maxAllowedReceiverVideoLods.TryGetValue(receiverSteamId, out int lod)
				? PortailStreamLodUtility.NormalizeLod(lod)
				: PortailStreamLodUtility.Highest;
		}

		public int GetReceiverMaxAllowedAudioLod(ulong receiverSteamId)
		{
			if (receiverSteamId == 0)
				return PortailStreamLodUtility.Off;

			if (_peerStatsByReceiver.TryGetValue(receiverSteamId, out PortailStreamSenderPeerStats stats))
				return PortailStreamLodUtility.NormalizeLod(stats.assigned_audio_lod);

			return _maxAllowedReceiverAudioLods.TryGetValue(receiverSteamId, out int lod)
				? PortailStreamLodUtility.NormalizeLod(lod)
				: PortailStreamLodUtility.Highest;
		}

		public int GetAvailableVideoLodForReceiver(ulong receiverSteamId)
		{
			if (receiverSteamId != 0 && _peerStatsByReceiver.TryGetValue(receiverSteamId, out PortailStreamSenderPeerStats stats))
				return PortailStreamLodUtility.NormalizeLod(stats.available_video_lod);

			return PortailStreamLodUtility.Off;
		}

		public int GetAvailableAudioLodForReceiver(ulong receiverSteamId)
		{
			if (receiverSteamId != 0 && _peerStatsByReceiver.TryGetValue(receiverSteamId, out PortailStreamSenderPeerStats stats))
				return PortailStreamLodUtility.NormalizeLod(stats.available_audio_lod);

			return PortailStreamLodUtility.Off;
		}

		public void SetReceiverVideoLod(ulong receiverSteamId, int lod)
		{
			SetReceiverMaxAllowedVideoLod(receiverSteamId, lod);
		}

		public void SetReceiverAudioLod(ulong receiverSteamId, int lod)
		{
			SetReceiverMaxAllowedAudioLod(receiverSteamId, lod);
		}

		public void SetReceiverMaxAllowedVideoLod(ulong receiverSteamId, int lod)
		{
			if (receiverSteamId == 0)
				return;

			lod = PortailStreamLodUtility.NormalizeLod(lod);
			_maxAllowedReceiverVideoLods[receiverSteamId] = lod;
			if (_sender != null && _sender.IsRunning())
				_sender.SetReceiverMaxAllowedVideoLod(receiverSteamId, lod);
		}

		public void SetReceiverMaxAllowedAudioLod(ulong receiverSteamId, int lod)
		{
			if (receiverSteamId == 0)
				return;

			lod = PortailStreamLodUtility.NormalizeLod(lod);
			_maxAllowedReceiverAudioLods[receiverSteamId] = lod;
			if (_sender != null && _sender.IsRunning())
				_sender.SetReceiverMaxAllowedAudioLod(receiverSteamId, lod);
		}

		public List<ulong> GetKnownRemoteReceiverSteamIds(List<ulong> destination)
		{
			if (destination == null)
				destination = new List<ulong>();

			destination.Clear();
			PortailStreamMirrorUtility.BuildRemoteStreamIds(_players, destination, _steamIdSeen, _localSource, requireBroadcasting: false);
			return destination;
		}

		public bool IsReceiverCurrentlyActive(ulong receiverSteamId)
		{
			return receiverSteamId != 0 && _lastAppliedSenderTargets.Contains(receiverSteamId);
		}

		public bool TryGetRateForReceiver(ulong receiverSteamId, out float bitrateKbps, out float fps)
		{
			bitrateKbps = 0f;
			fps = 0f;
			if (receiverSteamId == 0 || !_lastAppliedSenderTargets.Contains(receiverSteamId))
				return false;

			if (_receiverRateSamples.TryGetValue(receiverSteamId, out PortailStreamRateSample sample))
			{
				bitrateKbps = sample.bitrateKbps;
				fps = sample.fps;
				return true;
			}

			bitrateKbps = _lastAppliedSenderTargets.Count > 0 ? _senderRateSample.bitrateKbps / Mathf.Max(1, _lastAppliedSenderTargets.Count) : 0f;
			fps = _senderRateSample.fps;
			return true;
		}

		public bool TryGetPeerStatsObject(ulong receiverSteamId, out object stats)
		{
			if (_peerStatsByReceiver.TryGetValue(receiverSteamId, out PortailStreamSenderPeerStats typedStats))
			{
				stats = typedStats;
				return true;
			}

			stats = null;
			return false;
		}

		public bool TryGetPeerStats(ulong receiverSteamId, out PortailStreamSenderPeerStats stats)
		{
			return _peerStatsByReceiver.TryGetValue(receiverSteamId, out stats);
		}

		bool HasSenderRestartRequired()
		{
			if (_sender == null || !_sender.IsRunning())
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
				   _sender.appId != appId;
		}

		void RememberAppliedSenderSettings()
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

		void StopSenderStream()
		{
			if (_sender != null && _sender.IsRunning())
				_sender.StopStreaming();

			_senderStats = default;
			_videoLodStats = Array.Empty<PortailStreamSenderVideoLodStats>();
			_audioLodStats = Array.Empty<PortailStreamSenderAudioLodStats>();
			_senderRateSample = default;
			_senderEncodedVideoRateSample = default;
			_senderEncodedAudioRateSample = default;
			_videoLodRateSamples = Array.Empty<PortailStreamRateSample>();
			_audioLodRateSamples = Array.Empty<PortailStreamRateSample>();
			_lastAppliedSenderTargets.Clear();
			_lastAppliedMaxAllowedReceiverVideoLods.Clear();
			_lastAppliedMaxAllowedReceiverAudioLods.Clear();
			_lastAppliedVideoLodEnabled.Clear();
			_lastAppliedAudioLodEnabled.Clear();
			_peerStatsByReceiver.Clear();
			_receiverRateSamples.Clear();
		}

		void ApplyLodEnabledStates()
		{
			if (_sender == null || !_sender.IsRunning())
				return;

			EnsureLodDefaults();
			EnsureBoolListSize(_lastAppliedVideoLodEnabled, videoLods.Count, false);
			for (int i = 0; i < videoLods.Count; ++i)
			{
				bool enabled = videoLods[i] != null && videoLods[i].enabled;
				if (_lastAppliedVideoLodEnabled[i] == enabled)
					continue;
				_sender.SetVideoLodEnabled(i, enabled);
				_lastAppliedVideoLodEnabled[i] = enabled;
			}

			EnsureBoolListSize(_lastAppliedAudioLodEnabled, audioLods.Count, false);
			for (int i = 0; i < audioLods.Count; ++i)
			{
				bool enabled = audioLods[i] != null && audioLods[i].enabled;
				if (_lastAppliedAudioLodEnabled[i] == enabled)
					continue;
				_sender.SetAudioLodEnabled(i, enabled);
				_lastAppliedAudioLodEnabled[i] = enabled;
			}
		}

		void ApplySenderTargetsIfChanged(IReadOnlyList<ulong> desiredTargets)
		{
			if (_sender == null || !_sender.IsRunning())
				return;

			if (PortailStreamMirrorUtility.SteamIdListsEqual(_lastAppliedSenderTargets, desiredTargets))
				return;

			_sender.SetReceiverSteamIds(desiredTargets);
			PortailStreamMirrorUtility.CopySteamIds(desiredTargets, _lastAppliedSenderTargets);
		}

		void ApplyMaxAllowedReceiverLods()
		{
			if (_sender == null || !_sender.IsRunning())
				return;

			foreach (KeyValuePair<ulong, int> pair in _maxAllowedReceiverVideoLods)
			{
				if (_lastAppliedMaxAllowedReceiverVideoLods.TryGetValue(pair.Key, out int lastLod) && lastLod == pair.Value)
					continue;

				_sender.SetReceiverMaxAllowedVideoLod(pair.Key, pair.Value);
				_lastAppliedMaxAllowedReceiverVideoLods[pair.Key] = pair.Value;
			}

			foreach (KeyValuePair<ulong, int> pair in _maxAllowedReceiverAudioLods)
			{
				if (_lastAppliedMaxAllowedReceiverAudioLods.TryGetValue(pair.Key, out int lastLod) && lastLod == pair.Value)
					continue;

				_sender.SetReceiverMaxAllowedAudioLod(pair.Key, pair.Value);
				_lastAppliedMaxAllowedReceiverAudioLods[pair.Key] = pair.Value;
			}
		}

		public void SetVideoLodEnabled(int lodIndex, bool enabled)
		{
			EnsureLodDefaults();
			if (lodIndex < 0 || lodIndex >= videoLods.Count || videoLods[lodIndex] == null)
				return;

			videoLods[lodIndex].enabled = enabled;
			if (_sender != null && _sender.IsRunning())
			{
				_sender.SetVideoLodEnabled(lodIndex, enabled);
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
			if (_sender != null && _sender.IsRunning())
			{
				_sender.SetAudioLodEnabled(lodIndex, enabled);
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
				videoLods = PortailStreamSenderPlugin.CreateDefaultVideoLods();
			if (audioLods == null || audioLods.Count == 0)
				audioLods = PortailStreamSenderPlugin.CreateDefaultAudioLods();
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
