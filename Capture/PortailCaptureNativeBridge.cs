using System;
using System.IO;
using System.Runtime.InteropServices;
using UnityEngine;

namespace Portail.Capture
{
	internal static class PortailCaptureNativeBridge
	{
		public const string DllName = "portail_capture_plugin";
	}

	internal static class PortailCaptureNativeLoader
	{
		private static bool _attempted;
		private static bool _loaded;
		private static string _lastError = string.Empty;

		public static string LastError => _lastError;

		public static bool HasRunningNativeCapture()
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
				return PortailCaptureNative.SSPCAP_IsRunning();
			}
			catch (Exception ex) when (ex is DllNotFoundException || ex is EntryPointNotFoundException || ex is BadImageFormatException)
			{
				_lastError = $"Portail capture native state check failed: {ex.Message}";
				return true;
			}
#endif
		}

		public static bool EnsureLoaded()
		{
#if !(UNITY_EDITOR_WIN || UNITY_STANDALONE_WIN)
			_lastError = "Portail capture is only supported on Windows.";
			return false;
#else
			if (_attempted)
			{
				return _loaded;
			}

			_attempted = true;

			string packageRoot = GetEditorPackageRoot();
			string pluginDir = FindFirstExistingDirectory(
				Path.Combine(packageRoot, "Capture", "Plugins"),
				Path.Combine(Application.dataPath, "Portail", "Capture", "Plugins"),
				Path.Combine(Application.dataPath, "Plugins", "Portail", "Capture", "x86_64"),
				Path.Combine(Application.dataPath, "Plugins", "x86_64"),
				Path.Combine(AppContext.BaseDirectory, "Plugins", "Portail", "Capture", "x86_64"),
				Path.Combine(AppContext.BaseDirectory, "Plugins", "x86_64")
			);

			if (string.IsNullOrWhiteSpace(pluginDir))
			{
				_lastError = "Portail capture plugin directory not found.";
				Debug.LogError($"[PortailCaptureLoader] {_lastError}");
				return false;
			}

			string pluginPath = Path.Combine(pluginDir, $"{PortailCaptureNativeBridge.DllName}.dll");
			if (!File.Exists(pluginPath))
			{
				_lastError = $"Required runtime DLL missing: '{pluginPath}'.";
				Debug.LogError($"[PortailCaptureLoader] {_lastError}");
				return false;
			}

			try
			{
				PortailCaptureNative.SSPCAP_IsRunning();
			}
			catch (Exception ex) when (ex is DllNotFoundException || ex is EntryPointNotFoundException || ex is BadImageFormatException)
			{
				_lastError = $"Portail capture native load failed: {ex.Message}";
				Debug.LogError($"[PortailCaptureLoader] {_lastError}");
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

		private static string GetEditorPackageRoot()
		{
#if UNITY_EDITOR
			UnityEditor.PackageManager.PackageInfo packageInfo =
				UnityEditor.PackageManager.PackageInfo.FindForAssembly(typeof(PortailCaptureNativeLoader).Assembly);
			return packageInfo != null ? packageInfo.resolvedPath : string.Empty;
#else
			return string.Empty;
#endif
		}
	}
}
