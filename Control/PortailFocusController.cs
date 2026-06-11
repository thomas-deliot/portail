using System;
using System.Diagnostics;
using System.Runtime.InteropServices;
using Portail.Core;
using Portail.Playback;
using UnityEngine;
using Debug = UnityEngine.Debug;

namespace Portail.Control
{
	[DefaultExecutionOrder(-9500)]
	[DisallowMultipleComponent]
	public sealed class PortailFocusController : MonoBehaviour
	{
		public enum ControlMode
		{
			GameControl,
			PortailControl
		}

		private const int DefaultSwitchModeScanCode = 0x29; // Physical key left of 1 on common layouts.

		[Header("Toggle")]
		[Tooltip("Windows hardware scan code used for the mode switch. 0x29 is the physical key left of 1, printed as ² on French keyboards.")]
		public int switchModePhysicalScanCode = DefaultSwitchModeScanCode;
		[Tooltip("Minimum delay between toggles so a held key cannot bounce between modes.")]
		public float switchModeDebounceSeconds = 0.25f;
		[Tooltip("If enabled, holding the switch key temporarily switches mode until the key is released.")]
		public bool enableQuickPeekOnSwitchHold = true;
		[Tooltip("How long the switch key must be held before it becomes a quick peek instead of a normal toggle.")]
		public float quickPeekHoldSeconds = 0.22f;
		public LayerMask portailScreenLayerMask = ~0;

		[Header("Window Mode")]
		public bool applyWindowStyles = true;
		public bool applyWindowStylesInEditor;
		public bool makeTopmostInPortailControl = true;
		public bool makeClickThroughInPortailControl = true;
		public bool makeNoActivateInPortailControl = true;

		public static PortailFocusController Instance { get; private set; }
		public static bool IsPortailControlActive => Instance != null && Instance.CurrentMode == ControlMode.PortailControl;

		public ControlMode CurrentMode { get; private set; } = ControlMode.GameControl;

		private IntPtr _applicationWindow;
		private IntPtr _lastExternalForegroundWindow;
		private CursorLockMode _savedCursorLockState;
		private bool _savedCursorVisible;
		private bool _hasSavedCursorState;
		private CursorLockMode _pendingCursorLockState;
		private bool _pendingCursorVisible;
		private int _pendingCursorRestoreFrames;
		private float _nextToggleAllowedAt;

		private bool _switchModeKeyDownEvent;
		private bool _switchModeKeyUpEvent;
		private bool _switchModeKeyHeld;
		private float _switchModeKeyPressedAt;
		private bool _quickPeekActive;
		private bool _quickPeekConsumedCurrentPress;
		private ControlMode _quickPeekReturnMode;

#if UNITY_STANDALONE_WIN || UNITY_EDITOR_WIN
		private const int WH_KEYBOARD_LL = 13;
		private const int WM_KEYDOWN = 0x0100;
		private const int WM_KEYUP = 0x0101;
		private const int WM_SYSKEYDOWN = 0x0104;
		private const int WM_SYSKEYUP = 0x0105;
		private const int GWL_EXSTYLE = -20;
		private const long WS_EX_LAYERED = 0x00080000L;
		private const long WS_EX_TRANSPARENT = 0x00000020L;
		private const long WS_EX_TOPMOST = 0x00000008L;
		private const long WS_EX_NOACTIVATE = 0x08000000L;
		private const uint LWA_ALPHA = 0x00000002;
		private const uint SWP_NOSIZE = 0x0001;
		private const uint SWP_NOMOVE = 0x0002;
		private const uint SWP_NOZORDER = 0x0004;
		private const uint SWP_NOACTIVATE = 0x0010;
		private const uint SWP_FRAMECHANGED = 0x0020;
		private const uint SWP_NOOWNERZORDER = 0x0200;
		private const uint SWP_SHOWWINDOW = 0x0040;
		private const int SW_RESTORE = 9;
		private static readonly IntPtr HWND_TOPMOST = new IntPtr(-1);
		private static readonly IntPtr HWND_NOTOPMOST = new IntPtr(-2);
		private delegate bool EnumWindowsProc(IntPtr hWnd, IntPtr lParam);
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

