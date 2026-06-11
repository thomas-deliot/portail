using System.Collections;
using UnityEngine;

namespace Portail.Capture
{
	/// <summary>
	/// Optional scene addon that selects the first available full display capture target
	/// on PortailCaptureSession as soon as the manager has enumerated displays.
	///
	/// Add this to any active scene GameObject. Enable/disable this component to control
	/// whether the automatic selection runs, without changing PortailCaptureSession itself.
	/// </summary>
	[DisallowMultipleComponent]
	public sealed class AutoStartPortailCapture : MonoBehaviour
	{
		[SerializeField]
		private bool runOnce = true;

		[SerializeField]
		private bool stopCaptureOnDisable = false;

		[SerializeField]
		private float retryIntervalSeconds = 0.25f;

		private Coroutine _startupRoutine;
		private bool _hasSelectedDisplay;

		private void OnEnable()
		{
			if (runOnce && _hasSelectedDisplay)
			{
				return;
			}

			_startupRoutine = StartCoroutine(SelectFirstDisplayWhenReady());
		}

		private void OnDisable()
		{
			if (_startupRoutine != null)
			{
				StopCoroutine(_startupRoutine);
				_startupRoutine = null;
			}

			if (stopCaptureOnDisable && PortailCaptureSession.Instance != null)
			{
				PortailCaptureSession.Instance.SetCaptureWindowHandle(0);
			}
		}

		private IEnumerator SelectFirstDisplayWhenReady()
		{
			float retryDelay = Mathf.Max(0.05f, retryIntervalSeconds);

			while (enabled && isActiveAndEnabled)
			{
				PortailCaptureSession manager = PortailCaptureSession.Instance;
				if (manager == null)
				{
					yield return new WaitForSecondsRealtime(retryDelay);
					continue;
				}

				manager.RefreshAvailableCaptureTargets();

				if (manager.AvailableDisplays != null && manager.AvailableDisplays.Count > 0)
				{
					PortailCaptureSession.DisplayEntry display = manager.AvailableDisplays[0];
					manager.SetCaptureDisplay(display.Handle);

					_hasSelectedDisplay = true;
					_startupRoutine = null;
					yield break;
				}

				yield return new WaitForSecondsRealtime(retryDelay);
			}

			_startupRoutine = null;
		}
	}
}
