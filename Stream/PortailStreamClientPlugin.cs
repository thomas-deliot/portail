using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;
using UnityEngine;

namespace Portail.Stream
{
	[StructLayout(LayoutKind.Sequential)]
	public struct PortailStreamClientStartParams
	{
		public uint app_id;
		public ulong host_steam_id;
		public int disable_ice;
	}

	[StructLayout(LayoutKind.Sequential)]
	public struct PortailStreamClientStats
	{
		public ulong recv_chunks;
		public ulong recv_bytes;
		public ulong video_bytes;
		public ulong decoded_frames;
		public ulong audio_packets;
		public ulong audio_bytes;
		public ulong audio_frames;
		public double last_video_reassemble_ms;
		public double last_decode_ms;
		public double last_post_decode_ms;
		public double last_video_texture_copy_ms;
		public double last_total_latency_ms;
		public double last_video_capture_to_texture_ms;
		public double last_audio_decode_ms;
		public double last_audio_capture_to_push_ms;
		public double last_audio_capture_to_unity_push_ms;
		public ulong local_steam_id;
		public int connected;
		public int preview_width;
		public int preview_height;
		public int paused_by_host;
	}

	[StructLayout(LayoutKind.Sequential)]
	public struct PortailStreamClientPeerStats
	{
		public ulong host_steam_id;
		public ulong recv_chunks;
		public ulong recv_bytes;
		public ulong video_bytes;
		public ulong decoded_frames;
		public ulong audio_packets;
		public ulong audio_bytes;
		public ulong audio_frames;
		public double last_video_reassemble_ms;
		public double last_decode_ms;
		public double last_post_decode_ms;
		public double last_video_texture_copy_ms;
		public double last_total_latency_ms;
		public double last_video_capture_to_texture_ms;
		public double last_audio_decode_ms;
		public double last_audio_capture_to_push_ms;
		public double last_audio_capture_to_unity_push_ms;
		public int connected;
		public int connection_path;
		public int preview_width;
		public int preview_height;
		public int paused_by_host;
		public int assigned_video_lod;
		public int assigned_audio_lod;
		public int max_video_lod;
		public int max_audio_lod;
		public int available_video_lod;
		public int available_audio_lod;
		public int effective_video_lod;
		public int effective_audio_lod;
	}

	[Serializable]
	public struct PortailStreamClientOutputBinding
	{
		public ulong hostSteamId;
		public RenderTexture outputTexture;
	}

	[StructLayout(LayoutKind.Sequential)]
	public struct PortailStreamClientVideoLodConfigNative
	{
		public int enabled;
		public int width;
		public int height;
		public int fps;
		public int codec;
	}

	internal static class PortailStreamClientNative
	{
		private const string DllName = "portail_stream_client_plugin";

		[DllImport(DllName)]
		[return: MarshalAs(UnmanagedType.I1)]
		public static extern bool SSPC_Start(ref PortailStreamClientStartParams startParams);

		[DllImport(DllName)]
		public static extern void SSPC_Stop();

		[DllImport(DllName)]
		[return: MarshalAs(UnmanagedType.I1)]
		public static extern bool SSPC_IsRunning();

		[DllImport(DllName)]
		public static extern void SSPC_SetPaused([MarshalAs(UnmanagedType.I1)] bool paused);

		[DllImport(DllName)]
		public static extern void SSPC_SetRemoteSteamIds([In] ulong[] steam_ids, int count);

		[DllImport(DllName)]
		public static extern void SSPC_ConfigureVideoLods([In] PortailStreamClientVideoLodConfigNative[] lods, int count);

		[DllImport(DllName)]
		public static extern void SSPC_SetMaxAcceptedVideoLod(ulong host_steam_id, int lod);

		[DllImport(DllName)]
		public static extern void SSPC_SetMaxAcceptedAudioLod(ulong host_steam_id, int lod);

		[DllImport(DllName)]
		public static extern void SSPC_SetLoggingEnabled([MarshalAs(UnmanagedType.I1)] bool enabled);

		[DllImport(DllName)]
		public static extern void SSPC_SetOutputTextureForSteamId(ulong host_steam_id, IntPtr native_texture_ptr);

		[DllImport(DllName)]
		public static extern IntPtr SSPC_GetRenderEventFunc();

		[DllImport(DllName)]
		public static extern int SSPC_GetRenderEventId();

