using System;
using System.Collections.Generic;
using System.IO;
using System.Runtime.InteropServices;
using UnityEngine;

namespace Portail.Stream
{
	internal static class PortailStreamNativeLoader
	{
		private static bool _attempted;
		private static bool _loaded;
		private static string _lastError = string.Empty;

		[DllImport("kernel32", CharSet = CharSet.Unicode, SetLastError = true)]
		private static extern IntPtr LoadLibraryW(string lpFileName);

		public static string LastError => _lastError;

		public static bool HasRunningNativeStreams()
		{
#if !(UNITY_EDITOR_WIN || UNITY_STANDALONE_WIN)
            return false;
#else
			if (!EnsureLoaded())
			{
				return false;
			}

			try
			{
				return PortailStreamSenderNative.SSPS_IsRunning() ||
					   PortailStreamReceiverNative.SSPR_IsRunning();
			}
			catch (Exception ex) when (ex is DllNotFoundException || ex is EntryPointNotFoundException || ex is BadImageFormatException)
			{
				_lastError = $"Portail Stream native stream state check failed: {ex.Message}";
				return true;
			}
#endif
		}

		public static bool EnsureLoaded()
		{
#if !(UNITY_EDITOR_WIN || UNITY_STANDALONE_WIN)
            _lastError = "Portail streaming plugins are only supported on Windows.";
            return false;
#else
			if (_attempted)
			{
				return _loaded;
			}

			_attempted = true;

			string packageRoot = GetEditorPackageRoot();
			string streamDir = FindFirstExistingDirectory(
				Path.Combine(packageRoot, "Stream", "Plugins"),
				Path.Combine(Application.dataPath, "Portail", "Stream", "Plugins"),
				Path.Combine(Application.dataPath, "Plugins", "Portail", "Stream", "x86_64"),
				Path.Combine(Application.dataPath, "Plugins", "x86_64"),
				Path.Combine(AppContext.BaseDirectory, "Plugins", "Portail", "Stream", "x86_64"),
				Path.Combine(AppContext.BaseDirectory, "Plugins", "x86_64")
			);

			if (string.IsNullOrWhiteSpace(streamDir))
			{
				_lastError = "PortailStream plugin directory not found.";
				Debug.LogError($"[PortailStreamLoader] {_lastError}");
				return false;
			}

			string facepunchDir = FindFirstExistingDirectory(
				Path.Combine(Application.dataPath, "Third Party", "Mirror", "Plugins", "Facepunch.Steamworks", "redistributable_bin", "win64"),
				Path.Combine(Application.dataPath, "Plugins", "Facepunch.Steamworks", "redistributable_bin", "win64"),
				Path.Combine(AppContext.BaseDirectory, "Plugins", "Facepunch.Steamworks", "redistributable_bin", "win64")
			);

			string thirdPartyPluginDir = FindFirstExistingDirectory(
				streamDir,
				Path.Combine(Application.dataPath, "Third Party", "Plugins"),
				Path.Combine(Application.dataPath, "ThirdParty", "Plugins"),
				Path.Combine(Application.dataPath, "Plugins", "x86_64"),
				Path.Combine(AppContext.BaseDirectory, "Plugins", "Third Party"),
				Path.Combine(AppContext.BaseDirectory, "Plugins", "ThirdParty"),
				Path.Combine(AppContext.BaseDirectory, "Plugins", "x86_64")
			);

			string steamApiPath = FirstExistingFile(
				Path.Combine(facepunchDir ?? string.Empty, "steam_api64.dll"),
				Path.Combine(streamDir, "steam_api64.dll")
			);

			if (string.IsNullOrWhiteSpace(steamApiPath))
			{
				_lastError = "steam_api64.dll not found in Facepunch or PortailStream plugin folders.";
				Debug.LogError($"[PortailStreamLoader] {_lastError}");
				return false;
			}

			if (string.IsNullOrWhiteSpace(thirdPartyPluginDir))
			{
				_lastError = "Third Party plugin directory not found.";
				Debug.LogError($"[PortailStreamLoader] {_lastError}");
				return false;
			}

			List<string> requiredFiles = new List<string>
			{
				steamApiPath,
				Path.Combine(thirdPartyPluginDir, "avutil-60.dll"),
				Path.Combine(thirdPartyPluginDir, "swresample-6.dll"),
				Path.Combine(thirdPartyPluginDir, "swscale-9.dll"),
				Path.Combine(thirdPartyPluginDir, "avcodec-62.dll"),
				Path.Combine(thirdPartyPluginDir, "steamwebrtc64.dll"),
				Path.Combine(streamDir, "portail_stream_common.dll"),
				Path.Combine(streamDir, "portail_stream_sender_plugin.dll"),
				Path.Combine(streamDir, "portail_stream_receiver_plugin.dll"),
			};

			for (int i = 0; i < requiredFiles.Count; ++i)
			{
				string path = requiredFiles[i];

				IntPtr handle = LoadLibraryW(path);
				if (handle == IntPtr.Zero)
				{
					int error = Marshal.GetLastWin32Error();
					_lastError = $"LoadLibrary failed for '{path}' with Win32 error {error}.";
					Debug.LogError($"[PortailStreamLoader] {_lastError}");
					return false;
				}
			}

			try
			{
				for (int i = 0; i < requiredFiles.Count; ++i)
				{
					LoadLibraryW(requiredFiles[i]);
				}

				PortailStreamSenderNative.SSPS_IsRunning();
				PortailStreamReceiverNative.SSPR_IsRunning();
			}
			catch (Exception ex) when (ex is DllNotFoundException || ex is EntryPointNotFoundException || ex is BadImageFormatException)
			{
				_lastError = $"Portail Stream native plugin load failed: {ex.Message}";
				Debug.LogError($"[PortailStreamLoader] {_lastError}");
				return false;
			}

			_loaded = true;
			_lastError = string.Empty;
			return true;
#endif
		}

		private static string FindFirstExistingDirectory(params string[] candidates)
		{
			for (int i = 0; i < candidates.Length; ++i)
			{
				string candidate = candidates[i];
				if (!string.IsNullOrWhiteSpace(candidate) && Directory.Exists(candidate))
				{
					return candidate;
				}
			}
			return string.Empty;
		}

		private static string FirstExistingFile(params string[] candidates)
		{
			for (int i = 0; i < candidates.Length; ++i)
			{
				string candidate = candidates[i];
				if (!string.IsNullOrWhiteSpace(candidate) && File.Exists(candidate))
				{
					return candidate;
				}
			}
			return string.Empty;
		}

		private static string GetEditorPackageRoot()
		{
#if UNITY_EDITOR
			UnityEditor.PackageManager.PackageInfo packageInfo =
				UnityEditor.PackageManager.PackageInfo.FindForAssembly(typeof(PortailStreamNativeLoader).Assembly);
			return packageInfo != null ? packageInfo.resolvedPath : string.Empty;
#else
			return string.Empty;
#endif
		}
	}
}
