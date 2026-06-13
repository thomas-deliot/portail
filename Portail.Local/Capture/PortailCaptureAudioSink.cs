using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;
using Portail.Core;
using UnityEngine;
using Process = System.Diagnostics.Process;

namespace Portail.Capture
{
	[DefaultExecutionOrder(-10000)]
	[DisallowMultipleComponent]
	public sealed class PortailCaptureAudioSink : MonoBehaviour
	{
		private const int StgmRead = 0;
		private const uint CoInitApartmentThreaded = 0x2;
		private const int HResultFalse = 0x00000001;
		private const int HResultNotFound = unchecked((int)0x80070490);
		private const int HResultRpcChangedMode = unchecked((int)0x80010106);
		private const string PortailCaptureSpeakersName = "Steam Streaming Speakers";
		private const string AudioPolicyConfigClassName = "Windows.Media.Internal.AudioPolicyConfig";
		private const string MmdevapiToken = @"\\?\SWD#MMDEVAPI#";
		private const string RenderInterface = "#{e6327cad-dcec-4949-ae8a-991e976a79d2}";
		private const string CaptureInterface = "#{2eef81be-33fa-4800-9670-1cd474972c3f}";

		private static readonly Guid MMDeviceEnumeratorGuid = new Guid("BCDE0395-E52F-467C-8E3D-C4579291692E");
		private static readonly PropertyKey PKeyDeviceFriendlyName = new PropertyKey(new Guid("A45C254E-DF1C-4EFD-8020-67D146A850E0"), 14);
		private static readonly PropertyKey PKeyDeviceInterfaceFriendlyName = new PropertyKey(new Guid("026E516E-B814-414B-83CD-856D6FEF4822"), 2);
		private static readonly PropertyKey PKeyDeviceDeviceDesc = new PropertyKey(new Guid("A45C254E-DF1C-4EFD-8020-67D146A850E0"), 2);

#if UNITY_STANDALONE_WIN || UNITY_EDITOR_WIN
		private const int WH_KEYBOARD_LL = 13;
		private const int WM_KEYDOWN = 0x0100;
		private const int WM_KEYUP = 0x0101;
		private const int WM_SYSKEYDOWN = 0x0104;
		private const int WM_SYSKEYUP = 0x0105;
		private const uint VK_VOLUME_MUTE = 0xAD;
		private const uint VK_VOLUME_DOWN = 0xAE;
		private const uint VK_VOLUME_UP = 0xAF;

		private static readonly Guid EndpointVolumeGuid = new Guid("5CDF2C82-841E-4546-9722-0CF74078229A");
#endif

		[Header("Routing")]
		[SerializeField] private bool routeOnEnable = true;
		[SerializeField] private bool persistAcrossScenes = true;
		[SerializeField] private bool resetUnityAudioAfterPin = true;

		[Header("Volume Hotkeys")]
		[Tooltip("When enabled, hardware volume keys are redirected to the application pinned physical output while the player is controlling the application. In Portail control mode, Windows handles them normally, so they control Steam Streaming Speakers.")]
		[SerializeField] private bool redirectVolumeHotkeysToPinnedOutputInApplicationControl = true;

		[Header("Debug")]
		[SerializeField] private bool refreshNow;

		private readonly Dictionary<ERole, string> _savedDefaultRenderEndpoints = new Dictionary<ERole, string>();
		private bool _comInitializedHere;
		private bool _isActive;
		private bool _gameOutputPinned;
		private bool _audioDeviceResetRequested;
		private bool _gameOutputPinFailureLogged;
		private bool _quitting;
		private string _portailCaptureSpeakersId = string.Empty;
		private string _gameRenderEndpointId = string.Empty;

#if UNITY_STANDALONE_WIN || UNITY_EDITOR_WIN
		private LowLevelKeyboardProc _volumeKeyboardHookProc;
		private IntPtr _volumeKeyboardHook;
		private object _gameEndpointVolumeComObject;
		private IAudioEndpointVolume _gameEndpointVolume;
		private bool _volumeMuteKeyDown;
		private bool _volumeHotkeyFailureLogged;
		private readonly object _gameOutputVolumeEventLock = new object();
		private bool _gameOutputVolumeEventPending;
		private float _pendingGameOutputVolumeScalar = 1f;
		private bool _pendingGameOutputMuted;
#endif

		public bool IsActive => _isActive;
		public event Action<float, bool> GameOutputVolumeChangedByRedirectedHotkey;
#if UNITY_STANDALONE_WIN || UNITY_EDITOR_WIN
		public bool VolumeHotkeyRedirectionHookInstalled => _volumeKeyboardHook != IntPtr.Zero;
#endif
		public int RoutedRoleCount { get; private set; }
		public string LastError { get; private set; } = string.Empty;
		public string GameOutputPinError { get; private set; } = string.Empty;

		private void Awake()
		{
			if (persistAcrossScenes)
			{
				DontDestroyOnLoad(gameObject);
			}
		}

		private void OnEnable()
		{
			if (routeOnEnable)
			{
				Begin();
				ResetUnityAudioOutputIfRequested();
			}
		}

		private void Update()
		{
			DispatchPendingGameOutputVolumeChange();
			UpdateVolumeHotkeyRedirectionHook();

			if (!refreshNow)
			{
				return;
			}

			refreshNow = false;
			Refresh();
			ResetUnityAudioOutputIfRequested();
			UpdateVolumeHotkeyRedirectionHook();
			LogErrorIfNeeded();
		}

		private void OnDisable()
		{
			UninstallVolumeHotkeyRedirectionHook();
			ReleaseGameEndpointVolumeControl();
			Restore();
			ReleaseComIfNeeded();
		}

		private void OnDestroy()
		{
			UninstallVolumeHotkeyRedirectionHook();
			ReleaseGameEndpointVolumeControl();
			Restore();
			ReleaseComIfNeeded();
		}

		private void OnApplicationQuit()
		{
			_quitting = true;
			UninstallVolumeHotkeyRedirectionHook();
			ReleaseGameEndpointVolumeControl();
			Restore();
			ReleaseComIfNeeded();
		}

