using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;
using Steamworks;
using UnityEngine;

namespace Portail.Stream
{
	public enum PortailStreamVideoRateControl
	{
		Cbr = 0,
		Vbr = 1,
	}

	public static class PortailStreamLodUtility
	{
		public const int Off = -1;
		public const int Highest = 0;

		public static int NormalizeLod(int lod)
		{
			return lod < 0 ? Off : lod;
		}

		public static string ToVideoLabel(int lod)
		{
			return lod >= 0 ? $"V:lod{lod}" : "V:Off";
		}

		public static string ToAudioLabel(int lod)
		{
			return lod >= 0 ? $"A:lod{lod}" : "A:Off";
		}

		public static string ToVideoWorldLabel(int lod)
		{
			return lod >= 0 ? $"V_lod{lod}" : "Off";
		}

		public static string ToAudioWorldLabel(int lod)
		{
			return lod >= 0 ? $"A_lod{lod}" : "Off";
		}
	}

	[Serializable]
	public sealed class PortailStreamVideoLodConfig
	{
		public string name = "1080p60";
		public bool enabled = true;
		public int width = 1920;
		public int height = 1080;
		public int fps = 60;
		[Tooltip("CBR uses this as the fixed bitrate. VBR uses this as the maximum bitrate.")]
		public int targetBitrateKbps = 12000;
		public PortailStreamVideoRateControl rateControl = PortailStreamVideoRateControl.Cbr;
	}

	[Serializable]
	public sealed class PortailStreamAudioLodConfig
	{
		public string name = "Opus 96";
		public bool enabled = true;
		public int bitrateKbps = 96;
	}

	[StructLayout(LayoutKind.Sequential, CharSet = CharSet.Ansi)]
	public struct PortailStreamHostStartParams
	{
		public uint app_id;
		public int width;
		public int height;
		public int fps;
		public int target_bitrate_kbps;
		public int video_rate_control;
		public int disable_ice;
		public int chunk_payload_bytes;
		public int parity_shards;
		public int reliable_video;
		public int reliable_keyframes;
		public int max_queue_ms;
		public int enable_audio;
		public int audio_bitrate_kbps;
		[MarshalAs(UnmanagedType.LPStr)] public string codec;
		[MarshalAs(UnmanagedType.LPStr)] public string encoder;
	}

	[StructLayout(LayoutKind.Sequential)]
	public struct PortailStreamHostVideoLodConfigNative
	{
		public int enabled;
		public int width;
		public int height;
		public int fps;
		public int target_bitrate_kbps;
		public int video_rate_control;
	}

	[StructLayout(LayoutKind.Sequential)]
	public struct PortailStreamHostAudioLodConfigNative
	{
		public int enabled;
		public int bitrate_kbps;
	}

	[StructLayout(LayoutKind.Sequential)]
	public struct PortailStreamHostStats
	{
		public ulong capture_frames;
		public ulong encoded_frames;
		public ulong sent_chunks;
		public ulong sent_bytes;
		public double last_capture_ms;
		public double last_encode_ms;
		public double last_send_ms;
		public double last_video_encode_ms;
		public double last_video_send_ms;
		public double last_audio_encode_ms;
		public double last_audio_send_ms;
		public ulong local_steam_id;
		public int connected;
		public int connected_ice_clients;
		public int connected_sdr_clients;
		public int preview_width;
		public int preview_height;
		public int encoded_width;
		public int encoded_height;
		public int encoded_fps;
		public ulong sent_audio_frames;
		public ulong sent_audio_bytes;
		public int audio_capture_state;
		public int audio_target_pid;
		public int reserved_audio_muted_sessions;
		public ulong local_audio_samples_read;
		public ulong encoded_video_bytes;
		public ulong sent_video_bytes;
		public ulong encoded_audio_frames;
		public ulong encoded_audio_bytes;
	}

