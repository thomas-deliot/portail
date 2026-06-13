using System;
using System.Runtime.InteropServices;
using UnityEngine;

namespace Portail.Capture
{
	[StructLayout(LayoutKind.Sequential)]
	public struct PortailCaptureStartParams
	{
		public ulong window_handle;
		public int enable_audio;
		public int capture_mode;
		public int capture_cursor;
	}

	public enum PortailCaptureMode
	{
		Window = 0,
		Display = 1,
	}

	[StructLayout(LayoutKind.Sequential)]
	public struct PortailCaptureStats
	{
		public ulong capture_frames;
		public double last_capture_ms;
		public int preview_width;
		public int preview_height;
		public int audio_capture_state;
		public int audio_target_pid;
		public ulong local_audio_samples_read;
	}

	internal static class PortailCaptureNative
	{
		private const string DllName = PortailCaptureNativeBridge.DllName;

		[DllImport(DllName)]
		[return: MarshalAs(UnmanagedType.I1)]
		public static extern bool SSPCAP_Start(ref PortailCaptureStartParams startParams);

		[DllImport(DllName)]
		public static extern void SSPCAP_Stop();

		[DllImport(DllName)]
		[return: MarshalAs(UnmanagedType.I1)]
		public static extern bool SSPCAP_IsRunning();

		[DllImport(DllName)]
		public static extern void SSPCAP_SetWindowHandle(ulong window_handle);

		[DllImport(DllName)]
		public static extern void SSPCAP_SetLoggingEnabled([MarshalAs(UnmanagedType.I1)] bool enabled);

		[DllImport(DllName)]
		public static extern void SSPCAP_SetPreviewTexture(IntPtr native_texture_ptr);

		[DllImport(DllName)]
		public static extern IntPtr SSPCAP_GetRenderEventFunc();

		[DllImport(DllName)]
		public static extern int SSPCAP_GetRenderEventId();

		[DllImport(DllName)]
		public static extern void SSPCAP_GetStats(out PortailCaptureStats stats);

		[DllImport(DllName)]
		public static extern int SSPCAP_ReadLocalAudio([Out] float[] out_samples, int max_samples);

		[DllImport(DllName)]
		public static extern void SSPCAP_GetLocalAudioFormat(out int sample_rate, out int channels);

		[DllImport(DllName)]
		private static extern IntPtr SSPCAP_GetLastError();

		[DllImport(DllName)]
		private static extern IntPtr SSPCAP_GetLastAudioError();

		public static string GetLastError()
		{
			IntPtr ptr = SSPCAP_GetLastError();
			return ptr == IntPtr.Zero ? string.Empty : Marshal.PtrToStringAnsi(ptr) ?? string.Empty;
		}

		public static string GetLastAudioError()
		{
			IntPtr ptr = SSPCAP_GetLastAudioError();
			return ptr == IntPtr.Zero ? string.Empty : Marshal.PtrToStringAnsi(ptr) ?? string.Empty;
		}
	}

	public sealed class PortailCapturePlugin : MonoBehaviour
	{
		[Header("Capture Start Params")]
		public ulong windowHandle;
		public PortailCaptureMode captureMode;
		public bool enableAudio = true;
		public bool captureCursor = true;
		public bool autoStart;
		public bool enableNativeLogging = true;

		[Header("Unity Output")]
		public RenderTexture previewTexture;

		private IntPtr _renderEventFunc;
		private int _renderEventId;

		public bool StartCapture()
		{
			if (!PortailCaptureNativeLoader.EnsureLoaded())
			{
				Debug.LogError($"[PortailCapture] Native load failed: {PortailCaptureNativeLoader.LastError}");
				return false;
			}

			ApplyNativeLogging();

			PortailCaptureStartParams p = new PortailCaptureStartParams
			{
				window_handle = windowHandle,
				enable_audio = enableAudio ? 1 : 0,
				capture_mode = (int)captureMode,
				capture_cursor = captureCursor ? 1 : 0,
			};

			bool ok = PortailCaptureNative.SSPCAP_Start(ref p);
			if (!ok)
			{
				PortailCaptureNativeLogBridge.FlushPending();
				Debug.LogError($"[PortailCapture] Start failed: {PortailCaptureNative.GetLastError()}");
				return false;
			}

			_renderEventFunc = PortailCaptureNative.SSPCAP_GetRenderEventFunc();
			_renderEventId = PortailCaptureNative.SSPCAP_GetRenderEventId();
			PushPreviewTexture();
			return true;
		}

