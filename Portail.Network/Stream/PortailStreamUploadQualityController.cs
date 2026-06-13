using System.Collections.Generic;
using UnityEngine;

namespace Portail.Stream
{
	/// <summary>
	/// Sender-side upload bandwidth governor for outgoing Steam desktop streams.
	///
	/// This component drives PortailStreamSender's per-receiver max allowed video/audio LODs.
	/// The receiver still sends its own max accepted LOD; the native sender resolves the effective
	/// LOD as the worse quality of the sender cap and receiver cap.
	/// </summary>
	[DefaultExecutionOrder(-9390)]
	[DisallowMultipleComponent]
	public sealed class PortailStreamUploadQualityController : MonoBehaviour
	{
		sealed class ReceiverState
		{
			public bool capturedRestoreLods;
			public int restoreVideoLod = PortailStreamLodUtility.Highest;
			public int restoreAudioLod = PortailStreamLodUtility.Highest;
			public int appliedVideoLod = UnsetLod;
			public int appliedAudioLod = UnsetLod;
			public bool forceVideoOff;
			public bool forceAudioOff;
		}

		sealed class ReceiverBudgetDecision
		{
			public ulong receiverSteamId;
			public ReceiverState state;
			public bool active;
			public int connectionPath;
			public int receiverMaxVideoLod = PortailStreamLodUtility.Highest;
			public int receiverMaxAudioLod = PortailStreamLodUtility.Highest;
			public int nextVideoLod = PortailStreamLodUtility.Highest;
			public int nextAudioLod = PortailStreamLodUtility.Highest;
		}

		const int UnsetLod = int.MinValue;
		const int ConnectionPathSdr = 2;

		[Header("References")]
		[Tooltip("Sender manager to drive. If left empty, PortailStreamSender.Instance is used, then a scene search fallback.")]
		public PortailStreamSender senderManager;

		[Header("Bandwidth Budget")]
		[Min(0f)]
		[Tooltip("Maximum total outgoing PortailStream media bitrate in Mbps. Set to 0 to disable the global upload budget.")]
		public float maxTotalUploadBandwidthMbps;

		[Header("Per-Path Bandwidth")]
		[Tooltip("When a receiver connection is routed through Steam Datagram Relay instead of ICE/direct, cap that receiver's total video+audio media bitrate.")]
		public bool applySdrBandwidthLimit = true;

		[Min(0f)]
		[Tooltip("Per-receiver total video+audio media bitrate cap in Mbps while the Steam path reports SDR. Set to 0 to disable the SDR cap.")]
		public float sdrPerReceiverBandwidthMbps = 7f;

		[Header("Timing")]
		[Min(0.02f)]
		[Tooltip("How often upload budget decisions are recomputed.")]
		public float updateIntervalSeconds = 0.1f;

		[Header("Lifecycle")]
		[Tooltip("Restore each receiver's previous sender max allowed video/audio LODs when this component is disabled.")]
		public bool restoreQualitiesOnDisable = true;

		[Header("Debug")]
		public bool logQualityChanges;

		readonly Dictionary<ulong, ReceiverState> _states = new Dictionary<ulong, ReceiverState>();
		readonly List<ulong> _knownReceivers = new List<ulong>();
		readonly List<ulong> _staleReceivers = new List<ulong>();
		readonly List<ReceiverBudgetDecision> _budgetDecisions = new List<ReceiverBudgetDecision>();
		readonly List<PortailStreamVideoLodConfig> _defaultVideoLods = PortailStreamSenderPlugin.CreateDefaultVideoLods();
		readonly List<PortailStreamAudioLodConfig> _defaultAudioLods = PortailStreamSenderPlugin.CreateDefaultAudioLods();

		float _nextUpdateAt;

		void OnEnable()
		{
			ResolveReferences();
			_nextUpdateAt = 0f;
		}

