using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Runtime.InteropServices;
using System.Text;
using UnityEngine;
using Debug = UnityEngine.Debug;

namespace Portail.Control
{
	public sealed class PortailShellPopupDetector : MonoBehaviour
	{
		public bool IsShellPopupActive { get; private set; }

		// Call your capture pause/fade logic from here.
		public Action<bool> onShellPopupActiveChanged;

		private readonly HashSet<IntPtr> _activeBlockers = new();
		private WinEventDelegate _winEventProc;
		private IntPtr _hook;
		private float _nextSweepAt;

#if UNITY_STANDALONE_WIN || UNITY_EDITOR_WIN
	private const uint EVENT_OBJECT_CREATE = 0x8000;
	private const uint EVENT_OBJECT_DESTROY = 0x8001;
	private const uint EVENT_OBJECT_SHOW = 0x8002;
	private const uint EVENT_OBJECT_HIDE = 0x8003;

	private const int OBJID_WINDOW = 0;
	private const int CHILDID_SELF = 0;

	private const uint WINEVENT_OUTOFCONTEXT = 0x0000;
	private const uint WINEVENT_SKIPOWNPROCESS = 0x0002;

	private delegate void WinEventDelegate(
		IntPtr hWinEventHook,
		uint eventType,
		IntPtr hwnd,
		int idObject,
		int idChild,
		uint dwEventThread,
		uint dwmsEventTime);

	[DllImport("user32.dll")]
	private static extern IntPtr SetWinEventHook(
		uint eventMin,
		uint eventMax,
		IntPtr hmodWinEventProc,
		WinEventDelegate lpfnWinEventProc,
		uint idProcess,
		uint idThread,
		uint dwFlags);

	[DllImport("user32.dll")]
	private static extern bool UnhookWinEvent(IntPtr hWinEventHook);

	[DllImport("user32.dll")]
	private static extern bool IsWindow(IntPtr hWnd);

	[DllImport("user32.dll")]
	private static extern bool IsWindowVisible(IntPtr hWnd);

	[DllImport("user32.dll", CharSet = CharSet.Unicode)]
	private static extern int GetClassName(IntPtr hWnd, StringBuilder lpClassName, int nMaxCount);

	[DllImport("user32.dll", CharSet = CharSet.Unicode)]
	private static extern int GetWindowText(IntPtr hWnd, StringBuilder lpString, int nMaxCount);

	[DllImport("user32.dll")]
	private static extern uint GetWindowThreadProcessId(IntPtr hWnd, out uint processId);
#endif

		private void OnEnable()
	{
#if UNITY_STANDALONE_WIN || UNITY_EDITOR_WIN
		_winEventProc = OnWinEvent;

		_hook = SetWinEventHook(
			EVENT_OBJECT_CREATE,
			EVENT_OBJECT_HIDE,
			IntPtr.Zero,
			_winEventProc,
			0,
			0,
			WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);

		if (_hook == IntPtr.Zero)
		{
			Debug.LogWarning("[ShellPopupPauseDetector] SetWinEventHook failed.");
		}
#endif
	}

		private void OnDisable()
	{
#if UNITY_STANDALONE_WIN || UNITY_EDITOR_WIN
		if (_hook != IntPtr.Zero)
		{
			UnhookWinEvent(_hook);
			_hook = IntPtr.Zero;
		}

		_winEventProc = null;
#endif

		_activeBlockers.Clear();
		SetShellPopupActive(false);
	}