		[DllImport(DllName)]
		public static extern void SSPC_GetStats(out PortailStreamClientStats stats);

		[DllImport(DllName)]
		public static extern int SSPC_GetPeerStats([Out] PortailStreamClientPeerStats[] stats, int maxCount);

		[DllImport(DllName)]
		public static extern int SSPC_ReadAudioForSteamId(ulong host_steam_id, [Out] float[] out_samples, int max_samples);

		[DllImport(DllName)]
		public static extern void SSPC_MarkAudioPushedForSteamId(ulong host_steam_id);

		[DllImport(DllName)]
		public static extern void SSPC_GetAudioFormat(out int sample_rate, out int channels);

		[DllImport(DllName)]
		private static extern IntPtr SSPC_GetLastError();

		public static string GetLastError()
		{
			IntPtr ptr = SSPC_GetLastError();
			return ptr == IntPtr.Zero ? string.Empty : Marshal.PtrToStringAnsi(ptr) ?? string.Empty;
		}
	}

	public sealed class PortailStreamClientPlugin : MonoBehaviour
	{
		[Header("Client Start Params")]
		public uint appId = 480;
		public bool disableIce;
		public bool autoStart = false;
		public bool enableNativeLogging = true;
		public string codec = "h264";

		[Header("Shared LOD Table")]
		public List<PortailStreamVideoLodConfig> videoLods = PortailStreamHostPlugin.CreateDefaultVideoLods();

		[Header("Remote Hosts")]
		public ulong[] remoteHostSteamIds = Array.Empty<ulong>();

		[Header("Unity Outputs (One Per Host)")]
		public PortailStreamClientOutputBinding[] outputBindings = Array.Empty<PortailStreamClientOutputBinding>();

		private IntPtr _renderEventFunc;
		private int _renderEventId;

		public bool StartStreaming()
		{
			if (!PortailStreamSteamAvailability.TryGetAvailabilityError(out string steamError))
			{
				_renderEventFunc = IntPtr.Zero;
				_renderEventId = 0;
				Debug.LogError($"[PortailStreamClient] Start blocked: {steamError}");
				return false;
			}

			if (!PortailStreamNativeLoader.EnsureLoaded())
			{
				Debug.LogError($"[PortailStreamClient] Native load failed: {PortailStreamNativeLoader.LastError}");
				return false;
			}

			ApplyNativeLogging();
			ConfigureVideoLods();

			PortailStreamClientStartParams p = new PortailStreamClientStartParams
			{
				app_id = appId,
				host_steam_id = 0,
				disable_ice = disableIce ? 1 : 0,
			};

			bool ok = PortailStreamClientNative.SSPC_Start(ref p);
			if (!ok)
			{
				PortailStreamNativeLogBridge.FlushPending();
				Debug.LogError($"[PortailStreamClient] Start failed: {PortailStreamClientNative.GetLastError()}");
				return false;
			}

			_renderEventFunc = PortailStreamClientNative.SSPC_GetRenderEventFunc();
			_renderEventId = PortailStreamClientNative.SSPC_GetRenderEventId();
			ApplyRemoteHosts();
			PushOutputTextures();
			return true;
		}

		public void StopStreaming()
		{
			if (!PortailStreamNativeLoader.EnsureLoaded())
			{
				return;
			}
			ApplyNativeLogging();
			PortailStreamClientNative.SSPC_Stop();
			PortailStreamNativeLogBridge.UnregisterClientCallback();
			PortailStreamNativeLogBridge.FlushPending();
			_renderEventFunc = IntPtr.Zero;
			_renderEventId = 0;
		}

		public void ConfigureVideoLods()
		{
			if (!PortailStreamNativeLoader.EnsureLoaded())
			{
				return;
			}

			if (videoLods == null || videoLods.Count == 0)
			{
				videoLods = PortailStreamHostPlugin.CreateDefaultVideoLods();
			}

			int protocolCodec = ProtocolCodecFromString(codec);
			PortailStreamClientVideoLodConfigNative[] nativeVideo = new PortailStreamClientVideoLodConfigNative[videoLods.Count];
			for (int i = 0; i < videoLods.Count; ++i)
			{
				PortailStreamVideoLodConfig lod = videoLods[i] ?? new PortailStreamVideoLodConfig();
				nativeVideo[i] = new PortailStreamClientVideoLodConfigNative
				{
					enabled = lod.enabled ? 1 : 0,
					width = Mathf.Max(16, lod.width),
					height = Mathf.Max(16, lod.height),
					fps = Mathf.Clamp(lod.fps, 1, 240),
					codec = protocolCodec,
				};
			}

			PortailStreamClientNative.SSPC_ConfigureVideoLods(nativeVideo, nativeVideo.Length);
		}