		private IntPtr _savedExtendedStyle;
		private bool _hasSavedExtendedStyle;
		private LowLevelKeyboardProc _keyboardHookProc;
		private IntPtr _keyboardHook;
		private bool _hookSwitchModeKeyDown;

		[DllImport("user32.dll", SetLastError = true)]
		private static extern IntPtr SetWindowsHookEx(int idHook, LowLevelKeyboardProc lpfn, IntPtr hMod, uint dwThreadId);

		[DllImport("user32.dll", SetLastError = true)]
		[return: MarshalAs(UnmanagedType.Bool)]
		private static extern bool UnhookWindowsHookEx(IntPtr hhk);

		[DllImport("user32.dll")]
		private static extern IntPtr CallNextHookEx(IntPtr hhk, int nCode, IntPtr wParam, IntPtr lParam);

		[DllImport("kernel32.dll", CharSet = CharSet.Auto, SetLastError = true)]
		private static extern IntPtr GetModuleHandle(string lpModuleName);

		[DllImport("kernel32.dll")]
		private static extern uint GetCurrentThreadId();

		[DllImport("user32.dll")]
		private static extern IntPtr GetForegroundWindow();

		[DllImport("user32.dll")]
		private static extern IntPtr GetActiveWindow();

		[DllImport("user32.dll")]
		[return: MarshalAs(UnmanagedType.Bool)]
		private static extern bool SetForegroundWindow(IntPtr hWnd);

		[DllImport("user32.dll")]
		private static extern IntPtr SetActiveWindow(IntPtr hWnd);

		[DllImport("user32.dll")]
		private static extern IntPtr SetFocus(IntPtr hWnd);

		[DllImport("user32.dll")]
		[return: MarshalAs(UnmanagedType.Bool)]
		private static extern bool BringWindowToTop(IntPtr hWnd);

		[DllImport("user32.dll")]
		[return: MarshalAs(UnmanagedType.Bool)]
		private static extern bool AttachThreadInput(uint idAttach, uint idAttachTo, [MarshalAs(UnmanagedType.Bool)] bool fAttach);

		[DllImport("user32.dll")]
		[return: MarshalAs(UnmanagedType.Bool)]
		private static extern bool ShowWindow(IntPtr hWnd, int nCmdShow);

		[DllImport("user32.dll")]
		[return: MarshalAs(UnmanagedType.Bool)]
		private static extern bool IsWindow(IntPtr hWnd);

		[DllImport("user32.dll")]
		[return: MarshalAs(UnmanagedType.Bool)]
		private static extern bool IsWindowVisible(IntPtr hWnd);

		[DllImport("user32.dll")]
		private static extern uint GetWindowThreadProcessId(IntPtr hWnd, out uint processId);

		[DllImport("user32.dll")]
		[return: MarshalAs(UnmanagedType.Bool)]
		private static extern bool EnumWindows(EnumWindowsProc lpEnumFunc, IntPtr lParam);

		[DllImport("user32.dll")]
		[return: MarshalAs(UnmanagedType.Bool)]
		private static extern bool SetWindowPos(IntPtr hWnd, IntPtr hWndInsertAfter, int x, int y, int cx, int cy, uint flags);

		[DllImport("user32.dll")]
		[return: MarshalAs(UnmanagedType.Bool)]
		private static extern bool SetLayeredWindowAttributes(IntPtr hwnd, uint crKey, byte bAlpha, uint dwFlags);

		[DllImport("user32.dll", EntryPoint = "GetWindowLong")]
		private static extern int GetWindowLong32(IntPtr hWnd, int nIndex);

		[DllImport("user32.dll", EntryPoint = "GetWindowLongPtr")]
		private static extern IntPtr GetWindowLongPtr64(IntPtr hWnd, int nIndex);

