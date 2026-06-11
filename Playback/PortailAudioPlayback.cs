using System;
using Portail.Core;
using UnityEngine;
using UnityEngine.Scripting.APIUpdating;

namespace Portail.Playback
{
	[DisallowMultipleComponent]
	[RequireComponent(typeof(AudioSource))]
	[MovedFrom(true, "", null, "DesktopStreamAudioSource")]
	public sealed class PortailAudioPlayback : MonoBehaviour
	{
		public enum OutputChannel
		{
			Stereo = 0,
			Left = 1,
			Right = 2,
		}

	const int SampleRate = 48000;
	const int SourceChannels = 2;
	const int BufferSamples = SampleRate * SourceChannels * 2;
	const int DefaultMaxQueuedLatencyMs = PortailAudioLatencySettings.DefaultMaxQueuedLatencyMs;
	const int DefaultTargetQueuedLatencyMs = PortailAudioLatencySettings.DefaultTargetQueuedLatencyMs;
	const int DirectStartLatencyMs = 25;
	const int DirectTargetLatencyMs = 45;
	const int DirectMaxReadLatencyMs = 25;
	const int DirectStartSamples = (SampleRate * SourceChannels * DirectStartLatencyMs) / 1000;
	const int DirectTargetSamples = (SampleRate * SourceChannels * DirectTargetLatencyMs) / 1000;
	const int DirectMaxReadSamples = (SampleRate * SourceChannels * DirectMaxReadLatencyMs) / 1000;

	[SerializeField] OutputChannel outputChannel = OutputChannel.Stereo;

	readonly object _bufferLock = new object();
	readonly float[] _buffer = new float[BufferSamples];
	AudioSource _audioSource;
	AudioClip _driverClip;
	int _readIndex;
	int _writeIndex;
	int _sampleCount;
	float _cachedVolume = 1f;
	bool _cachedMute;
	int _maxQueuedLatencyMs = DefaultMaxQueuedLatencyMs;
	int _targetQueuedLatencyMs = DefaultTargetQueuedLatencyMs;
	PortailAudioSampleReader _directReader;
	bool _directPrebuffering = true;
	float[] _directReadScratch = Array.Empty<float>();
	float[] _outputScratch = Array.Empty<float>();

	public OutputChannel Channel
	{
		get => outputChannel;
		set => outputChannel = value;
	}

	public void SetOutputChannel(OutputChannel channel)
	{
		if (outputChannel == channel)
			return;

		outputChannel = channel;
		ClearQueuedSamples();
	}

	public void SetLatencySettings(int maxQueuedLatencyMs, int targetQueuedLatencyMs)
	{
		maxQueuedLatencyMs = Mathf.Clamp(maxQueuedLatencyMs, 1, 500);
		targetQueuedLatencyMs = Mathf.Clamp(targetQueuedLatencyMs, 0, maxQueuedLatencyMs);

		if (_maxQueuedLatencyMs == maxQueuedLatencyMs && _targetQueuedLatencyMs == targetQueuedLatencyMs)
			return;

		_maxQueuedLatencyMs = maxQueuedLatencyMs;
		_targetQueuedLatencyMs = targetQueuedLatencyMs;

		lock (_bufferLock)
		{
			TrimBufferedSamples(GetActiveQueuedSampleLimit());
		}
	}

	public void SetDirectSampleReader(PortailAudioSampleReader reader)
	{
		if (_directReader == reader)
			return;

		_directReader = reader;
		_directPrebuffering = true;
		ClearBuffer();
	}

	public void PushSamples(float[] samples, int sampleCount)
	{
		if (samples == null || sampleCount <= 0)
			return;

		sampleCount = Mathf.Min(sampleCount, samples.Length);
		sampleCount -= sampleCount % SourceChannels;
		if (sampleCount <= 0)
			return;

		Enqueue(samples, sampleCount);
	}

	public void ClearQueuedSamples()
	{
		_directPrebuffering = true;
		ClearBuffer();
	}

	void Awake()
	{
		EnsureAudioSource();
		CacheAudioSourceControls();
	}

	void OnEnable()
	{
		EnsureAudioSource();
		CacheAudioSourceControls();
	}

	void OnDisable()
	{
		_directReader = null;
		_directPrebuffering = true;
		ClearBuffer();
	}

	void Update()
	{
		EnsureAudioSource();
		CacheAudioSourceControls();
	}

	void OnDestroy()
	{
		if (_driverClip != null)
		{
			Destroy(_driverClip);
			_driverClip = null;
		}
	}

	void OnAudioFilterRead(float[] data, int channels)
	{
		MixWithAudioSourceDriver(data, channels);
	}

