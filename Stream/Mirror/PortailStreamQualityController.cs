using System;
using System.Collections.Generic;
using Portail.Core;
using Portail.Playback;
using Portail.Stream;
using UnityEngine;
using UnityEngine.Serialization;

namespace Portail.Stream.Mirror
{
	/// <summary>
	/// Client-side, scene-driven quality governor for received Steam desktop streams.
	///
	/// This component is intentionally standalone: disable it to stop automatic quality decisions.
	/// It never talks to the native plugin directly. It only drives MirrorPortailStreamReceiver's
	/// per-remote max accepted video/audio LOD settings.
	/// </summary>
	[DefaultExecutionOrder(-9400)]
	[DisallowMultipleComponent]
	public sealed class PortailStreamQualityController : MonoBehaviour
	{
		sealed class RemoteHostState
		{
			public bool capturedRestoreLods;
			public int restoreVideoLod = PortailStreamLodUtility.Highest;
			public int restoreAudioLod = PortailStreamLodUtility.Highest;
			public int appliedVideoLod = UnsetLod;
			public int appliedAudioLod = UnsetLod;
			public bool forceVideoOff;
			public bool forceAudioOff;
			public float lastProjectedPixelArea;
			public float lastRequiredVideoBitrateKbps;
			public float lastEstimatedAudioVolume;
			public int lastIdealVideoLod = PortailStreamLodUtility.Off;
			public int lastIdealAudioLod = PortailStreamLodUtility.Off;
		}

		sealed class RemoteHostBudgetDecision
		{
			public ulong hostSteamId;
			public RemoteHostState state;
			public int connectionPath;
			public int nextVideoLod = PortailStreamLodUtility.Off;
			public int nextAudioLod = PortailStreamLodUtility.Off;
			public float projectedPixelArea;
			public float estimatedAudioVolume;
			public float requiredVideoBitrateKbps;
		}

		const int UnsetLod = int.MinValue;
		const int ConnectionPathSdr = 2;

		// Desktop capture needs enough bits for sharp text/UI detail, but the amount required
		// should scale with how many pixels the in-world display actually occupies on screen.
		// The default LOD table uses roughly 0.10 bits per visible pixel per frame:
		// 1920x1080x60 at 12000 kbps = ~0.096 bpppf.
		const float GoodDesktopBitsPerPixelPerFrame = 0.10f;

		[Header("References")]
		[Tooltip("Client manager to drive. If left empty, MirrorPortailStreamReceiver.Instance is used, then a scene search fallback.")]
		public MirrorPortailStreamReceiver clientManager;

		[Tooltip("LOD table reference. Because all hosts use the same LOD table, this is used to read each video LOD's target bitrate. If empty, MirrorPortailStreamSender.Instance or default LODs are used.")]
		public MirrorPortailStreamSender hostLodReference;

		[Tooltip("Camera used to measure how large remote stream displays are on this client. If empty, Camera.main and active cameras are used.")]
		public Camera viewCamera;

		[Tooltip("Listener used for spatial audio audibility checks. If empty, this is resolved from the camera, then the active scene AudioListener.")]
		public AudioListener audioListenerOverride;

		[Header("Timing")]
		[Min(0.02f)]
		[Tooltip("How often quality decisions are recomputed. Projected area is recalculated every tick.")]
		public float updateIntervalSeconds = 0.1f;

		[Min(0.1f)]
		[Tooltip("How often the scene is scanned for PortailFeedPlayback displays. Keep this slower than updateIntervalSeconds; movement still reacts every quality tick.")]
		public float displayRefreshSeconds = 0.5f;

		[Header("Video")]
		[FormerlySerializedAs("videoPixelAreaBias")]
		[Range(0.25f, 4f)]
		[Tooltip("Multiplier applied to the bitrate required for the measured projected display area. Lower values favor cheaper/lower-bitrate LODs; higher values favor cleaner/higher-bitrate LODs.")]
		public float videoBitrateBias = 1f;


		[Min(0f)]
		[Tooltip("A display whose projected screen area is below this many pixels is treated as invisible/off.")]
		public float minimumVisiblePixelArea = 4f;

		[Tooltip("Ignore disabled entries in the referenced video LOD list when choosing a requested max LOD.")]
		public bool ignoreDisabledVideoLods = true;

		[Header("Audio")]
		[Range(0f, 1f)]
		[Tooltip("Remote audio is requested only when the estimated post-rolloff AudioSource volume is at or above this threshold.")]
		public float audibleVolumeThreshold = 0.01f;

		[Tooltip("Multiply estimated AudioSource audibility by AudioListener.volume.")]
		public bool includeGlobalAudioListenerVolume = true;

		[Header("Bandwidth Budget")]
		[Min(0f)]
		[Tooltip("Maximum total incoming PortailStream media bitrate in Mbps. Set to 0 to disable the global download budget.")]
		public float maxTotalDownloadBandwidthMbps;

		[Header("Per-Path Bandwidth")]
		[Tooltip("When a host connection is routed through Steam Datagram Relay instead of ICE/direct, cap that host's total video+audio media bitrate.")]
		public bool applySdrBandwidthLimit = true;

		[Min(0f)]
		[Tooltip("Per-host total video+audio media bitrate cap in Mbps while the Steam path reports SDR. Set to 0 to disable the SDR cap.")]
		public float sdrPerHostBandwidthMbps = 7f;

		[Header("Lifecycle")]
		[Tooltip("Restore each remote host's previous max accepted video/audio LODs when this component is disabled.")]
		public bool restoreQualitiesOnDisable = true;

		[Header("Debug")]
		public bool logQualityChanges;