		public int Begin()
		{
			if (_isActive)
			{
				return RoutedRoleCount;
			}

			LastError = string.Empty;
			RoutedRoleCount = 0;
			_savedDefaultRenderEndpoints.Clear();
			_portailCaptureSpeakersId = string.Empty;
			_gameRenderEndpointId = string.Empty;
			_gameOutputPinned = false;
			_audioDeviceResetRequested = false;
			_gameOutputPinFailureLogged = false;
			GameOutputPinError = string.Empty;

			try
			{
				EnsureComInitialized();

				using (ComScope<IMMDeviceEnumerator> enumerator = CreateDeviceEnumerator())
				{
					_gameRenderEndpointId = GetDefaultRenderEndpointId(enumerator.Value, ERole.Console);
					if (string.IsNullOrWhiteSpace(_gameRenderEndpointId))
					{
						LastError = "Default render endpoint is unavailable.";
						return 0;
					}

					_portailCaptureSpeakersId = FindPortailCaptureSpeakersId(enumerator.Value);
					if (string.IsNullOrWhiteSpace(_portailCaptureSpeakersId))
					{
						LastError = "Steam Streaming Speakers render endpoint was not found.";
						return 0;
					}

					if (string.Equals(_gameRenderEndpointId, _portailCaptureSpeakersId, StringComparison.OrdinalIgnoreCase))
					{
						LastError = "Default render endpoint is already Steam Streaming Speakers.";
						return 0;
					}

					foreach (ERole role in AllRoles)
					{
						string endpointId = GetDefaultRenderEndpointId(enumerator.Value, role);
						if (!string.IsNullOrWhiteSpace(endpointId))
						{
							_savedDefaultRenderEndpoints[role] = endpointId;
						}
					}
				}

				SetDefaultRenderEndpoint(_portailCaptureSpeakersId);

				_isActive = true;
				RoutedRoleCount = _savedDefaultRenderEndpoints.Count;
				LastError = string.Empty;
				Debug.Log("[PortailCaptureAudioSink] Default output routed to Steam Streaming Speakers.");

				TryPinGameOutput(logSuccess: true);
				UpdateVolumeHotkeyRedirectionHook();

				return RoutedRoleCount;
			}
			catch (Exception ex)
			{
				LastError = ex.Message;
				Restore();
				return 0;
			}
		}

		public int Refresh()
		{
			if (!_isActive)
			{
				return Begin();
			}

			try
			{
				EnsureComInitialized();
				using (ComScope<IMMDeviceEnumerator> enumerator = CreateDeviceEnumerator())
				{
					string portailCaptureSpeakersId = FindPortailCaptureSpeakersId(enumerator.Value);
					if (!string.IsNullOrWhiteSpace(portailCaptureSpeakersId))
					{
						_portailCaptureSpeakersId = portailCaptureSpeakersId;
					}
				}

				if (!string.IsNullOrWhiteSpace(_portailCaptureSpeakersId))
				{
					SetDefaultRenderEndpoint(_portailCaptureSpeakersId);
				}
			}
			catch (Exception ex)
			{
				LastError = ex.Message;
				return RoutedRoleCount;
			}

			TryPinGameOutput(logSuccess: false);
			RefreshGameEndpointVolumeControl();
			UpdateVolumeHotkeyRedirectionHook();
			LastError = string.Empty;

			return RoutedRoleCount;
		}

		public bool TryGetGameOutputVolume(out float volumeScalar, out bool muted)
		{
			volumeScalar = 1f;
			muted = false;
#if UNITY_STANDALONE_WIN || UNITY_EDITOR_WIN
			try
			{
				if ((_gameEndpointVolume == null || _gameEndpointVolumeComObject == null) &&
					!TryResolveGameEndpointVolumeControl(logFailures: false))
				{
					return false;
				}

				int result = _gameEndpointVolume.GetMasterVolumeLevelScalar(out volumeScalar);
				if (Failed(result))
				{
					throw new InvalidOperationException($"GetMasterVolumeLevelScalar failed: 0x{result:x8}");
				}

				result = _gameEndpointVolume.GetMute(out muted);
				if (Failed(result))
				{
					throw new InvalidOperationException($"GetMute failed: 0x{result:x8}");
				}

				volumeScalar = Clamp01(volumeScalar);
				return true;
			}
			catch (Exception ex)
			{
				ReleaseGameEndpointVolumeControl();
				if (!_volumeHotkeyFailureLogged)
				{
					_volumeHotkeyFailureLogged = true;
					Debug.LogWarning($"[PortailCaptureAudioSink] application output volume read failed: {ex.Message}");
				}
			}
#endif
			return false;
		}

		public bool TrySetGameOutputVolume(float volumeScalar)
		{
#if UNITY_STANDALONE_WIN || UNITY_EDITOR_WIN
			try
			{
				if ((_gameEndpointVolume == null || _gameEndpointVolumeComObject == null) &&
					!TryResolveGameEndpointVolumeControl(logFailures: true))
				{
					return false;
				}

				float clamped = Clamp01(volumeScalar);
				int result = _gameEndpointVolume.SetMasterVolumeLevelScalar(clamped, IntPtr.Zero);
				if (Failed(result))
				{
					throw new InvalidOperationException($"SetMasterVolumeLevelScalar failed: 0x{result:x8}");
				}

				if (clamped > 0.0001f)
				{
					result = _gameEndpointVolume.SetMute(false, IntPtr.Zero);
					if (Failed(result))
					{
						throw new InvalidOperationException($"SetMute(false) failed: 0x{result:x8}");
					}
				}

				_volumeHotkeyFailureLogged = false;
				return true;
			}
			catch (Exception ex)
			{
				ReleaseGameEndpointVolumeControl();
				if (!_volumeHotkeyFailureLogged)
				{
					_volumeHotkeyFailureLogged = true;
					Debug.LogWarning($"[PortailCaptureAudioSink] application output volume set failed: {ex.Message}");
				}
			}
#endif
			return false;
		}

		public bool TrySetGameOutputMute(bool muted)
		{
#if UNITY_STANDALONE_WIN || UNITY_EDITOR_WIN
			try
			{
				if ((_gameEndpointVolume == null || _gameEndpointVolumeComObject == null) &&
					!TryResolveGameEndpointVolumeControl(logFailures: true))
				{
					return false;
				}

				int result = _gameEndpointVolume.SetMute(muted, IntPtr.Zero);
				if (Failed(result))
				{
					throw new InvalidOperationException($"SetMute failed: 0x{result:x8}");
				}

				_volumeHotkeyFailureLogged = false;
				return true;
			}
			catch (Exception ex)
			{
				ReleaseGameEndpointVolumeControl();
				if (!_volumeHotkeyFailureLogged)
				{
					_volumeHotkeyFailureLogged = true;
					Debug.LogWarning($"[PortailCaptureAudioSink] application output mute set failed: {ex.Message}");
				}
			}
#endif
			return false;
		}

		public void Restore()
		{
			if (!_isActive && _savedDefaultRenderEndpoints.Count == 0)
			{
				RoutedRoleCount = 0;
				UninstallVolumeHotkeyRedirectionHook();
				ReleaseGameEndpointVolumeControl();
				return;
			}

			UninstallVolumeHotkeyRedirectionHook();
			ReleaseGameEndpointVolumeControl();

			try
			{
				EnsureComInitialized();

				int currentProcessId = Process.GetCurrentProcess().Id;
				using (IAudioPolicyConfigEndpoint audioPolicy = CreateAudioPolicyFactory())
				{
					ClearGameRenderEndpoints(audioPolicy, currentProcessId);
				}
			}
			catch (Exception ex)
			{
				LastError = $"application output override clear failed: {ex.Message}";
				Debug.LogWarning($"[PortailCaptureAudioSink] {LastError}");
			}

			try
			{
				foreach (KeyValuePair<ERole, string> pair in _savedDefaultRenderEndpoints)
				{
					SetDefaultRenderEndpoint(pair.Value, pair.Key);
				}

				LastError = string.Empty;
				Debug.Log("[PortailCaptureAudioSink] Default output restored.");
			}
			catch (Exception ex)
			{
				LastError = ex.Message;
				Debug.LogWarning($"[PortailCaptureAudioSink] Default output restore failed: {LastError}");
			}

			_isActive = false;
			RoutedRoleCount = 0;
			_savedDefaultRenderEndpoints.Clear();
			_portailCaptureSpeakersId = string.Empty;
			_gameRenderEndpointId = string.Empty;
			_gameOutputPinned = false;
			_audioDeviceResetRequested = false;
			_gameOutputPinFailureLogged = false;
			GameOutputPinError = string.Empty;
		}