	void ReadOutputData(float[] data, int outputChannels)
	{
		if (data == null || data.Length == 0)
		{
			FillSilence(data);
			return;
		}

		if (_directReader != null)
		{
			ReadDirectBuffered(data, outputChannels);
			return;
		}

		outputChannels = Mathf.Max(1, outputChannels);
		lock (_bufferLock)
		{
			for (int i = 0; i < data.Length; i += outputChannels)
			{
				float left = 0f;
				float right = 0f;
				if (_sampleCount >= SourceChannels)
				{
					left = _buffer[_readIndex];
					_readIndex = (_readIndex + 1) % _buffer.Length;
					right = _buffer[_readIndex];
					_readIndex = (_readIndex + 1) % _buffer.Length;
					_sampleCount -= SourceChannels;
				}

				WriteOutputFrame(data, i, outputChannels, left, right);
			}
		}
	}

	void ApplyAudioSourceControls(float[] data)
	{
		if (data == null || data.Length == 0)
			return;

		if (_cachedMute)
		{
			FillSilence(data);
			return;
		}

		float volume = _cachedVolume;
		if (volume <= 0f)
		{
			FillSilence(data);
			return;
		}

		if (volume >= 0.999f)
			return;

		for (int i = 0; i < data.Length; ++i)
			data[i] *= volume;
	}

	void MixWithAudioSourceDriver(float[] data, int channels)
	{
		if (data == null || data.Length == 0)
			return;

		EnsureOutputScratch(data.Length);
		ReadOutputData(_outputScratch, Mathf.Max(1, channels));

		for (int i = 0; i < data.Length; ++i)
			data[i] *= _outputScratch[i];

		ApplyAudioSourceControls(data);
	}

	void ReadDirectBuffered(float[] data, int outputChannels)
	{
		if (data == null || data.Length == 0)
			return;

		outputChannels = Mathf.Max(1, outputChannels);
		int sourceSamplesRequired = (data.Length / outputChannels) * SourceChannels;
		PumpDirectReader(sourceSamplesRequired);

		lock (_bufferLock)
		{
			if (_directPrebuffering)
			{
				int startSamples = Mathf.Clamp(DirectStartSamples - (DirectStartSamples % SourceChannels), SourceChannels, _buffer.Length);
				if (_sampleCount < startSamples)
				{
					FillSilence(data);
					return;
				}
				_directPrebuffering = false;
			}

			for (int i = 0; i < data.Length; i += outputChannels)
			{
				float left = 0f;
				float right = 0f;
				if (_sampleCount >= SourceChannels)
				{
					left = _buffer[_readIndex];
					_readIndex = (_readIndex + 1) % _buffer.Length;
					right = _buffer[_readIndex];
					_readIndex = (_readIndex + 1) % _buffer.Length;
					_sampleCount -= SourceChannels;
				}
				else
				{
					_directPrebuffering = true;
				}

				WriteOutputFrame(data, i, outputChannels, left, right);
			}
		}
	}

	void PumpDirectReader(int outputSampleCount)
	{
		PortailAudioSampleReader reader = _directReader;
		if (reader == null)
			return;

		int targetSamples = Mathf.Max(DirectTargetSamples, outputSampleCount + DirectStartSamples);
		targetSamples = Mathf.Clamp(targetSamples - (targetSamples % SourceChannels), SourceChannels, GetMaxQueuedSamples());

		int bufferedSamples;
		lock (_bufferLock)
			bufferedSamples = _sampleCount;

		int samplesToRead = targetSamples - bufferedSamples;
		while (samplesToRead >= SourceChannels)
		{
			int requestSamples = Mathf.Min(samplesToRead, Mathf.Max(DirectMaxReadSamples, outputSampleCount));
			requestSamples -= requestSamples % SourceChannels;
			if (requestSamples <= 0)
				return;

			EnsureDirectScratch(requestSamples);
			int read = Mathf.Clamp(reader(_directReadScratch), 0, requestSamples);
			read -= read % SourceChannels;
			if (read <= 0)
				return;

			Enqueue(_directReadScratch, read);
			samplesToRead -= read;
		}
	}

	void EnsureDirectScratch(int sampleCount)
	{
		if (_directReadScratch.Length == sampleCount)
			return;

		_directReadScratch = new float[sampleCount];
	}

	void WriteOutputFrame(float[] data, int offset, int outputChannels, float left, float right)
	{
		float value = outputChannel == OutputChannel.Left
			? left
			: outputChannel == OutputChannel.Right
				? right
				: 0f;

		if (outputChannel != OutputChannel.Stereo)
		{
			for (int channel = 0; channel < outputChannels && offset + channel < data.Length; ++channel)
				data[offset + channel] = value;
			return;
		}

		if (outputChannels == 1)
		{
			data[offset] = (left + right) * 0.5f;
			return;
		}

		data[offset] = left;
		if (offset + 1 < data.Length)
			data[offset + 1] = right;

		for (int channel = 2; channel < outputChannels && offset + channel < data.Length; ++channel)
			data[offset + channel] = (left + right) * 0.5f;
	}