	[StructLayout(LayoutKind.Sequential)]
	public struct PortailStreamHostPeerStats
	{
		public ulong client_steam_id;
		public ulong encoded_frames;
		public ulong sent_chunks;
		public ulong sent_bytes;
		public ulong sent_audio_frames;
		public ulong sent_audio_bytes;
		public int connected;
		public int connection_path;
		public int preview_width;
		public int preview_height;
		public int paused_by_client;
		public int assigned_video_lod;
		public int assigned_audio_lod;
		public int max_video_lod;
		public int max_audio_lod;
		public int available_video_lod;
		public int available_audio_lod;
		public int effective_video_lod;
		public int effective_audio_lod;
		public ulong encoded_video_bytes;
		public ulong sent_video_bytes;
		public ulong encoded_audio_frames;
		public ulong encoded_audio_bytes;
		public int video_lod_used;
		public int audio_lod_used;
		public double last_video_send_ms;
		public double last_audio_send_ms;
	}

	[StructLayout(LayoutKind.Sequential)]
	public struct PortailStreamHostVideoLodStats
	{
		public int index;
		public int enabled;
		public int width;
		public int height;
		public int fps;
		public int target_bitrate_kbps;
		public int client_count;
		public ulong encoded_frames;
		public ulong encoded_video_bytes;
		public double last_encode_ms;
		public double last_send_ms;
	}

	[StructLayout(LayoutKind.Sequential)]
	public struct PortailStreamHostAudioLodStats
	{
		public int index;
		public int enabled;
		public int bitrate_kbps;
		public int client_count;
		public ulong encoded_audio_frames;
		public ulong encoded_audio_bytes;
		public double last_encode_ms;
		public double last_send_ms;
	}

	public static class PortailStreamSteamAvailability
	{
		private const string DefaultError = "Steam is not running, not initialized, or not logged in. Start Steam before using the Portail streaming plugins.";

		public static bool TryGetAvailabilityError(out string error)
		{
			try
			{
				if (SteamClient.IsLoggedOn)
				{
					error = string.Empty;
					return true;
				}
			}
			catch
			{
				error = DefaultError;
				return false;
			}

			error = DefaultError;
			return false;
		}
	}

	internal static class PortailStreamHostNative
	{
		private const string DllName = "portail_stream_host_plugin";

		[DllImport(DllName)]
		[return: MarshalAs(UnmanagedType.I1)]
		public static extern bool SSPH_Start(ref PortailStreamHostStartParams startParams);

		[DllImport(DllName)]
		public static extern void SSPH_ConfigureVideoLods([In] PortailStreamHostVideoLodConfigNative[] lods, int count);

		[DllImport(DllName)]
		public static extern void SSPH_ConfigureAudioLods([In] PortailStreamHostAudioLodConfigNative[] lods, int count);

		[DllImport(DllName)]
		public static extern void SSPH_SetVideoLodEnabled(int lod_index, [MarshalAs(UnmanagedType.I1)] bool enabled);

		[DllImport(DllName)]
		public static extern void SSPH_SetAudioLodEnabled(int lod_index, [MarshalAs(UnmanagedType.I1)] bool enabled);

		[DllImport(DllName)]
		public static extern void SSPH_Stop();

		[DllImport(DllName)]
		[return: MarshalAs(UnmanagedType.I1)]
		public static extern bool SSPH_IsRunning();

		[DllImport(DllName)]
		public static extern void SSPH_SetPaused([MarshalAs(UnmanagedType.I1)] bool paused);

		[DllImport(DllName)]
		public static extern void SSPH_SetClientSteamIds([In] ulong[] steam_ids, int count);

		[DllImport(DllName)]
		public static extern void SSPH_SetClientVideoLod(ulong client_steam_id, int lod);

		[DllImport(DllName)]
		public static extern void SSPH_SetClientAudioLod(ulong client_steam_id, int lod);

		[DllImport(DllName)]
		public static extern void SSPH_SetLoggingEnabled([MarshalAs(UnmanagedType.I1)] bool enabled);

		[DllImport(DllName)]
		public static extern void SSPH_SetPreviewTexture(IntPtr native_texture_ptr);

