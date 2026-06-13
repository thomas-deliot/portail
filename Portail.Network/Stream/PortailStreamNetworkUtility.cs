using System;
using System.Collections.Generic;
using System.Reflection;
using System.Text;
using Mirror;
using Steamworks;
using UnityEngine;
using UnityEngine.Experimental.Rendering;

namespace Portail.Stream
{
	internal struct PortailStreamRateSample
	{
		public ulong lastBytes;
		public ulong lastFrames;
		public float lastTime;
		public float bitrateKbps;
		public float fps;
		public bool initialized;
	}

	internal static class PortailStreamNetworkUtility
	{
		public static void RefreshPlayerViews(
			Dictionary<int, PortailStreamParticipant> players,
			HashSet<int> livePlayerIds,
			List<int> stalePlayerIds,
			out PortailStreamParticipant localSource)
		{
			livePlayerIds.Clear();
			localSource = null;

			if (NetworkClient.spawned != null)
			{
				foreach (KeyValuePair<uint, NetworkIdentity> entry in NetworkClient.spawned)
				{
					NetworkIdentity identity = entry.Value;
					if (identity == null || !identity.TryGetComponent(out PortailStreamParticipant source) || source == null)
						continue;

					int id = source.GetInstanceID();
					livePlayerIds.Add(id);
					players[id] = source;
					if (source.isLocalPlayer)
						localSource = source;
				}
			}

			stalePlayerIds.Clear();
			foreach (KeyValuePair<int, PortailStreamParticipant> pair in players)
			{
				int id = pair.Key;
				if (livePlayerIds.Contains(id) && pair.Value != null)
					continue;

				stalePlayerIds.Add(id);
			}

			for (int i = 0; i < stalePlayerIds.Count; ++i)
				players.Remove(stalePlayerIds[i]);
		}

		public static List<ulong> BuildRemoteStreamIds(
			Dictionary<int, PortailStreamParticipant> players,
			List<ulong> destination,
			HashSet<ulong> seen,
			PortailStreamParticipant localSource,
			bool requireBroadcasting)
		{
			destination.Clear();
			seen.Clear();
			ulong localSteamId = ResolveLocalSteamId(localSource, null, null);

			foreach (KeyValuePair<int, PortailStreamParticipant> pair in players)
			{
				PortailStreamParticipant source = pair.Value;
				if (source == null || source.isLocalPlayer)
					continue;

				if (requireBroadcasting && !source.IsBroadcasting)
					continue;

				ulong steamId = source.OwnerStreamId;
				if (steamId == 0 || steamId == localSteamId || !seen.Add(steamId))
					continue;

				destination.Add(steamId);
			}

			destination.Sort();
			return destination;
		}

		public static List<ulong> SanitizeSteamIds(IReadOnlyList<ulong> source, List<ulong> destination, HashSet<ulong> seen)
		{
			destination.Clear();
			if (source == null)
				return destination;

			seen.Clear();
			for (int i = 0; i < source.Count; ++i)
			{
				ulong steamId = source[i];
				if (steamId == 0 || !seen.Add(steamId))
					continue;

				destination.Add(steamId);
			}

			destination.Sort();
			return destination;
		}

		public static ulong ResolveLocalSteamId(
			PortailStreamParticipant localSource,
			PortailStreamSenderPlugin sender,
			PortailStreamReceiverPlugin receiver)
		{
			if (localSource != null && localSource.OwnerStreamId != 0)
				return localSource.OwnerStreamId;

			if (TryGetActiveFizzySteamId(out ulong fizzySteamId))
				return fizzySteamId;

			try
			{
				if (SteamClient.IsLoggedOn &&
					ulong.TryParse(SteamClient.SteamId.ToString(), out ulong steamId) &&
					steamId != 0)
				{
					return steamId;
				}
			}
			catch
			{
				// Facepunch Steamworks is optional at runtime.
			}

			if (sender != null && sender.IsRunning())
			{
				PortailStreamSenderStats senderStats = sender.GetStats();
				if (senderStats.local_steam_id != 0)
					return senderStats.local_steam_id;
			}

			if (receiver != null && receiver.IsRunning())
			{
				PortailStreamReceiverStats receiverStats = receiver.GetStats();
				if (receiverStats.local_steam_id != 0)
					return receiverStats.local_steam_id;
			}

			return 0;
		}

		public static bool IsStreamingAvailable(out string error)
		{
			if (!IsFizzyFacepunchActive())
			{
				error = "Portail streaming requires FizzyFacepunch to be the active Mirror transport.";
				return false;
			}

			return PortailStreamSteamAvailability.TryGetAvailabilityError(out error);
		}

		private static bool IsFizzyFacepunchActive()
		{
			Transport transport = Transport.active;
			return transport != null &&
				string.Equals(
					transport.GetType().FullName,
					"Mirror.FizzySteam.FizzyFacepunch",
					StringComparison.Ordinal);
		}

		private static bool TryGetActiveFizzySteamId(out ulong steamId)
		{
			steamId = 0;
			Transport transport = Transport.active;
			if (transport == null || !IsFizzyFacepunchActive())
			{
				return false;
			}

			Type type = transport.GetType();
			object value = null;
			PropertyInfo property = type.GetProperty("SteamUserID", BindingFlags.Instance | BindingFlags.Public);
			if (property != null)
			{
				value = property.GetValue(transport);
			}
			else
			{
				FieldInfo field = type.GetField("SteamUserID", BindingFlags.Instance | BindingFlags.Public);
				if (field != null)
				{
					value = field.GetValue(transport);
				}
			}

			if (value == null)
			{
				return false;
			}

			try
			{
				steamId = Convert.ToUInt64(value);
				return steamId != 0;
			}
			catch
			{
				steamId = 0;
				return false;
			}
		}