		private void UpdateVolumeHotkeyRedirectionHook()
		{
#if UNITY_STANDALONE_WIN || UNITY_EDITOR_WIN
			if (ShouldInstallVolumeHotkeyRedirectionHook())
			{
				InstallVolumeHotkeyRedirectionHook();
			}
			else
			{
				UninstallVolumeHotkeyRedirectionHook();
				ReleaseGameEndpointVolumeControl();
			}
#endif
		}

		private void RefreshGameEndpointVolumeControl()
		{
#if UNITY_STANDALONE_WIN || UNITY_EDITOR_WIN
			if (_gameEndpointVolume != null && _gameEndpointVolumeComObject != null)
			{
				return;
			}

			TryResolveGameEndpointVolumeControl(logFailures: false);
#endif
		}

		private void UninstallVolumeHotkeyRedirectionHook()
		{
#if UNITY_STANDALONE_WIN || UNITY_EDITOR_WIN
			_volumeMuteKeyDown = false;

			if (_volumeKeyboardHook == IntPtr.Zero)
			{
				return;
			}

			UnhookWindowsHookEx(_volumeKeyboardHook);
			_volumeKeyboardHook = IntPtr.Zero;
			_volumeKeyboardHookProc = null;
#endif
		}

		private void ReleaseGameEndpointVolumeControl()
		{
#if UNITY_STANDALONE_WIN || UNITY_EDITOR_WIN
			_gameEndpointVolume = null;
			ReleaseComObject(_gameEndpointVolumeComObject);
			_gameEndpointVolumeComObject = null;
#endif
		}

		private void DispatchPendingGameOutputVolumeChange()
		{
#if UNITY_STANDALONE_WIN || UNITY_EDITOR_WIN
			bool pending;
			float volumeScalar;
			bool muted;
			lock (_gameOutputVolumeEventLock)
			{
				pending = _gameOutputVolumeEventPending;
				volumeScalar = _pendingGameOutputVolumeScalar;
				muted = _pendingGameOutputMuted;
				_gameOutputVolumeEventPending = false;
			}

			if (pending)
			{
				GameOutputVolumeChangedByRedirectedHotkey?.Invoke(volumeScalar, muted);
			}
#endif
		}

#if UNITY_STANDALONE_WIN || UNITY_EDITOR_WIN
		private bool ShouldInstallVolumeHotkeyRedirectionHook()
		{
			return redirectVolumeHotkeysToPinnedOutputInApplicationControl &&
				_isActive &&
				!string.IsNullOrWhiteSpace(_gameRenderEndpointId);
		}

		private void InstallVolumeHotkeyRedirectionHook()
		{
			if (_volumeKeyboardHook != IntPtr.Zero)
			{
				return;
			}

			TryResolveGameEndpointVolumeControl(logFailures: false);

			_volumeKeyboardHookProc = HandleVolumeHotkeyLowLevelKeyboard;
			string moduleName = Process.GetCurrentProcess().MainModule?.ModuleName;
			IntPtr moduleHandle = string.IsNullOrEmpty(moduleName) ? IntPtr.Zero : GetModuleHandle(moduleName);
			_volumeKeyboardHook = SetWindowsHookEx(WH_KEYBOARD_LL, _volumeKeyboardHookProc, moduleHandle, 0);
			if (_volumeKeyboardHook == IntPtr.Zero)
			{
				Debug.LogWarning($"[PortailCaptureAudioSink] Volume hotkey hook install failed: {Marshal.GetLastWin32Error()}.");
				_volumeKeyboardHookProc = null;
			}
		}

		private bool TryResolveGameEndpointVolumeControl(bool logFailures)
		{
			if (_gameEndpointVolume != null && _gameEndpointVolumeComObject != null)
			{
				return true;
			}

			if (string.IsNullOrWhiteSpace(_gameRenderEndpointId))
			{
				return false;
			}

			try
			{
				EnsureComInitialized();

				using (ComScope<IMMDeviceEnumerator> enumerator = CreateDeviceEnumerator())
				{
					IMMDevice device = null;
					object endpointVolumeObject = null;
					try
					{
						int result = enumerator.Value.GetDevice(_gameRenderEndpointId, out device);
						if (Failed(result) || device == null)
						{
							throw new InvalidOperationException($"application physical output endpoint was not found: 0x{result:x8}");
						}

						Guid iid = EndpointVolumeGuid;
						result = device.Activate(ref iid, ClsCtx.All, IntPtr.Zero, out endpointVolumeObject);
						if (Failed(result) || endpointVolumeObject == null)
						{
							throw new InvalidOperationException($"IAudioEndpointVolume activation failed: 0x{result:x8}");
						}

						_gameEndpointVolumeComObject = endpointVolumeObject;
						_gameEndpointVolume = (IAudioEndpointVolume)endpointVolumeObject;
						endpointVolumeObject = null;
						_volumeHotkeyFailureLogged = false;
						return true;
					}
					finally
					{
						ReleaseComObject(endpointVolumeObject);
						ReleaseComObject(device);
					}
				}
			}
			catch (Exception ex)
			{
				ReleaseGameEndpointVolumeControl();
				if (logFailures && !_volumeHotkeyFailureLogged)
				{
					_volumeHotkeyFailureLogged = true;
					Debug.LogWarning($"[PortailCaptureAudioSink] Volume hotkey redirection unavailable: {ex.Message}");
				}
				return false;
			}
		}

		private IntPtr HandleVolumeHotkeyLowLevelKeyboard(int nCode, IntPtr wParam, IntPtr lParam)
		{
			if (nCode >= 0)
			{
				int message = wParam.ToInt32();
				bool keyDown = message == WM_KEYDOWN || message == WM_SYSKEYDOWN;
				bool keyUp = message == WM_KEYUP || message == WM_SYSKEYUP;
				if (keyDown || keyUp)
				{
					KBDLLHOOKSTRUCT keyboard = Marshal.PtrToStructure<KBDLLHOOKSTRUCT>(lParam);
					if (IsVolumeHotkey(keyboard.vkCode))
					{
						if (!ShouldRedirectVolumeHotkeyNow())
						{
							if (keyboard.vkCode == VK_VOLUME_MUTE && keyUp)
							{
								_volumeMuteKeyDown = false;
							}
							return CallNextHookEx(_volumeKeyboardHook, nCode, wParam, lParam);
						}

						if (keyDown)
						{
							ApplyRedirectedVolumeHotkey(keyboard.vkCode);
						}
						else if (keyboard.vkCode == VK_VOLUME_MUTE)
						{
							_volumeMuteKeyDown = false;
						}

						return new IntPtr(1);
					}
				}
			}

			return CallNextHookEx(_volumeKeyboardHook, nCode, wParam, lParam);
		}