		[DllImport("user32.dll", EntryPoint = "SetWindowLong")]
		private static extern int SetWindowLong32(IntPtr hWnd, int nIndex, int dwNewLong);

		[DllImport("user32.dll", EntryPoint = "SetWindowLongPtr")]
		private static extern IntPtr SetWindowLongPtr64(IntPtr hWnd, int nIndex, IntPtr dwNewLong);

		[DllImport("user32.dll", SetLastError = true)]
		[return: MarshalAs(UnmanagedType.Bool)]
		private static extern bool SetCursorPos(int x, int y);
#endif

		private void Awake()
		{
			if (Instance != null && Instance != this)
			{
				Destroy(this);
				return;
			}

			Instance = this;
			Application.runInBackground = true;
			SetCurrentMode(CurrentMode);
			CacheApplicationWindow();
			InstallKeyboardHook();

			var detector = FindObjectOfType<PortailShellPopupDetector>();
			if (detector != null)
				detector.onShellPopupActiveChanged += SetDesktopCapturePaused;
		}

		private void OnEnable()
		{
#if UNITY_EDITOR_WIN
			enabled = false;
			return;
#endif

			if (Instance != null && Instance != this)
			{
				enabled = false;
				return;
			}

			Instance = this;
			Application.runInBackground = true;
			SetCurrentMode(CurrentMode);
			CacheApplicationWindow();
			InstallKeyboardHook();
		}

		private void OnDisable()
		{
			if (CurrentMode == ControlMode.PortailControl)
			{
				EnterGameControl(false);
			}

			if (Instance == this)
			{
				UninstallKeyboardHook();
				PortailControlState.IsDesktopControlActive = false;
				Instance = null;
			}
		}

		private void OnDestroy()
		{
			if (CurrentMode == ControlMode.PortailControl)
			{
				EnterGameControl(false);
			}

			if (Instance == this)
			{
				UninstallKeyboardHook();
				PortailControlState.IsDesktopControlActive = false;
				Instance = null;
			}
		}

		private void Update()
		{
			CacheApplicationWindow();
			TrackExternalForegroundWindow();

			ProcessSwitchModeInput();

			ReapplyPendingCursorRestore();
		}

		private void LateUpdate()
		{
#if UNITY_STANDALONE_WIN || UNITY_EDITOR_WIN
			if (CurrentMode == ControlMode.PortailControl)
			{
				KeepApplicationAboveDesktopPopups();
			}
#endif
		}

#if UNITY_STANDALONE_WIN || UNITY_EDITOR_WIN
		private void KeepApplicationAboveDesktopPopups()
		{
			if (!ShouldApplyWindowStyles() || !IsValidWindow(_applicationWindow))
			{
				return;
			}

			if (!makeTopmostInPortailControl)
			{
				return;
			}

			const uint flags =
				SWP_NOMOVE |
				SWP_NOSIZE |
				SWP_NOACTIVATE |
				SWP_NOOWNERZORDER |
				SWP_SHOWWINDOW;

			SetWindowPos(_applicationWindow, HWND_TOPMOST, 0, 0, 0, 0, flags);
		}
#endif

		private void ProcessSwitchModeInput()
		{
			if (_switchModeKeyDownEvent)
			{
				_switchModeKeyDownEvent = false;

				if (!_switchModeKeyHeld)
				{
					_switchModeKeyHeld = true;
					_switchModeKeyPressedAt = Time.unscaledTime;
					_quickPeekConsumedCurrentPress = false;
				}
			}

			if (_switchModeKeyHeld &&
				!_quickPeekActive &&
				!_quickPeekConsumedCurrentPress &&
				enableQuickPeekOnSwitchHold &&
				Time.unscaledTime - _switchModeKeyPressedAt >= Mathf.Max(0.01f, quickPeekHoldSeconds))
			{
				BeginQuickPeek();
				_quickPeekConsumedCurrentPress = true;
			}

			if (_switchModeKeyUpEvent)
			{
				_switchModeKeyUpEvent = false;

				bool wasHeld = _switchModeKeyHeld;
				_switchModeKeyHeld = false;

				if (_quickPeekActive)
				{
					EndQuickPeek();
				}
				else if (wasHeld && !_quickPeekConsumedCurrentPress)
				{
					TryToggleControlModeFromSwitchKey();
				}

				_quickPeekConsumedCurrentPress = false;
			}
		}