		[Tooltip("Logs each host's measured visible pixel area every decision tick. Useful while validating camera/projection behavior; leave disabled in normal play.")]
		public bool logVideoAreaEveryUpdate;

		readonly Dictionary<ulong, RemoteHostState> _states = new Dictionary<ulong, RemoteHostState>();
		readonly List<ulong> _knownHosts = new List<ulong>();
		readonly List<ulong> _staleHosts = new List<ulong>();
		readonly HashSet<AudioSource> _audioSourcesVisited = new HashSet<AudioSource>();
		readonly Vector3[] _localScreenCorners = new Vector3[4];
		readonly List<Vector3> _worldPolygon = new List<Vector3>(8);
		readonly List<Vector3> _worldClipScratch = new List<Vector3>(8);
		readonly List<Vector2> _viewportPolygon = new List<Vector2>(8);
		readonly List<Vector2> _viewportClipScratch = new List<Vector2>(8);
		readonly List<RemoteHostBudgetDecision> _budgetDecisions = new List<RemoteHostBudgetDecision>();
		readonly List<PortailStreamVideoLodConfig> _defaultVideoLods = PortailStreamHostPlugin.CreateDefaultVideoLods();
		readonly List<PortailStreamAudioLodConfig> _defaultAudioLods = PortailStreamHostPlugin.CreateDefaultAudioLods();

		PortailFeedPlayback[] _streamPlayers = Array.Empty<PortailFeedPlayback>();
		float _nextUpdateAt;
		float _nextDisplayRefreshAt;

		void OnEnable()
		{
			ResolveReferences();
			RefreshStreamPlayers();
			_nextUpdateAt = 0f;
			_nextDisplayRefreshAt = 0f;
		}

		void OnDisable()
		{
			if (restoreQualitiesOnDisable)
				RestoreManagedRemoteQualities();

			_states.Clear();
		}

		void OnValidate()
		{
			updateIntervalSeconds = Mathf.Max(0.02f, updateIntervalSeconds);
			displayRefreshSeconds = Mathf.Max(0.1f, displayRefreshSeconds);
			videoBitrateBias = Mathf.Clamp(videoBitrateBias, 0.25f, 4f);
			minimumVisiblePixelArea = Mathf.Max(0f, minimumVisiblePixelArea);
			audibleVolumeThreshold = Mathf.Clamp01(audibleVolumeThreshold);
			maxTotalDownloadBandwidthMbps = Mathf.Max(0f, maxTotalDownloadBandwidthMbps);
			sdrPerHostBandwidthMbps = Mathf.Max(0f, sdrPerHostBandwidthMbps);
		}

		void LateUpdate()
		{
			float now = Time.unscaledTime;
			if (now < _nextUpdateAt)
				return;

			_nextUpdateAt = now + Mathf.Max(0.02f, updateIntervalSeconds);
			ResolveReferences();

			if (now >= _nextDisplayRefreshAt)
			{
				_nextDisplayRefreshAt = now + Mathf.Max(0.1f, displayRefreshSeconds);
				RefreshStreamPlayers();
			}

			UpdateRemoteHostQualities();
		}

		public bool IsVideoForcedOff(ulong hostSteamId)
		{
			return hostSteamId != 0 &&
				_states.TryGetValue(hostSteamId, out RemoteHostState state) &&
				state.forceVideoOff;
		}

		public bool IsAudioForcedOff(ulong hostSteamId)
		{
			return hostSteamId != 0 &&
				_states.TryGetValue(hostSteamId, out RemoteHostState state) &&
				state.forceAudioOff;
		}

		public bool ToggleVideoForcedOff(ulong hostSteamId)
		{
			bool next = !IsVideoForcedOff(hostSteamId);
			SetVideoForcedOff(hostSteamId, next);
			return next;
		}

		public bool ToggleAudioForcedOff(ulong hostSteamId)
		{
			bool next = !IsAudioForcedOff(hostSteamId);
			SetAudioForcedOff(hostSteamId, next);
			return next;
		}

		public void SetVideoForcedOff(ulong hostSteamId, bool forcedOff)
		{
			if (hostSteamId == 0)
				return;

			ResolveReferences();
			if (clientManager == null)
				return;

			RemoteHostState state = GetOrCreateState(hostSteamId);
			CaptureRestoreLodsIfNeeded(hostSteamId, state);
			if (state.forceVideoOff == forcedOff)
				return;

			state.forceVideoOff = forcedOff;
			if (forcedOff)
				ApplyVideoLod(hostSteamId, state, PortailStreamLodUtility.Off);
			else
				state.appliedVideoLod = UnsetLod;

			_nextUpdateAt = 0f;
		}

		public void SetAudioForcedOff(ulong hostSteamId, bool forcedOff)
		{
			if (hostSteamId == 0)
				return;

			ResolveReferences();
			if (clientManager == null)
				return;

			RemoteHostState state = GetOrCreateState(hostSteamId);
			CaptureRestoreLodsIfNeeded(hostSteamId, state);
			if (state.forceAudioOff == forcedOff)
				return;

			state.forceAudioOff = forcedOff;
			if (forcedOff)
				ApplyAudioLod(hostSteamId, state, PortailStreamLodUtility.Off);
			else
				state.appliedAudioLod = UnsetLod;

			_nextUpdateAt = 0f;
		}

