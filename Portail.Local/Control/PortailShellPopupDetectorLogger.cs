using System;
using System.Collections.Concurrent;
using System.Collections.Generic;
using System.Diagnostics;
using System.Runtime.InteropServices;
using System.Text;
using UnityEngine;
using Debug = UnityEngine.Debug;

namespace Portail.Control
{
	[DefaultExecutionOrder(-9400)]
	public sealed class PortailShellPopupDetectorLogger : MonoBehaviour
	{
		[Header("Logging")]
		public bool logEvents = true;
		public bool logOnlySuspiciousWindows = true;
		public bool logCreateDestroy = true;
		public bool logShowHide = true;
		public bool logForegroundChanges = true;

		[Header("Manual snapshot")]
		public bool enableSnapshotHotkey = true;
		public KeyCode snapshotHotkey = KeyCode.F9;
		public bool snapshotOnlyVisibleWindows = true;

		[Header("Spam control")]
		[Tooltip("Max WinEvent messages processed per Unity frame.")]
		public int maxEventsPerFrame = 64;

		[Tooltip("Suppress repeated logs for the same hwnd/event for this many seconds.")]
		public float duplicateLogCooldownSeconds = 0.50f;

		private readonly ConcurrentQueue<RawWinEvent> _pendingEvents = new();
		private readonly Dictionary<string, double> _lastLogTimeByKey = new();

#if UNITY_STANDALONE_WIN || UNITY_EDITOR_WIN
	private WinEventDelegate _winEventProc;
	private IntPtr _objectHook;
	private IntPtr _foregroundHook;

	private const uint EVENT_SYSTEM_FOREGROUND = 0x0003;

	private const uint EVENT_OBJECT_CREATE = 0x8000;
	private const uint EVENT_OBJECT_DESTROY = 0x8001;
	private const uint EVENT_OBJECT_SHOW = 0x8002;
	private const uint EVENT_OBJECT_HIDE = 0x8003;

	private const int OBJID_WINDOW = 0;
	private const int CHILDID_SELF = 0;

	private const uint WINEVENT_OUTOFCONTEXT = 0x0000;
	private const uint WINEVENT_SKIPOWNPROCESS = 0x0002;

	private const uint GA_ROOT = 2;

	private const int GWL_STYLE = -16;
	private const int GWL_EXSTYLE = -20;

	private const long WS_VISIBLE = 0x10000000L;
	private const long WS_POPUP = unchecked((long)0x80000000);
	private const long WS_CHILD = 0x40000000L;

	private const long WS_EX_TOPMOST = 0x00000008L;
	private const long WS_EX_TOOLWINDOW = 0x00000080L;
	private const long WS_EX_LAYERED = 0x00080000L;
	private const long WS_EX_NOACTIVATE = 0x08000000L;

	private delegate void WinEventDelegate(
		IntPtr hWinEventHook,
		uint eventType,
		IntPtr hwnd,
		int idObject,
		int idChild,
		uint dwEventThread,
		uint dwmsEventTime);

	private delegate bool EnumWindowsProc(IntPtr hwnd, IntPtr lParam);

	[StructLayout(LayoutKind.Sequential)]
	private struct RECT
	{
		public int left;
		public int top;
		public int right;
		public int bottom;

		public int Width => right - left;
		public int Height => bottom - top;
	}

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
	private static extern bool IsWindow(IntPtr hwnd);

	[DllImport("user32.dll")]
	private static extern bool IsWindowVisible(IntPtr hwnd);

	[DllImport("user32.dll")]
	private static extern IntPtr GetAncestor(IntPtr hwnd, uint gaFlags);

	[DllImport("user32.dll", CharSet = CharSet.Unicode)]
	private static extern int GetClassName(IntPtr hwnd, StringBuilder className, int maxCount);

	[DllImport("user32.dll", CharSet = CharSet.Unicode)]
	private static extern int GetWindowText(IntPtr hwnd, StringBuilder text, int maxCount);

	[DllImport("user32.dll")]
	private static extern uint GetWindowThreadProcessId(IntPtr hwnd, out uint processId);