		private void TryToggleControlModeFromSwitchKey()
		{
			if (Time.unscaledTime < _nextToggleAllowedAt)
			{
				return;
			}

			ControlMode previousMode = CurrentMode;
			ToggleControlMode();

			if (CurrentMode != previousMode)
			{
				_nextToggleAllowedAt = Time.unscaledTime + Mathf.Max(0.05f, switchModeDebounceSeconds);
			}
		}

		private bool BeginQuickPeek()
		{
			if (_quickPeekActive)
			{
				return false;
			}

			ControlMode returnMode = CurrentMode;

			if (CurrentMode == ControlMode.PortailControl)
			{
				_quickPeekReturnMode = returnMode;
				_quickPeekActive = true;
				EnterGameControl(true);
			}
			else
			{
				EnterPortailControl(true);
				if (CurrentMode != ControlMode.PortailControl)
				{
					return false;
				}

				_quickPeekReturnMode = returnMode;
				_quickPeekActive = true;
			}

			Debug.Log("[PortailFocusController] Began quick peek.");
			return true;
		}

		private void EndQuickPeek()
		{
			if (!_quickPeekActive)
			{
				return;
			}

			ControlMode returnMode = _quickPeekReturnMode;
			_quickPeekActive = false;

			if (returnMode == ControlMode.PortailControl)
			{
				EnterPortailControl(false);
			}
			else
			{
				EnterGameControl(true);
			}

			_nextToggleAllowedAt = Time.unscaledTime + Mathf.Max(0.05f, switchModeDebounceSeconds);

			Debug.Log("[PortailFocusController] Ended quick peek.");
		}

		public void ToggleControlMode()
		{
			if (CurrentMode == ControlMode.PortailControl)
			{
				EnterGameControl(true);
				return;
			}

			EnterPortailControl();
		}

		public void EnterPortailControl(bool warpCursor = true)
		{
			if (CurrentMode == ControlMode.PortailControl)
			{
				return;
			}

			int warpScreenX = 0;
			int warpScreenY = 0;
			bool hasWarpTarget = false;

			if (warpCursor)
			{
				hasWarpTarget = TryGetLookedDesktopScreenPoint(out warpScreenX, out warpScreenY);
				if (!hasWarpTarget)
				{
					Debug.Log("[PortailFocusController] Ignored PortailControl switch because the camera is not aimed at the local desktop screen.");
					return;
				}
			}

			SaveCursorState();
			_pendingCursorRestoreFrames = 0;
			Cursor.lockState = CursorLockMode.None;
			Cursor.visible = true;

			ApplyPortailWindowState();
			FocusPortailTarget();

			if (hasWarpTarget)
			{
				SetSystemCursorPosition(warpScreenX, warpScreenY);
			}

			ApplyPortailTopmostState();
			SetCurrentMode(ControlMode.PortailControl);
			Debug.Log("[PortailFocusController] Entered PortailControl.");
		}

		public void EnterGameControl(bool focusApplicationWindow)
		{
			if (CurrentMode == ControlMode.GameControl)
			{
				return;
			}

			SetCurrentMode(ControlMode.GameControl);
			ApplyApplicationWindowState(focusApplicationWindow);
			RestoreCursorState();
			Debug.Log("[PortailFocusController] Entered GameControl.");
		}