		void ResolveReferences()
		{
			if (clientManager == null)
			{
				clientManager = MirrorPortailStreamReceiver.Instance;
				if (clientManager == null)
					clientManager = FindFirstObjectByType<MirrorPortailStreamReceiver>();
			}

			if (hostLodReference == null)
			{
				hostLodReference = MirrorPortailStreamSender.Instance;
				if (hostLodReference == null)
					hostLodReference = FindFirstObjectByType<MirrorPortailStreamSender>();
			}

			if (!IsUsableCamera(viewCamera))
				viewCamera = ResolveFallbackViewCamera();

			AudioListener viewCameraListener = ResolveAudioListenerOnCamera(viewCamera);
			if (viewCameraListener != null)
				audioListenerOverride = viewCameraListener;
			else if (!IsUsableAudioListener(audioListenerOverride))
				audioListenerOverride = ResolveFallbackAudioListener();
		}

		Camera ResolveFallbackViewCamera()
		{
			Camera main = Camera.main;
			if (IsUsableCamera(main))
				return main;

			Camera[] cameras = FindObjectsByType<Camera>(FindObjectsSortMode.None);
			for (int i = 0; i < cameras.Length; ++i)
			{
				Camera camera = cameras[i];
				if (IsUsableCamera(camera))
					return camera;
			}

			return null;
		}

		static bool IsUsableCamera(Camera camera)
		{
			return camera != null &&
				camera.enabled &&
				camera.gameObject.activeInHierarchy &&
				camera.pixelWidth > 0 &&
				camera.pixelHeight > 0;
		}

		AudioListener ResolveAudioListenerOnCamera(Camera camera)
		{
			if (camera != null && camera.TryGetComponent(out AudioListener cameraListener) && IsUsableAudioListener(cameraListener))
				return cameraListener;

			return null;
		}

		AudioListener ResolveFallbackAudioListener()
		{
			AudioListener[] listeners = FindObjectsByType<AudioListener>(FindObjectsSortMode.None);
			for (int i = 0; i < listeners.Length; ++i)
			{
				AudioListener listener = listeners[i];
				if (IsUsableAudioListener(listener))
					return listener;
			}

			return null;
		}

		static bool IsUsableAudioListener(AudioListener listener)
		{
			return listener != null &&
				listener.enabled &&
				listener.gameObject.activeInHierarchy;
		}

		void RefreshStreamPlayers()
		{
			_streamPlayers = FindObjectsByType<PortailFeedPlayback>(FindObjectsSortMode.None);
		}

		void UpdateRemoteHostQualities()
		{
			if (clientManager == null)
				return;

			clientManager.GetKnownRemoteHostSteamIds(_knownHosts);
			PruneStaleHosts();

			_budgetDecisions.Clear();
			for (int i = 0; i < _knownHosts.Count; ++i)
			{
				ulong hostSteamId = _knownHosts[i];
				if (hostSteamId == 0)
					continue;

				RemoteHostState state = GetOrCreateState(hostSteamId);
				CaptureRestoreLodsIfNeeded(hostSteamId, state);
				int connectionPath = GetConnectionPathForHost(hostSteamId);

				float projectedPixelArea = ComputeLargestProjectedPixelAreaForHost(hostSteamId);
				float estimatedAudioVolume = ComputeHighestEstimatedAudioVolumeForHost(hostSteamId);

				float requiredVideoBitrateKbps = projectedPixelArea >= minimumVisiblePixelArea
					? ComputeRequiredVideoBitrateKbps(projectedPixelArea)
					: 0f;
				int idealVideoLod = requiredVideoBitrateKbps > 0f
					? ChooseVideoLod(requiredVideoBitrateKbps)
					: PortailStreamLodUtility.Off;
				int idealAudioLod = estimatedAudioVolume >= audibleVolumeThreshold
					? PortailStreamLodUtility.Highest
					: PortailStreamLodUtility.Off;

				state.lastProjectedPixelArea = projectedPixelArea;
				state.lastRequiredVideoBitrateKbps = requiredVideoBitrateKbps;
				state.lastEstimatedAudioVolume = estimatedAudioVolume;
				state.lastIdealVideoLod = idealVideoLod;
				state.lastIdealAudioLod = idealAudioLod;

				if (logVideoAreaEveryUpdate)
				{
					Debug.Log($"[PortailStreamQualityController] Host {hostSteamId} visible pixels {projectedPixelArea:0.0}, " +
						$"required bitrate {requiredVideoBitrateKbps:0} kbps, ideal video {PortailStreamLodUtility.ToVideoLabel(idealVideoLod)}, " +
						$"camera {(viewCamera != null ? viewCamera.name : "none")}");
				}

				_budgetDecisions.Add(new RemoteHostBudgetDecision
				{
					hostSteamId = hostSteamId,
					state = state,
					connectionPath = connectionPath,
					nextVideoLod = idealVideoLod,
					nextAudioLod = idealAudioLod,
					projectedPixelArea = projectedPixelArea,
					estimatedAudioVolume = estimatedAudioVolume,
					requiredVideoBitrateKbps = requiredVideoBitrateKbps,
				});
			}

			ApplySdrPathBudgets(_budgetDecisions);
			ApplyDownloadBudget(_budgetDecisions);

			for (int i = 0; i < _budgetDecisions.Count; ++i)
			{
				RemoteHostBudgetDecision decision = _budgetDecisions[i];
				if (decision.state.forceVideoOff)
					decision.nextVideoLod = PortailStreamLodUtility.Off;
				if (decision.state.forceAudioOff)
					decision.nextAudioLod = PortailStreamLodUtility.Off;

				ApplyVideoLod(decision.hostSteamId, decision.state, decision.nextVideoLod);
				ApplyAudioLod(decision.hostSteamId, decision.state, decision.nextAudioLod);
			}
		}