	[DllImport("user32.dll")]
	private static extern bool GetWindowRect(IntPtr hwnd, out RECT rect);

	[DllImport("user32.dll")]
	private static extern bool EnumWindows(EnumWindowsProc enumFunc, IntPtr lParam);

	[DllImport("user32.dll", EntryPoint = "GetWindowLong")]
	private static extern int GetWindowLong32(IntPtr hwnd, int index);

	[DllImport("user32.dll", EntryPoint = "GetWindowLongPtr")]
	private static extern IntPtr GetWindowLongPtr64(IntPtr hwnd, int index);
#endif

	private struct RawWinEvent
	{
		public uint eventType;
		public IntPtr hwnd;
		public int idObject;
		public int idChild;
	}

	private void OnEnable()
	{
#if UNITY_STANDALONE_WIN || UNITY_EDITOR_WIN
		_winEventProc = OnWinEvent;

		if (logCreateDestroy || logShowHide)
		{
			_objectHook = SetWinEventHook(
				EVENT_OBJECT_CREATE,
				EVENT_OBJECT_HIDE,
				IntPtr.Zero,
				_winEventProc,
				0,
				0,
				WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);

			if (_objectHook == IntPtr.Zero)
			{
				Debug.LogWarning("[ShellPopupWindowLogger] Failed to install object WinEvent hook.");
			}
		}

		if (logForegroundChanges)
		{
			_foregroundHook = SetWinEventHook(
				EVENT_SYSTEM_FOREGROUND,
				EVENT_SYSTEM_FOREGROUND,
				IntPtr.Zero,
				_winEventProc,
				0,
				0,
				WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);

			if (_foregroundHook == IntPtr.Zero)
			{
				Debug.LogWarning("[ShellPopupWindowLogger] Failed to install foreground WinEvent hook.");
			}
		}

		Debug.Log("[ShellPopupWindowLogger] Enabled.");
#endif
	}

	private void OnDisable()
	{
#if UNITY_STANDALONE_WIN || UNITY_EDITOR_WIN
		if (_objectHook != IntPtr.Zero)
		{
			UnhookWinEvent(_objectHook);
			_objectHook = IntPtr.Zero;
		}

		if (_foregroundHook != IntPtr.Zero)
		{
			UnhookWinEvent(_foregroundHook);
			_foregroundHook = IntPtr.Zero;
		}

		_winEventProc = null;
		_pendingEvents.Clear();
		_lastLogTimeByKey.Clear();

		Debug.Log("[ShellPopupWindowLogger] Disabled.");
#endif
	}

	private void Update()
	{
#if UNITY_STANDALONE_WIN || UNITY_EDITOR_WIN
		if (enableSnapshotHotkey && Input.GetKeyDown(snapshotHotkey))
		{
			DumpVisibleTopLevelWindows("manual snapshot");
		}

		if (!logEvents)
		{
			return;
		}

		int processed = 0;
		while (processed < maxEventsPerFrame && _pendingEvents.TryDequeue(out RawWinEvent rawEvent))
		{
			processed++;
			ProcessWinEvent(rawEvent);
		}
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
		if (hwnd == IntPtr.Zero)
			return;

		_pendingEvents.Enqueue(new RawWinEvent
		{
			eventType = eventType,
			hwnd = hwnd,
			idObject = idObject,
			idChild = idChild
		});
	}

	private void ProcessWinEvent(RawWinEvent rawEvent)
	{
		if (rawEvent.eventType != EVENT_SYSTEM_FOREGROUND)
		{
			if (rawEvent.idObject != OBJID_WINDOW || rawEvent.idChild != CHILDID_SELF)
				return;
		}

		if (!ShouldLogEventType(rawEvent.eventType))
			return;

		WindowInfo info = ReadWindowInfo(rawEvent.hwnd);
		if (!info.exists)
			return;

		if (!info.isTopLevelish)
			return;

		string reason = GetSuspiciousReason(info);
		bool suspicious = !string.IsNullOrEmpty(reason);

		if (logOnlySuspiciousWindows && !suspicious)
			return;

		string eventName = EventName(rawEvent.eventType);
		string key = $"{rawEvent.hwnd.ToInt64():X}:{eventName}:{info.className}:{info.processName}:{info.title}";

		if (IsDuplicateSuppressed(key))
			return;

		Debug.Log(
			"[ShellPopupWindowLogger] " + eventName +
			"\n" + FormatWindowInfo(info, reason));
	}