		private void Update()
	{
#if UNITY_STANDALONE_WIN || UNITY_EDITOR_WIN
		// Clean up missed hide/destroy events.
		if (Time.unscaledTime >= _nextSweepAt)
		{
			_nextSweepAt = Time.unscaledTime + 0.10f;
			SweepInactiveBlockers();
		}

		SetShellPopupActive(_activeBlockers.Count > 0);
#endif
	}

#if UNITY_STANDALONE_WIN || UNITY_EDITOR_WIN
	private void OnWinEvent(
		IntPtr hWinEventHook,
		uint eventType,
		IntPtr hwnd,
		int idObject,
		int idChild,
		uint dwEventThread,
		uint dwmsEventTime)
	{
		if (hwnd == IntPtr.Zero || idObject != OBJID_WINDOW || idChild != CHILDID_SELF)
			return;

		if (eventType == EVENT_OBJECT_SHOW || eventType == EVENT_OBJECT_CREATE)
		{
			if (LooksLikeBlockingShellPopup(hwnd))
			{
				_activeBlockers.Add(hwnd);
			}
		}
		else if (eventType == EVENT_OBJECT_HIDE || eventType == EVENT_OBJECT_DESTROY)
		{
			_activeBlockers.Remove(hwnd);
		}
	}

	private void SweepInactiveBlockers()
	{
		var dead = new List<IntPtr>();

		foreach (IntPtr hwnd in _activeBlockers)
		{
			if (!IsWindow(hwnd) || !IsWindowVisible(hwnd) || !LooksLikeBlockingShellPopup(hwnd))
			{
				dead.Add(hwnd);
			}
		}

		foreach (IntPtr hwnd in dead)
		{
			_activeBlockers.Remove(hwnd);
		}
	}

	private static bool LooksLikeBlockingShellPopup(IntPtr hwnd)
	{
		if (!IsWindow(hwnd) || !IsWindowVisible(hwnd))
			return false;

		string className = GetClassNameSafe(hwnd);
		string title = GetWindowTextSafe(hwnd);
		string processName = GetProcessNameSafe(hwnd);

		// Taskbar hover thumbnails.
		// Empirical class name used by Windows taskbar thumbnail surfaces.
		if (className == "TaskListThumbnailWnd")
			return true;

		// Windows 11 shell/XAML surfaces. These names are empirical and can change
		// across Windows builds, so keep a diagnostic logger during development.
		if (processName.Equals("StartMenuExperienceHost", StringComparison.OrdinalIgnoreCase))
			return true;

		if (processName.Equals("SearchHost", StringComparison.OrdinalIgnoreCase))
			return true;

		if (processName.Equals("ShellExperienceHost", StringComparison.OrdinalIgnoreCase))
			return true;

		// Task View / some Win11 shell islands.
		if (className == "XamlExplorerHostIslandWindow")
			return true;

		// Control Center, Notification Center, Start/Search on some builds.
		if (className == "Windows.UI.Core.CoreWindow")
		{
			if (title.Contains("Start", StringComparison.OrdinalIgnoreCase) ||
				title.Contains("Search", StringComparison.OrdinalIgnoreCase) ||
				title.Contains("Control Center", StringComparison.OrdinalIgnoreCase) ||
				title.Contains("Notification Center", StringComparison.OrdinalIgnoreCase))
			{
				return true;
			}
		}

		return false;
	}

	private static string GetClassNameSafe(IntPtr hwnd)
	{
		var sb = new StringBuilder(256);
		return GetClassName(hwnd, sb, sb.Capacity) > 0 ? sb.ToString() : "";
	}

	private static string GetWindowTextSafe(IntPtr hwnd)
	{
		var sb = new StringBuilder(256);
		return GetWindowText(hwnd, sb, sb.Capacity) > 0 ? sb.ToString() : "";
	}

	private static string GetProcessNameSafe(IntPtr hwnd)
	{
		try
		{
			GetWindowThreadProcessId(hwnd, out uint pid);
			if (pid == 0)
				return "";

			return Process.GetProcessById(unchecked((int)pid)).ProcessName;
		}
		catch
		{
			return "";
		}
	}
#endif

		private void SetShellPopupActive(bool active)
		{
			if (IsShellPopupActive == active)
				return;

			IsShellPopupActive = active;
			onShellPopupActiveChanged?.Invoke(active);
		}
	}
}