		private bool ShouldRedirectVolumeHotkeyNow()
		{
			return redirectVolumeHotkeysToPinnedOutputInApplicationControl &&
				_isActive &&
				!PortailControlState.IsDesktopControlActive &&
				IsApplicationForegroundProcess() &&
				(_gameEndpointVolume != null || TryResolveGameEndpointVolumeControl(logFailures: true));
		}

		private void ApplyRedirectedVolumeHotkey(uint vkCode)
		{
			if (_gameEndpointVolume == null)
			{
				return;
			}

			try
			{
				int result = 0;
				bool changed = false;
				if (vkCode == VK_VOLUME_UP)
				{
					result = _gameEndpointVolume.VolumeStepUp(IntPtr.Zero);
					changed = true;
				}
				else if (vkCode == VK_VOLUME_DOWN)
				{
					result = _gameEndpointVolume.VolumeStepDown(IntPtr.Zero);
					changed = true;
				}
				else if (vkCode == VK_VOLUME_MUTE)
				{
					if (_volumeMuteKeyDown)
					{
						return;
					}

					result = _gameEndpointVolume.GetMute(out bool muted);
					if (!Failed(result))
					{
						result = _gameEndpointVolume.SetMute(!muted, IntPtr.Zero);
					}
					_volumeMuteKeyDown = true;
					changed = true;
				}

				if (Failed(result))
				{
					throw new InvalidOperationException($"IAudioEndpointVolume command failed: 0x{result:x8}");
				}

				if (changed)
				{
					QueueGameOutputVolumeChangedByRedirectedHotkey();
				}
			}
			catch (Exception ex)
			{
				ReleaseGameEndpointVolumeControl();
				if (!_volumeHotkeyFailureLogged)
				{
					_volumeHotkeyFailureLogged = true;
					Debug.LogWarning($"[PortailCaptureAudioSink] Volume hotkey redirection failed: {ex.Message}");
				}
			}
		}

		private void QueueGameOutputVolumeChangedByRedirectedHotkey()
		{
			if (!TryGetGameOutputVolume(out float volumeScalar, out bool muted))
			{
				return;
			}

			lock (_gameOutputVolumeEventLock)
			{
				_pendingGameOutputVolumeScalar = volumeScalar;
				_pendingGameOutputMuted = muted;
				_gameOutputVolumeEventPending = true;
			}
		}

		private static bool IsVolumeHotkey(uint vkCode)
		{
			return vkCode == VK_VOLUME_UP || vkCode == VK_VOLUME_DOWN || vkCode == VK_VOLUME_MUTE;
		}

		private static bool IsApplicationForegroundProcess()
		{
			IntPtr foregroundWindow = GetForegroundWindow();
			if (foregroundWindow == IntPtr.Zero)
			{
				return Application.isFocused;
			}

			GetWindowThreadProcessId(foregroundWindow, out uint processId);
			return processId == unchecked((uint)Process.GetCurrentProcess().Id);
		}
#endif

		private void ReleaseComIfNeeded()
		{
			if (_comInitializedHere)
			{
				CoUninitialize();
				_comInitializedHere = false;
			}
		}

		private static readonly ERole[] AllRoles =
		{
			ERole.Console,
			ERole.Multimedia,
			ERole.Communications,
		};

		public bool ConsumeAudioDeviceResetRequest()
		{
			if (!_audioDeviceResetRequested)
			{
				return false;
			}

			_audioDeviceResetRequested = false;
			return true;
		}

		private void MarkGameOutputPinned()
		{
			if (!_gameOutputPinned)
			{
				_audioDeviceResetRequested = true;
			}

			_gameOutputPinned = true;
		}

		private void ResetUnityAudioOutputIfRequested()
		{
			if (!resetUnityAudioAfterPin || !ConsumeAudioDeviceResetRequest())
			{
				return;
			}

			AudioConfiguration configuration = AudioSettings.GetConfiguration();
			if (AudioSettings.Reset(configuration))
			{
				Debug.Log("[PortailCaptureAudioSink] Unity audio output reset after endpoint pin.");
			}
			else
			{
				Debug.LogWarning("[PortailCaptureAudioSink] Unity audio output reset failed after endpoint pin.");
			}
		}

		private void LogErrorIfNeeded()
		{
			if (_quitting || string.IsNullOrWhiteSpace(LastError))
			{
				return;
			}

			Debug.LogWarning($"[PortailCaptureAudioSink] {LastError}");
		}

		private bool TryPinGameOutput(bool logSuccess)
		{
			if (string.IsNullOrWhiteSpace(_gameRenderEndpointId))
			{
				return false;
			}

			try
			{
				int currentProcessId = Process.GetCurrentProcess().Id;
				using (IAudioPolicyConfigEndpoint audioPolicy = CreateAudioPolicyFactory())
				{
					ClearGameRenderEndpoints(audioPolicy, currentProcessId);
					SetGameRenderEndpoint(audioPolicy, currentProcessId, _gameRenderEndpointId);
					VerifyGameRenderEndpoint(audioPolicy, currentProcessId, _gameRenderEndpointId);
				}

				MarkGameOutputPinned();
				GameOutputPinError = string.Empty;
				if (logSuccess)
				{
					Debug.Log("[PortailCaptureAudioSink] application output pinned to the current real output.");
				}
				return true;
			}
			catch (Exception ex)
			{
				GameOutputPinError = ex.Message;
				if (!_gameOutputPinFailureLogged)
				{
					_gameOutputPinFailureLogged = true;
					Debug.Log($"[PortailCaptureAudioSink] application output pin failed: {GameOutputPinError}");
				}
				return false;
			}
		}

		private static readonly ERole[] ApplicationEndpointRoles =
		{
			ERole.Console,
			ERole.Multimedia,
		};

		private static void SetGameRenderEndpoint(IAudioPolicyConfigEndpoint audioPolicy, int processId, string endpointId)
		{
			foreach (ERole role in ApplicationEndpointRoles)
			{
				SetGameRenderEndpoint(audioPolicy, processId, role, endpointId);
			}
		}

		private static void SetGameRenderEndpoint(IAudioPolicyConfigEndpoint audioPolicy, int processId, ERole role, string endpointId)
		{
			IntPtr hstring = IntPtr.Zero;
			try
			{
				string packedEndpointId = PackRenderDeviceId(endpointId);
				WindowsCreateString(packedEndpointId, (uint)packedEndpointId.Length, out hstring);
				uint result = audioPolicy.SetPersistedDefaultAudioEndpoint(processId, EDataFlow.Render, role, hstring);
				if (FailedRequiredPolicyResult(result))
				{
					throw new InvalidOperationException($"SetPersistedDefaultAudioEndpoint({role}) failed: 0x{result:x8}");
				}
			}
			finally
			{
				if (hstring != IntPtr.Zero)
				{
					WindowsDeleteString(hstring);
				}
			}
		}