	private bool ShouldLogEventType(uint eventType)
	{
		return eventType switch
		{
			EVENT_OBJECT_CREATE => logCreateDestroy,
			EVENT_OBJECT_DESTROY => logCreateDestroy,
			EVENT_OBJECT_SHOW => logShowHide,
			EVENT_OBJECT_HIDE => logShowHide,
			EVENT_SYSTEM_FOREGROUND => logForegroundChanges,
			_ => false
		};
	}

	public void DumpVisibleTopLevelWindows(string label = "snapshot")
	{
		Debug.Log($"[ShellPopupWindowLogger] Dumping top-level windows: {label}");

		EnumWindows((hwnd, _) =>
		{
			WindowInfo info = ReadWindowInfo(hwnd);

			if (!info.exists)
				return true;

			if (!info.isTopLevelish)
				return true;

			if (snapshotOnlyVisibleWindows && !info.visible)
				return true;

			string reason = GetSuspiciousReason(info);

			if (logOnlySuspiciousWindows && string.IsNullOrEmpty(reason))
				return true;

			Debug.Log("[ShellPopupWindowLogger] SNAPSHOT\n" + FormatWindowInfo(info, reason));
			return true;
		}, IntPtr.Zero);
	}

	private bool IsDuplicateSuppressed(string key)
	{
		double now = Time.realtimeSinceStartupAsDouble;

		if (_lastLogTimeByKey.TryGetValue(key, out double lastTime))
		{
			if (now - lastTime < duplicateLogCooldownSeconds)
				return true;
		}

		_lastLogTimeByKey[key] = now;
		return false;
	}

	private static string FormatWindowInfo(WindowInfo info, string reason)
	{
		return
			$"reason={Safe(reason)}\n" +
			$"hwnd=0x{info.hwnd.ToInt64():X}\n" +
			$"class={Safe(info.className)}\n" +
			$"title={Safe(info.title)}\n" +
			$"process={Safe(info.processName)} pid={info.processId}\n" +
			$"visible={info.visible} topLevelish={info.isTopLevelish}\n" +
			$"rect=({info.rect.left},{info.rect.top}) {info.rect.Width}x{info.rect.Height}\n" +
			$"style=0x{info.style:X16} exStyle=0x{info.exStyle:X16}\n" +
			$"flags={FormatFlags(info)}";
	}

	private static string FormatFlags(WindowInfo info)
	{
		List<string> flags = new();

		if ((info.style & WS_VISIBLE) != 0) flags.Add("WS_VISIBLE");
		if ((info.style & WS_POPUP) != 0) flags.Add("WS_POPUP");
		if ((info.style & WS_CHILD) != 0) flags.Add("WS_CHILD");

		if ((info.exStyle & WS_EX_TOPMOST) != 0) flags.Add("WS_EX_TOPMOST");
		if ((info.exStyle & WS_EX_TOOLWINDOW) != 0) flags.Add("WS_EX_TOOLWINDOW");
		if ((info.exStyle & WS_EX_LAYERED) != 0) flags.Add("WS_EX_LAYERED");
		if ((info.exStyle & WS_EX_NOACTIVATE) != 0) flags.Add("WS_EX_NOACTIVATE");

		return flags.Count > 0 ? string.Join(", ", flags) : "none";
	}