		void OnDisable()
		{
			if (restoreQualitiesOnDisable)
				RestoreManagedReceiverQualities();

			_states.Clear();
		}

		void OnValidate()
		{
			maxTotalUploadBandwidthMbps = Mathf.Max(0f, maxTotalUploadBandwidthMbps);
			sdrPerReceiverBandwidthMbps = Mathf.Max(0f, sdrPerReceiverBandwidthMbps);
			updateIntervalSeconds = Mathf.Max(0.02f, updateIntervalSeconds);
		}

		void LateUpdate()
		{
			float now = Time.unscaledTime;
			if (now < _nextUpdateAt)
				return;

			_nextUpdateAt = now + Mathf.Max(0.02f, updateIntervalSeconds);
			ResolveReferences();

			if (senderManager == null)
				return;

			if (!HasGlobalUploadBudget() && !HasSdrPathBudget() && !HasForcedOffOverrides())
			{
				RestoreManagedReceiverQualities();
				_states.Clear();
				return;
			}

			UpdateReceiverQualities();
		}

		public bool IsVideoForcedOff(ulong receiverSteamId)
		{
			return receiverSteamId != 0 &&
				_states.TryGetValue(receiverSteamId, out ReceiverState state) &&
				state.forceVideoOff;
		}

		public bool IsAudioForcedOff(ulong receiverSteamId)
		{
			return receiverSteamId != 0 &&
				_states.TryGetValue(receiverSteamId, out ReceiverState state) &&
				state.forceAudioOff;
		}

		public bool ToggleVideoForcedOff(ulong receiverSteamId)
		{
			bool next = !IsVideoForcedOff(receiverSteamId);
			SetVideoForcedOff(receiverSteamId, next);
			return next;
		}

		public bool ToggleAudioForcedOff(ulong receiverSteamId)
		{
			bool next = !IsAudioForcedOff(receiverSteamId);
			SetAudioForcedOff(receiverSteamId, next);
			return next;
		}

		public void SetVideoForcedOff(ulong receiverSteamId, bool forcedOff)
		{
			if (receiverSteamId == 0)
				return;

			ResolveReferences();
			if (senderManager == null)
				return;

			ReceiverState state = GetOrCreateState(receiverSteamId);
			CaptureRestoreLodsIfNeeded(receiverSteamId, state);
			if (state.forceVideoOff == forcedOff)
				return;

			state.forceVideoOff = forcedOff;
			if (forcedOff)
				ApplyVideoLod(receiverSteamId, state, PortailStreamLodUtility.Off);
			else
				state.appliedVideoLod = UnsetLod;

			_nextUpdateAt = 0f;
		}

		public void SetAudioForcedOff(ulong receiverSteamId, bool forcedOff)
		{
			if (receiverSteamId == 0)
				return;

			ResolveReferences();
			if (senderManager == null)
				return;

			ReceiverState state = GetOrCreateState(receiverSteamId);
			CaptureRestoreLodsIfNeeded(receiverSteamId, state);
			if (state.forceAudioOff == forcedOff)
				return;

			state.forceAudioOff = forcedOff;
			if (forcedOff)
				ApplyAudioLod(receiverSteamId, state, PortailStreamLodUtility.Off);
			else
				state.appliedAudioLod = UnsetLod;

			_nextUpdateAt = 0f;
		}

		void ResolveReferences()
		{
			if (senderManager != null)
				return;

			senderManager = PortailStreamSender.Instance;
			if (senderManager == null)
				senderManager = FindFirstObjectByType<PortailStreamSender>();
		}

		bool HasGlobalUploadBudget()
		{
			return maxTotalUploadBandwidthMbps > 0f;
		}

		bool HasSdrPathBudget()
		{
			return applySdrBandwidthLimit && sdrPerReceiverBandwidthMbps > 0f;
		}

		bool HasForcedOffOverrides()
		{
			foreach (ReceiverState state in _states.Values)
			{
				if (state != null && (state.forceVideoOff || state.forceAudioOff))
					return true;
			}

			return false;
		}

