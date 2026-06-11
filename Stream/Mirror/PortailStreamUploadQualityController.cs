using System.Collections.Generic;
using Portail.Stream;
using UnityEngine;

namespace Portail.Stream.Mirror
{
	/// <summary>
	/// Host-side upload bandwidth governor for outgoing Steam desktop streams.
	///
	/// This component drives MirrorPortailStreamSender's per-client max allowed video/audio LODs.
	/// The client still sends its own max accepted LOD; the native host resolves the effective
	/// LOD as the worse quality of the host cap and client cap.
	/// </summary>
	[DefaultExecutionOrder(-9390)]
	[DisallowMultipleComponent]
	public sealed class PortailStreamUploadQualityController : MonoBehaviour
	{
		sealed class ClientState
		{
			public bool capturedRestoreLods;
			public int restoreVideoLod = PortailStreamLodUtility.Highest;
			public int restoreAudioLod = PortailStreamLodUtility.Highest;
			public int appliedVideoLod = UnsetLod;
			public int appliedAudioLod = UnsetLod;
			public bool forceVideoOff;
			public bool forceAudioOff;
		}

		sealed class ClientBudgetDecision
		{
			public ulong clientSteamId;
			public ClientState state;
			public bool active;
			public int connectionPath;
			public int clientMaxVideoLod = PortailStreamLodUtility.Highest;
			public int clientMaxAudioLod = PortailStreamLodUtility.Highest;
			public int nextVideoLod = PortailStreamLodUtility.Highest;
			public int nextAudioLod = PortailStreamLodUtility.Highest;
		}

		const int UnsetLod = int.MinValue;
		const int ConnectionPathSdr = 2;

		[Header("References")]
		[Tooltip("Host manager to drive. If left empty, MirrorPortailStreamSender.Instance is used, then a scene search fallback.")]
		public MirrorPortailStreamSender hostManager;

		[Header("Bandwidth Budget")]
		[Min(0f)]
		[Tooltip("Maximum total outgoing PortailStream media bitrate in Mbps. Set to 0 to disable the global upload budget.")]
		public float maxTotalUploadBandwidthMbps;

		[Header("Per-Path Bandwidth")]
		[Tooltip("When a client connection is routed through Steam Datagram Relay instead of ICE/direct, cap that client's total video+audio media bitrate.")]
		public bool applySdrBandwidthLimit = true;

		[Min(0f)]
		[Tooltip("Per-client total video+audio media bitrate cap in Mbps while the Steam path reports SDR. Set to 0 to disable the SDR cap.")]
		public float sdrPerClientBandwidthMbps = 7f;

		[Header("Timing")]
		[Min(0.02f)]
		[Tooltip("How often upload budget decisions are recomputed.")]
		public float updateIntervalSeconds = 0.1f;

		[Header("Lifecycle")]
		[Tooltip("Restore each client's previous host max allowed video/audio LODs when this component is disabled.")]
		public bool restoreQualitiesOnDisable = true;

		[Header("Debug")]
		public bool logQualityChanges;

		readonly Dictionary<ulong, ClientState> _states = new Dictionary<ulong, ClientState>();
		readonly List<ulong> _knownClients = new List<ulong>();
		readonly List<ulong> _staleClients = new List<ulong>();
		readonly List<ClientBudgetDecision> _budgetDecisions = new List<ClientBudgetDecision>();
		readonly List<PortailStreamVideoLodConfig> _defaultVideoLods = PortailStreamHostPlugin.CreateDefaultVideoLods();
		readonly List<PortailStreamAudioLodConfig> _defaultAudioLods = PortailStreamHostPlugin.CreateDefaultAudioLods();

		float _nextUpdateAt;

		void OnEnable()
		{
			ResolveReferences();
			_nextUpdateAt = 0f;
		}

		void OnDisable()
		{
			if (restoreQualitiesOnDisable)
				RestoreManagedClientQualities();

			_states.Clear();
		}

		void OnValidate()
		{
			maxTotalUploadBandwidthMbps = Mathf.Max(0f, maxTotalUploadBandwidthMbps);
			sdrPerClientBandwidthMbps = Mathf.Max(0f, sdrPerClientBandwidthMbps);
			updateIntervalSeconds = Mathf.Max(0.02f, updateIntervalSeconds);
		}