		public void ForceGameControlForMenu(bool focusApplicationWindow)
		{
			_quickPeekActive = false;
			_quickPeekConsumedCurrentPress = false;
			_switchModeKeyHeld = false;
			_switchModeKeyDownEvent = false;
			_switchModeKeyUpEvent = false;

			SetCurrentMode(ControlMode.GameControl);
			ApplyApplicationWindowState(focusApplicationWindow);
			_hasSavedCursorState = false;
			_pendingCursorLockState = CursorLockMode.None;
			_pendingCursorVisible = true;
			_pendingCursorRestoreFrames = 3;
			Cursor.lockState = CursorLockMode.None;
			Cursor.visible = true;
		}

		private void SaveCursorState()
		{
			if (_hasSavedCursorState)
			{
				return;
			}

			_savedCursorLockState = Cursor.lockState;
			_savedCursorVisible = Cursor.visible;
			_hasSavedCursorState = true;
		}

		private void RestoreCursorState()
		{
			if (!_hasSavedCursorState)
			{
				return;
			}

			Cursor.lockState = _savedCursorLockState;
			Cursor.visible = _savedCursorVisible;
			_pendingCursorLockState = _savedCursorLockState;
			_pendingCursorVisible = _savedCursorVisible;
			_pendingCursorRestoreFrames = 3;
			_hasSavedCursorState = false;
		}

		private void ReapplyPendingCursorRestore()
		{
			if (_pendingCursorRestoreFrames <= 0)
			{
				return;
			}

			_pendingCursorRestoreFrames--;
			Cursor.lockState = _pendingCursorLockState;
			Cursor.visible = _pendingCursorVisible;
		}

#if UNITY_STANDALONE_WIN || UNITY_EDITOR_WIN
		private void InstallKeyboardHook()
		{
			if (_keyboardHook != IntPtr.Zero)
			{
				return;
			}

			_keyboardHookProc = HandleLowLevelKeyboard;
			string moduleName = Process.GetCurrentProcess().MainModule?.ModuleName;
			IntPtr moduleHandle = string.IsNullOrEmpty(moduleName) ? IntPtr.Zero : GetModuleHandle(moduleName);
			_keyboardHook = SetWindowsHookEx(WH_KEYBOARD_LL, _keyboardHookProc, moduleHandle, 0);
			if (_keyboardHook == IntPtr.Zero)
			{
				Debug.LogWarning($"[PortailFocusController] Keyboard hook install failed: {Marshal.GetLastWin32Error()}.");
			}
		}

		private void UninstallKeyboardHook()
		{
			_hookSwitchModeKeyDown = false;
			_switchModeKeyDownEvent = false;
			_switchModeKeyUpEvent = false;
			_switchModeKeyHeld = false;
			_quickPeekActive = false;
			_quickPeekConsumedCurrentPress = false;

			if (_keyboardHook == IntPtr.Zero)
			{
				return;
			}

			UnhookWindowsHookEx(_keyboardHook);
			_keyboardHook = IntPtr.Zero;
			_keyboardHookProc = null;
		}

		private IntPtr HandleLowLevelKeyboard(int nCode, IntPtr wParam, IntPtr lParam)
		{
			if (nCode >= 0)
			{
				int message = wParam.ToInt32();
				bool keyDown = message == WM_KEYDOWN || message == WM_SYSKEYDOWN;
				bool keyUp = message == WM_KEYUP || message == WM_SYSKEYUP;
				if (keyDown || keyUp)
				{
					KBDLLHOOKSTRUCT keyboard = Marshal.PtrToStructure<KBDLLHOOKSTRUCT>(lParam);
					if (IsSwitchModeHookKey(keyboard.scanCode))
					{
						if (keyDown && !_hookSwitchModeKeyDown)
						{
							_switchModeKeyDownEvent = true;
							_hookSwitchModeKeyDown = true;
						}
						else if (keyUp && _hookSwitchModeKeyDown)
						{
							_switchModeKeyUpEvent = true;
							_hookSwitchModeKeyDown = false;
						}
					}
				}
			}

			return CallNextHookEx(_keyboardHook, nCode, wParam, lParam);
		}

		private bool IsSwitchModeHookKey(uint scanCode)
		{
			return switchModePhysicalScanCode > 0 && scanCode == unchecked((uint)switchModePhysicalScanCode);
		}
#endif