		private static string PackRenderDeviceId(string endpointId)
		{
			if (string.IsNullOrWhiteSpace(endpointId))
			{
				return string.Empty;
			}

			if (endpointId.StartsWith(MmdevapiToken, StringComparison.OrdinalIgnoreCase))
			{
				return endpointId;
			}

			return MmdevapiToken + endpointId + RenderInterface;
		}

		private static void VerifyGameRenderEndpoint(IAudioPolicyConfigEndpoint audioPolicy, int processId, string expectedEndpointId)
		{
			foreach (ERole role in ApplicationEndpointRoles)
			{
				string endpointId = GetPersistedGameRenderEndpoint(audioPolicy, processId, role);
				if (!string.Equals(endpointId, expectedEndpointId, StringComparison.OrdinalIgnoreCase))
				{
					throw new InvalidOperationException(
						$"SetPersistedDefaultAudioEndpoint({role}) did not stick. Expected {expectedEndpointId}, got {endpointId}.");
				}
			}
		}

		private static string GetPersistedGameRenderEndpoint(IAudioPolicyConfigEndpoint audioPolicy, int processId, ERole role)
		{
			uint result = audioPolicy.GetPersistedDefaultAudioEndpoint(processId, EDataFlow.Render, role, out IntPtr endpointHstring);
			string endpointId = string.Empty;
			try
			{
				endpointId = HStringToString(endpointHstring);
			}
			finally
			{
				if (endpointHstring != IntPtr.Zero)
				{
					WindowsDeleteString(endpointHstring);
				}
			}

			if (FailedRequiredPolicyResult(result))
			{
				throw new InvalidOperationException($"GetPersistedDefaultAudioEndpoint({role}) failed: 0x{result:x8}");
			}

			return UnpackDeviceId(endpointId);
		}

		private static void ClearGameRenderEndpoint(IAudioPolicyConfigEndpoint audioPolicy, int processId, ERole role)
		{
			uint result = audioPolicy.SetPersistedDefaultAudioEndpoint(processId, EDataFlow.Render, role, IntPtr.Zero);
			if (FailedOptionalPolicyResult(result))
			{
				throw new InvalidOperationException($"ClearPersistedDefaultAudioEndpoint({role}) failed: 0x{result:x8}");
			}
		}

		private static void ClearGameRenderEndpoints(IAudioPolicyConfigEndpoint audioPolicy, int processId)
		{
			foreach (ERole role in AllRoles)
			{
				ClearGameRenderEndpoint(audioPolicy, processId, role);
			}
		}

		private static IAudioPolicyConfigEndpoint CreateAudioPolicyFactory()
		{
			try
			{
				Guid iid = typeof(IAudioPolicyConfigFactory).GUID;
				return new AudioPolicyConfigFactoryRaw(GetActivationFactoryPointer(iid));
			}
			catch
			{
				Guid iid = typeof(IAudioPolicyConfigFactoryDownlevel).GUID;
				return new AudioPolicyConfigFactoryRaw(GetActivationFactoryPointer(iid));
			}
		}

		private static IntPtr GetActivationFactoryPointer(Guid iid)
		{
			IntPtr className = IntPtr.Zero;
			IntPtr factoryPtr = IntPtr.Zero;
			try
			{
				WindowsCreateString(AudioPolicyConfigClassName, (uint)AudioPolicyConfigClassName.Length, out className);
				int result = RoGetActivationFactory(className, ref iid, out factoryPtr);
				if (Failed(result))
				{
					throw new InvalidOperationException($"RoGetActivationFactory failed: 0x{result:x8}");
				}

				IntPtr resultPtr = factoryPtr;
				factoryPtr = IntPtr.Zero;
				return resultPtr;
			}
			finally
			{
				if (factoryPtr != IntPtr.Zero)
				{
					Marshal.Release(factoryPtr);
				}
				if (className != IntPtr.Zero)
				{
					WindowsDeleteString(className);
				}
			}
		}

		private static ComScope<IMMDeviceEnumerator> CreateDeviceEnumerator()
		{
			object enumeratorObject = Activator.CreateInstance(Type.GetTypeFromCLSID(MMDeviceEnumeratorGuid));
			return new ComScope<IMMDeviceEnumerator>((IMMDeviceEnumerator)enumeratorObject, enumeratorObject);
		}

		private static string GetDefaultRenderEndpointId(IMMDeviceEnumerator enumerator, ERole role)
		{
			IMMDevice device = null;
			try
			{
				int result = enumerator.GetDefaultAudioEndpoint(EDataFlow.Render, role, out device);
				if (Failed(result) || device == null)
				{
					return string.Empty;
				}

				result = device.GetId(out IntPtr idPtr);
				if (Failed(result) || idPtr == IntPtr.Zero)
				{
					return string.Empty;
				}

				try
				{
					return Marshal.PtrToStringUni(idPtr) ?? string.Empty;
				}
				finally
				{
					Marshal.FreeCoTaskMem(idPtr);
				}
			}
			finally
			{
				ReleaseComObject(device);
			}
		}

		private static string FindPortailCaptureSpeakersId(IMMDeviceEnumerator enumerator)
		{
			IMMDeviceCollection devices = null;
			try
			{
				if (Failed(enumerator.EnumAudioEndpoints(EDataFlow.Render, DeviceState.Active, out devices)) || devices == null)
				{
					return string.Empty;
				}

				if (Failed(devices.GetCount(out uint deviceCount)))
				{
					return string.Empty;
				}

				for (uint i = 0; i < deviceCount; ++i)
				{
					IMMDevice device = null;
					try
					{
						if (Failed(devices.Item(i, out device)) || device == null)
						{
							continue;
						}

						string id = GetDeviceId(device);
						if (string.IsNullOrWhiteSpace(id))
						{
							continue;
						}

						if (DevicePropertyEquals(device, PKeyDeviceInterfaceFriendlyName, PortailCaptureSpeakersName) ||
							DevicePropertyEquals(device, PKeyDeviceFriendlyName, PortailCaptureSpeakersName) ||
							DevicePropertyEquals(device, PKeyDeviceDeviceDesc, PortailCaptureSpeakersName))
						{
							return id;
						}
					}
					finally
					{
						ReleaseComObject(device);
					}
				}
			}
			finally
			{
				ReleaseComObject(devices);
			}

			return string.Empty;
		}

		private static string GetDeviceId(IMMDevice device)
		{
			if (Failed(device.GetId(out IntPtr idPtr)) || idPtr == IntPtr.Zero)
			{
				return string.Empty;
			}

			try
			{
				return Marshal.PtrToStringUni(idPtr) ?? string.Empty;
			}
			finally
			{
				Marshal.FreeCoTaskMem(idPtr);
			}
		}