		void UpdateReceiverQualities()
		{
			senderManager.GetKnownRemoteReceiverSteamIds(_knownReceivers);
			PruneStaleReceivers();

			_budgetDecisions.Clear();
			for (int i = 0; i < _knownReceivers.Count; ++i)
			{
				ulong receiverSteamId = _knownReceivers[i];
				if (receiverSteamId == 0)
					continue;

				ReceiverState state = GetOrCreateState(receiverSteamId);
				CaptureRestoreLodsIfNeeded(receiverSteamId, state);

				ReceiverBudgetDecision decision = new ReceiverBudgetDecision
				{
					receiverSteamId = receiverSteamId,
					state = state,
					active = senderManager.IsReceiverCurrentlyActive(receiverSteamId),
					nextVideoLod = state.restoreVideoLod,
					nextAudioLod = state.restoreAudioLod,
				};

				if (senderManager.TryGetPeerStats(receiverSteamId, out PortailStreamSenderPeerStats stats))
				{
					decision.connectionPath = stats.connected != 0 ? stats.connection_path : 0;
					decision.receiverMaxVideoLod = PortailStreamLodUtility.NormalizeLod(stats.max_video_lod);
					decision.receiverMaxAudioLod = PortailStreamLodUtility.NormalizeLod(stats.max_audio_lod);
				}

				_budgetDecisions.Add(decision);
			}

			ApplySdrPathBudgets(_budgetDecisions);
			ApplyUploadBudget(_budgetDecisions);

			for (int i = 0; i < _budgetDecisions.Count; ++i)
			{
				ReceiverBudgetDecision decision = _budgetDecisions[i];
				if (decision.state.forceVideoOff)
					decision.nextVideoLod = PortailStreamLodUtility.Off;
				if (decision.state.forceAudioOff)
					decision.nextAudioLod = PortailStreamLodUtility.Off;

				ApplyVideoLod(decision.receiverSteamId, decision.state, decision.nextVideoLod);
				ApplyAudioLod(decision.receiverSteamId, decision.state, decision.nextAudioLod);
			}
		}

		void PruneStaleReceivers()
		{
			_staleReceivers.Clear();
			foreach (ulong receiverSteamId in _states.Keys)
			{
				if (!_knownReceivers.Contains(receiverSteamId))
					_staleReceivers.Add(receiverSteamId);
			}

			for (int i = 0; i < _staleReceivers.Count; ++i)
			{
				ulong receiverSteamId = _staleReceivers[i];
				if (restoreQualitiesOnDisable && _states.TryGetValue(receiverSteamId, out ReceiverState state))
					RestoreReceiverQuality(receiverSteamId, state);

				_states.Remove(receiverSteamId);
			}
		}

		ReceiverState GetOrCreateState(ulong receiverSteamId)
		{
			if (!_states.TryGetValue(receiverSteamId, out ReceiverState state))
			{
				state = new ReceiverState();
				_states.Add(receiverSteamId, state);
			}

			return state;
		}

		void CaptureRestoreLodsIfNeeded(ulong receiverSteamId, ReceiverState state)
		{
			if (state.capturedRestoreLods || senderManager == null)
				return;

			state.restoreVideoLod = senderManager.GetReceiverMaxAllowedVideoLod(receiverSteamId);
			state.restoreAudioLod = senderManager.GetReceiverMaxAllowedAudioLod(receiverSteamId);
			state.appliedVideoLod = state.restoreVideoLod;
			state.appliedAudioLod = state.restoreAudioLod;
			state.capturedRestoreLods = true;
		}