		void LateUpdate()
		{
			float now = Time.unscaledTime;
			if (now < _nextUpdateAt)
				return;

			_nextUpdateAt = now + Mathf.Max(0.02f, updateIntervalSeconds);
			ResolveReferences();

			if (hostManager == null)
				return;

			if (!HasGlobalUploadBudget() && !HasSdrPathBudget() && !HasForcedOffOverrides())
			{
				RestoreManagedClientQualities();
				_states.Clear();
				return;
			}

			UpdateClientQualities();
		}

		public bool IsVideoForcedOff(ulong clientSteamId)
		{
			return clientSteamId != 0 &&
				_states.TryGetValue(clientSteamId, out ClientState state) &&
				state.forceVideoOff;
		}

		public bool IsAudioForcedOff(ulong clientSteamId)
		{
			return clientSteamId != 0 &&
				_states.TryGetValue(clientSteamId, out ClientState state) &&
				state.forceAudioOff;
		}

		public bool ToggleVideoForcedOff(ulong clientSteamId)
		{
			bool next = !IsVideoForcedOff(clientSteamId);
			SetVideoForcedOff(clientSteamId, next);
			return next;
		}

		public bool ToggleAudioForcedOff(ulong clientSteamId)
		{
			bool next = !IsAudioForcedOff(clientSteamId);
			SetAudioForcedOff(clientSteamId, next);
			return next;
		}

		public void SetVideoForcedOff(ulong clientSteamId, bool forcedOff)
		{
			if (clientSteamId == 0)
				return;

			ResolveReferences();
			if (hostManager == null)
				return;

			ClientState state = GetOrCreateState(clientSteamId);
			CaptureRestoreLodsIfNeeded(clientSteamId, state);
			if (state.forceVideoOff == forcedOff)
				return;

			state.forceVideoOff = forcedOff;
			if (forcedOff)
				ApplyVideoLod(clientSteamId, state, PortailStreamLodUtility.Off);
			else
				state.appliedVideoLod = UnsetLod;

			_nextUpdateAt = 0f;
		}

		public void SetAudioForcedOff(ulong clientSteamId, bool forcedOff)
		{
			if (clientSteamId == 0)
				return;

			ResolveReferences();
			if (hostManager == null)
				return;

			ClientState state = GetOrCreateState(clientSteamId);
			CaptureRestoreLodsIfNeeded(clientSteamId, state);
			if (state.forceAudioOff == forcedOff)
				return;

			state.forceAudioOff = forcedOff;
			if (forcedOff)
				ApplyAudioLod(clientSteamId, state, PortailStreamLodUtility.Off);
			else
				state.appliedAudioLod = UnsetLod;

			_nextUpdateAt = 0f;
		}

		void ResolveReferences()
		{
			if (hostManager != null)
				return;

			hostManager = MirrorPortailStreamSender.Instance;
			if (hostManager == null)
				hostManager = FindFirstObjectByType<MirrorPortailStreamSender>();
		}

		bool HasGlobalUploadBudget()
		{
			return maxTotalUploadBandwidthMbps > 0f;
		}

		bool HasSdrPathBudget()
		{
			return applySdrBandwidthLimit && sdrPerClientBandwidthMbps > 0f;
		}

		bool HasForcedOffOverrides()
		{
			foreach (ClientState state in _states.Values)
			{
				if (state != null && (state.forceVideoOff || state.forceAudioOff))
					return true;
			}

			return false;
		}