		[DllImport(DllName)]
		public static extern IntPtr SSPH_GetRenderEventFunc();

		[DllImport(DllName)]
		public static extern int SSPH_GetRenderEventId();

		[DllImport(DllName)]
		public static extern void SSPH_GetStats(out PortailStreamHostStats stats);

		[DllImport(DllName)]
		public static extern int SSPH_GetPeerStats([Out] PortailStreamHostPeerStats[] stats, int maxCount);

		[DllImport(DllName)]
		public static extern int SSPH_GetVideoLodStats([Out] PortailStreamHostVideoLodStats[] stats, int maxCount);

		[DllImport(DllName)]
		public static extern int SSPH_GetAudioLodStats([Out] PortailStreamHostAudioLodStats[] stats, int maxCount);

		[DllImport(DllName)]
		private static extern IntPtr SSPH_GetLastError();

		public static string GetLastError()
		{
			IntPtr ptr = SSPH_GetLastError();
			return ptr == IntPtr.Zero ? string.Empty : Marshal.PtrToStringAnsi(ptr) ?? string.Empty;
		}

	}

	public sealed class PortailStreamHostPlugin : MonoBehaviour
	{
		[Header("Host Start Params")]
		public uint appId = 480;
		public List<PortailStreamVideoLodConfig> videoLods = CreateDefaultVideoLods();
		public List<PortailStreamAudioLodConfig> audioLods = CreateDefaultAudioLods();
		public bool disableIce;
		public int chunkPayloadBytes = 24000;
		public int parityShards;
		public bool reliableVideo;
		public bool reliableKeyframes;
		public int maxQueueMs = 120;
		public string codec = "h264";
		public string encoder = "auto";
		public bool autoStart = false;
		public bool enableNativeLogging = true;

		[Header("Unity Output")]
		public RenderTexture previewTexture;

		private IntPtr _renderEventFunc;
		private int _renderEventId;

		public static List<PortailStreamVideoLodConfig> CreateDefaultVideoLods()
		{
			return new List<PortailStreamVideoLodConfig>
			{
				new PortailStreamVideoLodConfig { name = "1080p60", enabled = true, width = 1920, height = 1080, fps = 60, targetBitrateKbps = 12000, rateControl = PortailStreamVideoRateControl.Cbr },
				new PortailStreamVideoLodConfig { name = "720p60", enabled = true, width = 1280, height = 720, fps = 60, targetBitrateKbps = 6000, rateControl = PortailStreamVideoRateControl.Cbr },
				new PortailStreamVideoLodConfig { name = "480p60", enabled = true, width = 854, height = 480, fps = 60, targetBitrateKbps = 3000, rateControl = PortailStreamVideoRateControl.Cbr },
				new PortailStreamVideoLodConfig { name = "360p60", enabled = true, width = 640, height = 360, fps = 60, targetBitrateKbps = 1500, rateControl = PortailStreamVideoRateControl.Cbr },
			};
		}

		public static List<PortailStreamAudioLodConfig> CreateDefaultAudioLods()
		{
			return new List<PortailStreamAudioLodConfig>
			{
				new PortailStreamAudioLodConfig { name = "Opus 96", enabled = true, bitrateKbps = 96 },
			};
		}

		public void EnsureLodDefaults()
		{
			if (videoLods == null || videoLods.Count == 0)
				videoLods = CreateDefaultVideoLods();
			if (audioLods == null || audioLods.Count == 0)
				audioLods = CreateDefaultAudioLods();
		}

