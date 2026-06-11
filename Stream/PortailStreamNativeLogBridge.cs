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
		private static readonly NativeLogCallback SenderCallback = OnNativeSenderLog;
		private static readonly NativeLogCallback ReceiverCallback = OnNativeReceiverLog;
		private static readonly object RegistrationLock = new object();
		private static int _senderConsoleLogLevel = (int)PortailStreamNativeConsoleLogLevel.All;
		private static int _receiverConsoleLogLevel = (int)PortailStreamNativeConsoleLogLevel.All;
		private static bool _senderCallbackRegistered;
		private static bool _receiverCallbackRegistered;
		private static PortailStreamNativeLogPump _pump;

		[DllImport("portail_stream_sender_plugin", CallingConvention = CallingConvention.Cdecl)]
		private static extern void SSPS_SetLogCallback(NativeLogCallback callback);

		[DllImport("portail_stream_receiver_plugin", CallingConvention = CallingConvention.Cdecl)]
		private static extern void SSPR_SetLogCallback(NativeLogCallback callback);

		public static PortailStreamNativeConsoleLogLevel SenderConsoleLogLevel
		{
			get => (PortailStreamNativeConsoleLogLevel)Volatile.Read(ref _senderConsoleLogLevel);
			set => Volatile.Write(ref _senderConsoleLogLevel, (int)value);
		}

		public static PortailStreamNativeConsoleLogLevel ReceiverConsoleLogLevel
		{
			get => (PortailStreamNativeConsoleLogLevel)Volatile.Read(ref _receiverConsoleLogLevel);
			set => Volatile.Write(ref _receiverConsoleLogLevel, (int)value);
		}

		public static void RegisterSenderCallback()
		{
			lock (RegistrationLock)
			{
				_senderCallbackRegistered = true;
				ApplySenderCallback();
				EnsurePump();
			}
		}

		public static void UnregisterSenderCallback()
		{
			lock (RegistrationLock)
			{
				if (!_senderCallbackRegistered)
				{
					return;
				}

				_senderCallbackRegistered = false;
				ApplySenderCallback();
			}
		}

		public static void RegisterReceiverCallback()
		{
			lock (RegistrationLock)
			{
				_receiverCallbackRegistered = true;
				ApplyReceiverCallback();
				EnsurePump();
			}
		}

		public static void UnregisterReceiverCallback()
		{
			lock (RegistrationLock)
			{
				if (!_receiverCallbackRegistered)
				{
					return;
				}

				_receiverCallbackRegistered = false;
				ApplyReceiverCallback();
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

		private static void OnNativeSenderLog(int level, IntPtr messagePtr)
		{
			OnNativeLog(level, messagePtr, SenderConsoleLogLevel);
		}

		private static void OnNativeReceiverLog(int level, IntPtr messagePtr)
		{
			OnNativeLog(level, messagePtr, ReceiverConsoleLogLevel);
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

		private static void ApplySenderCallback()
		{
			try
			{
				SSPS_SetLogCallback(_senderCallbackRegistered ? SenderCallback : null);
			}
			catch (Exception ex) when (ex is DllNotFoundException || ex is EntryPointNotFoundException || ex is BadImageFormatException)
			{
				// Logging hooks are optional and should not block plugin startup.
			}
		}

		private static void ApplyReceiverCallback()
		{
			try
			{
				SSPR_SetLogCallback(_receiverCallbackRegistered ? ReceiverCallback : null);
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