		void UpdateClientQualities()
		{
			hostManager.GetKnownRemoteClientSteamIds(_knownClients);
			PruneStaleClients();

			_budgetDecisions.Clear();
			for (int i = 0; i < _knownClients.Count; ++i)
			{
				ulong clientSteamId = _knownClients[i];
				if (clientSteamId == 0)
					continue;

				ClientState state = GetOrCreateState(clientSteamId);
				CaptureRestoreLodsIfNeeded(clientSteamId, state);

				ClientBudgetDecision decision = new ClientBudgetDecision
				{
					clientSteamId = clientSteamId,
					state = state,
					active = hostManager.IsClientCurrentlyActive(clientSteamId),
					nextVideoLod = state.restoreVideoLod,
					nextAudioLod = state.restoreAudioLod,
				};

				if (hostManager.TryGetPeerStats(clientSteamId, out PortailStreamHostPeerStats stats))
				{
					decision.connectionPath = stats.connected != 0 ? stats.connection_path : 0;
					decision.clientMaxVideoLod = PortailStreamLodUtility.NormalizeLod(stats.max_video_lod);
					decision.clientMaxAudioLod = PortailStreamLodUtility.NormalizeLod(stats.max_audio_lod);
				}

				_budgetDecisions.Add(decision);
			}

			ApplySdrPathBudgets(_budgetDecisions);
			ApplyUploadBudget(_budgetDecisions);

			for (int i = 0; i < _budgetDecisions.Count; ++i)
			{
				ClientBudgetDecision decision = _budgetDecisions[i];
				if (decision.state.forceVideoOff)
					decision.nextVideoLod = PortailStreamLodUtility.Off;
				if (decision.state.forceAudioOff)
					decision.nextAudioLod = PortailStreamLodUtility.Off;

				ApplyVideoLod(decision.clientSteamId, decision.state, decision.nextVideoLod);
				ApplyAudioLod(decision.clientSteamId, decision.state, decision.nextAudioLod);
			}
		}

		void PruneStaleClients()
		{
			_staleClients.Clear();
			foreach (ulong clientSteamId in _states.Keys)
			{
				if (!_knownClients.Contains(clientSteamId))
					_staleClients.Add(clientSteamId);
			}

			for (int i = 0; i < _staleClients.Count; ++i)
			{
				ulong clientSteamId = _staleClients[i];
				if (restoreQualitiesOnDisable && _states.TryGetValue(clientSteamId, out ClientState state))
					RestoreClientQuality(clientSteamId, state);

				_states.Remove(clientSteamId);
			}
		}

		ClientState GetOrCreateState(ulong clientSteamId)
		{
			if (!_states.TryGetValue(clientSteamId, out ClientState state))
			{
				state = new ClientState();
				_states.Add(clientSteamId, state);
			}

			return state;
		}

		void CaptureRestoreLodsIfNeeded(ulong clientSteamId, ClientState state)
		{
			if (state.capturedRestoreLods || hostManager == null)
				return;

			state.restoreVideoLod = hostManager.GetClientMaxAllowedVideoLod(clientSteamId);
			state.restoreAudioLod = hostManager.GetClientMaxAllowedAudioLod(clientSteamId);
			state.appliedVideoLod = state.restoreVideoLod;
			state.appliedAudioLod = state.restoreAudioLod;
			state.capturedRestoreLods = true;
		}

		void ApplySdrPathBudgets(List<ClientBudgetDecision> decisions)
		{
			if (!HasSdrPathBudget())
				return;

			float limitKbps = Mathf.Max(0f, sdrPerClientBandwidthMbps) * 1000f;
			if (limitKbps <= 0f)
				return;

			for (int i = 0; i < decisions.Count; ++i)
			{
				ClientBudgetDecision decision = decisions[i];
				if (!decision.active || !IsSdrConnectionPath(decision.connectionPath))
					continue;

				ApplyPerClientPathBudget(decision, limitKbps);
			}
		}

		void ApplyPerClientPathBudget(ClientBudgetDecision decision, float limitKbps)
		{
			int guard = CountUsableVideoLods() + CountUsableAudioLods() + 4;
			while (guard-- > 0 && EstimateDecisionMediaKbps(decision) > limitKbps + 0.1f)
			{
				if (TryLowerVideoForDecision(decision))
					continue;

				if (TryLowerAudioForDecision(decision))
					continue;

				break;
			}
		}

		float EstimateDecisionMediaKbps(ClientBudgetDecision decision)
		{
			return EstimateDecisionVideoKbps(decision, decision.nextVideoLod) +
				EstimateDecisionAudioKbps(decision, decision.nextAudioLod);
		}

		bool TryLowerVideoForDecision(ClientBudgetDecision decision)
		{
			float currentKbps = EstimateDecisionVideoKbps(decision, decision.nextVideoLod);
			if (currentKbps <= 0f)
				return false;

			if (!TryFindNextVideoLodWithSaving(decision, currentKbps, out int nextLod, out _))
				return false;

			decision.nextVideoLod = nextLod;
			return true;
		}