		private static bool DevicePropertyEquals(IMMDevice device, PropertyKey key, string expected)
		{
			IPropertyStore store = null;
			try
			{
				if (Failed(device.OpenPropertyStore(StgmRead, out store)) || store == null)
				{
					return false;
				}

				PropVariant value = default;
				try
				{
					if (Failed(store.GetValue(ref key, out value)))
					{
						return false;
					}

					string actual = value.ValueType == VarEnum.VT_LPWSTR && value.PointerValue != IntPtr.Zero
						? Marshal.PtrToStringUni(value.PointerValue)
						: string.Empty;
					return string.Equals(actual, expected, StringComparison.OrdinalIgnoreCase);
				}
				finally
				{
					PropVariantClear(ref value);
				}
			}
			finally
			{
				ReleaseComObject(store);
			}
		}

		private static void SetDefaultRenderEndpoint(string endpointId)
		{
			foreach (ERole role in AllRoles)
			{
				SetDefaultRenderEndpoint(endpointId, role);
			}
		}

		private static void SetDefaultRenderEndpoint(string endpointId, ERole role)
		{
			object policyObject = null;
			try
			{
				policyObject = new PolicyConfigClient();
				IPolicyConfig policy = (IPolicyConfig)policyObject;
				int result = policy.SetDefaultEndpoint(endpointId, role);
				if (Failed(result))
				{
					throw new InvalidOperationException($"SetDefaultEndpoint({role}) failed: 0x{result:x8}");
				}
			}
			finally
			{
				ReleaseComObject(policyObject);
			}
		}

		private void EnsureComInitialized()
		{
			if (_comInitializedHere)
			{
				return;
			}

			int result = CoInitializeEx(IntPtr.Zero, CoInitApartmentThreaded);
			if (result == 0 || result == HResultFalse)
			{
				_comInitializedHere = true;
			}
			else if (result != HResultRpcChangedMode)
			{
				throw new InvalidOperationException($"CoInitializeEx failed: 0x{result:x8}");
			}
		}

		private static string UnpackDeviceId(string deviceId)
		{
			if (string.IsNullOrWhiteSpace(deviceId))
			{
				return string.Empty;
			}

			if (deviceId.StartsWith(MmdevapiToken, StringComparison.OrdinalIgnoreCase))
			{
				deviceId = deviceId.Substring(MmdevapiToken.Length);
			}
			if (deviceId.EndsWith(RenderInterface, StringComparison.OrdinalIgnoreCase))
			{
				deviceId = deviceId.Substring(0, deviceId.Length - RenderInterface.Length);
			}
			if (deviceId.EndsWith(CaptureInterface, StringComparison.OrdinalIgnoreCase))
			{
				deviceId = deviceId.Substring(0, deviceId.Length - CaptureInterface.Length);
			}

			return deviceId;
		}

		private static string HStringToString(IntPtr hstring)
		{
			if (hstring == IntPtr.Zero)
			{
				return string.Empty;
			}

			IntPtr buffer = WindowsGetStringRawBuffer(hstring, out uint length);
			if (buffer == IntPtr.Zero || length == 0)
			{
				return string.Empty;
			}

			return Marshal.PtrToStringUni(buffer, checked((int)length)) ?? string.Empty;
		}

		private static float Clamp01(float value)
		{
			if (float.IsNaN(value))
			{
				return 0f;
			}
			if (value < 0f)
			{
				return 0f;
			}
			if (value > 1f)
			{
				return 1f;
			}
			return value;
		}

		private static bool Failed(int hresult)
		{
			return hresult < 0;
		}

		private static bool FailedRequiredPolicyResult(uint hresult)
		{
			return unchecked((int)hresult) < 0;
		}

		private static bool FailedOptionalPolicyResult(uint hresult)
		{
			return unchecked((int)hresult) < 0 && unchecked((int)hresult) != HResultNotFound;
		}

		private static void ReleaseComObject(object comObject)
		{
			if (comObject != null && Marshal.IsComObject(comObject))
			{
				Marshal.ReleaseComObject(comObject);
			}
		}

		private sealed class ComScope<T> : IDisposable where T : class
		{
			private readonly object _owner;

			public ComScope(T value, object owner)
			{
				Value = value;
				_owner = owner;
			}

			public T Value { get; }

			public void Dispose()
			{
				ReleaseComObject(_owner);
			}
		}

		private interface IAudioPolicyConfigEndpoint : IDisposable
		{
			uint SetPersistedDefaultAudioEndpoint(int processId, EDataFlow flow, ERole role, IntPtr deviceId);
			uint GetPersistedDefaultAudioEndpoint(int processId, EDataFlow flow, ERole role, out IntPtr deviceId);
			uint ClearAllPersistedApplicationDefaultEndpoints();
		}

		private sealed class AudioPolicyConfigFactoryRaw : IAudioPolicyConfigEndpoint
		{
			private const int SetPersistedDefaultAudioEndpointSlot = 25;
			private const int GetPersistedDefaultAudioEndpointSlot = 26;
			private const int ClearAllPersistedApplicationDefaultEndpointsSlot = 27;

			private readonly IntPtr _factoryPtr;
			private readonly SetPersistedDefaultAudioEndpointDelegate _setPersistedDefaultAudioEndpoint;
			private readonly GetPersistedDefaultAudioEndpointDelegate _getPersistedDefaultAudioEndpoint;
			private readonly ClearAllPersistedApplicationDefaultEndpointsDelegate _clearAllPersistedApplicationDefaultEndpoints;

			public AudioPolicyConfigFactoryRaw(IntPtr factoryPtr)
			{
				if (factoryPtr == IntPtr.Zero)
				{
					throw new ArgumentNullException(nameof(factoryPtr));
				}

				_factoryPtr = factoryPtr;
				_setPersistedDefaultAudioEndpoint = GetVTableDelegate<SetPersistedDefaultAudioEndpointDelegate>(
					factoryPtr,
					SetPersistedDefaultAudioEndpointSlot);
				_getPersistedDefaultAudioEndpoint = GetVTableDelegate<GetPersistedDefaultAudioEndpointDelegate>(
					factoryPtr,
					GetPersistedDefaultAudioEndpointSlot);
				_clearAllPersistedApplicationDefaultEndpoints = GetVTableDelegate<ClearAllPersistedApplicationDefaultEndpointsDelegate>(
					factoryPtr,
					ClearAllPersistedApplicationDefaultEndpointsSlot);
			}

			public uint SetPersistedDefaultAudioEndpoint(int processId, EDataFlow flow, ERole role, IntPtr deviceId)
			{
				return _setPersistedDefaultAudioEndpoint(_factoryPtr, processId, flow, role, deviceId);
			}

			public uint GetPersistedDefaultAudioEndpoint(int processId, EDataFlow flow, ERole role, out IntPtr deviceId)
			{
				return _getPersistedDefaultAudioEndpoint(_factoryPtr, processId, flow, role, out deviceId);
			}

			public uint ClearAllPersistedApplicationDefaultEndpoints()
			{
				return _clearAllPersistedApplicationDefaultEndpoints(_factoryPtr);
			}

			public void Dispose()
			{
				Marshal.Release(_factoryPtr);
			}