		public bool StartStreaming()
		{
			if (!PortailStreamSteamAvailability.TryGetAvailabilityError(out string steamError))
			{
				_renderEventFunc = IntPtr.Zero;
				_renderEventId = 0;
				Debug.LogError($"[PortailStreamHost] Start blocked: {steamError}");
				return false;
			}

			if (!PortailStreamNativeLoader.EnsureLoaded())
			{
				Debug.LogError($"[PortailStreamHost] Native load failed: {PortailStreamNativeLoader.LastError}");
				return false;
			}

			ApplyNativeLogging();
			EnsureLodDefaults();
			PushLodConfiguration();
			PortailStreamVideoLodConfig primaryVideo = GetPrimaryVideoLod();
			PortailStreamAudioLodConfig primaryAudio = GetPrimaryAudioLod();

			PortailStreamHostStartParams p = new PortailStreamHostStartParams
			{
				app_id = appId,
				width = primaryVideo != null ? Mathf.Max(16, primaryVideo.width) : 1280,
				height = primaryVideo != null ? Mathf.Max(16, primaryVideo.height) : 720,
				fps = primaryVideo != null ? Mathf.Clamp(primaryVideo.fps, 1, 240) : 60,
				target_bitrate_kbps = primaryVideo != null ? Mathf.Max(100, primaryVideo.targetBitrateKbps) : 7000,
				video_rate_control = primaryVideo != null ? (int)primaryVideo.rateControl : 0,
				disable_ice = disableIce ? 1 : 0,
				chunk_payload_bytes = chunkPayloadBytes,
				parity_shards = parityShards,
				reliable_video = reliableVideo ? 1 : 0,
				reliable_keyframes = reliableKeyframes ? 1 : 0,
				max_queue_ms = maxQueueMs,
				enable_audio = primaryAudio != null ? 1 : 0,
				audio_bitrate_kbps = primaryAudio != null ? Mathf.Clamp(primaryAudio.bitrateKbps, 32, 512) : 96,
				codec = codec,
				encoder = encoder,
			};

			bool ok = PortailStreamHostNative.SSPH_Start(ref p);
			if (!ok)
			{
				PortailStreamNativeLogBridge.FlushPending();
				Debug.LogError($"[PortailStreamHost] Start failed: {PortailStreamHostNative.GetLastError()}");
				return false;
			}

			_renderEventFunc = PortailStreamHostNative.SSPH_GetRenderEventFunc();
			_renderEventId = PortailStreamHostNative.SSPH_GetRenderEventId();
			PushPreviewTexture();
			return true;
		}

		public void StopStreaming()
		{
			if (!PortailStreamNativeLoader.EnsureLoaded())
			{
				return;
			}
			ApplyNativeLogging();
			PortailStreamHostNative.SSPH_Stop();
			PortailStreamNativeLogBridge.UnregisterHostCallback();
			PortailStreamNativeLogBridge.FlushPending();
			_renderEventFunc = IntPtr.Zero;
			_renderEventId = 0;
		}

		public void SetPaused(bool paused)
		{
			if (!PortailStreamNativeLoader.EnsureLoaded())
			{
				return;
			}
			PortailStreamHostNative.SSPH_SetPaused(paused);
		}

		public void PushLodConfiguration()
		{
			if (!PortailStreamNativeLoader.EnsureLoaded())
				return;

			EnsureLodDefaults();
			PortailStreamHostVideoLodConfigNative[] nativeVideo = new PortailStreamHostVideoLodConfigNative[videoLods.Count];
			for (int i = 0; i < videoLods.Count; ++i)
			{
				PortailStreamVideoLodConfig lod = videoLods[i] ?? new PortailStreamVideoLodConfig();
				nativeVideo[i] = new PortailStreamHostVideoLodConfigNative
				{
					enabled = lod.enabled ? 1 : 0,
					width = Mathf.Max(16, lod.width),
					height = Mathf.Max(16, lod.height),
					fps = Mathf.Clamp(lod.fps, 1, 240),
					target_bitrate_kbps = Mathf.Max(100, lod.targetBitrateKbps),
					video_rate_control = (int)lod.rateControl,
				};
			}

			PortailStreamHostAudioLodConfigNative[] nativeAudio = new PortailStreamHostAudioLodConfigNative[audioLods.Count];
			for (int i = 0; i < audioLods.Count; ++i)
			{
				PortailStreamAudioLodConfig lod = audioLods[i] ?? new PortailStreamAudioLodConfig();
				nativeAudio[i] = new PortailStreamHostAudioLodConfigNative
				{
					enabled = lod.enabled ? 1 : 0,
					bitrate_kbps = Mathf.Clamp(lod.bitrateKbps, 32, 512),
				};
			}

			PortailStreamHostNative.SSPH_ConfigureVideoLods(nativeVideo, nativeVideo.Length);
			PortailStreamHostNative.SSPH_ConfigureAudioLods(nativeAudio, nativeAudio.Length);
		}