		public void SetPaused(bool paused)
		{
			if (!PortailStreamNativeLoader.EnsureLoaded())
			{
				return;
			}
			PortailStreamClientNative.SSPC_SetPaused(paused);
		}

		public void SetRemoteHostSteamIds(IReadOnlyList<ulong> hostSteamIds)
		{
			if (!PortailStreamNativeLoader.EnsureLoaded())
			{
				return;
			}

			if (hostSteamIds == null || hostSteamIds.Count == 0)
			{
				remoteHostSteamIds = Array.Empty<ulong>();
				PortailStreamClientNative.SSPC_SetRemoteSteamIds(Array.Empty<ulong>(), 0);
				return;
			}

			List<ulong> values = new List<ulong>(hostSteamIds.Count);
			HashSet<ulong> seen = new HashSet<ulong>();
			for (int i = 0; i < hostSteamIds.Count; ++i)
			{
				ulong id = hostSteamIds[i];
				if (id == 0 || !seen.Add(id))
				{
					continue;
				}
				values.Add(id);
			}

			remoteHostSteamIds = values.ToArray();
			PortailStreamClientNative.SSPC_SetRemoteSteamIds(remoteHostSteamIds, remoteHostSteamIds.Length);
		}

		public void SetMaxAcceptedVideoLod(ulong hostSteamId, int lod)
		{
			if (hostSteamId == 0 || !PortailStreamNativeLoader.EnsureLoaded())
			{
				return;
			}

			PortailStreamClientNative.SSPC_SetMaxAcceptedVideoLod(hostSteamId, PortailStreamLodUtility.NormalizeLod(lod));
		}

		public void SetMaxAcceptedAudioLod(ulong hostSteamId, int lod)
		{
			if (hostSteamId == 0 || !PortailStreamNativeLoader.EnsureLoaded())
			{
				return;
			}

			PortailStreamClientNative.SSPC_SetMaxAcceptedAudioLod(hostSteamId, PortailStreamLodUtility.NormalizeLod(lod));
		}

		public void SetOutputTextureForHost(ulong hostSteamId, RenderTexture texture)
		{
			if (!PortailStreamNativeLoader.EnsureLoaded())
			{
				return;
			}

			ApplyNativeLogging();

			if (hostSteamId == 0)
			{
				return;
			}
			PortailStreamClientNative.SSPC_SetOutputTextureForSteamId(
				hostSteamId,
				texture != null ? texture.GetNativeTexturePtr() : IntPtr.Zero
			);
		}

		public bool IsRunning()
		{
			if (!PortailStreamNativeLoader.EnsureLoaded())
			{
				return false;
			}
			return PortailStreamClientNative.SSPC_IsRunning();
		}

		public PortailStreamClientStats GetStats()
		{
			if (!PortailStreamNativeLoader.EnsureLoaded())
			{
				return default;
			}
			PortailStreamClientNative.SSPC_GetStats(out PortailStreamClientStats stats);
			return stats;
		}

		public PortailStreamClientPeerStats[] GetPeerStats(int maxPeers = 64)
		{
			if (!PortailStreamNativeLoader.EnsureLoaded())
			{
				return Array.Empty<PortailStreamClientPeerStats>();
			}

			if (maxPeers <= 0)
			{
				return Array.Empty<PortailStreamClientPeerStats>();
			}

			PortailStreamClientPeerStats[] buffer = new PortailStreamClientPeerStats[maxPeers];
			int count = PortailStreamClientNative.SSPC_GetPeerStats(buffer, buffer.Length);
			if (count <= 0)
			{
				return Array.Empty<PortailStreamClientPeerStats>();
			}
			if (count == buffer.Length)
			{
				return buffer;
			}

			PortailStreamClientPeerStats[] trimmed = new PortailStreamClientPeerStats[count];
			Array.Copy(buffer, trimmed, count);
			return trimmed;
		}