		private void SetCurrentMode(ControlMode mode)
		{
			CurrentMode = mode;
			PortailControlState.IsDesktopControlActive = CurrentMode == ControlMode.PortailControl;
		}

		private void CacheApplicationWindow()
		{
#if UNITY_STANDALONE_WIN || UNITY_EDITOR_WIN
			if (IsValidWindow(_applicationWindow))
			{
				return;
			}

			_applicationWindow = FindOwnProcessWindow();
#endif
		}

		private void TrackExternalForegroundWindow()
		{
#if UNITY_STANDALONE_WIN || UNITY_EDITOR_WIN
			IntPtr foregroundWindow = GetForegroundWindow();
			if (IsValidWindow(foregroundWindow) && foregroundWindow != _applicationWindow)
			{
				_lastExternalForegroundWindow = foregroundWindow;
			}
#endif
		}

		private void FocusPortailTarget()
		{
#if UNITY_STANDALONE_WIN || UNITY_EDITOR_WIN
			IntPtr targetWindow = GetSelectedCaptureWindow();
			if (!IsValidWindow(targetWindow))
			{
				targetWindow = _lastExternalForegroundWindow;
			}

			if (!IsValidWindow(targetWindow) || targetWindow == _applicationWindow)
			{
				Debug.LogWarning("[PortailFocusController] No external window to focus. Click through the application window to choose a desktop target.");
				return;
			}

			SetForegroundWindow(targetWindow);
#endif
		}

		private void ApplyPortailWindowState()
		{
#if UNITY_STANDALONE_WIN || UNITY_EDITOR_WIN
			if (!ShouldApplyWindowStyles() || !IsValidWindow(_applicationWindow))
			{
				return;
			}

			if (!_hasSavedExtendedStyle)
			{
				_savedExtendedStyle = GetWindowLongPtr(_applicationWindow, GWL_EXSTYLE);
				_hasSavedExtendedStyle = true;
			}

			long style = _savedExtendedStyle.ToInt64();
			if (makeNoActivateInPortailControl)
			{
				style |= WS_EX_NOACTIVATE;
			}

			if (makeClickThroughInPortailControl)
			{
				style |= WS_EX_LAYERED;
				style |= WS_EX_TRANSPARENT;
			}

			SetWindowLongPtr(_applicationWindow, GWL_EXSTYLE, new IntPtr(style));
			if (makeClickThroughInPortailControl)
			{
				SetLayeredWindowAttributes(_applicationWindow, 0, 255, LWA_ALPHA);
			}

			ApplyPortailTopmostState();
#endif
		}

		private void ApplyPortailTopmostState()
		{
#if UNITY_STANDALONE_WIN || UNITY_EDITOR_WIN
			if (!ShouldApplyWindowStyles() || !IsValidWindow(_applicationWindow))
			{
				return;
			}

			IntPtr zOrder = makeTopmostInPortailControl ? HWND_TOPMOST : IntPtr.Zero;
			uint flags = SWP_NOMOVE | SWP_NOSIZE | SWP_NOOWNERZORDER | SWP_FRAMECHANGED | SWP_NOACTIVATE | SWP_SHOWWINDOW;
			if (zOrder == IntPtr.Zero)
			{
				flags |= SWP_NOZORDER;
			}
			SetWindowPos(_applicationWindow, zOrder, 0, 0, 0, 0, flags);
#endif
		}