	private static string GetSuspiciousReason(WindowInfo info)
	{
		string c = info.className;
		string p = info.processName;
		string t = info.title;

		if (c == "TaskListThumbnailWnd")
			return "taskbar thumbnail preview";

		if (c == "XamlExplorerHostIslandWindow")
			return "Windows shell XAML island / task view / shell popup candidate";

		if (c == "Windows.UI.Core.CoreWindow")
			return "Windows shell CoreWindow candidate";

		if (p.Equals("StartMenuExperienceHost", StringComparison.OrdinalIgnoreCase))
			return "Start menu process";

		if (p.Equals("SearchHost", StringComparison.OrdinalIgnoreCase))
			return "Windows Search process";

		if (p.Equals("ShellExperienceHost", StringComparison.OrdinalIgnoreCase))
			return "Shell Experience Host process";

		if (p.Equals("explorer", StringComparison.OrdinalIgnoreCase) &&
			((info.exStyle & WS_EX_TOPMOST) != 0 || (info.exStyle & WS_EX_TOOLWINDOW) != 0) &&
			((info.style & WS_POPUP) != 0))
		{
			return "topmost/tool popup owned by explorer";
		}

		if (t.Contains("Start", StringComparison.OrdinalIgnoreCase) &&
			p.Contains("Start", StringComparison.OrdinalIgnoreCase))
		{
			return "Start-like title/process";
		}

		if (t.Contains("Notification", StringComparison.OrdinalIgnoreCase) ||
			t.Contains("Control Center", StringComparison.OrdinalIgnoreCase))
		{
			return "notification/control center title";
		}

		return "";
	}

	private static WindowInfo ReadWindowInfo(IntPtr hwnd)
	{
		WindowInfo info = new();
		info.hwnd = hwnd;

		if (hwnd == IntPtr.Zero || !IsWindow(hwnd))
			return info;

		info.exists = true;
		info.visible = IsWindowVisible(hwnd);
		info.className = GetClassNameSafe(hwnd);
		info.title = GetWindowTextSafe(hwnd);
		info.processId = GetProcessId(hwnd);
		info.processName = GetProcessNameSafe(info.processId);
		info.style = GetWindowLongPtr(hwnd, GWL_STYLE).ToInt64();
		info.exStyle = GetWindowLongPtr(hwnd, GWL_EXSTYLE).ToInt64();

		if (GetWindowRect(hwnd, out RECT rect))
		{
			info.rect = rect;
		}

		IntPtr root = GetAncestor(hwnd, GA_ROOT);
		info.isTopLevelish = root == hwnd || root == IntPtr.Zero;

		return info;
	}

	private static string GetClassNameSafe(IntPtr hwnd)
	{
		StringBuilder sb = new(512);
		int len = GetClassName(hwnd, sb, sb.Capacity);
		return len > 0 ? sb.ToString() : "";
	}

	private static string GetWindowTextSafe(IntPtr hwnd)
	{
		StringBuilder sb = new(512);
		int len = GetWindowText(hwnd, sb, sb.Capacity);
		return len > 0 ? sb.ToString() : "";
	}

	private static uint GetProcessId(IntPtr hwnd)
	{
		GetWindowThreadProcessId(hwnd, out uint pid);
		return pid;
	}

	private static string GetProcessNameSafe(uint pid)
	{
		if (pid == 0)
			return "";

		try
		{
			return Process.GetProcessById(unchecked((int)pid)).ProcessName;
		}
		catch
		{
			return "";
		}
	}

	private static IntPtr GetWindowLongPtr(IntPtr hwnd, int index)
	{
		return IntPtr.Size == 8
			? GetWindowLongPtr64(hwnd, index)
			: new IntPtr(GetWindowLong32(hwnd, index));
	}

	private static string EventName(uint eventType)
	{
		return eventType switch
		{
			EVENT_SYSTEM_FOREGROUND => "FOREGROUND",
			EVENT_OBJECT_CREATE => "CREATE",
			EVENT_OBJECT_DESTROY => "DESTROY",
			EVENT_OBJECT_SHOW => "SHOW",
			EVENT_OBJECT_HIDE => "HIDE",
			_ => $"EVENT_0x{eventType:X}"
		};
	}

	private static string Safe(string value)
	{
		return string.IsNullOrEmpty(value) ? "<empty>" : value;
	}

	private struct WindowInfo
	{
		public bool exists;
		public IntPtr hwnd;
		public string className;
		public string title;
		public uint processId;
		public string processName;
		public bool visible;
		public bool isTopLevelish;
		public long style;
		public long exStyle;
		public RECT rect;
	}
#endif
	}
}