		void PruneStaleHosts()
		{
			_staleHosts.Clear();
			foreach (ulong hostSteamId in _states.Keys)
			{
				if (!_knownHosts.Contains(hostSteamId))
					_staleHosts.Add(hostSteamId);
			}

			for (int i = 0; i < _staleHosts.Count; ++i)
			{
				ulong hostSteamId = _staleHosts[i];
				if (restoreQualitiesOnDisable && _states.TryGetValue(hostSteamId, out RemoteHostState state))
					RestoreRemoteQuality(hostSteamId, state);

				_states.Remove(hostSteamId);
			}
		}

		RemoteHostState GetOrCreateState(ulong hostSteamId)
		{
			if (!_states.TryGetValue(hostSteamId, out RemoteHostState state))
			{
				state = new RemoteHostState();
				_states.Add(hostSteamId, state);
			}

			return state;
		}

		void CaptureRestoreLodsIfNeeded(ulong hostSteamId, RemoteHostState state)
		{
			if (state.capturedRestoreLods || clientManager == null)
				return;

			state.restoreVideoLod = clientManager.GetMaxAcceptedVideoLod(hostSteamId);
			state.restoreAudioLod = clientManager.GetMaxAcceptedAudioLod(hostSteamId);
			state.appliedVideoLod = state.restoreVideoLod;
			state.appliedAudioLod = state.restoreAudioLod;
			state.capturedRestoreLods = true;
		}

		int GetConnectionPathForHost(ulong hostSteamId)
		{
			if (clientManager == null || hostSteamId == 0)
				return 0;

			if (clientManager.TryGetPeerStatsObject(hostSteamId, out object statsObject) &&
				statsObject is PortailStreamClientPeerStats stats &&
				stats.connected != 0)
			{
				return stats.connection_path;
			}

			return 0;
		}

		float ComputeLargestProjectedPixelAreaForHost(ulong hostSteamId)
		{
			if (viewCamera == null || _streamPlayers == null || _streamPlayers.Length == 0)
				return 0f;

			float largestArea = 0f;
			for (int i = 0; i < _streamPlayers.Length; ++i)
			{
				PortailFeedPlayback player = _streamPlayers[i];
				if (!IsPlayerShowingHost(player, hostSteamId))
					continue;

				float area = ComputeProjectedPixelArea(viewCamera, player.ScreenDisplaySurface);
				if (area > largestArea)
					largestArea = area;
			}

			return largestArea;
		}

		float ComputeProjectedPixelArea(Camera camera, MeshRenderer renderer)
		{
			if (!IsUsableCamera(camera) || renderer == null || !renderer.enabled || !renderer.gameObject.activeInHierarchy)
				return 0f;

			Plane[] frustumPlanes = GeometryUtility.CalculateFrustumPlanes(camera);
			if (!GeometryUtility.TestPlanesAABB(frustumPlanes, renderer.bounds))
				return 0f;

			FillScreenSurfaceCorners(renderer, _localScreenCorners);

			_worldPolygon.Clear();
			Transform rendererTransform = renderer.transform;
			for (int i = 0; i < _localScreenCorners.Length; ++i)
				_worldPolygon.Add(rendererTransform.TransformPoint(_localScreenCorners[i]));

			ClipWorldPolygonAgainstCameraNearPlane(camera, _worldPolygon, _worldClipScratch);
			if (_worldPolygon.Count < 3)
				return 0f;

			_viewportPolygon.Clear();
			for (int i = 0; i < _worldPolygon.Count; ++i)
			{
				Vector3 viewport = camera.WorldToViewportPoint(_worldPolygon[i]);
				_viewportPolygon.Add(new Vector2(viewport.x, viewport.y));
			}

			ClipViewportPolygonToUnitRect(_viewportPolygon, _viewportClipScratch);
			if (_viewportPolygon.Count < 3)
				return 0f;

			float normalizedArea = Mathf.Abs(ComputePolygonArea(_viewportPolygon));
			return normalizedArea * camera.pixelWidth * camera.pixelHeight;
		}

		static void FillScreenSurfaceCorners(MeshRenderer renderer, Vector3[] corners)
		{
			Bounds localBounds;
			MeshFilter meshFilter = renderer.GetComponent<MeshFilter>();
			if (meshFilter != null && meshFilter.sharedMesh != null)
			{
				localBounds = meshFilter.sharedMesh.bounds;
			}
			else
			{
				Bounds worldBounds = renderer.bounds;
				Vector3 localMin = renderer.transform.InverseTransformPoint(worldBounds.min);
				Vector3 localMax = renderer.transform.InverseTransformPoint(worldBounds.max);
				localBounds = new Bounds((localMin + localMax) * 0.5f, new Vector3(
					Mathf.Abs(localMax.x - localMin.x),
					Mathf.Abs(localMax.y - localMin.y),
					Mathf.Abs(localMax.z - localMin.z)));
			}

			Vector3 min = localBounds.min;
			Vector3 max = localBounds.max;
			Vector3 center = localBounds.center;
			Vector3 size = localBounds.size;

			// Measure the visible screen face, not the full 3D renderer AABB. For a normal quad this
			// chooses the zero-thickness axis; for a slightly thick mesh it chooses the thinnest axis.
			if (size.x <= size.y && size.x <= size.z)
			{
				float x = center.x;
				corners[0] = new Vector3(x, min.y, min.z);
				corners[1] = new Vector3(x, max.y, min.z);
				corners[2] = new Vector3(x, max.y, max.z);
				corners[3] = new Vector3(x, min.y, max.z);
			}
			else if (size.y <= size.x && size.y <= size.z)
			{
				float y = center.y;
				corners[0] = new Vector3(min.x, y, min.z);
				corners[1] = new Vector3(max.x, y, min.z);
				corners[2] = new Vector3(max.x, y, max.z);
				corners[3] = new Vector3(min.x, y, max.z);
			}
			else
			{
				float z = center.z;
				corners[0] = new Vector3(min.x, min.y, z);
				corners[1] = new Vector3(max.x, min.y, z);
				corners[2] = new Vector3(max.x, max.y, z);
				corners[3] = new Vector3(min.x, max.y, z);
			}
		}