		void ApplySdrPathBudgets(List<ReceiverBudgetDecision> decisions)
		{
			if (!HasSdrPathBudget())
				return;

			float limitKbps = Mathf.Max(0f, sdrPerReceiverBandwidthMbps) * 1000f;
			if (limitKbps <= 0f)
				return;

			for (int i = 0; i < decisions.Count; ++i)
			{
				ReceiverBudgetDecision decision = decisions[i];
				if (!decision.active || !IsSdrConnectionPath(decision.connectionPath))
					continue;

				ApplyPerReceiverPathBudget(decision, limitKbps);
			}
		}

		void ApplyPerReceiverPathBudget(ReceiverBudgetDecision decision, float limitKbps)
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

		float EstimateDecisionMediaKbps(ReceiverBudgetDecision decision)
		{
			return EstimateDecisionVideoKbps(decision, decision.nextVideoLod) +
				EstimateDecisionAudioKbps(decision, decision.nextAudioLod);
		}

		bool TryLowerVideoForDecision(ReceiverBudgetDecision decision)
		{
			float currentKbps = EstimateDecisionVideoKbps(decision, decision.nextVideoLod);
			if (currentKbps <= 0f)
				return false;

			if (!TryFindNextVideoLodWithSaving(decision, currentKbps, out int nextLod, out _))
				return false;

			decision.nextVideoLod = nextLod;
			return true;
		}

		bool TryLowerAudioForDecision(ReceiverBudgetDecision decision)
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

		void ApplyUploadBudget(List<ReceiverBudgetDecision> decisions)
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

		float EstimateTotalUploadKbps(List<ReceiverBudgetDecision> decisions)
		{
			float total = 0f;
			for (int i = 0; i < decisions.Count; ++i)
			{
				ReceiverBudgetDecision decision = decisions[i];
				if (!decision.active)
					continue;

				total += EstimateDecisionVideoKbps(decision, decision.nextVideoLod);
				total += EstimateDecisionAudioKbps(decision, decision.nextAudioLod);
			}

			return total;
		}