			private static T GetVTableDelegate<T>(IntPtr unknown, int slot) where T : Delegate
			{
				IntPtr vtable = Marshal.ReadIntPtr(unknown);
				IntPtr functionPointer = Marshal.ReadIntPtr(vtable, slot * IntPtr.Size);
				return Marshal.GetDelegateForFunctionPointer<T>(functionPointer);
			}
		}

		[UnmanagedFunctionPointer(CallingConvention.StdCall)]
		private delegate uint SetPersistedDefaultAudioEndpointDelegate(
			IntPtr self,
			int processId,
			EDataFlow flow,
			ERole role,
			IntPtr deviceId);

		[UnmanagedFunctionPointer(CallingConvention.StdCall)]
		private delegate uint GetPersistedDefaultAudioEndpointDelegate(
			IntPtr self,
			int processId,
			EDataFlow flow,
			ERole role,
			out IntPtr deviceId);

		[UnmanagedFunctionPointer(CallingConvention.StdCall)]
		private delegate uint ClearAllPersistedApplicationDefaultEndpointsDelegate(IntPtr self);


#if UNITY_STANDALONE_WIN || UNITY_EDITOR_WIN
		private delegate IntPtr LowLevelKeyboardProc(int nCode, IntPtr wParam, IntPtr lParam);

		[StructLayout(LayoutKind.Sequential)]
		private struct KBDLLHOOKSTRUCT
		{
			public uint vkCode;
			public uint scanCode;
			public uint flags;
			public uint time;
			public IntPtr dwExtraInfo;
		}

		[DllImport("user32.dll", SetLastError = true)]
		private static extern IntPtr SetWindowsHookEx(int idHook, LowLevelKeyboardProc lpfn, IntPtr hMod, uint dwThreadId);

		[DllImport("user32.dll", SetLastError = true)]
		[return: MarshalAs(UnmanagedType.Bool)]
		private static extern bool UnhookWindowsHookEx(IntPtr hhk);

		[DllImport("user32.dll")]
		private static extern IntPtr CallNextHookEx(IntPtr hhk, int nCode, IntPtr wParam, IntPtr lParam);

		[DllImport("kernel32.dll", CharSet = CharSet.Auto, SetLastError = true)]
		private static extern IntPtr GetModuleHandle(string lpModuleName);

		[DllImport("user32.dll")]
		private static extern IntPtr GetForegroundWindow();

		[DllImport("user32.dll")]
		private static extern uint GetWindowThreadProcessId(IntPtr hWnd, out uint processId);
#endif

		[DllImport("ole32.dll")]
		private static extern int CoInitializeEx(IntPtr pvReserved, uint dwCoInit);

		[DllImport("ole32.dll")]
		private static extern void CoUninitialize();

		[DllImport("ole32.dll")]
		private static extern int PropVariantClear(ref PropVariant propVariant);

		[DllImport("combase.dll")]
		private static extern int RoGetActivationFactory(
			IntPtr activatableClassId,
			ref Guid iid,
			out IntPtr factory);

		[DllImport("combase.dll", PreserveSig = false)]
		private static extern void WindowsCreateString(
			[MarshalAs(UnmanagedType.LPWStr)] string sourceString,
			uint length,
			out IntPtr hstring);

		[DllImport("combase.dll")]
		private static extern int WindowsDeleteString(IntPtr hstring);

		[DllImport("combase.dll")]
		private static extern IntPtr WindowsGetStringRawBuffer(IntPtr hstring, out uint length);

		private enum EDataFlow
		{
			Render = 0,
			Capture = 1,
			All = 2,
		}

		private enum ERole
		{
			Console = 0,
			Multimedia = 1,
			Communications = 2,
		}

		[Flags]
		private enum DeviceState : uint
		{
			Active = 0x00000001,
		}

		[Flags]
		private enum ClsCtx : uint
		{
			All = 23,
		}

		private enum VarEnum : ushort
		{
			VT_EMPTY = 0,
			VT_LPWSTR = 31,
		}

		[StructLayout(LayoutKind.Sequential)]
		private struct PropertyKey
		{
			public Guid FormatId;
			public int PropertyId;

			public PropertyKey(Guid formatId, int propertyId)
			{
				FormatId = formatId;
				PropertyId = propertyId;
			}
		}

		[StructLayout(LayoutKind.Sequential)]
		private struct PropVariant
		{
			public VarEnum ValueType;
			private ushort _reserved1;
			private ushort _reserved2;
			private ushort _reserved3;
			public IntPtr PointerValue;
			private IntPtr _reserved4;
		}