		public int GetPeerStatsNonAlloc(PortailStreamClientPeerStats[] buffer)
		{
			if (!PortailStreamNativeLoader.EnsureLoaded() || buffer == null || buffer.Length == 0)
			{
				return 0;
			}

			int count = PortailStreamClientNative.SSPC_GetPeerStats(buffer, buffer.Length);
			return Mathf.Clamp(count, 0, buffer.Length);
		}

		public int ReadAudioForHost(ulong hostSteamId, float[] samples)
		{
			if (hostSteamId == 0 || samples == null || samples.Length == 0 || !PortailStreamNativeLoader.EnsureLoaded())
			{
				return 0;
			}

			return PortailStreamClientNative.SSPC_ReadAudioForSteamId(hostSteamId, samples, samples.Length);
		}

		public void MarkAudioPushedForHost(ulong hostSteamId)
		{
			if (hostSteamId == 0 || !PortailStreamNativeLoader.EnsureLoaded())
			{
				return;
			}

			PortailStreamClientNative.SSPC_MarkAudioPushedForSteamId(hostSteamId);
		}

		public static void GetAudioFormat(out int sampleRate, out int channels)
		{
			sampleRate = 48000;
			channels = 2;
			if (!PortailStreamNativeLoader.EnsureLoaded())
			{
				return;
			}

			PortailStreamClientNative.SSPC_GetAudioFormat(out sampleRate, out channels);
		}

		private void ApplyNativeLogging()
		{
			if (!PortailStreamNativeLoader.EnsureLoaded())
			{
				return;
			}

			PortailStreamClientNative.SSPC_SetLoggingEnabled(enableNativeLogging);
			if (enableNativeLogging)
			{
				PortailStreamNativeLogBridge.RegisterClientCallback();
			}
			else
			{
				PortailStreamNativeLogBridge.UnregisterClientCallback();
			}
		}

		private static int ProtocolCodecFromString(string value)
		{
			if (string.Equals(value, "hevc", StringComparison.OrdinalIgnoreCase) ||
				string.Equals(value, "h265", StringComparison.OrdinalIgnoreCase))
			{
				return 2;
			}
			if (string.Equals(value, "av1", StringComparison.OrdinalIgnoreCase))
			{
				return 3;
			}
			return 1;
		}

		private void OnEnable()
		{
			if (autoStart)
			{
				StartStreaming();
			}
		}

		private void LateUpdate()
		{
			PortailStreamNativeLogBridge.FlushPending();

			if (!PortailStreamNativeLoader.EnsureLoaded())
			{
				return;
			}

			if (!PortailStreamClientNative.SSPC_IsRunning())
			{
				return;
			}

			PushOutputTextures();
			if (_renderEventFunc != IntPtr.Zero)
			{
				GL.IssuePluginEvent(_renderEventFunc, _renderEventId);
			}
		}

		private void OnDisable()
		{
			PortailStreamNativeLogBridge.FlushPending();

			if (!PortailStreamNativeLoader.EnsureLoaded())
			{
				return;
			}

			if (PortailStreamClientNative.SSPC_IsRunning())
			{
				StopStreaming();
			}
			else
			{
				PortailStreamNativeLogBridge.UnregisterClientCallback();
			}
		}

		private void ApplyRemoteHosts()
		{
			if (remoteHostSteamIds != null && remoteHostSteamIds.Length > 0)
			{
				SetRemoteHostSteamIds(remoteHostSteamIds);
				return;
			}

			if (outputBindings != null && outputBindings.Length > 0)
			{
				List<ulong> hostIds = new List<ulong>(outputBindings.Length);
				for (int i = 0; i < outputBindings.Length; ++i)
				{
					ulong hostId = outputBindings[i].hostSteamId;
					if (hostId != 0)
					{
						hostIds.Add(hostId);
					}
				}
				SetRemoteHostSteamIds(hostIds);
				return;
			}

			SetRemoteHostSteamIds(Array.Empty<ulong>());
		}

		private void PushOutputTextures()
		{
			ApplyNativeLogging();

			if (outputBindings == null || outputBindings.Length == 0)
			{
				return;
			}

			for (int i = 0; i < outputBindings.Length; ++i)
			{
				PortailStreamClientOutputBinding binding = outputBindings[i];
				if (binding.hostSteamId == 0)
				{
					continue;
				}
				SetOutputTextureForHost(binding.hostSteamId, binding.outputTexture);
			}
		}
	}
}