		public void StopCapture()
		{
			if (!PortailCaptureNativeLoader.EnsureLoaded())
			{
				return;
			}
			ApplyNativeLogging();
			PortailCaptureNative.SSPCAP_Stop();
			PortailCaptureNativeLogBridge.UnregisterCaptureCallback();
			PortailCaptureNativeLogBridge.FlushPending();
			_renderEventFunc = IntPtr.Zero;
			_renderEventId = 0;
		}

		public void SetWindowHandle(ulong newWindowHandle)
		{
			windowHandle = newWindowHandle;
			if (!PortailCaptureNativeLoader.EnsureLoaded())
			{
				return;
			}
			PortailCaptureNative.SSPCAP_SetWindowHandle(newWindowHandle);
		}

		public bool IsRunning()
		{
			if (!PortailCaptureNativeLoader.EnsureLoaded())
			{
				return false;
			}
			return PortailCaptureNative.SSPCAP_IsRunning();
		}

		public PortailCaptureStats GetStats()
		{
			if (!PortailCaptureNativeLoader.EnsureLoaded())
			{
				return default;
			}
			PortailCaptureNative.SSPCAP_GetStats(out PortailCaptureStats stats);
			return stats;
		}

		public int ReadLocalAudio(float[] samples)
		{
			if (samples == null || samples.Length == 0 || !PortailCaptureNativeLoader.EnsureLoaded())
			{
				return 0;
			}

			return PortailCaptureNative.SSPCAP_ReadLocalAudio(samples, samples.Length);
		}

		public static void GetLocalAudioFormat(out int sampleRate, out int channels)
		{
			sampleRate = 48000;
			channels = 2;
			if (!PortailCaptureNativeLoader.EnsureLoaded())
			{
				return;
			}

			PortailCaptureNative.SSPCAP_GetLocalAudioFormat(out sampleRate, out channels);
		}

		public string GetLastAudioError()
		{
			return !PortailCaptureNativeLoader.EnsureLoaded() ? PortailCaptureNativeLoader.LastError : PortailCaptureNative.GetLastAudioError();
		}

		private void ApplyNativeLogging()
		{
			if (!PortailCaptureNativeLoader.EnsureLoaded())
			{
				return;
			}

			PortailCaptureNative.SSPCAP_SetLoggingEnabled(enableNativeLogging);
			if (enableNativeLogging)
			{
				PortailCaptureNativeLogBridge.RegisterCaptureCallback();
			}
			else
			{
				PortailCaptureNativeLogBridge.UnregisterCaptureCallback();
			}
		}

		private void OnEnable()
		{
			if (autoStart)
			{
				StartCapture();
			}
		}

		private void LateUpdate()
		{
			PortailCaptureNativeLogBridge.FlushPending();

			if (!PortailCaptureNativeLoader.EnsureLoaded())
			{
				return;
			}

			if (!PortailCaptureNative.SSPCAP_IsRunning())
			{
				return;
			}

			PushPreviewTexture();
			if (_renderEventFunc != IntPtr.Zero)
			{
				GL.IssuePluginEvent(_renderEventFunc, _renderEventId);
			}
		}

		private void OnDisable()
		{
			PortailCaptureNativeLogBridge.FlushPending();

			if (!PortailCaptureNativeLoader.EnsureLoaded())
			{
				return;
			}

			if (PortailCaptureNative.SSPCAP_IsRunning())
			{
				StopCapture();
			}
			else
			{
				PortailCaptureNativeLogBridge.UnregisterCaptureCallback();
			}
		}

		private void PushPreviewTexture()
		{
			if (!PortailCaptureNativeLoader.EnsureLoaded())
			{
				return;
			}

			ApplyNativeLogging();

			if (previewTexture == null)
			{
				return;
			}
			PortailCaptureNative.SSPCAP_SetPreviewTexture(previewTexture.GetNativeTexturePtr());
		}
	}
}
