using System;
using System.Collections.Concurrent;
using System.Runtime.InteropServices;
using System.Threading;
using UnityEngine;

namespace Portail.Capture
{
	public enum PortailCaptureNativeConsoleLogLevel
	{
		All = 0,
		[InspectorName("Errors & Warnings")]
		ErrorsAndWarnings = 1,
		Errors = 2,
	}

	internal static class PortailCaptureNativeLogBridge
	{
		private enum NativeLogLevel
		{
			Info = 0,
			Warning = 1,
			Error = 2,
		}

		[UnmanagedFunctionPointer(CallingConvention.Cdecl)]
		private delegate void NativeLogCallback(int level, IntPtr messagePtr);

		private readonly struct PendingLog
		{
			public PendingLog(LogType type, string message)
			{
				Type = type;
				Message = message;
			}

			public LogType Type { get; }
			public string Message { get; }
		}

		private static readonly ConcurrentQueue<PendingLog> PendingLogs = new ConcurrentQueue<PendingLog>();
		private static readonly NativeLogCallback Callback = OnNativeLog;
		private static readonly object RegistrationLock = new object();
		private static int _consoleLogLevel = (int)PortailCaptureNativeConsoleLogLevel.All;
		private static bool _captureCallbackRegistered;
		private static PortailCaptureNativeLogPump _pump;

		[DllImport(PortailCaptureNativeBridge.DllName, CallingConvention = CallingConvention.Cdecl)]
		private static extern void SSPCAP_SetLogCallback(NativeLogCallback callback);

		public static PortailCaptureNativeConsoleLogLevel ConsoleLogLevel
		{
			get => (PortailCaptureNativeConsoleLogLevel)Volatile.Read(ref _consoleLogLevel);
			set => Volatile.Write(ref _consoleLogLevel, (int)value);
		}

		public static void RegisterCaptureCallback()
		{
			lock (RegistrationLock)
			{
				_captureCallbackRegistered = true;
				ApplyCaptureCallback();
				EnsurePump();
			}
		}

		public static void UnregisterCaptureCallback()
		{
			lock (RegistrationLock)
			{
				if (!_captureCallbackRegistered)
				{
					return;
				}

				_captureCallbackRegistered = false;
				ApplyCaptureCallback();
			}
		}

		public static void FlushPending()
		{
			while (PendingLogs.TryDequeue(out PendingLog entry))
			{
				switch (entry.Type)
				{
					case LogType.Warning:
						Debug.LogWarning(entry.Message);
						break;
					case LogType.Error:
					case LogType.Exception:
					case LogType.Assert:
						Debug.LogError(entry.Message);
						break;
					default:
						Debug.Log(entry.Message);
						break;
				}
			}
		}

		private static void OnNativeLog(int level, IntPtr messagePtr)
		{
			try
			{
				string message = messagePtr == IntPtr.Zero ? string.Empty : Marshal.PtrToStringAnsi(messagePtr) ?? string.Empty;
				if (string.IsNullOrWhiteSpace(message))
				{
					return;
				}

				LogType logType = level switch
				{
					(int)NativeLogLevel.Warning => LogType.Warning,
					(int)NativeLogLevel.Error => LogType.Error,
					_ => LogType.Log,
				};

				if (!ShouldForwardToConsole(logType))
				{
					return;
				}

				PendingLogs.Enqueue(new PendingLog(logType, message));
			}
			catch
			{
				// Native log forwarding should never interrupt capture threads.
			}
		}

		private static bool ShouldForwardToConsole(LogType logType)
		{
			PortailCaptureNativeConsoleLogLevel level = ConsoleLogLevel;
			bool isError = logType == LogType.Error || logType == LogType.Exception || logType == LogType.Assert;

			return level switch
			{
				PortailCaptureNativeConsoleLogLevel.Errors => isError,
				PortailCaptureNativeConsoleLogLevel.ErrorsAndWarnings => isError || logType == LogType.Warning,
				_ => true,
			};
		}

		private static void ApplyCaptureCallback()
		{
			try
			{
				SSPCAP_SetLogCallback(_captureCallbackRegistered ? Callback : null);
			}
			catch (Exception ex) when (ex is DllNotFoundException || ex is EntryPointNotFoundException || ex is BadImageFormatException)
			{
				// Loader reports the concrete failure when capture starts.
			}
		}

		private static void EnsurePump()
		{
			if (_pump != null)
			{
				return;
			}

#if UNITY_2023_1_OR_NEWER
			_pump = UnityEngine.Object.FindFirstObjectByType<PortailCaptureNativeLogPump>();
#else
			_pump = UnityEngine.Object.FindObjectOfType<PortailCaptureNativeLogPump>();
#endif
			if (_pump != null)
			{
				return;
			}

			GameObject pumpObject = new GameObject("Portail Capture Native Log Pump")
			{
				hideFlags = HideFlags.HideAndDontSave,
			};
			UnityEngine.Object.DontDestroyOnLoad(pumpObject);
			_pump = pumpObject.AddComponent<PortailCaptureNativeLogPump>();
		}
	}

	[DefaultExecutionOrder(-32000)]
	internal sealed class PortailCaptureNativeLogPump : MonoBehaviour
	{
		private void LateUpdate()
		{
			PortailCaptureNativeLogBridge.FlushPending();
		}

		private void OnDisable()
		{
			PortailCaptureNativeLogBridge.FlushPending();
		}
	}
}
