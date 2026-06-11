using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Runtime.InteropServices;
using System.Text;
using Portail.Core;
using UnityEngine;
using UnityEngine.Experimental.Rendering;

namespace Portail.Capture
{
	[DefaultExecutionOrder(-9600)]
	[DisallowMultipleComponent]
	public sealed class PortailCaptureSession : MonoBehaviour, IPortailCaptureTarget
	{
		private delegate bool EnumWindowsProc(IntPtr hWnd, IntPtr lParam);
		private delegate bool MonitorEnumProc(IntPtr hMonitor, IntPtr hdcMonitor, ref RECT lprcMonitor, IntPtr dwData);

		public readonly struct WindowEntry
		{
			public WindowEntry(ulong handle, string title)
			{
				Handle = handle;
				Title = title;
			}

			public ulong Handle { get; }
			public string Title { get; }
		}

		public readonly struct DisplayEntry
		{
			public DisplayEntry(ulong handle, string label, int width, int height, bool isPrimary)
			{
				Handle = handle;
				Label = label;
				Width = width;
				Height = height;
				IsPrimary = isPrimary;
			}

			public ulong Handle { get; }
			public string Label { get; }
			public int Width { get; }
			public int Height { get; }
			public bool IsPrimary { get; }
		}

		[StructLayout(LayoutKind.Sequential)]
		private struct RECT
		{
			public int left;
			public int top;
			public int right;
			public int bottom;
		}

		[StructLayout(LayoutKind.Sequential)]
		private struct POINT
		{
			public int x;
			public int y;
		}