		static void ClipWorldPolygonAgainstCameraNearPlane(Camera camera, List<Vector3> polygon, List<Vector3> scratch)
		{
			if (polygon.Count == 0)
				return;

			scratch.Clear();
			Vector3 cameraPosition = camera.transform.position;
			Vector3 cameraForward = camera.transform.forward;
			float near = camera.nearClipPlane;

			Vector3 previous = polygon[polygon.Count - 1];
			float previousDistance = Vector3.Dot(previous - cameraPosition, cameraForward);
			bool previousInside = previousDistance >= near;

			for (int i = 0; i < polygon.Count; ++i)
			{
				Vector3 current = polygon[i];
				float currentDistance = Vector3.Dot(current - cameraPosition, cameraForward);
				bool currentInside = currentDistance >= near;

				if (currentInside != previousInside)
				{
					float denominator = currentDistance - previousDistance;
					float t = Mathf.Abs(denominator) > 0.000001f ? (near - previousDistance) / denominator : 0f;
					scratch.Add(Vector3.LerpUnclamped(previous, current, Mathf.Clamp01(t)));
				}

				if (currentInside)
					scratch.Add(current);

				previous = current;
				previousDistance = currentDistance;
				previousInside = currentInside;
			}

			polygon.Clear();
			polygon.AddRange(scratch);
		}

		static void ClipViewportPolygonToUnitRect(List<Vector2> polygon, List<Vector2> scratch)
		{
			ClipViewportPolygon(polygon, scratch, 0);
			ClipViewportPolygon(polygon, scratch, 1);
			ClipViewportPolygon(polygon, scratch, 2);
			ClipViewportPolygon(polygon, scratch, 3);
		}

		static void ClipViewportPolygon(List<Vector2> polygon, List<Vector2> scratch, int edge)
		{
			if (polygon.Count == 0)
				return;

			scratch.Clear();
			Vector2 previous = polygon[polygon.Count - 1];
			bool previousInside = IsInsideViewportEdge(previous, edge);

			for (int i = 0; i < polygon.Count; ++i)
			{
				Vector2 current = polygon[i];
				bool currentInside = IsInsideViewportEdge(current, edge);

				if (currentInside != previousInside)
					scratch.Add(IntersectViewportEdge(previous, current, edge));

				if (currentInside)
					scratch.Add(current);

				previous = current;
				previousInside = currentInside;
			}

			polygon.Clear();
			polygon.AddRange(scratch);
		}

		static bool IsInsideViewportEdge(Vector2 point, int edge)
		{
			switch (edge)
			{
				case 0: return point.x >= 0f;
				case 1: return point.x <= 1f;
				case 2: return point.y >= 0f;
				default: return point.y <= 1f;
			}
		}

		static Vector2 IntersectViewportEdge(Vector2 from, Vector2 to, int edge)
		{
			Vector2 delta = to - from;
			switch (edge)
			{
				case 0:
				{
					float t = Mathf.Abs(delta.x) > 0.000001f ? (0f - from.x) / delta.x : 0f;
					return Vector2.LerpUnclamped(from, to, Mathf.Clamp01(t));
				}
				case 1:
				{
					float t = Mathf.Abs(delta.x) > 0.000001f ? (1f - from.x) / delta.x : 0f;
					return Vector2.LerpUnclamped(from, to, Mathf.Clamp01(t));
				}
				case 2:
				{
					float t = Mathf.Abs(delta.y) > 0.000001f ? (0f - from.y) / delta.y : 0f;
					return Vector2.LerpUnclamped(from, to, Mathf.Clamp01(t));
				}
				default:
				{
					float t = Mathf.Abs(delta.y) > 0.000001f ? (1f - from.y) / delta.y : 0f;
					return Vector2.LerpUnclamped(from, to, Mathf.Clamp01(t));
				}
			}
		}

		static float ComputePolygonArea(List<Vector2> polygon)
		{
			float area = 0f;
			for (int i = 0, j = polygon.Count - 1; i < polygon.Count; j = i++)
			{
				Vector2 a = polygon[j];
				Vector2 b = polygon[i];
				area += (a.x * b.y) - (b.x * a.y);
			}

			return area * 0.5f;
		}

		float ComputeRequiredVideoBitrateKbps(float projectedPixelArea)
		{
			float referenceFps = GetVideoBitrateReferenceFps();
			float bitsPerSecond = Mathf.Max(0f, projectedPixelArea) * referenceFps * GoodDesktopBitsPerPixelPerFrame;
			return (bitsPerSecond / 1000f) * Mathf.Max(0.01f, videoBitrateBias);
		}

		float GetVideoBitrateReferenceFps()
		{
			IList<PortailStreamVideoLodConfig> lods = GetVideoLods();
			if (lods == null || lods.Count == 0)
				return 60f;

			int highestAvailableLod = FindHighestAvailableVideoLod(lods);
			if (highestAvailableLod >= 0 && highestAvailableLod < lods.Count && lods[highestAvailableLod] != null)
				return Mathf.Clamp(lods[highestAvailableLod].fps, 1, 240);

			return 60f;
		}

