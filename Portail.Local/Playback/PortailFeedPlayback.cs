using Portail.Core;
using UnityEngine;

namespace Portail.Playback
{
	[DisallowMultipleComponent]
	public sealed class PortailFeedPlayback : MonoBehaviour, IPortailFeedPlayback
	{
		static bool s_localControlMenuVisible;

	[SerializeField] PortailFeed feed;
	[SerializeField] MeshRenderer screenDisplaySurface;
	[SerializeField] Transform leftSpeakerTransform;
	[SerializeField] Transform rightSpeakerTransform;

	MeshRenderer _streamRenderer;
	MaterialPropertyBlock _streamProperties;
	PortailAudioPlayback _fallbackAudioSource;
	PortailAudioPlayback _leftAudioSource;
	PortailAudioPlayback _rightAudioSource;
	PortailFeed _subscribedAudioSource;
	Texture _lastStreamTexture;

	public static bool IsLocalControlMenuVisible => s_localControlMenuVisible;
	public PortailFeed Feed => feed;
	public PortailFeed StreamSource => feed;
	public MeshRenderer ScreenDisplaySurface => screenDisplaySurface;
	public Transform LeftSpeakerTransform => leftSpeakerTransform;
	public Transform RightSpeakerTransform => rightSpeakerTransform;

	public static void SetLocalControlMenuVisible(bool visible)
	{
		s_localControlMenuVisible = visible;
	}

	public void SetDisplayOutputs(MeshRenderer displaySurface, Transform leftSpeaker, Transform rightSpeaker)
	{
		if (screenDisplaySurface == displaySurface &&
			leftSpeakerTransform == leftSpeaker &&
			rightSpeakerTransform == rightSpeaker)
		{
			return;
		}

		ClearAudioOutputs();
		screenDisplaySurface = displaySurface;
		leftSpeakerTransform = leftSpeaker;
		rightSpeakerTransform = rightSpeaker;
		_streamRenderer = null;
		_leftAudioSource = null;
		_rightAudioSource = null;
		_fallbackAudioSource = null;
		_lastStreamTexture = null;
		EnsureReferences();
		RefreshDisplay(force: true);
	}

	public void SetStreamSource(PortailFeed source)
	{
		feed = source;
		EnsureReferences();
		SyncAudioSubscription();
		_lastStreamTexture = null;
		RefreshDisplay(force: true);
	}

	public void SetStreamPlayer(PortailFeed source)
	{
		SetStreamSource(source);
	}

	void Awake()
	{
		EnsureReferences();
		EnsureFeed();
		SyncAudioSubscription();
		RefreshDisplay(force: true);
	}

	void OnEnable()
	{
		EnsureReferences();
		EnsureFeed();
		SyncAudioSubscription();
		RefreshDisplay(force: true);
	}

	void OnDisable()
	{
		SetSubscribedAudioSource(null);
		ClearAudioOutputs();
	}

	void LateUpdate()
	{
		RefreshDisplay(force: false);
	}

	void OnDestroy()
	{
		SetSubscribedAudioSource(null);
	}

	void RefreshDisplay(bool force)
	{
		EnsureReferences();
		EnsureFeed();
		SyncAudioSubscription();
		ApplyAudioLatencySettings();

		Texture streamTexture = feed != null ? feed.StreamTexture : null;
		if (force || _lastStreamTexture != streamTexture)
		{
			ApplyStreamTexture(streamTexture);
			_lastStreamTexture = streamTexture;
		}
	}

	void EnsureReferences()
	{
		EnsureRenderer();
		EnsureAudioSources();
	}

	void EnsureFeed()
	{
		if (feed == null)
			feed = GetComponentInParent<PortailFeed>();
	}

	void EnsureRenderer()
	{
		if (_streamRenderer != null)
			return;

		if (screenDisplaySurface != null)
		{
			_streamRenderer = screenDisplaySurface;
			return;
		}

		if (TryGetComponent(out MeshRenderer localRenderer))
		{
			_streamRenderer = localRenderer;
			screenDisplaySurface = localRenderer;
			return;
		}

		Transform screen = transform.Find("Presentation/Screen");
		if (screen != null && screen.TryGetComponent(out MeshRenderer screenRenderer))
		{
			_streamRenderer = screenRenderer;
			screenDisplaySurface = screenRenderer;
		}
	}

