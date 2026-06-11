using UnityEngine;

namespace Portail.Core
{
	public interface IPortailCaptureTarget
	{
		bool IsDisplayCaptureSelected { get; }
		ulong SelectedWindowHandle { get; }
		bool TryGetSelectedCaptureScreenRect(out RectInt screenRect);
	}

	public static class PortailCaptureTargetRegistry
	{
		public static IPortailCaptureTarget Current { get; set; }
	}

	public static class PortailControlState
	{
		public static bool IsDesktopControlActive { get; set; }
	}
}