		int ChooseVideoLod(float requiredBitrateKbps)
		{
			IList<PortailStreamVideoLodConfig> lods = GetVideoLods();
			if (lods == null || lods.Count == 0)
				return PortailStreamLodUtility.Highest;

			requiredBitrateKbps = Mathf.Max(0f, requiredBitrateKbps);
			int highestAvailableLod = FindHighestAvailableVideoLod(lods);
			if (highestAvailableLod < 0)
				return PortailStreamLodUtility.Off;

			int bestFitLod = PortailStreamLodUtility.Off;
			float bestFitBitrateKbps = float.MaxValue;
			int highestBitrateLod = highestAvailableLod;
			float highestBitrateKbps = -1f;

			for (int i = 0; i < lods.Count; ++i)
			{
				PortailStreamVideoLodConfig lod = lods[i];
				if (!IsVideoLodUsable(lod))
					continue;

				float lodBitrateKbps = Mathf.Max(100, lod.targetBitrateKbps);
				if (lodBitrateKbps > highestBitrateKbps ||
					(Mathf.Approximately(lodBitrateKbps, highestBitrateKbps) && i < highestBitrateLod))
				{
					highestBitrateKbps = lodBitrateKbps;
					highestBitrateLod = i;
				}

				if (lodBitrateKbps >= requiredBitrateKbps &&
					(lodBitrateKbps < bestFitBitrateKbps ||
					(Mathf.Approximately(lodBitrateKbps, bestFitBitrateKbps) && i > bestFitLod)))
				{
					bestFitBitrateKbps = lodBitrateKbps;
					bestFitLod = i;
				}
			}

			if (bestFitLod >= 0)
				return bestFitLod;

			// The display needs more bitrate than every configured LOD. Request the highest-bitrate/sharpest one.
			return highestBitrateLod;
		}

		IList<PortailStreamVideoLodConfig> GetVideoLods()
		{
			if (hostLodReference != null && hostLodReference.videoLods != null && hostLodReference.videoLods.Count > 0)
				return hostLodReference.videoLods;

			return _defaultVideoLods;
		}

		IList<PortailStreamAudioLodConfig> GetAudioLods()
		{
			if (hostLodReference != null && hostLodReference.audioLods != null && hostLodReference.audioLods.Count > 0)
				return hostLodReference.audioLods;

			return _defaultAudioLods;
		}

		int FindHighestAvailableVideoLod(IList<PortailStreamVideoLodConfig> lods)
		{
			for (int i = 0; i < lods.Count; ++i)
			{
				if (IsVideoLodUsable(lods[i]))
					return i;
			}

			return PortailStreamLodUtility.Off;
		}