		public static bool EnsureSteamAvailable(string logPrefix, ref bool logged)
		{
			if (IsStreamingAvailable(out string error))
			{
				logged = false;
				return true;
			}

			if (!logged)
			{
				Debug.LogError($"[{logPrefix}] Streaming blocked: {error}");
				logged = true;
			}

			return false;
		}

		public static bool SteamIdListsEqual(IReadOnlyList<ulong> lhs, IReadOnlyList<ulong> rhs)
		{
			int lhsCount = lhs != null ? lhs.Count : 0;
			int rhsCount = rhs != null ? rhs.Count : 0;
			if (lhsCount != rhsCount)
				return false;

			for (int i = 0; i < lhsCount; ++i)
			{
				if (lhs[i] != rhs[i])
					return false;
			}

			return true;
		}

		public static void CopySteamIds(IReadOnlyList<ulong> source, List<ulong> destination)
		{
			destination.Clear();
			if (source == null)
				return;

			for (int i = 0; i < source.Count; ++i)
				destination.Add(source[i]);
		}

		public static void UpdateRateSample(ref PortailStreamRateSample sample, ulong totalBytes, ulong totalFrames, float now, float staleAfterSeconds = 1.25f)
		{
			if (!sample.initialized)
			{
				sample.lastBytes = totalBytes;
				sample.lastFrames = totalFrames;
				sample.lastTime = now;
				sample.bitrateKbps = 0.0f;
				sample.fps = 0.0f;
				sample.initialized = true;
				return;
			}

			float deltaTime = now - sample.lastTime;
			if (deltaTime <= 0.0001f)
				return;

			if (totalBytes < sample.lastBytes || totalFrames < sample.lastFrames)
			{
				sample.lastBytes = totalBytes;
				sample.lastFrames = totalFrames;
				sample.lastTime = now;
				sample.bitrateKbps = 0.0f;
				sample.fps = 0.0f;
				return;
			}

			ulong deltaBytes = totalBytes - sample.lastBytes;
			ulong deltaFrames = totalFrames - sample.lastFrames;
			if (deltaBytes == 0 && deltaFrames == 0)
			{
				if (deltaTime >= Mathf.Max(0.1f, staleAfterSeconds))
				{
					sample.bitrateKbps = 0.0f;
					sample.fps = 0.0f;
					sample.lastTime = now;
				}
				return;
			}

			sample.bitrateKbps = (float)(deltaBytes * 8.0 / 1000.0 / deltaTime);
			sample.fps = (float)(deltaFrames / deltaTime);
			sample.lastBytes = totalBytes;
			sample.lastFrames = totalFrames;
			sample.lastTime = now;
		}

		public static RenderTexture CreatePreviewTexture(string name, int width, int height)
		{
			int w = Mathf.Max(16, width);
			int h = Mathf.Max(16, height);
			bool useSrgb = QualitySettings.activeColorSpace == ColorSpace.Linear;

			GraphicsFormat preferred = useSrgb
				? GraphicsFormat.B8G8R8A8_SRGB
				: GraphicsFormat.B8G8R8A8_UNorm;
			GraphicsFormat fallback = useSrgb
				? GraphicsFormat.R8G8B8A8_SRGB
				: GraphicsFormat.R8G8B8A8_UNorm;

			GraphicsFormat chosen;
			if (SystemInfo.IsFormatSupported(preferred, GraphicsFormatUsage.Render))
			{
				chosen = preferred;
			}
			else if (SystemInfo.IsFormatSupported(fallback, GraphicsFormatUsage.Render))
			{
				chosen = fallback;
			}
			else
			{
				GraphicsFormat linearPreferred = GraphicsFormat.B8G8R8A8_UNorm;
				GraphicsFormat linearFallback = GraphicsFormat.R8G8B8A8_UNorm;
				chosen = SystemInfo.IsFormatSupported(linearPreferred, GraphicsFormatUsage.Render)
					? linearPreferred
					: linearFallback;
				useSrgb = false;
			}

			RenderTextureDescriptor descriptor = new RenderTextureDescriptor(w, h)
			{
				depthBufferBits = 0,
				msaaSamples = 1,
				graphicsFormat = chosen,
				useMipMap = true,
				autoGenerateMips = false,
				sRGB = useSrgb,
			};

			RenderTexture texture = new RenderTexture(descriptor)
			{
				name = name,
				wrapMode = TextureWrapMode.Clamp,
				filterMode = FilterMode.Trilinear,
			};
			texture.Create();
			return texture;
		}

		public static void ReleaseTexture(RenderTexture texture)
		{
			if (texture == null)
				return;

			texture.Release();
			UnityEngine.Object.Destroy(texture);
		}

		public static string FormatSteamIds(IReadOnlyList<ulong> steamIds)
		{
			if (steamIds == null || steamIds.Count == 0)
				return "None";

			StringBuilder builder = new StringBuilder();
			for (int i = 0; i < steamIds.Count; ++i)
			{
				if (i > 0)
					builder.Append(", ");

				builder.Append(steamIds[i]);
			}
			return builder.ToString();
		}
	}
}