		bool TryLowerAudioForDecision(ClientBudgetDecision decision)
		{
			float currentKbps = EstimateDecisionAudioKbps(decision, decision.nextAudioLod);
			if (currentKbps <= 0f)
				return false;

			if (!TryFindNextAudioLodWithSaving(decision, currentKbps, out int nextLod, out _))
				return false;

			decision.nextAudioLod = nextLod;
			return true;
		}

		static bool IsSdrConnectionPath(int connectionPath)
		{
			return connectionPath == ConnectionPathSdr;
		}

		void ApplyUploadBudget(List<ClientBudgetDecision> decisions)
		{
			float limitKbps = Mathf.Max(0f, maxTotalUploadBandwidthMbps) * 1000f;
			if (limitKbps <= 0f)
				return;

			int guard = Mathf.Max(1, decisions.Count) * (CountUsableVideoLods() + CountUsableAudioLods() + 4);
			while (guard-- > 0 && EstimateTotalUploadKbps(decisions) > limitKbps + 0.1f)
			{
				if (TryLowerLargestVideoConsumer(decisions))
					continue;

				if (TryLowerLargestAudioConsumer(decisions))
					continue;

				break;
			}
		}

		float EstimateTotalUploadKbps(List<ClientBudgetDecision> decisions)
		{
			float total = 0f;
			for (int i = 0; i < decisions.Count; ++i)
			{
				ClientBudgetDecision decision = decisions[i];
				if (!decision.active)
					continue;

				total += EstimateDecisionVideoKbps(decision, decision.nextVideoLod);
				total += EstimateDecisionAudioKbps(decision, decision.nextAudioLod);
			}

			return total;
		}

		bool TryLowerLargestVideoConsumer(List<ClientBudgetDecision> decisions)
		{
			int bestIndex = -1;
			int bestNextLod = PortailStreamLodUtility.Off;
			float bestCurrentKbps = -1f;
			float bestSavingKbps = -1f;

			for (int i = 0; i < decisions.Count; ++i)
			{
				ClientBudgetDecision decision = decisions[i];
				if (!decision.active)
					continue;

				float currentKbps = EstimateDecisionVideoKbps(decision, decision.nextVideoLod);
				if (currentKbps <= 0f)
					continue;

				if (!TryFindNextVideoLodWithSaving(decision, currentKbps, out int nextLod, out float savingKbps))
					continue;

				if (currentKbps > bestCurrentKbps ||
					(Mathf.Approximately(currentKbps, bestCurrentKbps) && savingKbps > bestSavingKbps))
				{
					bestIndex = i;
					bestNextLod = nextLod;
					bestCurrentKbps = currentKbps;
					bestSavingKbps = savingKbps;
				}
			}

			if (bestIndex < 0)
				return false;

			decisions[bestIndex].nextVideoLod = bestNextLod;
			return true;
		}

		bool TryLowerLargestAudioConsumer(List<ClientBudgetDecision> decisions)
		{
			int bestIndex = -1;
			int bestNextLod = PortailStreamLodUtility.Off;
			float bestCurrentKbps = -1f;
			float bestSavingKbps = -1f;

			for (int i = 0; i < decisions.Count; ++i)
			{
				ClientBudgetDecision decision = decisions[i];
				if (!decision.active)
					continue;

				float currentKbps = EstimateDecisionAudioKbps(decision, decision.nextAudioLod);
				if (currentKbps <= 0f)
					continue;

				if (!TryFindNextAudioLodWithSaving(decision, currentKbps, out int nextLod, out float savingKbps))
					continue;

				if (currentKbps > bestCurrentKbps ||
					(Mathf.Approximately(currentKbps, bestCurrentKbps) && savingKbps > bestSavingKbps))
				{
					bestIndex = i;
					bestNextLod = nextLod;
					bestCurrentKbps = currentKbps;
					bestSavingKbps = savingKbps;
				}
			}

			if (bestIndex < 0)
				return false;

			decisions[bestIndex].nextAudioLod = bestNextLod;
			return true;
		}

		bool TryFindNextVideoLodWithSaving(ClientBudgetDecision decision, float currentKbps, out int nextLod, out float savingKbps)
		{
			int candidate = decision.nextVideoLod;
			while (candidate >= 0)
			{
				candidate = StepVideoLodDown(candidate);
				float nextKbps = EstimateDecisionVideoKbps(decision, candidate);
				savingKbps = currentKbps - nextKbps;
				if (savingKbps > 0.1f)
				{
					nextLod = candidate;
					return true;
				}
			}

			nextLod = PortailStreamLodUtility.Off;
			savingKbps = 0f;
			return false;
		}