		bool TryLowerLargestVideoConsumer(List<ReceiverBudgetDecision> decisions)
		{
			int bestIndex = -1;
			int bestNextLod = PortailStreamLodUtility.Off;
			float bestCurrentKbps = -1f;
			float bestSavingKbps = -1f;

			for (int i = 0; i < decisions.Count; ++i)
			{
				ReceiverBudgetDecision decision = decisions[i];
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

		bool TryLowerLargestAudioConsumer(List<ReceiverBudgetDecision> decisions)
		{
			int bestIndex = -1;
			int bestNextLod = PortailStreamLodUtility.Off;
			float bestCurrentKbps = -1f;
			float bestSavingKbps = -1f;

			for (int i = 0; i < decisions.Count; ++i)
			{
				ReceiverBudgetDecision decision = decisions[i];
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

		bool TryFindNextVideoLodWithSaving(ReceiverBudgetDecision decision, float currentKbps, out int nextLod, out float savingKbps)
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

		bool TryFindNextAudioLodWithSaving(ReceiverBudgetDecision decision, float currentKbps, out int nextLod, out float savingKbps)
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

		float EstimateDecisionVideoKbps(ReceiverBudgetDecision decision, int senderVideoLod)
		{
			int effectiveLod = ResolveEffectiveVideoLod(senderVideoLod, decision.receiverMaxVideoLod);
			return EstimateVideoLodBitrateKbps(effectiveLod);
		}

		float EstimateDecisionAudioKbps(ReceiverBudgetDecision decision, int senderAudioLod)
		{
			int effectiveLod = ResolveEffectiveAudioLod(senderAudioLod, decision.receiverMaxAudioLod);
			return EstimateAudioLodBitrateKbps(effectiveLod);
		}

		int ResolveEffectiveVideoLod(int senderMaxLod, int receiverMaxLod)
		{
			return ResolveEffectiveLod(senderMaxLod, receiverMaxLod, GetVideoLods(), IsVideoLodUsable);
		}

		int ResolveEffectiveAudioLod(int senderMaxLod, int receiverMaxLod)
		{
			return ResolveEffectiveLod(senderMaxLod, receiverMaxLod, GetAudioLods(), IsAudioLodUsable);
		}

		static int ResolveEffectiveLod<T>(int senderMaxLod, int receiverMaxLod, IList<T> lods, System.Predicate<T> isUsable)
		{
			senderMaxLod = PortailStreamLodUtility.NormalizeLod(senderMaxLod);
			receiverMaxLod = PortailStreamLodUtility.NormalizeLod(receiverMaxLod);
			if (senderMaxLod < 0 || receiverMaxLod < 0 || lods == null || lods.Count == 0)
				return PortailStreamLodUtility.Off;

			int firstAllowed = Mathf.Max(senderMaxLod, receiverMaxLod);
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
			if (senderManager != null && senderManager.videoLods != null && senderManager.videoLods.Count > 0)
				return senderManager.videoLods;

			return _defaultVideoLods;
		}

		IList<PortailStreamAudioLodConfig> GetAudioLods()
		{
			if (senderManager != null && senderManager.audioLods != null && senderManager.audioLods.Count > 0)
				return senderManager.audioLods;

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

		void ApplyVideoLod(ulong receiverSteamId, ReceiverState state, int lod)
		{
			lod = PortailStreamLodUtility.NormalizeLod(lod);
			if (state.appliedVideoLod == lod)
				return;

			senderManager.SetReceiverMaxAllowedVideoLod(receiverSteamId, lod);
			state.appliedVideoLod = lod;

			if (logQualityChanges)
			{
				Debug.Log($"[PortailStreamUploadQualityController] Receiver {receiverSteamId} video max -> {PortailStreamLodUtility.ToVideoLabel(lod)} " +
					$"(global upload budget {maxTotalUploadBandwidthMbps:0.##} Mbps, SDR cap {sdrPerReceiverBandwidthMbps:0.##} Mbps)");
			}
		}

		void ApplyAudioLod(ulong receiverSteamId, ReceiverState state, int lod)
		{
			lod = PortailStreamLodUtility.NormalizeLod(lod);
			if (state.appliedAudioLod == lod)
				return;

			senderManager.SetReceiverMaxAllowedAudioLod(receiverSteamId, lod);
			state.appliedAudioLod = lod;

			if (logQualityChanges)
			{
				Debug.Log($"[PortailStreamUploadQualityController] Receiver {receiverSteamId} audio max -> {PortailStreamLodUtility.ToAudioLabel(lod)} " +
					$"(global upload budget {maxTotalUploadBandwidthMbps:0.##} Mbps, SDR cap {sdrPerReceiverBandwidthMbps:0.##} Mbps)");
			}
		}

		void RestoreManagedReceiverQualities()
		{
			if (senderManager == null)
				senderManager = PortailStreamSender.Instance;

			if (senderManager == null)
				return;

			foreach (KeyValuePair<ulong, ReceiverState> pair in _states)
				RestoreReceiverQuality(pair.Key, pair.Value);
		}

		void RestoreReceiverQuality(ulong receiverSteamId, ReceiverState state)
		{
			if (senderManager == null || receiverSteamId == 0 || state == null || !state.capturedRestoreLods)
				return;

			senderManager.SetReceiverMaxAllowedVideoLod(receiverSteamId, state.restoreVideoLod);
			senderManager.SetReceiverMaxAllowedAudioLod(receiverSteamId, state.restoreAudioLod);
			state.appliedVideoLod = state.restoreVideoLod;
			state.appliedAudioLod = state.restoreAudioLod;

			if (logQualityChanges)
			{
				Debug.Log($"[PortailStreamUploadQualityController] Receiver {receiverSteamId} restored to " +
					$"{PortailStreamLodUtility.ToVideoLabel(state.restoreVideoLod)} / {PortailStreamLodUtility.ToAudioLabel(state.restoreAudioLod)}");
			}
		}
	}
}