		bool IsVideoLodUsable(PortailStreamVideoLodConfig lod)
		{
			if (lod == null)
				return false;

			return !ignoreDisabledVideoLods || lod.enabled;
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

		int StepVideoLodDownToward(int current, int target)
		{
			if (current < 0)
				return PortailStreamLodUtility.Off;

			IList<PortailStreamVideoLodConfig> lods = GetVideoLods();
			if (lods == null || lods.Count == 0)
				return target;

			int nextLower = PortailStreamLodUtility.Off;
			for (int i = current + 1; i < lods.Count; ++i)
			{
				if (IsVideoLodUsable(lods[i]))
				{
					nextLower = i;
					break;
				}
			}

			if (nextLower < 0)
				return PortailStreamLodUtility.Off;

			if (target >= 0 && nextLower > target)
				return target;

			return nextLower;
		}

		void ApplySdrPathBudgets(List<RemoteHostBudgetDecision> decisions)
		{
			if (!HasSdrPathBudget())
				return;

			float limitKbps = Mathf.Max(0f, sdrPerHostBandwidthMbps) * 1000f;
			if (limitKbps <= 0f)
				return;

			for (int i = 0; i < decisions.Count; ++i)
			{
				RemoteHostBudgetDecision decision = decisions[i];
				if (!IsSdrConnectionPath(decision.connectionPath))
					continue;

				ApplyPerHostPathBudget(decision, limitKbps);
			}
		}

		bool HasSdrPathBudget()
		{
			return applySdrBandwidthLimit && sdrPerHostBandwidthMbps > 0f;
		}

		void ApplyPerHostPathBudget(RemoteHostBudgetDecision decision, float limitKbps)
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

		float EstimateDecisionMediaKbps(RemoteHostBudgetDecision decision)
		{
			return EstimateVideoLodBitrateKbps(decision.nextVideoLod) + EstimateAudioLodBitrateKbps(decision.nextAudioLod);
		}

		bool TryLowerVideoForDecision(RemoteHostBudgetDecision decision)
		{
			float currentKbps = EstimateVideoLodBitrateKbps(decision.nextVideoLod);
			if (currentKbps <= 0f)
				return false;

			int nextLod = StepVideoLodDownToward(decision.nextVideoLod, PortailStreamLodUtility.Off);
			float savingKbps = currentKbps - EstimateVideoLodBitrateKbps(nextLod);
			if (savingKbps <= 0.1f)
				return false;

			decision.nextVideoLod = nextLod;
			return true;
		}

		bool TryLowerAudioForDecision(RemoteHostBudgetDecision decision)
		{
			float currentKbps = EstimateAudioLodBitrateKbps(decision.nextAudioLod);
			if (currentKbps <= 0f)
				return false;

			int nextLod = StepAudioLodDown(decision.nextAudioLod);
			float savingKbps = currentKbps - EstimateAudioLodBitrateKbps(nextLod);
			if (savingKbps <= 0.1f)
				return false;

			decision.nextAudioLod = nextLod;
			return true;
		}

		static bool IsSdrConnectionPath(int connectionPath)
		{
			return connectionPath == ConnectionPathSdr;
		}

		void ApplyDownloadBudget(List<RemoteHostBudgetDecision> decisions)
		{
			float limitKbps = Mathf.Max(0f, maxTotalDownloadBandwidthMbps) * 1000f;
			if (limitKbps <= 0f)
				return;

			int guard = Mathf.Max(1, decisions.Count) * (CountUsableVideoLods() + CountUsableAudioLods() + 4);
			while (guard-- > 0 && EstimateTotalDownloadKbps(decisions) > limitKbps + 0.1f)
			{
				if (TryLowerLowestPriorityVideo(decisions))
					continue;

				if (TryLowerLowestPriorityAudio(decisions))
					continue;

				break;
			}
		}

		float EstimateTotalDownloadKbps(List<RemoteHostBudgetDecision> decisions)
		{
			float total = 0f;
			for (int i = 0; i < decisions.Count; ++i)
			{
				total += EstimateVideoLodBitrateKbps(decisions[i].nextVideoLod);
				total += EstimateAudioLodBitrateKbps(decisions[i].nextAudioLod);
			}

			return total;
		}

		bool TryLowerLowestPriorityVideo(List<RemoteHostBudgetDecision> decisions)
		{
			int bestIndex = -1;
			int bestNextLod = PortailStreamLodUtility.Off;
			float bestPriority = float.MaxValue;
			float bestSavingKbps = -1f;

			for (int i = 0; i < decisions.Count; ++i)
			{
				RemoteHostBudgetDecision decision = decisions[i];
				float currentKbps = EstimateVideoLodBitrateKbps(decision.nextVideoLod);
				if (currentKbps <= 0f)
					continue;

				int nextLod = StepVideoLodDownToward(decision.nextVideoLod, PortailStreamLodUtility.Off);
				float savingKbps = currentKbps - EstimateVideoLodBitrateKbps(nextLod);
				if (savingKbps <= 0.1f)
					continue;

				float priority = Mathf.Max(0f, decision.projectedPixelArea);
				if (priority < bestPriority ||
					(Mathf.Approximately(priority, bestPriority) && savingKbps > bestSavingKbps))
				{
					bestIndex = i;
					bestNextLod = nextLod;
					bestPriority = priority;
					bestSavingKbps = savingKbps;
				}
			}

			if (bestIndex < 0)
				return false;

			decisions[bestIndex].nextVideoLod = bestNextLod;
			return true;
		}

		bool TryLowerLowestPriorityAudio(List<RemoteHostBudgetDecision> decisions)
		{
			int bestIndex = -1;
			int bestNextLod = PortailStreamLodUtility.Off;
			float bestPriority = float.MaxValue;
			float bestSavingKbps = -1f;

			for (int i = 0; i < decisions.Count; ++i)
			{
				RemoteHostBudgetDecision decision = decisions[i];
				float currentKbps = EstimateAudioLodBitrateKbps(decision.nextAudioLod);
				if (currentKbps <= 0f)
					continue;

				int nextLod = StepAudioLodDown(decision.nextAudioLod);
				float savingKbps = currentKbps - EstimateAudioLodBitrateKbps(nextLod);
				if (savingKbps <= 0.1f)
					continue;

				float priority = Mathf.Max(0f, decision.estimatedAudioVolume);
				if (priority < bestPriority ||
					(Mathf.Approximately(priority, bestPriority) && savingKbps > bestSavingKbps))
				{
					bestIndex = i;
					bestNextLod = nextLod;
					bestPriority = priority;
					bestSavingKbps = savingKbps;
				}
			}

			if (bestIndex < 0)
				return false;

			decisions[bestIndex].nextAudioLod = bestNextLod;
			return true;
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

		float ComputeHighestEstimatedAudioVolumeForHost(ulong hostSteamId)
		{
			if (audioListenerOverride == null || _streamPlayers == null || _streamPlayers.Length == 0)
				return 0f;

			Transform listenerTransform = audioListenerOverride.transform;
			float highestVolume = 0f;
			for (int i = 0; i < _streamPlayers.Length; ++i)
			{
				PortailFeedPlayback player = _streamPlayers[i];
				if (!IsPlayerShowingHost(player, hostSteamId))
					continue;

				float volume = ComputeHighestEstimatedAudioVolumeForPlayer(player, listenerTransform);
				if (volume > highestVolume)
					highestVolume = volume;
			}

			return highestVolume;
		}

		float ComputeHighestEstimatedAudioVolumeForPlayer(PortailFeedPlayback player, Transform listenerTransform)
		{
			if (player == null || listenerTransform == null)
				return 0f;

			_audioSourcesVisited.Clear();
			float highestVolume = 0f;
			EvaluateAudioSourceOnTransform(player.LeftSpeakerTransform, listenerTransform, ref highestVolume);
			EvaluateAudioSourceOnTransform(player.RightSpeakerTransform, listenerTransform, ref highestVolume);
			EvaluateAudioSourceOnTransform(player.transform, listenerTransform, ref highestVolume);

			PortailAudioPlayback[] desktopAudioSources = player.GetComponentsInChildren<PortailAudioPlayback>(false);
			for (int i = 0; i < desktopAudioSources.Length; ++i)
			{
				PortailAudioPlayback desktopAudioSource = desktopAudioSources[i];
				if (desktopAudioSource == null)
					continue;

				EvaluateAudioSource(desktopAudioSource.GetComponent<AudioSource>(), listenerTransform, ref highestVolume);
			}

			return highestVolume;
		}

		void EvaluateAudioSourceOnTransform(Transform sourceTransform, Transform listenerTransform, ref float highestVolume)
		{
			if (sourceTransform == null)
				return;

			EvaluateAudioSource(sourceTransform.GetComponent<AudioSource>(), listenerTransform, ref highestVolume);
		}

		void EvaluateAudioSource(AudioSource source, Transform listenerTransform, ref float highestVolume)
		{
			if (source == null || !_audioSourcesVisited.Add(source))
				return;

			float volume = EstimateAudibleVolume(source, listenerTransform);
			if (volume > highestVolume)
				highestVolume = volume;
		}

		float EstimateAudibleVolume(AudioSource source, Transform listenerTransform)
		{
			if (source == null || listenerTransform == null)
				return 0f;

			if (!source.enabled || !source.gameObject.activeInHierarchy || source.mute)
				return 0f;

			float volume = Mathf.Clamp01(source.volume);
			if (includeGlobalAudioListenerVolume)
				volume *= Mathf.Clamp01(AudioListener.volume);

			if (volume <= 0f)
				return 0f;

			float spatialBlend = Mathf.Clamp01(source.spatialBlend);
			if (spatialBlend <= 0f)
				return volume;

			float distance = Vector3.Distance(listenerTransform.position, source.transform.position);
			float spatialAttenuation = EstimateSpatialAttenuation(source, distance);
			float mixedAttenuation = Mathf.Lerp(1f, spatialAttenuation, spatialBlend);
			return volume * mixedAttenuation;
		}

		float EstimateSpatialAttenuation(AudioSource source, float distance)
		{
			float minDistance = Mathf.Max(0.0001f, source.minDistance);
			float maxDistance = Mathf.Max(minDistance + 0.0001f, source.maxDistance);

			if (distance <= minDistance)
				return 1f;

			if (distance >= maxDistance)
				return 0f;

			switch (source.rolloffMode)
			{
				case AudioRolloffMode.Linear:
					return Mathf.Clamp01(1f - ((distance - minDistance) / (maxDistance - minDistance)));

				case AudioRolloffMode.Custom:
					AnimationCurve customCurve = source.GetCustomCurve(AudioSourceCurveType.CustomRolloff);
					if (customCurve != null && customCurve.length > 0)
					{
						float normalizedDistance = Mathf.Clamp01(distance / maxDistance);
						return Mathf.Clamp01(customCurve.Evaluate(normalizedDistance));
					}
					return Mathf.Clamp01(minDistance / distance);

				case AudioRolloffMode.Logarithmic:
				default:
					return Mathf.Clamp01(minDistance / distance);
			}
		}

		bool IsPlayerShowingHost(PortailFeedPlayback player, ulong hostSteamId)
		{
			if (player == null || !player.isActiveAndEnabled)
				return false;

			PortailFeed feed = player.Feed;
			MirrorPortailParticipant participant = feed != null ? feed.GetComponent<MirrorPortailParticipant>() : null;
			return participant != null && !participant.IsLocalParticipant && participant.OwnerStreamId == hostSteamId;
		}

		void ApplyVideoLod(ulong hostSteamId, RemoteHostState state, int lod)
		{
			lod = PortailStreamLodUtility.NormalizeLod(lod);
			if (state.appliedVideoLod == lod)
				return;

			clientManager.SetMaxAcceptedVideoLod(hostSteamId, lod);
			state.appliedVideoLod = lod;

			if (logQualityChanges)
			{
				Debug.Log($"[PortailStreamQualityController] Host {hostSteamId} video -> {PortailStreamLodUtility.ToVideoLabel(lod)} " +
					$"(visible pixels {state.lastProjectedPixelArea:0}, required bitrate {state.lastRequiredVideoBitrateKbps:0} kbps, " +
					$"ideal {PortailStreamLodUtility.ToVideoLabel(state.lastIdealVideoLod)}, SDR cap {sdrPerHostBandwidthMbps:0.##} Mbps)");
			}
		}

		void ApplyAudioLod(ulong hostSteamId, RemoteHostState state, int lod)
		{
			lod = PortailStreamLodUtility.NormalizeLod(lod);
			if (state.appliedAudioLod == lod)
				return;

			clientManager.SetMaxAcceptedAudioLod(hostSteamId, lod);
			state.appliedAudioLod = lod;

			if (logQualityChanges)
			{
				Debug.Log($"[PortailStreamQualityController] Host {hostSteamId} audio -> {PortailStreamLodUtility.ToAudioLabel(lod)} " +
					$"(estimated volume {state.lastEstimatedAudioVolume:0.000}, threshold {audibleVolumeThreshold:0.000}, SDR cap {sdrPerHostBandwidthMbps:0.##} Mbps)");
			}
		}

		void RestoreManagedRemoteQualities()
		{
			if (clientManager == null)
				clientManager = MirrorPortailStreamReceiver.Instance;

			if (clientManager == null)
				return;

			foreach (KeyValuePair<ulong, RemoteHostState> pair in _states)
				RestoreRemoteQuality(pair.Key, pair.Value);
		}

		void RestoreRemoteQuality(ulong hostSteamId, RemoteHostState state)
		{
			if (clientManager == null || hostSteamId == 0 || state == null || !state.capturedRestoreLods)
				return;

			clientManager.SetMaxAcceptedVideoLod(hostSteamId, state.restoreVideoLod);
			clientManager.SetMaxAcceptedAudioLod(hostSteamId, state.restoreAudioLod);

			if (logQualityChanges)
			{
				Debug.Log($"[PortailStreamQualityController] Host {hostSteamId} restored to " +
					$"{PortailStreamLodUtility.ToVideoLabel(state.restoreVideoLod)} / {PortailStreamLodUtility.ToAudioLabel(state.restoreAudioLod)}");
			}
		}
	}
}