		bool TryFindNextAudioLodWithSaving(ClientBudgetDecision decision, float currentKbps, out int nextLod, out float savingKbps)
		{
			int candidate = decision.nextAudioLod;
			while (candidate >= 0)
			{
				candidate = StepAudioLodDown(candidate);
				float nextKbps = EstimateDecisionAudioKbps(decision, candidate);
				savingKbps = currentKbps - nextKbps;
				if (savingKbps > 0.1f)
				{
					nextLod = candidate;
					return true;
				}
			}

			nextLod = PortailStreamLodUtility.Off;
			savingKbps = 0f;
			return false;
		}

		float EstimateDecisionVideoKbps(ClientBudgetDecision decision, int hostVideoLod)
		{
			int effectiveLod = ResolveEffectiveVideoLod(hostVideoLod, decision.clientMaxVideoLod);
			return EstimateVideoLodBitrateKbps(effectiveLod);
		}

		float EstimateDecisionAudioKbps(ClientBudgetDecision decision, int hostAudioLod)
		{
			int effectiveLod = ResolveEffectiveAudioLod(hostAudioLod, decision.clientMaxAudioLod);
			return EstimateAudioLodBitrateKbps(effectiveLod);
		}

		int ResolveEffectiveVideoLod(int hostMaxLod, int clientMaxLod)
		{
			return ResolveEffectiveLod(hostMaxLod, clientMaxLod, GetVideoLods(), IsVideoLodUsable);
		}

		int ResolveEffectiveAudioLod(int hostMaxLod, int clientMaxLod)
		{
			return ResolveEffectiveLod(hostMaxLod, clientMaxLod, GetAudioLods(), IsAudioLodUsable);
		}

		static int ResolveEffectiveLod<T>(int hostMaxLod, int clientMaxLod, IList<T> lods, System.Predicate<T> isUsable)
		{
			hostMaxLod = PortailStreamLodUtility.NormalizeLod(hostMaxLod);
			clientMaxLod = PortailStreamLodUtility.NormalizeLod(clientMaxLod);
			if (hostMaxLod < 0 || clientMaxLod < 0 || lods == null || lods.Count == 0)
				return PortailStreamLodUtility.Off;

			int firstAllowed = Mathf.Max(hostMaxLod, clientMaxLod);
			for (int i = firstAllowed; i < lods.Count; ++i)
			{
				if (isUsable(lods[i]))
					return i;
			}

			return PortailStreamLodUtility.Off;
		}

		int StepVideoLodDown(int current)
		{
			if (current < 0)
				return PortailStreamLodUtility.Off;

			IList<PortailStreamVideoLodConfig> lods = GetVideoLods();
			if (lods == null || lods.Count == 0)
				return PortailStreamLodUtility.Off;

			for (int i = current + 1; i < lods.Count; ++i)
			{
				if (IsVideoLodUsable(lods[i]))
					return i;
			}

			return PortailStreamLodUtility.Off;
		}

		int StepAudioLodDown(int current)
		{
			if (current < 0)
				return PortailStreamLodUtility.Off;

			IList<PortailStreamAudioLodConfig> lods = GetAudioLods();
			if (lods == null || lods.Count == 0)
				return PortailStreamLodUtility.Off;

			for (int i = current + 1; i < lods.Count; ++i)
			{
				if (IsAudioLodUsable(lods[i]))
					return i;
			}

			return PortailStreamLodUtility.Off;
		}

		float EstimateVideoLodBitrateKbps(int lod)
		{
			if (lod < 0)
				return 0f;

			IList<PortailStreamVideoLodConfig> lods = GetVideoLods();
			if (lods == null || lod >= lods.Count || !IsVideoLodUsable(lods[lod]))
				return 0f;

			return Mathf.Max(100, lods[lod].targetBitrateKbps);
		}

		float EstimateAudioLodBitrateKbps(int lod)
		{
			if (lod < 0)
				return 0f;

			IList<PortailStreamAudioLodConfig> lods = GetAudioLods();
			if (lods == null || lod >= lods.Count || !IsAudioLodUsable(lods[lod]))
				return 0f;

			return Mathf.Max(32, lods[lod].bitrateKbps);
		}