		public void SetVideoLodEnabled(int lodIndex, bool enabled)
		{
			EnsureLodDefaults();
			if (lodIndex < 0 || lodIndex >= videoLods.Count)
				return;

			videoLods[lodIndex].enabled = enabled;
			if (PortailStreamNativeLoader.EnsureLoaded())
				PortailStreamHostNative.SSPH_SetVideoLodEnabled(lodIndex, enabled);
		}

		public void SetAudioLodEnabled(int lodIndex, bool enabled)
		{
			EnsureLodDefaults();
			if (lodIndex < 0 || lodIndex >= audioLods.Count)
				return;

			audioLods[lodIndex].enabled = enabled;
			if (PortailStreamNativeLoader.EnsureLoaded())
				PortailStreamHostNative.SSPH_SetAudioLodEnabled(lodIndex, enabled);
		}

		PortailStreamVideoLodConfig GetPrimaryVideoLod()
		{
			EnsureLodDefaults();
			for (int i = 0; i < videoLods.Count; ++i)
			{
				if (videoLods[i] != null)
					return videoLods[i];
			}
			return null;
		}

		PortailStreamAudioLodConfig GetPrimaryAudioLod()
		{
			EnsureLodDefaults();
			for (int i = 0; i < audioLods.Count; ++i)
			{
				if (audioLods[i] != null)
					return audioLods[i];
			}
			return null;
		}

		public void SetClientSteamIds(IReadOnlyList<ulong> steamIds)
		{
			if (!PortailStreamNativeLoader.EnsureLoaded())
			{
				return;
			}

			if (steamIds == null || steamIds.Count == 0)
			{
				PortailStreamHostNative.SSPH_SetClientSteamIds(Array.Empty<ulong>(), 0);
				return;
			}

			ulong[] values = new ulong[steamIds.Count];
			for (int i = 0; i < steamIds.Count; ++i)
			{
				values[i] = steamIds[i];
			}
			PortailStreamHostNative.SSPH_SetClientSteamIds(values, values.Length);
		}

		public void SetClientVideoLod(ulong clientSteamId, int lod)
		{
			SetClientMaxAllowedVideoLod(clientSteamId, lod);
		}

		public void SetClientAudioLod(ulong clientSteamId, int lod)
		{
			SetClientMaxAllowedAudioLod(clientSteamId, lod);
		}

		public void SetClientMaxAllowedVideoLod(ulong clientSteamId, int lod)
		{
			if (clientSteamId == 0 || !PortailStreamNativeLoader.EnsureLoaded())
			{
				return;
			}

			PortailStreamHostNative.SSPH_SetClientVideoLod(clientSteamId, PortailStreamLodUtility.NormalizeLod(lod));
		}

		public void SetClientMaxAllowedAudioLod(ulong clientSteamId, int lod)
		{
			if (clientSteamId == 0 || !PortailStreamNativeLoader.EnsureLoaded())
			{
				return;
			}

			PortailStreamHostNative.SSPH_SetClientAudioLod(clientSteamId, PortailStreamLodUtility.NormalizeLod(lod));
		}

		public bool IsRunning()
		{
			if (!PortailStreamNativeLoader.EnsureLoaded())
			{
				return false;
			}
			return PortailStreamHostNative.SSPH_IsRunning();
		}

		public PortailStreamHostStats GetStats()
		{
			if (!PortailStreamNativeLoader.EnsureLoaded())
			{
				return default;
			}
			PortailStreamHostNative.SSPH_GetStats(out PortailStreamHostStats stats);
			return stats;
		}