	void EnsureAudioSources()
	{
		if (leftSpeakerTransform != null && rightSpeakerTransform != null)
		{
			_leftAudioSource = EnsureAudioSource(leftSpeakerTransform, PortailAudioPlayback.OutputChannel.Left);
			_rightAudioSource = EnsureAudioSource(rightSpeakerTransform, PortailAudioPlayback.OutputChannel.Right);
			return;
		}

		if (_fallbackAudioSource == null)
			_fallbackAudioSource = EnsureAudioSource(transform, PortailAudioPlayback.OutputChannel.Stereo);
	}

	PortailAudioPlayback EnsureAudioSource(Transform sourceTransform, PortailAudioPlayback.OutputChannel channel)
	{
		if (sourceTransform == null)
			return null;

		PortailAudioPlayback audioSource = sourceTransform.GetComponent<PortailAudioPlayback>();
		if (audioSource == null)
			audioSource = sourceTransform.gameObject.AddComponent<PortailAudioPlayback>();

		audioSource.SetOutputChannel(channel);
		return audioSource;
	}

	void ApplyAudioLatencySettings()
	{
		PortailAudioLatencySettings settings = feed != null
			? feed.AudioLatency
			: PortailAudioLatencySettings.Default;
		settings.Normalize();

		ApplyAudioLatencySettings(_leftAudioSource, settings);
		ApplyAudioLatencySettings(_rightAudioSource, settings);
		ApplyAudioLatencySettings(_fallbackAudioSource, settings);
	}

	static void ApplyAudioLatencySettings(PortailAudioPlayback audioSource, PortailAudioLatencySettings settings)
	{
		if (audioSource == null)
			return;

		audioSource.SetLatencySettings(settings.maxQueuedLatencyMs, settings.targetQueuedLatencyMs);
	}

	void SyncAudioSubscription()
	{
		SetSubscribedAudioSource(feed);
	}

	void SetSubscribedAudioSource(PortailFeed nextSource)
	{
		if (_subscribedAudioSource == nextSource)
			return;

		if (_subscribedAudioSource != null)
			_subscribedAudioSource.AudioSamplesPushed -= OnStreamSourceAudioSamplesPushed;

		_subscribedAudioSource = nextSource;

		if (_subscribedAudioSource != null)
			_subscribedAudioSource.AudioSamplesPushed += OnStreamSourceAudioSamplesPushed;

		ClearAudioOutputs();
	}

	void OnStreamSourceAudioSamplesPushed(PortailFeed source, float[] samples, int sampleCount)
	{
		if (source == null || source != feed)
			return;

		PushSamplesToAudioOutputs(samples, sampleCount);
	}

	void PushSamplesToAudioOutputs(float[] samples, int sampleCount)
	{
		if (_leftAudioSource != null && _rightAudioSource != null)
		{
			_leftAudioSource.PushSamples(samples, sampleCount);
			_rightAudioSource.PushSamples(samples, sampleCount);
			return;
		}

		if (_fallbackAudioSource != null)
			_fallbackAudioSource.PushSamples(samples, sampleCount);
	}

	void ClearAudioOutputs()
	{
		ClearAudioOutput(_leftAudioSource);
		ClearAudioOutput(_rightAudioSource);
		ClearAudioOutput(_fallbackAudioSource);
	}

	static void ClearAudioOutput(PortailAudioPlayback audioSource)
	{
		if (audioSource == null)
			return;

		audioSource.SetDirectSampleReader(null);
		audioSource.ClearQueuedSamples();
	}

	void ApplyStreamTexture(Texture texture)
	{
		if (_streamRenderer == null)
			return;

		if (_streamProperties == null)
			_streamProperties = new MaterialPropertyBlock();

		_streamRenderer.GetPropertyBlock(_streamProperties);
		_streamProperties.SetTexture("_EmissionMap", texture != null ? texture : Texture2D.blackTexture);
		_streamRenderer.SetPropertyBlock(_streamProperties);
	}
}
}