		IList<PortailStreamVideoLodConfig> GetVideoLods()
		{
			if (hostManager != null && hostManager.videoLods != null && hostManager.videoLods.Count > 0)
				return hostManager.videoLods;

			return _defaultVideoLods;
		}

		IList<PortailStreamAudioLodConfig> GetAudioLods()
		{
			if (hostManager != null && hostManager.audioLods != null && hostManager.audioLods.Count > 0)
				return hostManager.audioLods;

			return _defaultAudioLods;
		}

		bool IsVideoLodUsable(PortailStreamVideoLodConfig lod)
		{
			return lod != null && lod.enabled;
		}

		bool IsAudioLodUsable(PortailStreamAudioLodConfig lod)
		{
			return lod != null && lod.enabled;
		}

		int CountUsableVideoLods()
		{
			IList<PortailStreamVideoLodConfig> lods = GetVideoLods();
			if (lods == null || lods.Count == 0)
				return 1;

			int count = 0;
			for (int i = 0; i < lods.Count; ++i)
			{
				if (IsVideoLodUsable(lods[i]))
					++count;
			}

			return Mathf.Max(1, count);
		}

		int CountUsableAudioLods()
		{
			IList<PortailStreamAudioLodConfig> lods = GetAudioLods();
			if (lods == null || lods.Count == 0)
				return 1;

			int count = 0;
			for (int i = 0; i < lods.Count; ++i)
			{
				if (IsAudioLodUsable(lods[i]))
					++count;
			}

			return Mathf.Max(1, count);
		}

		void ApplyVideoLod(ulong clientSteamId, ClientState state, int lod)
		{
			lod = PortailStreamLodUtility.NormalizeLod(lod);
			if (state.appliedVideoLod == lod)
				return;

			hostManager.SetClientMaxAllowedVideoLod(clientSteamId, lod);
			state.appliedVideoLod = lod;

			if (logQualityChanges)
			{
				Debug.Log($"[PortailStreamUploadQualityController] Client {clientSteamId} video max -> {PortailStreamLodUtility.ToVideoLabel(lod)} " +
					$"(global upload budget {maxTotalUploadBandwidthMbps:0.##} Mbps, SDR cap {sdrPerClientBandwidthMbps:0.##} Mbps)");
			}
		}

		void ApplyAudioLod(ulong clientSteamId, ClientState state, int lod)
		{
			lod = PortailStreamLodUtility.NormalizeLod(lod);
			if (state.appliedAudioLod == lod)
				return;

			hostManager.SetClientMaxAllowedAudioLod(clientSteamId, lod);
			state.appliedAudioLod = lod;

			if (logQualityChanges)
			{
				Debug.Log($"[PortailStreamUploadQualityController] Client {clientSteamId} audio max -> {PortailStreamLodUtility.ToAudioLabel(lod)} " +
					$"(global upload budget {maxTotalUploadBandwidthMbps:0.##} Mbps, SDR cap {sdrPerClientBandwidthMbps:0.##} Mbps)");
			}
		}

		void RestoreManagedClientQualities()
		{
			if (hostManager == null)
				hostManager = MirrorPortailStreamSender.Instance;

			if (hostManager == null)
				return;

			foreach (KeyValuePair<ulong, ClientState> pair in _states)
				RestoreClientQuality(pair.Key, pair.Value);
		}

		void RestoreClientQuality(ulong clientSteamId, ClientState state)
		{
			if (hostManager == null || clientSteamId == 0 || state == null || !state.capturedRestoreLods)
				return;

			hostManager.SetClientMaxAllowedVideoLod(clientSteamId, state.restoreVideoLod);
			hostManager.SetClientMaxAllowedAudioLod(clientSteamId, state.restoreAudioLod);
			state.appliedVideoLod = state.restoreVideoLod;
			state.appliedAudioLod = state.restoreAudioLod;

			if (logQualityChanges)
			{
				Debug.Log($"[PortailStreamUploadQualityController] Client {clientSteamId} restored to " +
					$"{PortailStreamLodUtility.ToVideoLabel(state.restoreVideoLod)} / {PortailStreamLodUtility.ToAudioLabel(state.restoreAudioLod)}");
			}
		}
	}
}