	void EnsureOutputScratch(int sampleCount)
	{
		if (_outputScratch.Length == sampleCount)
			return;

		_outputScratch = new float[sampleCount];
	}

	void Enqueue(float[] samples, int sampleCount)
	{
		lock (_bufferLock)
		{
			int offset = 0;
			if (sampleCount >= _buffer.Length)
			{
				offset = sampleCount - _buffer.Length;
				sampleCount = _buffer.Length;
				_readIndex = 0;
				_writeIndex = 0;
				_sampleCount = 0;
			}

			int free = _buffer.Length - _sampleCount;
			if (sampleCount > free)
			{
				int drop = sampleCount - free;
				_readIndex = (_readIndex + drop) % _buffer.Length;
				_sampleCount -= drop;
			}

			int maxQueuedSamples = GetMaxQueuedSamples();
			int queuedAfterWrite = _sampleCount + sampleCount;
			if (queuedAfterWrite > maxQueuedSamples)
			{
				TrimBufferedSamples(maxQueuedSamples - sampleCount);
			}

			if (outputChannel == OutputChannel.Stereo)
			{
				for (int i = 0; i < sampleCount; ++i)
				{
					_buffer[_writeIndex] = samples[offset + i];
					_writeIndex = (_writeIndex + 1) % _buffer.Length;
				}
			}
			else
			{
				int channelOffset = outputChannel == OutputChannel.Left ? 0 : 1;
				for (int i = 0; i < sampleCount; i += SourceChannels)
				{
					float value = samples[offset + i + channelOffset];
					_buffer[_writeIndex] = value;
					_writeIndex = (_writeIndex + 1) % _buffer.Length;
					_buffer[_writeIndex] = value;
					_writeIndex = (_writeIndex + 1) % _buffer.Length;
				}
			}
			_sampleCount += sampleCount;
			TrimBufferedSamples(GetActiveQueuedSampleLimit());
		}
	}

	int GetActiveQueuedSampleLimit()
	{
		return _targetQueuedLatencyMs > 0 ? GetTargetQueuedSamples() : GetMaxQueuedSamples();
	}

	int GetMaxQueuedSamples()
	{
		return LatencyMsToSamples(_maxQueuedLatencyMs);
	}

	int GetTargetQueuedSamples()
	{
		return LatencyMsToSamples(Mathf.Min(_targetQueuedLatencyMs, _maxQueuedLatencyMs));
	}

	int LatencyMsToSamples(int latencyMs)
	{
		int samples = (SampleRate * SourceChannels * Mathf.Max(1, latencyMs)) / 1000;
		samples -= samples % SourceChannels;
		return Mathf.Clamp(samples, SourceChannels, _buffer.Length);
	}

	void TrimBufferedSamples(int targetSampleCount)
	{
		targetSampleCount = Mathf.Clamp(targetSampleCount, 0, _buffer.Length);
		targetSampleCount -= targetSampleCount % SourceChannels;
		if (_sampleCount <= targetSampleCount)
			return;

		int drop = _sampleCount - targetSampleCount;
		drop -= drop % SourceChannels;
		if (drop <= 0)
			return;

		_readIndex = (_readIndex + drop) % _buffer.Length;
		_sampleCount -= drop;
	}

	void ClearBuffer()
	{
		lock (_bufferLock)
		{
			_readIndex = 0;
			_writeIndex = 0;
			_sampleCount = 0;
		}
	}

	void EnsureAudioSource()
	{
		if (_audioSource == null)
			_audioSource = GetComponent<AudioSource>();

		if (_audioSource == null)
			return;

		_audioSource.playOnAwake = false;
		_audioSource.loop = true;
		_audioSource.spatialBlend = 1f;
		_audioSource.dopplerLevel = 0f;
		_audioSource.rolloffMode = AudioRolloffMode.Logarithmic;

		if (_driverClip == null)
			_driverClip = AudioClip.Create("PortailAudioPlaybackDriver", SampleRate, SourceChannels, SampleRate, false, FillDriverSignal);

		if (_audioSource.clip != _driverClip)
			_audioSource.clip = _driverClip;

		if (_audioSource.enabled && gameObject.activeInHierarchy && !_audioSource.isPlaying)
			_audioSource.Play();
	}

	void CacheAudioSourceControls()
	{
		if (_audioSource == null)
			_audioSource = GetComponent<AudioSource>();

		if (_audioSource == null)
		{
			_cachedVolume = 0f;
			_cachedMute = true;
			return;
		}

		_cachedVolume = Mathf.Clamp01(_audioSource.volume);
		_cachedMute = _audioSource.mute;
	}

	static void FillSilence(float[] data)
	{
		if (data == null)
			return;

		for (int i = 0; i < data.Length; ++i)
			data[i] = 0f;
	}

	static void FillDriverSignal(float[] data)
	{
		if (data == null)
			return;

		for (int i = 0; i < data.Length; ++i)
			data[i] = 1f;
	}
}
}
