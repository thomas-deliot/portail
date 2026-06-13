using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;
using UnityEngine;

namespace Portail.Stream
{
	[StructLayout(LayoutKind.Sequential)]
	public struct PortailStreamReceiverStartParams
	{
		public uint app_id;
		public ulong sender_steam_id;
		public int disable_ice;
	}

	[StructLayout(LayoutKind.Sequential)]
	public struct PortailStreamReceiverStats
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
		public int paused_by_sender;
	}

	[StructLayout(LayoutKind.Sequential)]
	public struct PortailStreamReceiverPeerStats
	{
		public ulong sender_steam_id;
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
		public int paused_by_sender;
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
	public struct PortailStreamReceiverOutputBinding
	{
		public ulong senderSteamId;
		public RenderTexture outputTexture;
	}

	[StructLayout(LayoutKind.Sequential)]
	public struct PortailStreamReceiverVideoLodConfigNative
	{
		public int enabled;
		public int width;
		public int height;
		public int fps;
		public int codec;
	}

	internal static class PortailStreamReceiverNative
	{
		private const string DllName = "portail_stream_receiver_plugin";

		[DllImport(DllName)]
		[return: MarshalAs(UnmanagedType.I1)]
		public static extern bool SSPR_Start(ref PortailStreamReceiverStartParams startParams);

		[DllImport(DllName)]
		public static extern void SSPR_Stop();

		[DllImport(DllName)]
		[return: MarshalAs(UnmanagedType.I1)]
		public static extern bool SSPR_IsRunning();

		[DllImport(DllName)]
		public static extern void SSPR_SetPaused([MarshalAs(UnmanagedType.I1)] bool paused);

		[DllImport(DllName)]
		public static extern void SSPR_SetRemoteSteamIds([In] ulong[] steam_ids, int count);

		[DllImport(DllName)]
		public static extern void SSPR_ConfigureVideoLods([In] PortailStreamReceiverVideoLodConfigNative[] lods, int count);

		[DllImport(DllName)]
		public static extern void SSPR_SetMaxAcceptedVideoLod(ulong sender_steam_id, int lod);

		[DllImport(DllName)]
		public static extern void SSPR_SetMaxAcceptedAudioLod(ulong sender_steam_id, int lod);

		[DllImport(DllName)]
		public static extern void SSPR_SetLoggingEnabled([MarshalAs(UnmanagedType.I1)] bool enabled);

		[DllImport(DllName)]
		public static extern void SSPR_SetOutputTextureForSteamId(ulong sender_steam_id, IntPtr native_texture_ptr);

		[DllImport(DllName)]
		public static extern IntPtr SSPR_GetRenderEventFunc();

		[DllImport(DllName)]
		public static extern int SSPR_GetRenderEventId();

		[DllImport(DllName)]
		public static extern void SSPR_GetStats(out PortailStreamReceiverStats stats);

		[DllImport(DllName)]
		public static extern int SSPR_GetPeerStats([Out] PortailStreamReceiverPeerStats[] stats, int maxCount);

		[DllImport(DllName)]
		public static extern int SSPR_ReadAudioForSteamId(ulong sender_steam_id, [Out] float[] out_samples, int max_samples);

		[DllImport(DllName)]
		public static extern void SSPR_MarkAudioPushedForSteamId(ulong sender_steam_id);

		[DllImport(DllName)]
		public static extern void SSPR_GetAudioFormat(out int sample_rate, out int channels);

		[DllImport(DllName)]
		private static extern IntPtr SSPR_GetLastError();

		public static string GetLastError()
		{
			IntPtr ptr = SSPR_GetLastError();
			return ptr == IntPtr.Zero ? string.Empty : Marshal.PtrToStringAnsi(ptr) ?? string.Empty;
		}
	}

	public sealed class PortailStreamReceiverPlugin : MonoBehaviour
	{
		[Header("Receiver Start Params")]
		public uint appId = 480;
		public bool disableIce;
		public bool autoStart = false;
		public bool enableNativeLogging = true;
		public string codec = "h264";

		[Header("Shared LOD Table")]
		public List<PortailStreamVideoLodConfig> videoLods = PortailStreamSenderPlugin.CreateDefaultVideoLods();

		[Header("Remote Senders")]
		public ulong[] remoteSenderSteamIds = Array.Empty<ulong>();

		[Header("Unity Outputs (One Per Sender)")]
		public PortailStreamReceiverOutputBinding[] outputBindings = Array.Empty<PortailStreamReceiverOutputBinding>();

		private IntPtr _renderEventFunc;
		private int _renderEventId;

		public bool StartStreaming()
		{
			if (!PortailStreamSteamAvailability.TryGetAvailabilityError(out string steamError))
			{
				_renderEventFunc = IntPtr.Zero;
				_renderEventId = 0;
				Debug.LogError($"[PortailStreamReceiver] Start blocked: {steamError}");
				return false;
			}

			if (!PortailStreamNativeLoader.EnsureLoaded())
			{
				Debug.LogError($"[PortailStreamReceiver] Native load failed: {PortailStreamNativeLoader.LastError}");
				return false;
			}

			ApplyNativeLogging();
			ConfigureVideoLods();

			PortailStreamReceiverStartParams p = new PortailStreamReceiverStartParams
			{
				app_id = appId,
				sender_steam_id = 0,
				disable_ice = disableIce ? 1 : 0,
			};

			bool ok = PortailStreamReceiverNative.SSPR_Start(ref p);
			if (!ok)
			{
				PortailStreamNativeLogBridge.FlushPending();
				Debug.LogError($"[PortailStreamReceiver] Start failed: {PortailStreamReceiverNative.GetLastError()}");
				return false;
			}

			_renderEventFunc = PortailStreamReceiverNative.SSPR_GetRenderEventFunc();
			_renderEventId = PortailStreamReceiverNative.SSPR_GetRenderEventId();
			ApplyRemoteSenders();
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
			PortailStreamReceiverNative.SSPR_Stop();
			PortailStreamNativeLogBridge.UnregisterReceiverCallback();
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
				videoLods = PortailStreamSenderPlugin.CreateDefaultVideoLods();
			}

			int protocolCodec = ProtocolCodecFromString(codec);
			PortailStreamReceiverVideoLodConfigNative[] nativeVideo = new PortailStreamReceiverVideoLodConfigNative[videoLods.Count];
			for (int i = 0; i < videoLods.Count; ++i)
			{
				PortailStreamVideoLodConfig lod = videoLods[i] ?? new PortailStreamVideoLodConfig();
				nativeVideo[i] = new PortailStreamReceiverVideoLodConfigNative
				{
					enabled = lod.enabled ? 1 : 0,
					width = Mathf.Max(16, lod.width),
					height = Mathf.Max(16, lod.height),
					fps = Mathf.Clamp(lod.fps, 1, 240),
					codec = protocolCodec,
				};
			}

			PortailStreamReceiverNative.SSPR_ConfigureVideoLods(nativeVideo, nativeVideo.Length);
		}

		public void SetPaused(bool paused)
		{
			if (!PortailStreamNativeLoader.EnsureLoaded())
			{
				return;
			}
			PortailStreamReceiverNative.SSPR_SetPaused(paused);
		}

		public void SetRemoteSenderSteamIds(IReadOnlyList<ulong> senderSteamIds)
		{
			if (!PortailStreamNativeLoader.EnsureLoaded())
			{
				return;
			}

			if (senderSteamIds == null || senderSteamIds.Count == 0)
			{
				remoteSenderSteamIds = Array.Empty<ulong>();
				PortailStreamReceiverNative.SSPR_SetRemoteSteamIds(Array.Empty<ulong>(), 0);
				return;
			}

			List<ulong> values = new List<ulong>(senderSteamIds.Count);
			HashSet<ulong> seen = new HashSet<ulong>();
			for (int i = 0; i < senderSteamIds.Count; ++i)
			{
				ulong id = senderSteamIds[i];
				if (id == 0 || !seen.Add(id))
				{
					continue;
				}
				values.Add(id);
			}

			remoteSenderSteamIds = values.ToArray();
			PortailStreamReceiverNative.SSPR_SetRemoteSteamIds(remoteSenderSteamIds, remoteSenderSteamIds.Length);
		}

		public void SetMaxAcceptedVideoLod(ulong senderSteamId, int lod)
		{
			if (senderSteamId == 0 || !PortailStreamNativeLoader.EnsureLoaded())
			{
				return;
			}

			PortailStreamReceiverNative.SSPR_SetMaxAcceptedVideoLod(senderSteamId, PortailStreamLodUtility.NormalizeLod(lod));
		}

		public void SetMaxAcceptedAudioLod(ulong senderSteamId, int lod)
		{
			if (senderSteamId == 0 || !PortailStreamNativeLoader.EnsureLoaded())
			{
				return;
			}

			PortailStreamReceiverNative.SSPR_SetMaxAcceptedAudioLod(senderSteamId, PortailStreamLodUtility.NormalizeLod(lod));
		}

		public void SetOutputTextureForSender(ulong senderSteamId, RenderTexture texture)
		{
			if (!PortailStreamNativeLoader.EnsureLoaded())
			{
				return;
			}

			ApplyNativeLogging();

			if (senderSteamId == 0)
			{
				return;
			}
			PortailStreamReceiverNative.SSPR_SetOutputTextureForSteamId(
				senderSteamId,
				texture != null ? texture.GetNativeTexturePtr() : IntPtr.Zero
			);
		}

		public bool IsRunning()
		{
			if (!PortailStreamNativeLoader.EnsureLoaded())
			{
				return false;
			}
			return PortailStreamReceiverNative.SSPR_IsRunning();
		}

		public PortailStreamReceiverStats GetStats()
		{
			if (!PortailStreamNativeLoader.EnsureLoaded())
			{
				return default;
			}
			PortailStreamReceiverNative.SSPR_GetStats(out PortailStreamReceiverStats stats);
			return stats;
		}

		public PortailStreamReceiverPeerStats[] GetPeerStats(int maxPeers = 64)
		{
			if (!PortailStreamNativeLoader.EnsureLoaded())
			{
				return Array.Empty<PortailStreamReceiverPeerStats>();
			}

			if (maxPeers <= 0)
			{
				return Array.Empty<PortailStreamReceiverPeerStats>();
			}

			PortailStreamReceiverPeerStats[] buffer = new PortailStreamReceiverPeerStats[maxPeers];
			int count = PortailStreamReceiverNative.SSPR_GetPeerStats(buffer, buffer.Length);
			if (count <= 0)
			{
				return Array.Empty<PortailStreamReceiverPeerStats>();
			}
			if (count == buffer.Length)
			{
				return buffer;
			}

			PortailStreamReceiverPeerStats[] trimmed = new PortailStreamReceiverPeerStats[count];
			Array.Copy(buffer, trimmed, count);
			return trimmed;
		}

		public int GetPeerStatsNonAlloc(PortailStreamReceiverPeerStats[] buffer)
		{
			if (!PortailStreamNativeLoader.EnsureLoaded() || buffer == null || buffer.Length == 0)
			{
				return 0;
			}

			int count = PortailStreamReceiverNative.SSPR_GetPeerStats(buffer, buffer.Length);
			return Mathf.Clamp(count, 0, buffer.Length);
		}

		public int ReadAudioForSender(ulong senderSteamId, float[] samples)
		{
			if (senderSteamId == 0 || samples == null || samples.Length == 0 || !PortailStreamNativeLoader.EnsureLoaded())
			{
				return 0;
			}

			return PortailStreamReceiverNative.SSPR_ReadAudioForSteamId(senderSteamId, samples, samples.Length);
		}

		public void MarkAudioPushedForSender(ulong senderSteamId)
		{
			if (senderSteamId == 0 || !PortailStreamNativeLoader.EnsureLoaded())
			{
				return;
			}

			PortailStreamReceiverNative.SSPR_MarkAudioPushedForSteamId(senderSteamId);
		}

		public static void GetAudioFormat(out int sampleRate, out int channels)
		{
			sampleRate = 48000;
			channels = 2;
			if (!PortailStreamNativeLoader.EnsureLoaded())
			{
				return;
			}

			PortailStreamReceiverNative.SSPR_GetAudioFormat(out sampleRate, out channels);
		}

		private void ApplyNativeLogging()
		{
			if (!PortailStreamNativeLoader.EnsureLoaded())
			{
				return;
			}

			PortailStreamReceiverNative.SSPR_SetLoggingEnabled(enableNativeLogging);
			if (enableNativeLogging)
			{
				PortailStreamNativeLogBridge.RegisterReceiverCallback();
			}
			else
			{
				PortailStreamNativeLogBridge.UnregisterReceiverCallback();
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

			if (!PortailStreamReceiverNative.SSPR_IsRunning())
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

			if (PortailStreamReceiverNative.SSPR_IsRunning())
			{
				StopStreaming();
			}
			else
			{
				PortailStreamNativeLogBridge.UnregisterReceiverCallback();
			}
		}

		private void ApplyRemoteSenders()
		{
			if (remoteSenderSteamIds != null && remoteSenderSteamIds.Length > 0)
			{
				SetRemoteSenderSteamIds(remoteSenderSteamIds);
				return;
			}

			if (outputBindings != null && outputBindings.Length > 0)
			{
				List<ulong> senderIds = new List<ulong>(outputBindings.Length);
				for (int i = 0; i < outputBindings.Length; ++i)
				{
					ulong senderId = outputBindings[i].senderSteamId;
					if (senderId != 0)
					{
						senderIds.Add(senderId);
					}
				}
				SetRemoteSenderSteamIds(senderIds);
				return;
			}

			SetRemoteSenderSteamIds(Array.Empty<ulong>());
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
				PortailStreamReceiverOutputBinding binding = outputBindings[i];
				if (binding.senderSteamId == 0)
				{
					continue;
				}
				SetOutputTextureForSender(binding.senderSteamId, binding.outputTexture);
			}
		}
	}
}