		[StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
		private struct MONITORINFOEX
		{
			public int cbSize;
			public RECT rcMonitor;
			public RECT rcWork;
			public int dwFlags;
			[MarshalAs(UnmanagedType.ByValTStr, SizeConst = 32)]
			public string szDevice;
		}

		private const int MONITORINFOF_PRIMARY = 1;

		[DllImport("user32.dll")]
		private static extern bool EnumWindows(EnumWindowsProc lpEnumFunc, IntPtr lParam);

		[DllImport("user32.dll")]
		private static extern bool EnumDisplayMonitors(IntPtr hdc, IntPtr lprcClip, MonitorEnumProc lpfnEnum, IntPtr dwData);

		[DllImport("user32.dll", CharSet = CharSet.Unicode)]
		private static extern bool GetMonitorInfo(IntPtr hMonitor, ref MONITORINFOEX lpmi);

		[DllImport("user32.dll")]
		private static extern bool IsWindowVisible(IntPtr hWnd);

		[DllImport("user32.dll")]
		private static extern bool IsWindow(IntPtr hWnd);

		[DllImport("user32.dll", CharSet = CharSet.Unicode)]
		private static extern int GetWindowTextLength(IntPtr hWnd);

		[DllImport("user32.dll", CharSet = CharSet.Unicode)]
		private static extern int GetWindowText(IntPtr hWnd, StringBuilder lpString, int nMaxCount);

		[DllImport("user32.dll")]
		private static extern IntPtr GetShellWindow();

		[DllImport("user32.dll")]
		private static extern bool GetClientRect(IntPtr hWnd, out RECT lpRect);

		[DllImport("user32.dll")]
		private static extern bool ClientToScreen(IntPtr hWnd, ref POINT lpPoint);

		[DllImport("user32.dll")]
		private static extern IntPtr GetActiveWindow();

		[DllImport("user32.dll")]
		private static extern uint GetWindowThreadProcessId(IntPtr hWnd, out uint processId);

		[DllImport("user32.dll", SetLastError = true)]
		[return: MarshalAs(UnmanagedType.Bool)]
		private static extern bool SetWindowDisplayAffinity(IntPtr hWnd, uint dwAffinity);

		[Header("Lifecycle")]
		[Tooltip("Controls which native plugin logs are forwarded to the Unity console. Native logging stays registered so errors can still be forwarded when this is set to Errors.")]
		public PortailCaptureNativeConsoleLogLevel nativeConsoleLogLevel = PortailCaptureNativeConsoleLogLevel.All;
		public bool enableAudio = true;
		public bool streamerMode = false;

		[Header("Local Audio Latency")]
		[Tooltip("Number of interleaved samples requested from the native local audio ring per read. Lower values can reduce native backlog latency, but require more reads and increase underrun risk.")]
		public int localAudioReadScratchSamples = PortailAudioLatencySettings.DefaultDirectReadScratchSamples;
		[Tooltip("Maximum native audio reads the stream player can perform each frame. Higher values drain bursts faster at the cost of more main-thread work.")]
		public int localAudioMaxReadsPerFrame = PortailAudioLatencySettings.DefaultMaxDirectReadsPerFrame;
		[Tooltip("Hard cap for queued speaker audio in milliseconds. Lower values reduce latency but drop older samples sooner during stalls.")]
		public int localAudioMaxQueuedLatencyMs = PortailAudioLatencySettings.DefaultMaxQueuedLatencyMs;
		[Tooltip("Optional queue target in milliseconds. Set to 0 to disable active trimming; low values reduce latency but can cause audible discontinuities.")]
		public int localAudioTargetQueuedLatencyMs = PortailAudioLatencySettings.DefaultTargetQueuedLatencyMs;

		[Header("Texture")]
		public int fallbackTextureWidth = 1280;
		public int fallbackTextureHeight = 720;

		[Header("Refresh")]
		public float windowListRefreshSeconds = 1.0f;
		[Min(0.05f)]
		public float statsRefreshSeconds = 0.5f;

		public static PortailCaptureSession Instance { get; private set; }

		private readonly List<DisplayEntry> _availableDisplays = new List<DisplayEntry>();
		private readonly List<WindowEntry> _availableWindows = new List<WindowEntry>();
		private readonly List<DisplayEntry> _displayRefreshScratch = new List<DisplayEntry>();
		private readonly List<WindowEntry> _windowRefreshScratch = new List<WindowEntry>();

		private PortailCapturePlugin _capture;
		private RenderTexture _captureTexture;
		private float _nextWindowRefreshAt;
		private float _nextStatsRefreshAt;
		private ulong _selectedWindowHandle;
		private ulong _selectedDisplayHandle;
		private ulong _lastAppliedWindowHandle;
		private PortailCaptureStats _captureStats;
		private string _captureAudioError = string.Empty;
		private ulong _lastCaptureRateFrameCount;
		private float _lastCaptureRateSampleTime;
		private float _currentCaptureFps;
		private bool _lastAppliedCaptureEnableAudio;
		private PortailCaptureMode _lastAppliedCaptureMode;
		private bool _initialized;
		private bool _captureDisplaySelected;
		[SerializeField] private PortailFeed outputFeed;

		public bool IsCapturing => _capture != null && _capture.IsRunning();
		public bool IsDisplayCaptureSelected => _captureDisplaySelected;
		public ulong SelectedWindowHandle => _selectedWindowHandle;
		public ulong SelectedDisplayHandle => _selectedDisplayHandle;
		public Texture CaptureTexture => IsCapturing ? _captureTexture : null;
		public PortailCaptureStats CaptureStats => _captureStats;
		public PortailFeed CurrentFeed => outputFeed;
		public string CaptureAudioError => _captureAudioError;
		public float CurrentCaptureFps => _currentCaptureFps;
		public string CurrentCaptureSelectionLabel => BuildCaptureSelectionLabel();
		public float CurrentRawVideoBitrateKbps
		{
			get
			{
				int width = _captureStats.preview_width > 0 ? _captureStats.preview_width : _captureTexture != null ? _captureTexture.width : Mathf.Max(16, fallbackTextureWidth);
				int height = _captureStats.preview_height > 0 ? _captureStats.preview_height : _captureTexture != null ? _captureTexture.height : Mathf.Max(16, fallbackTextureHeight);
				return Mathf.Max(0f, _currentCaptureFps) * width * height * 4f * 8f / 1000f;
			}
		}
		public string CaptureAudioStatusText => BuildAudioOverlayText();
		public IReadOnlyList<DisplayEntry> AvailableDisplays => _availableDisplays;
		public IReadOnlyList<WindowEntry> AvailableWindows => _availableWindows;

		private const uint WDA_NONE = 0x00000000;
		private const uint WDA_EXCLUDEFROMCAPTURE = 0x00000011;
		private IntPtr _applicationWindow;
		private bool _applicationExcludedFromCapture;

		private void Awake()
		{
			if (Instance != null && Instance != this)
			{
				Destroy(this);
				return;
			}

			_applicationWindow = FindOwnProcessWindow();

			// Restore exclude from capture state in case it saved window settings from previous session
			if (IsValidWindow(_applicationWindow))
			{
				SetWindowDisplayAffinity(_applicationWindow, WDA_NONE);
				UnityEngine.Debug.Log("Awake Restored application capture affinity.");
			}
			_applicationExcludedFromCapture = false;
		}

		private void OnEnable()
		{
			if (Instance != null && Instance != this)
			{
				enabled = false;
				return;
			}

			Instance = this;
			PortailCaptureTargetRegistry.Current = this;
			if (_initialized)
			{
				return;
			}

			RefreshAvailableCaptureTargets();
			_initialized = true;
		}

		private void OnDisable()
		{
			if (Instance == this)
			{
				Instance = null;
			}

			if (PortailCaptureTargetRegistry.Current == this)
				PortailCaptureTargetRegistry.Current = null;

			ClearOutputFeed();
			StopCapture();
			RestoreApplicationCaptureExclusion();
			if (_capture != null)
			{
				_capture.enabled = false;
			}
		}

		private void OnDestroy()
		{
			if (Instance == this)
			{
				Instance = null;
			}

			if (PortailCaptureTargetRegistry.Current == this)
				PortailCaptureTargetRegistry.Current = null;

			ClearOutputFeed();
			StopCapture();
			RestoreApplicationCaptureExclusion();
			ReleaseTexture(_captureTexture);
			_captureTexture = null;
		}

		private void Update()
		{
			EnsurePlugin();

			float now = Time.unscaledTime;
			if (now >= _nextWindowRefreshAt)
			{
				_nextWindowRefreshAt = now + Mathf.Max(0.25f, windowListRefreshSeconds);
				RefreshAvailableCaptureTargets();
			}

			EnsureCaptureRunning();

			if (IsCapturing && now >= _nextStatsRefreshAt)
			{
				_nextStatsRefreshAt = now + Mathf.Max(0.05f, statsRefreshSeconds);
				PullStats(now);
			}

			UpdateOutputFeed();

			if (!_capture.IsRunning() && _applicationExcludedFromCapture == true)
				RestoreApplicationCaptureExclusion();
		}

		public void RefreshAvailableCaptureTargets()
		{
			RefreshAvailableWindows();
		}

		public void SetCaptureWindowHandle(ulong windowHandle)
		{
			if (windowHandle != 0)
			{
				IntPtr nativeHandle = new IntPtr(unchecked((long)windowHandle));
				if (!IsWindow(nativeHandle))
				{
					UnityEngine.Debug.LogWarning($"[PortailCapture] Ignoring invalid window handle {FormatWindowHandle(windowHandle)}.");
					return;
				}
			}

			_captureDisplaySelected = false;
			_selectedDisplayHandle = 0;
			_selectedWindowHandle = windowHandle;
		}

		public void SetCaptureDisplay(ulong displayHandle)
		{
			if (displayHandle == 0 || !TryGetDisplayBounds(displayHandle, out _))
			{
				UnityEngine.Debug.LogWarning($"[PortailCapture] Ignoring invalid display handle {FormatHandle(displayHandle)}.");
				return;
			}

			_captureDisplaySelected = true;
			_selectedDisplayHandle = displayHandle;
			_selectedWindowHandle = 0;
		}

		public void SetCaptureAudioEnabled(bool enabled)
		{
			enableAudio = enabled;
		}

		public void SetFallbackTextureSize(int width, int height)
		{
			fallbackTextureWidth = Mathf.Max(16, width);
			fallbackTextureHeight = Mathf.Max(16, height);
		}

		public void SetOutputFeed(PortailFeed feed)
		{
			if (outputFeed == feed)
				return;

			ClearOutputFeed();
			outputFeed = feed;
			UpdateOutputFeed();
		}

		public void ClearOutputFeed(PortailFeed feed)
		{
			if (outputFeed != feed)
				return;

			ClearOutputFeed();
			outputFeed = null;
		}

		public bool TryGetSelectedCaptureScreenRect(out RectInt screenRect)
		{
			screenRect = default;
			if (_captureDisplaySelected)
			{
				if (!TryGetDisplayBounds(_selectedDisplayHandle, out RECT monitorRect))
				{
					return false;
				}

				screenRect = RectFromNativeRect(monitorRect);
				return screenRect.width > 0 && screenRect.height > 0;
			}

			if (_selectedWindowHandle == 0)
			{
				return false;
			}

			IntPtr nativeHandle = new IntPtr(unchecked((long)_selectedWindowHandle));
			if (!IsWindow(nativeHandle) || !GetClientRect(nativeHandle, out RECT clientRect))
			{
				return false;
			}

			POINT topLeft = new POINT { x = clientRect.left, y = clientRect.top };
			POINT bottomRight = new POINT { x = clientRect.right, y = clientRect.bottom };
			if (!ClientToScreen(nativeHandle, ref topLeft) || !ClientToScreen(nativeHandle, ref bottomRight))
			{
				return false;
			}

			screenRect = new RectInt(topLeft.x, topLeft.y, bottomRight.x - topLeft.x, bottomRight.y - topLeft.y);
			return screenRect.width > 0 && screenRect.height > 0;
		}

		public int ReadCapturedAudio(float[] samples)
		{
			PortailCapturePlugin capture = _capture;
			if (samples == null || samples.Length == 0 || capture == null || !capture.IsRunning())
			{
				return 0;
			}

			return capture.ReadLocalAudio(samples);
		}

		public string BuildOverlayText(string routeLabel, float bitrateKbps, int fps)
		{
			if (!_captureDisplaySelected && _selectedWindowHandle == 0)
			{
				return "Select a window";
			}

			if (!IsCapturing)
			{
				return "Starting...";
			}

			string route = string.IsNullOrWhiteSpace(routeLabel) ? "local" : routeLabel;
			int width = _captureStats.preview_width > 0 ? _captureStats.preview_width : _captureTexture != null ? _captureTexture.width : Mathf.Max(16, fallbackTextureWidth);
			int height = _captureStats.preview_height > 0 ? _captureStats.preview_height : _captureTexture != null ? _captureTexture.height : Mathf.Max(16, fallbackTextureHeight);
			int displayFps = Mathf.Max(0, fps);
			return
				$"{route}\n" +
				$"{Mathf.Max(0f, bitrateKbps):0} kbps {width}x{height} {(displayFps > 0 ? displayFps.ToString() : "-")} fps\n" +
				BuildAudioOverlayText();
		}

		private void EnsurePlugin()
		{
			if (_capture == null)
			{
				_capture = GetComponent<PortailCapturePlugin>();
				if (_capture == null)
				{
					_capture = gameObject.AddComponent<PortailCapturePlugin>();
				}
				_capture.autoStart = false;
			}

			_capture.enabled = true;
			ApplyNativeConsoleLoggingSettings();
		}

		private void EnsureCaptureRunning()
		{
			if (_capture == null)
			{
				return;
			}

			PortailCaptureMode captureMode = GetCaptureMode();
			bool shouldRun = _captureDisplaySelected || _selectedWindowHandle != 0;
			ulong captureTargetHandle = GetCaptureTargetHandle();
			bool configChanged = _capture.IsRunning() &&
				(_lastAppliedCaptureEnableAudio != enableAudio ||
				_lastAppliedCaptureMode != captureMode ||
				_lastAppliedWindowHandle != captureTargetHandle);

			if (!shouldRun)
			{
				StopCapture();
				return;
			}

			if (_capture.IsRunning() && configChanged)
			{
				StopCapture();
			}

			if (!EnsureCaptureTexture())
			{
				return;
			}

			_capture.windowHandle = captureTargetHandle;
			_capture.captureMode = captureMode;
			_capture.enableAudio = enableAudio;
			_capture.captureCursor = false;
			_capture.previewTexture = _captureTexture;
			ApplyNativeConsoleLoggingSettings();

			if (!_capture.IsRunning())
			{
				if (!_capture.StartCapture())
				{
					return;
				}
				_lastAppliedWindowHandle = captureTargetHandle;
				_lastAppliedCaptureEnableAudio = enableAudio;
				_lastAppliedCaptureMode = captureMode;
				return;
			}

			if (captureMode == PortailCaptureMode.Window && _lastAppliedWindowHandle != _selectedWindowHandle)
			{
				_capture.SetWindowHandle(_selectedWindowHandle);
				_lastAppliedWindowHandle = _selectedWindowHandle;
			}

			if (captureMode == PortailCaptureMode.Display && _applicationExcludedFromCapture == false)
				ApplyApplicationCaptureExclusion();
			if (captureMode == PortailCaptureMode.Window && _applicationExcludedFromCapture == true)
				RestoreApplicationCaptureExclusion();
		}

		private void ApplyNativeConsoleLoggingSettings()
		{
			PortailCaptureNativeLogBridge.ConsoleLogLevel = nativeConsoleLogLevel;

			if (_capture != null)
			{
				// Keep the native callback registered for every filter mode.
				// The bridge decides which native messages reach the Unity console.
				_capture.enableNativeLogging = true;
			}
		}

		private bool EnsureCaptureTexture()
		{
			int targetWidth = Mathf.Max(16, fallbackTextureWidth);
			int targetHeight = Mathf.Max(16, fallbackTextureHeight);

			if (_captureDisplaySelected)
			{
				if (!TryGetDisplayBounds(_selectedDisplayHandle, out RECT monitorRect))
				{
					_captureDisplaySelected = false;
					_selectedDisplayHandle = 0;
					return false;
				}

				targetWidth = Mathf.Max(16, monitorRect.right - monitorRect.left);
				targetHeight = Mathf.Max(16, monitorRect.bottom - monitorRect.top);
			}
			else if (_selectedWindowHandle != 0)
			{
				IntPtr nativeHandle = new IntPtr(unchecked((long)_selectedWindowHandle));
				if (!IsWindow(nativeHandle))
				{
					_selectedWindowHandle = 0;
					return false;
				}

				if (GetClientRect(nativeHandle, out RECT rect))
				{
					int clientWidth = rect.right - rect.left;
					int clientHeight = rect.bottom - rect.top;
					if (clientWidth > 0 && clientHeight > 0)
					{
						targetWidth = Mathf.Max(16, clientWidth);
						targetHeight = Mathf.Max(16, clientHeight);
					}
				}
			}

			if (_captureTexture != null &&
				_captureTexture.width == targetWidth &&
				_captureTexture.height == targetHeight)
			{
				return true;
			}

			ReleaseTexture(_captureTexture);
			_captureTexture = CreatePreviewTexture("Portail_Capture", targetWidth, targetHeight);
			if (_capture != null)
			{
				_capture.previewTexture = _captureTexture;
			}
			return _captureTexture != null;
		}

		private void PullStats(float now)
		{
			if (_capture != null && _capture.IsRunning())
			{
				_captureStats = _capture.GetStats();
				_captureAudioError = _captureStats.audio_capture_state < 0 ? _capture.GetLastAudioError() : string.Empty;
				UpdateCaptureRateSample(now, _captureStats.capture_frames);
				return;
			}

			_captureStats = default;
			_captureAudioError = string.Empty;
			_currentCaptureFps = 0f;
			_lastCaptureRateFrameCount = 0;
			_lastCaptureRateSampleTime = 0f;
		}

		private void UpdateCaptureRateSample(float now, ulong frameCount)
		{
			if (_lastCaptureRateSampleTime <= 0f)
			{
				_lastCaptureRateSampleTime = now;
				_lastCaptureRateFrameCount = frameCount;
				_currentCaptureFps = 0f;
				return;
			}

			float deltaTime = now - _lastCaptureRateSampleTime;
			if (deltaTime <= 0.0001f)
				return;

			if (frameCount < _lastCaptureRateFrameCount)
			{
				_lastCaptureRateFrameCount = frameCount;
				_lastCaptureRateSampleTime = now;
				_currentCaptureFps = 0f;
				return;
			}

			ulong deltaFrames = frameCount - _lastCaptureRateFrameCount;
			if (deltaFrames == 0)
			{
				if (deltaTime >= 1.25f)
				{
					_currentCaptureFps = 0f;
					_lastCaptureRateSampleTime = now;
				}
				return;
			}

			_currentCaptureFps = deltaFrames / deltaTime;
			_lastCaptureRateFrameCount = frameCount;
			_lastCaptureRateSampleTime = now;
		}

		private void UpdateOutputFeed()
		{
			if (outputFeed == null)
				return;

			bool isCapturing = IsCapturing;
			outputFeed.SetAudioLatencySettings(BuildLocalAudioLatencySettings());
			outputFeed.SetDirectAudioReader(isCapturing ? ReadCapturedAudio : null);

			if (!isCapturing)
			{
				outputFeed.ClearDisplayOutput();
				return;
			}

			outputFeed.SetDisplayOutput(
				CaptureTexture,
				BuildLocalStreamDisplayInfo(),
				string.Empty);
		}

		private PortailAudioLatencySettings BuildLocalAudioLatencySettings()
		{
			PortailAudioLatencySettings settings = PortailAudioLatencySettings.Default;
			settings.directReadScratchSamples = localAudioReadScratchSamples;
			settings.maxDirectReadsPerFrame = localAudioMaxReadsPerFrame;
			settings.maxQueuedLatencyMs = localAudioMaxQueuedLatencyMs;
			settings.targetQueuedLatencyMs = localAudioTargetQueuedLatencyMs;
			settings.Normalize();
			return settings;
		}

		private PortailFeedInfo BuildLocalStreamDisplayInfo()
		{
			PortailFeedInfo info = default;
			if (!_captureDisplaySelected && _selectedWindowHandle == 0)
			{
				return info;
			}

			info.isLocalSource = true;
			info.width = _captureStats.preview_width > 0 ? _captureStats.preview_width : _captureTexture != null ? _captureTexture.width : Mathf.Max(16, fallbackTextureWidth);
			info.height = _captureStats.preview_height > 0 ? _captureStats.preview_height : _captureTexture != null ? _captureTexture.height : Mathf.Max(16, fallbackTextureHeight);
			info.fps = _currentCaptureFps;

			if (!IsCapturing)
			{
				return info;
			}

			info.hasSignal = true;
			info.pathLabel = _captureDisplaySelected ? "display" : "local";
			info.bitrateKbps = CurrentRawVideoBitrateKbps;
			return info;
		}

		private void ClearOutputFeed()
		{
			if (outputFeed == null)
				return;

			outputFeed.SetDirectAudioReader(null);
			outputFeed.ClearDisplayOutput();
		}

		private void StopCapture()
		{
			if (_capture != null && _capture.IsRunning())
			{
				_capture.StopCapture();
			}

			_captureStats = default;
			_captureAudioError = string.Empty;
			_currentCaptureFps = 0f;
			_lastCaptureRateFrameCount = 0;
			_lastCaptureRateSampleTime = 0f;
			_lastAppliedWindowHandle = 0;
			_lastAppliedCaptureEnableAudio = false;
			_lastAppliedCaptureMode = PortailCaptureMode.Window;
		}

		private string BuildAudioOverlayText()
		{
			if (!IsCapturing)
			{
				return "audio idle";
			}

			if (!enableAudio)
			{
				return "audio off";
			}

			if (_captureStats.audio_capture_state < 0)
			{
				string error = string.IsNullOrWhiteSpace(_captureAudioError) ? "failed" : _captureAudioError;
				return $"audio error: {error}";
			}

			if (_captureStats.audio_capture_state == 0)
			{
				return $"audio starting pid {_captureStats.audio_target_pid}";
			}

			string target = _captureDisplaySelected ? "Steam Streaming Speakers" : $"pid {_captureStats.audio_target_pid}";
			return
				$"audio {target} " +
				$"samples {_captureStats.local_audio_samples_read}";
		}

		private PortailCaptureMode GetCaptureMode()
		{
			return _captureDisplaySelected ? PortailCaptureMode.Display : PortailCaptureMode.Window;
		}

		private ulong GetCaptureTargetHandle()
		{
			return _captureDisplaySelected ? _selectedDisplayHandle : _selectedWindowHandle;
		}

		private void RefreshAvailableWindows()
		{
			RefreshAvailableDisplays();

			IntPtr shellWindow = GetShellWindow();
			_windowRefreshScratch.Clear();
			StringBuilder titleBuilder = new StringBuilder(256);

			EnumWindows((hWnd, _) =>
			{
				if (hWnd == IntPtr.Zero || hWnd == shellWindow || !IsWindowVisible(hWnd))
				{
					return true;
				}

				int length = GetWindowTextLength(hWnd);
				if (length <= 0)
				{
					return true;
				}

				titleBuilder.Clear();
				if (titleBuilder.Capacity < length + 1)
				{
					titleBuilder.EnsureCapacity(length + 1);
				}

				if (GetWindowText(hWnd, titleBuilder, titleBuilder.Capacity) <= 0)
				{
					return true;
				}

				string title = titleBuilder.ToString().Trim();
				if (string.IsNullOrWhiteSpace(title))
				{
					return true;
				}

				_windowRefreshScratch.Add(new WindowEntry(unchecked((ulong)hWnd.ToInt64()), title));
				return true;
			}, IntPtr.Zero);

			_windowRefreshScratch.Sort((lhs, rhs) =>
			{
				int byTitle = string.Compare(lhs.Title, rhs.Title, StringComparison.OrdinalIgnoreCase);
				return byTitle != 0 ? byTitle : lhs.Handle.CompareTo(rhs.Handle);
			});

			_availableWindows.Clear();
			_availableWindows.AddRange(_windowRefreshScratch);
		}

		private void RefreshAvailableDisplays()
		{
			_displayRefreshScratch.Clear();
			EnumDisplayMonitors(IntPtr.Zero, IntPtr.Zero, (IntPtr hMonitor, IntPtr hdcMonitor, ref RECT monitorRect, IntPtr userData) =>
			{
				if (!TryGetMonitorInfo(hMonitor, out MONITORINFOEX info))
					return true;

				int width = Mathf.Max(0, info.rcMonitor.right - info.rcMonitor.left);
				int height = Mathf.Max(0, info.rcMonitor.bottom - info.rcMonitor.top);
				if (width <= 0 || height <= 0)
					return true;

				bool isPrimary = (info.dwFlags & MONITORINFOF_PRIMARY) != 0;
				int displayNumber = _displayRefreshScratch.Count + 1;
				string label = BuildDisplayLabel(displayNumber, info, width, height, isPrimary);
				_displayRefreshScratch.Add(new DisplayEntry(unchecked((ulong)hMonitor.ToInt64()), label, width, height, isPrimary));
				return true;
			}, IntPtr.Zero);

			_availableDisplays.Clear();
			_availableDisplays.AddRange(_displayRefreshScratch);

			if (_captureDisplaySelected && !TryGetDisplayBounds(_selectedDisplayHandle, out _))
			{
				_captureDisplaySelected = false;
				_selectedDisplayHandle = 0;
			}
		}

		private static bool TryGetDisplayBounds(ulong displayHandle, out RECT bounds)
		{
			bounds = default;
			if (displayHandle == 0)
				return false;

			IntPtr monitor = new IntPtr(unchecked((long)displayHandle));
			if (!TryGetMonitorInfo(monitor, out MONITORINFOEX info))
				return false;

			bounds = info.rcMonitor;
			return bounds.right > bounds.left && bounds.bottom > bounds.top;
		}

		private static bool TryGetMonitorInfo(IntPtr monitor, out MONITORINFOEX info)
		{
			info = new MONITORINFOEX
			{
				cbSize = Marshal.SizeOf<MONITORINFOEX>(),
				szDevice = string.Empty,
			};
			return monitor != IntPtr.Zero && GetMonitorInfo(monitor, ref info);
		}

		private static string BuildDisplayLabel(int displayNumber, MONITORINFOEX info, int width, int height, bool isPrimary)
		{
			string deviceName = string.IsNullOrWhiteSpace(info.szDevice) ? string.Empty : $" {info.szDevice}";
			string primary = isPrimary ? " (Primary)" : string.Empty;
			return $"Display {displayNumber}{primary} - {width}x{height}{deviceName}";
		}

		private static RectInt RectFromNativeRect(RECT rect)
		{
			return new RectInt(rect.left, rect.top, rect.right - rect.left, rect.bottom - rect.top);
		}

		private static RenderTexture CreatePreviewTexture(string name, int width, int height)
		{
			int w = Mathf.Max(16, width);
			int h = Mathf.Max(16, height);
			bool useSrgb = QualitySettings.activeColorSpace == ColorSpace.Linear;

			GraphicsFormat preferred = useSrgb
				? GraphicsFormat.B8G8R8A8_SRGB
				: GraphicsFormat.B8G8R8A8_UNorm;
			GraphicsFormat fallback = useSrgb
				? GraphicsFormat.R8G8B8A8_SRGB
				: GraphicsFormat.R8G8B8A8_UNorm;

			GraphicsFormat chosen;
			if (SystemInfo.IsFormatSupported(preferred, FormatUsage.Render))
			{
				chosen = preferred;
			}
			else if (SystemInfo.IsFormatSupported(fallback, FormatUsage.Render))
			{
				chosen = fallback;
			}
			else
			{
				GraphicsFormat linearPreferred = GraphicsFormat.B8G8R8A8_UNorm;
				GraphicsFormat linearFallback = GraphicsFormat.R8G8B8A8_UNorm;
				chosen = SystemInfo.IsFormatSupported(linearPreferred, FormatUsage.Render)
					? linearPreferred
					: linearFallback;
				useSrgb = false;
			}

			RenderTextureDescriptor descriptor = new RenderTextureDescriptor(w, h)
			{
				depthBufferBits = 0,
				msaaSamples = 1,
				graphicsFormat = chosen,
				useMipMap = true,
				autoGenerateMips = false,
				sRGB = useSrgb,
			};

			RenderTexture texture = new RenderTexture(descriptor)
			{
				name = name,
				wrapMode = TextureWrapMode.Clamp,
				filterMode = FilterMode.Trilinear,
			};
			texture.Create();
			return texture;
		}

		private static void ReleaseTexture(RenderTexture texture)
		{
			if (texture == null)
			{
				return;
			}

			texture.Release();
			Destroy(texture);
		}

		private static string FormatWindowHandle(ulong handle)
		{
			return handle == 0 ? "None" : $"0x{handle:X}";
		}

		private static string FormatHandle(ulong handle)
		{
			return handle == 0 ? "None" : $"0x{handle:X}";
		}

		private string BuildCaptureSelectionLabel()
		{
			if (!_captureDisplaySelected)
				return FormatWindowHandle(_selectedWindowHandle);

			for (int i = 0; i < _availableDisplays.Count; ++i)
			{
				DisplayEntry display = _availableDisplays[i];
				if (display.Handle == _selectedDisplayHandle)
					return display.Label;
			}

			return "Display";
		}

		private void ApplyApplicationCaptureExclusion()
		{
#if UNITY_STANDALONE_WIN || UNITY_EDITOR_WIN

			if (_applicationExcludedFromCapture || streamerMode == true)
			{
				return;
			}

			if (SetWindowDisplayAffinity(_applicationWindow, WDA_EXCLUDEFROMCAPTURE))
			{
				_applicationExcludedFromCapture = true;
			}
			else
			{
				UnityEngine.Debug.LogWarning($"[PortailCapture] Failed to exclude application from display capture: {Marshal.GetLastWin32Error()}.");
			}
#endif
		}

		private void RestoreApplicationCaptureExclusion()
		{
#if UNITY_STANDALONE_WIN || UNITY_EDITOR_WIN
			if (!_applicationExcludedFromCapture)
			{
				return;
			}

			if (IsValidWindow(_applicationWindow))
			{
				SetWindowDisplayAffinity(_applicationWindow, WDA_NONE);
			}
			_applicationExcludedFromCapture = false;
#endif
		}

		private static bool IsValidWindow(IntPtr hWnd)
		{
#if UNITY_STANDALONE_WIN || UNITY_EDITOR_WIN
			return hWnd != IntPtr.Zero && IsWindow(hWnd);
#else
			return false;
#endif
		}

#if UNITY_STANDALONE_WIN || UNITY_EDITOR_WIN
		private static IntPtr FindOwnProcessWindow()
		{
			IntPtr mainWindow = Process.GetCurrentProcess().MainWindowHandle;
			if (IsValidWindow(mainWindow))
			{
				return mainWindow;
			}

			IntPtr activeWindow = GetActiveWindow();
			if (IsValidWindow(activeWindow))
			{
				return activeWindow;
			}

			IntPtr foundWindow = IntPtr.Zero;
			uint currentProcessId = unchecked((uint)Process.GetCurrentProcess().Id);
			EnumWindows((window, _) =>
			{
				if (!IsWindowVisible(window))
				{
					return true;
				}

				GetWindowThreadProcessId(window, out uint windowProcessId);
				if (windowProcessId != currentProcessId)
				{
					return true;
				}

				foundWindow = window;
				return false;
			}, IntPtr.Zero);

			return foundWindow;
		}
#endif
	}
}
