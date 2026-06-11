using System;
using System.Collections.Concurrent;
using System.Runtime.InteropServices;
using System.Threading;
using UnityEngine;

namespace Portail.Stream
{
	public enum PortailStreamNativeConsoleLogLevel
	{
		All = 0,
		[InspectorName("Errors & Warnings")]
		ErrorsAndWarnings = 1,
		Errors = 2,
	}

	public static class PortailStreamNativeLogBridge
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
		private static readonly NativeLogCallback HostCallback = OnNativeHostLog;
		private static readonly NativeLogCallback ClientCallback = OnNativeClientLog;
		private static readonly object RegistrationLock = new object();
		private static int _hostConsoleLogLevel = (int)PortailStreamNativeConsoleLogLevel.All;
		private static int _clientConsoleLogLevel = (int)PortailStreamNativeConsoleLogLevel.All;
		private static bool _hostCallbackRegistered;
		private static bool _clientCallbackRegistered;
		private static PortailStreamNativeLogPump _pump;

		[DllImport("portail_stream_host_plugin", CallingConvention = CallingConvention.Cdecl)]
		private static extern void SSPH_SetLogCallback(NativeLogCallback callback);

		[DllImport("portail_stream_client_plugin", CallingConvention = CallingConvention.Cdecl)]
		private static extern void SSPC_SetLogCallback(NativeLogCallback callback);

		public static PortailStreamNativeConsoleLogLevel HostConsoleLogLevel
		{
			get => (PortailStreamNativeConsoleLogLevel)Volatile.Read(ref _hostConsoleLogLevel);
			set => Volatile.Write(ref _hostConsoleLogLevel, (int)value);
		}

		public static PortailStreamNativeConsoleLogLevel ClientConsoleLogLevel
		{
			get => (PortailStreamNativeConsoleLogLevel)Volatile.Read(ref _clientConsoleLogLevel);
			set => Volatile.Write(ref _clientConsoleLogLevel, (int)value);
		}

		public static void RegisterHostCallback()
		{
			lock (RegistrationLock)
			{
				_hostCallbackRegistered = true;
				ApplyHostCallback();
				EnsurePump();
			}
		}

		public static void UnregisterHostCallback()
		{
			lock (RegistrationLock)
			{
				if (!_hostCallbackRegistered)
				{
					return;
				}

				_hostCallbackRegistered = false;
				ApplyHostCallback();
			}
		}

		public static void RegisterClientCallback()
		{
			lock (RegistrationLock)
			{
				_clientCallbackRegistered = true;
				ApplyClientCallback();
				EnsurePump();
			}
		}

		public static void UnregisterClientCallback()
		{
			lock (RegistrationLock)
			{
				if (!_clientCallbackRegistered)
				{
					return;
				}

				_clientCallbackRegistered = false;
				ApplyClientCallback();
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

		private static void OnNativeHostLog(int level, IntPtr messagePtr)
		{
			OnNativeLog(level, messagePtr, HostConsoleLogLevel);
		}

		private static void OnNativeClientLog(int level, IntPtr messagePtr)
		{
			OnNativeLog(level, messagePtr, ClientConsoleLogLevel);
		}

		private static void OnNativeLog(int level, IntPtr messagePtr, PortailStreamNativeConsoleLogLevel consoleLogLevel)
		{
			try
			{
				string message = messagePtr == IntPtr.Zero ? string.Empty : Marshal.PtrToStringAnsi(messagePtr) ?? string.Empty;
				EnqueueLog(level, message, consoleLogLevel);
			}
			catch
			{
				// Native log forwarding should never interrupt the streaming threads.
			}
		}

		private static void EnqueueLog(int level, string message, PortailStreamNativeConsoleLogLevel consoleLogLevel)
		{
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

			if (!ShouldForwardToConsole(logType, consoleLogLevel))
			{
				return;
			}

			PendingLogs.Enqueue(new PendingLog(logType, message));
		}

		private static bool ShouldForwardToConsole(LogType logType, PortailStreamNativeConsoleLogLevel consoleLogLevel)
		{
			bool isError = logType == LogType.Error || logType == LogType.Exception || logType == LogType.Assert;

			return consoleLogLevel switch
			{
				PortailStreamNativeConsoleLogLevel.Errors => isError,
				PortailStreamNativeConsoleLogLevel.ErrorsAndWarnings => isError || logType == LogType.Warning,
				_ => true,
			};
		}

		private static void ApplyHostCallback()
		{
			try
			{
				SSPH_SetLogCallback(_hostCallbackRegistered ? HostCallback : null);
			}
			catch (Exception ex) when (ex is DllNotFoundException || ex is EntryPointNotFoundException || ex is BadImageFormatException)
			{
				// Logging hooks are optional and should not block plugin startup.
			}
		}

		private static void ApplyClientCallback()
		{
			try
			{
				SSPC_SetLogCallback(_clientCallbackRegistered ? ClientCallback : null);
			}
			catch (Exception ex) when (ex is DllNotFoundException || ex is EntryPointNotFoundException || ex is BadImageFormatException)
			{
				// Logging hooks are optional and should not block plugin startup.
			}
		}

		private static void EnsurePump()
		{
			if (_pump != null)
			{
				return;
			}

#if UNITY_2023_1_OR_NEWER
			_pump = UnityEngine.Object.FindFirstObjectByType<PortailStreamNativeLogPump>();
#else
			_pump = UnityEngine.Object.FindObjectOfType<PortailStreamNativeLogPump>();
#endif
			if (_pump != null)
			{
				return;
			}

			GameObject pumpObject = new GameObject("PortailStream Native Log Pump")
			{
				hideFlags = HideFlags.HideAndDontSave,
			};
			UnityEngine.Object.DontDestroyOnLoad(pumpObject);
			_pump = pumpObject.AddComponent<PortailStreamNativeLogPump>();
		}
	}

	[DefaultExecutionOrder(-32000)]
	internal sealed class PortailStreamNativeLogPump : MonoBehaviour
	{
		private void LateUpdate()
		{
			PortailStreamNativeLogBridge.FlushPending();
		}

		private void OnDisable()
		{
			PortailStreamNativeLogBridge.FlushPending();
		}
	}
}