		public PortailStreamHostPeerStats[] GetPeerStats(int maxPeers = 64)
		{
			if (!PortailStreamNativeLoader.EnsureLoaded() || maxPeers <= 0)
			{
				return Array.Empty<PortailStreamHostPeerStats>();
			}

			try
			{
				PortailStreamHostPeerStats[] buffer = new PortailStreamHostPeerStats[maxPeers];
				int count = PortailStreamHostNative.SSPH_GetPeerStats(buffer, buffer.Length);
				if (count <= 0)
				{
					return Array.Empty<PortailStreamHostPeerStats>();
				}
				if (count == buffer.Length)
				{
					return buffer;
				}

				PortailStreamHostPeerStats[] trimmed = new PortailStreamHostPeerStats[count];
				Array.Copy(buffer, trimmed, count);
				return trimmed;
			}
			catch (EntryPointNotFoundException)
			{
				return Array.Empty<PortailStreamHostPeerStats>();
			}
		}

		public PortailStreamHostVideoLodStats[] GetVideoLodStats(int maxLods = 16)
		{
			if (!PortailStreamNativeLoader.EnsureLoaded() || maxLods <= 0)
				return Array.Empty<PortailStreamHostVideoLodStats>();

			try
			{
				PortailStreamHostVideoLodStats[] buffer = new PortailStreamHostVideoLodStats[maxLods];
				int count = PortailStreamHostNative.SSPH_GetVideoLodStats(buffer, buffer.Length);
				if (count <= 0)
					return Array.Empty<PortailStreamHostVideoLodStats>();
				if (count == buffer.Length)
					return buffer;

				PortailStreamHostVideoLodStats[] trimmed = new PortailStreamHostVideoLodStats[count];
				Array.Copy(buffer, trimmed, count);
				return trimmed;
			}
			catch (EntryPointNotFoundException)
			{
				return Array.Empty<PortailStreamHostVideoLodStats>();
			}
		}

		public PortailStreamHostAudioLodStats[] GetAudioLodStats(int maxLods = 16)
		{
			if (!PortailStreamNativeLoader.EnsureLoaded() || maxLods <= 0)
				return Array.Empty<PortailStreamHostAudioLodStats>();

			try
			{
				PortailStreamHostAudioLodStats[] buffer = new PortailStreamHostAudioLodStats[maxLods];
				int count = PortailStreamHostNative.SSPH_GetAudioLodStats(buffer, buffer.Length);
				if (count <= 0)
					return Array.Empty<PortailStreamHostAudioLodStats>();
				if (count == buffer.Length)
					return buffer;

				PortailStreamHostAudioLodStats[] trimmed = new PortailStreamHostAudioLodStats[count];
				Array.Copy(buffer, trimmed, count);
				return trimmed;
			}
			catch (EntryPointNotFoundException)
			{
				return Array.Empty<PortailStreamHostAudioLodStats>();
			}
		}

		private void ApplyNativeLogging()
		{
			if (!PortailStreamNativeLoader.EnsureLoaded())
			{
				return;
			}

			PortailStreamHostNative.SSPH_SetLoggingEnabled(enableNativeLogging);
			if (enableNativeLogging)
			{
				PortailStreamNativeLogBridge.RegisterHostCallback();
			}
			else
			{
				PortailStreamNativeLogBridge.UnregisterHostCallback();
			}
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

			if (!PortailStreamHostNative.SSPH_IsRunning())
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
			PortailStreamNativeLogBridge.FlushPending();

			if (!PortailStreamNativeLoader.EnsureLoaded())
			{
				return;
			}

			if (PortailStreamHostNative.SSPH_IsRunning())
			{
				StopStreaming();
			}
			else
			{
				PortailStreamNativeLogBridge.UnregisterHostCallback();
			}
		}

		private void PushPreviewTexture()
		{
			if (!PortailStreamNativeLoader.EnsureLoaded())
			{
				return;
			}

			ApplyNativeLogging();

			if (previewTexture == null)
			{
				return;
			}
			PortailStreamHostNative.SSPH_SetPreviewTexture(previewTexture.GetNativeTexturePtr());
		}
	}
}