		[ComImport]
		[Guid("A95664D2-9614-4F35-A746-DE8DB63617E6")]
		[InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
		private interface IMMDeviceEnumerator
		{
			[PreserveSig]
			int EnumAudioEndpoints(EDataFlow dataFlow, DeviceState stateMask, out IMMDeviceCollection devices);

			[PreserveSig]
			int GetDefaultAudioEndpoint(EDataFlow dataFlow, ERole role, out IMMDevice endpoint);

			[PreserveSig]
			int GetDevice([MarshalAs(UnmanagedType.LPWStr)] string id, out IMMDevice device);
		}

		[ComImport]
		[Guid("0BD7A1BE-7A1A-44DB-8397-CC5392387B5E")]
		[InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
		private interface IMMDeviceCollection
		{
			[PreserveSig]
			int GetCount(out uint count);

			[PreserveSig]
			int Item(uint index, out IMMDevice device);
		}

		[ComImport]
		[Guid("D666063F-1587-4E43-81F1-B948E807363F")]
		[InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
		private interface IMMDevice
		{
			[PreserveSig]
			int Activate(
				ref Guid interfaceId,
				ClsCtx clsCtx,
				IntPtr activationParams,
				[MarshalAs(UnmanagedType.IUnknown)] out object instance);

			[PreserveSig]
			int OpenPropertyStore(int access, out IPropertyStore properties);

			[PreserveSig]
			int GetId(out IntPtr id);

			[PreserveSig]
			int GetState(out DeviceState state);
		}

		[ComImport]
		[Guid("886D8EEB-8CF2-4446-8D02-CDBA1DBDCF99")]
		[InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
		private interface IPropertyStore
		{
			[PreserveSig]
			int GetCount(out uint propertyCount);

			[PreserveSig]
			int GetAt(uint propertyIndex, out PropertyKey key);

			[PreserveSig]
			int GetValue(ref PropertyKey key, out PropVariant value);

			[PreserveSig]
			int SetValue(ref PropertyKey key, ref PropVariant value);

			[PreserveSig]
			int Commit();
		}

		[ComImport]
		[Guid("5CDF2C82-841E-4546-9722-0CF74078229A")]
		[InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
		private interface IAudioEndpointVolume
		{
			[PreserveSig]
			int RegisterControlChangeNotify(IntPtr client);

			[PreserveSig]
			int UnregisterControlChangeNotify(IntPtr client);

			[PreserveSig]
			int GetChannelCount(out uint channelCount);

			[PreserveSig]
			int SetMasterVolumeLevel(float levelDb, IntPtr eventContext);

			[PreserveSig]
			int SetMasterVolumeLevelScalar(float level, IntPtr eventContext);

			[PreserveSig]
			int GetMasterVolumeLevel(out float levelDb);

			[PreserveSig]
			int GetMasterVolumeLevelScalar(out float level);

			[PreserveSig]
			int SetChannelVolumeLevel(uint channel, float levelDb, IntPtr eventContext);

			[PreserveSig]
			int SetChannelVolumeLevelScalar(uint channel, float level, IntPtr eventContext);

			[PreserveSig]
			int GetChannelVolumeLevel(uint channel, out float levelDb);

			[PreserveSig]
			int GetChannelVolumeLevelScalar(uint channel, out float level);

			[PreserveSig]
			int SetMute([MarshalAs(UnmanagedType.Bool)] bool muted, IntPtr eventContext);

			[PreserveSig]
			int GetMute([MarshalAs(UnmanagedType.Bool)] out bool muted);

			[PreserveSig]
			int GetVolumeStepInfo(out uint step, out uint stepCount);

			[PreserveSig]
			int VolumeStepUp(IntPtr eventContext);

			[PreserveSig]
			int VolumeStepDown(IntPtr eventContext);

			[PreserveSig]
			int QueryHardwareSupport(out uint hardwareSupportMask);

			[PreserveSig]
			int GetVolumeRange(out float minDb, out float maxDb, out float incrementDb);
		}

		[ComImport]
		[Guid("870AF99C-171D-4F9E-AF0D-E63DF40C2BC9")]
		private class PolicyConfigClient
		{
		}

		[ComImport]
		[Guid("F8679F50-850A-41CF-9C72-430F290290C8")]
		[InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
		private interface IPolicyConfig
		{
			[PreserveSig]
			int GetMixFormat([MarshalAs(UnmanagedType.LPWStr)] string deviceName, out IntPtr format);

			[PreserveSig]
			int GetDeviceFormat([MarshalAs(UnmanagedType.LPWStr)] string deviceName, int defaultFormat, out IntPtr format);

			[PreserveSig]
			int ResetDeviceFormat([MarshalAs(UnmanagedType.LPWStr)] string deviceName);

			[PreserveSig]
			int SetDeviceFormat([MarshalAs(UnmanagedType.LPWStr)] string deviceName, IntPtr endpointFormat, IntPtr mixFormat);

			[PreserveSig]
			int GetProcessingPeriod([MarshalAs(UnmanagedType.LPWStr)] string deviceName, int defaultPeriod, out long defaultPeriodValue, out long minimumPeriodValue);

			[PreserveSig]
			int SetProcessingPeriod([MarshalAs(UnmanagedType.LPWStr)] string deviceName, ref long period);

			[PreserveSig]
			int GetShareMode([MarshalAs(UnmanagedType.LPWStr)] string deviceName, IntPtr mode);

			[PreserveSig]
			int SetShareMode([MarshalAs(UnmanagedType.LPWStr)] string deviceName, IntPtr mode);

			[PreserveSig]
			int GetPropertyValue([MarshalAs(UnmanagedType.LPWStr)] string deviceName, ref PropertyKey key, out PropVariant value);

			[PreserveSig]
			int SetPropertyValue([MarshalAs(UnmanagedType.LPWStr)] string deviceName, ref PropertyKey key, ref PropVariant value);

			[PreserveSig]
			int SetDefaultEndpoint([MarshalAs(UnmanagedType.LPWStr)] string deviceName, ERole role);

			[PreserveSig]
			int SetEndpointVisibility([MarshalAs(UnmanagedType.LPWStr)] string deviceName, int visible);
		}

		[Guid("AB3D4648-E242-459F-B02F-541C70306324")]
		[InterfaceType(ComInterfaceType.InterfaceIsIInspectable)]
		private interface IAudioPolicyConfigFactory
		{
			int __incomplete__add_CtxVolumeChange();
			int __incomplete__remove_CtxVolumeChanged();
			int __incomplete__add_RingerVibrateStateChanged();
			int __incomplete__remove_RingerVibrateStateChange();
			int __incomplete__SetVolumeGroupGainForId();
			int __incomplete__GetVolumeGroupGainForId();
			int __incomplete__GetActiveVolumeGroupForEndpointId();
			int __incomplete__GetVolumeGroupsForEndpoint();
			int __incomplete__GetCurrentVolumeContext();
			int __incomplete__SetVolumeGroupMuteForId();
			int __incomplete__GetVolumeGroupMuteForId();
			int __incomplete__SetRingerVibrateState();
			int __incomplete__GetRingerVibrateState();
			int __incomplete__SetPreferredChatApplication();
			int __incomplete__ResetPreferredChatApplication();
			int __incomplete__GetPreferredChatApplication();
			int __incomplete__GetCurrentChatApplications();
			int __incomplete__add_ChatContextChanged();
			int __incomplete__remove_ChatContextChanged();

			[PreserveSig]
			uint SetPersistedDefaultAudioEndpoint(int processId, EDataFlow flow, ERole role, IntPtr deviceId);

			[PreserveSig]
			uint GetPersistedDefaultAudioEndpoint(int processId, EDataFlow flow, ERole role, out IntPtr deviceId);

			[PreserveSig]
			uint ClearAllPersistedApplicationDefaultEndpoints();
		}

		[Guid("2A59116D-6C4F-45E0-A74F-707E3FEF9258")]
		[InterfaceType(ComInterfaceType.InterfaceIsIInspectable)]
		private interface IAudioPolicyConfigFactoryDownlevel
		{
			int __incomplete__add_CtxVolumeChange();
			int __incomplete__remove_CtxVolumeChanged();
			int __incomplete__add_RingerVibrateStateChanged();
			int __incomplete__remove_RingerVibrateStateChange();
			int __incomplete__SetVolumeGroupGainForId();
			int __incomplete__GetVolumeGroupGainForId();
			int __incomplete__GetActiveVolumeGroupForEndpointId();
			int __incomplete__GetVolumeGroupsForEndpoint();
			int __incomplete__GetCurrentVolumeContext();
			int __incomplete__SetVolumeGroupMuteForId();
			int __incomplete__GetVolumeGroupMuteForId();
			int __incomplete__SetRingerVibrateState();
			int __incomplete__GetRingerVibrateState();
			int __incomplete__SetPreferredChatApplication();
			int __incomplete__ResetPreferredChatApplication();
			int __incomplete__GetPreferredChatApplication();
			int __incomplete__GetCurrentChatApplications();
			int __incomplete__add_ChatContextChanged();
			int __incomplete__remove_ChatContextChanged();

			[PreserveSig]
			uint SetPersistedDefaultAudioEndpoint(int processId, EDataFlow flow, ERole role, IntPtr deviceId);

			[PreserveSig]
			uint GetPersistedDefaultAudioEndpoint(int processId, EDataFlow flow, ERole role, out IntPtr deviceId);

			[PreserveSig]
			uint ClearAllPersistedApplicationDefaultEndpoints();
		}
	}
}