		private void ApplyApplicationWindowState(bool focusApplicationWindow)
		{
#if UNITY_STANDALONE_WIN || UNITY_EDITOR_WIN
			if (!IsValidWindow(_applicationWindow))
			{
				return;
			}

			if (ShouldApplyWindowStyles())
			{
				bool wasTopmost = false;
				if (_hasSavedExtendedStyle)
				{
					wasTopmost = (_savedExtendedStyle.ToInt64() & WS_EX_TOPMOST) != 0;
					SetWindowLongPtr(_applicationWindow, GWL_EXSTYLE, _savedExtendedStyle);
					_hasSavedExtendedStyle = false;
				}

				IntPtr zOrder = wasTopmost ? HWND_TOPMOST : HWND_NOTOPMOST;
				SetWindowPos(_applicationWindow, zOrder, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
			}

			if (focusApplicationWindow)
			{
				FocusApplicationWindow();
			}
#endif
		}

		private void FocusApplicationWindow()
		{
#if UNITY_STANDALONE_WIN || UNITY_EDITOR_WIN
			if (!IsValidWindow(_applicationWindow))
			{
				return;
			}

			IntPtr foregroundWindow = GetForegroundWindow();
			uint currentThreadId = GetCurrentThreadId();
			uint foregroundThreadId = foregroundWindow != IntPtr.Zero
				? GetWindowThreadProcessId(foregroundWindow, out _)
				: 0;

			bool attached = foregroundThreadId != 0 &&
				foregroundThreadId != currentThreadId &&
				AttachThreadInput(currentThreadId, foregroundThreadId, true);

			ShowWindow(_applicationWindow, SW_RESTORE);
			BringWindowToTop(_applicationWindow);
			SetForegroundWindow(_applicationWindow);
			SetActiveWindow(_applicationWindow);
			SetFocus(_applicationWindow);

			if (attached)
			{
				AttachThreadInput(currentThreadId, foregroundThreadId, false);
			}
#endif
		}

		private bool ShouldApplyWindowStyles()
		{
			return applyWindowStyles && (!Application.isEditor || applyWindowStylesInEditor);
		}

		private IntPtr GetSelectedCaptureWindow()
		{
#if UNITY_STANDALONE_WIN || UNITY_EDITOR_WIN
			IPortailCaptureTarget captureTarget = PortailCaptureTargetRegistry.Current;
			if (captureTarget == null ||
				captureTarget.IsDisplayCaptureSelected ||
				captureTarget.SelectedWindowHandle == 0)
			{
				return IntPtr.Zero;
			}

			return new IntPtr(unchecked((long)captureTarget.SelectedWindowHandle));
#else
			return IntPtr.Zero;
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

		private static IntPtr GetWindowLongPtr(IntPtr hWnd, int nIndex)
		{
			return IntPtr.Size == 8
				? GetWindowLongPtr64(hWnd, nIndex)
				: new IntPtr(GetWindowLong32(hWnd, nIndex));
		}

		private static IntPtr SetWindowLongPtr(IntPtr hWnd, int nIndex, IntPtr dwNewLong)
		{
			return IntPtr.Size == 8
				? SetWindowLongPtr64(hWnd, nIndex, dwNewLong)
				: new IntPtr(SetWindowLong32(hWnd, nIndex, dwNewLong.ToInt32()));
		}
#endif

		private void SetDesktopCapturePaused(bool paused)
		{
			// Better than destroying/recreating the capture session:
			// - stop applying new frames to the in-game desktop texture
			// - fade/hide the capture layer
			// - optionally keep showing the last safe frame blurred/dimmed
		}

		private void WarpCursorToLookedDesktopPoint()
		{
			if (!TryGetLookedDesktopScreenPoint(out int screenX, out int screenY))
			{
				return;
			}

			SetSystemCursorPosition(screenX, screenY);
		}

		private void SetSystemCursorPosition(int screenX, int screenY)
		{
#if UNITY_STANDALONE_WIN || UNITY_EDITOR_WIN
			if (!SetCursorPos(screenX, screenY))
			{
				Debug.LogWarning($"[PortailFocusController] SetCursorPos failed: {Marshal.GetLastWin32Error()}.");
			}
#endif
		}

		private bool TryGetLookedDesktopScreenPoint(out int screenX, out int screenY)
		{
			screenX = 0;
			screenY = 0;

			IPortailCaptureTarget captureTarget = PortailCaptureTargetRegistry.Current;
			if (captureTarget == null ||
				!captureTarget.TryGetSelectedCaptureScreenRect(out RectInt captureRect) ||
				captureRect.width <= 0 ||
				captureRect.height <= 0)
			{
				return false;
			}

			Camera camera = Camera.main;
			if (camera == null)
			{
				return false;
			}

			Ray ray = camera.ViewportPointToRay(new Vector3(0.5f, 0.5f, 0f));

			if (!Physics.Raycast(
					ray,
					out RaycastHit hit,
					Mathf.Max(0.01f, 1000.0f),
					portailScreenLayerMask,
					QueryTriggerInteraction.Ignore))
			{
				return false;
			}

			if (!IsValidDesktopScreenHit(hit))
			{
				return false;
			}

			if (!TryGetScreenUvFromBoxColliderHit(hit, out Vector2 uv))
			{
				return false;
			}

			uv.x = Mathf.Clamp01(uv.x);
			uv.y = Mathf.Clamp01(uv.y);

			int x = captureRect.x + Mathf.RoundToInt(uv.x * (captureRect.width - 1));

			// Windows screen coordinates are top-left origin.
			// Unity mesh UVs are normally bottom-left origin, so V needs 1 - uv.y.
			int y = captureRect.y + Mathf.RoundToInt((1f - uv.y) * (captureRect.height - 1));

			screenX = Mathf.Clamp(x, captureRect.x, captureRect.xMax - 1);
			screenY = Mathf.Clamp(y, captureRect.y, captureRect.yMax - 1);
			return true;
		}

		private bool IsValidDesktopScreenHit(RaycastHit hit)
		{
			if (hit.collider == null)
			{
				return false;
			}

			PortailFeedPlayback streamPlayer =
				hit.collider.GetComponentInParent<PortailFeedPlayback>();

			MeshRenderer screenRenderer = null;

			if (streamPlayer != null)
			{
				screenRenderer = streamPlayer.ScreenDisplaySurface;
			}

			if (screenRenderer == null)
			{
				return false;
			}

			PortailFeed source = streamPlayer != null ? streamPlayer.StreamSource : null;
			if (source == null || !source.CurrentStreamInfo.isLocalSource)
			{
				return false;
			}

			Transform hitTransform = hit.collider.transform;
			Transform screenTransform = screenRenderer.transform;

			return hitTransform == screenTransform || hitTransform.IsChildOf(screenTransform);
		}

		private bool TryGetScreenUvFromBoxColliderHit(RaycastHit hit, out Vector2 uv)
		{
			uv = default;

			if (hit.collider == null)
			{
				return false;
			}

			BoxCollider box = hit.collider as BoxCollider;
			if (box == null)
			{
				Debug.LogWarning($"[PortailFocusController] Cursor warp hit '{hit.collider.name}', but it is not a BoxCollider.");
				return false;
			}

			Transform boxTransform = box.transform;

			// Convert the world hit point into the BoxCollider object's local space.
			Vector3 localPoint = boxTransform.InverseTransformPoint(hit.point);

			Vector3 center = box.center;
			Vector3 size = box.size;

			if (Mathf.Abs(size.x) < 0.000001f || Mathf.Abs(size.y) < 0.000001f)
			{
				Debug.LogWarning($"[PortailFocusController] Cursor warp BoxCollider '{box.name}' has invalid XY size: {size}.");
				return false;
			}

			float minX = center.x - size.x * 0.5f;
			float maxX = center.x + size.x * 0.5f;
			float minY = center.y - size.y * 0.5f;
			float maxY = center.y + size.y * 0.5f;

			float u = Mathf.InverseLerp(minX, maxX, localPoint.x);
			float v = Mathf.InverseLerp(minY, maxY, localPoint.y);

			uv = new Vector2(u, v);

			Debug.Log(
				$"[PortailFocusController] Cursor warp BoxUV hit={hit.collider.name}, " +
				$"localPoint={localPoint}, center={center}, size={size}, uv={uv}");

			return true;
		}
	}
}
