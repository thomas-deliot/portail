using System;
using UnityEngine;

namespace Portail.Core
{
	public delegate int PortailAudioSampleReader(float[] samples);

	public struct PortailFeedInfo
	{
		public bool hasSignal;
		public bool isLocalSource;
		public string pathLabel;
		public float bitrateKbps;
		public int width;
		public int height;
		public float fps;
	}

	[Serializable]
	public struct PortailAudioLatencySettings
	{
		public const int DefaultDirectReadScratchSamples = 16384;
		public const int DefaultMaxDirectReadsPerFrame = 8;
		public const int DefaultMaxQueuedLatencyMs = 80;
		public const int DefaultTargetQueuedLatencyMs = 0;

		public int directReadScratchSamples;
		public int maxDirectReadsPerFrame;
		public int maxQueuedLatencyMs;
		public int targetQueuedLatencyMs;

		public static PortailAudioLatencySettings Default => new PortailAudioLatencySettings
		{
			directReadScratchSamples = DefaultDirectReadScratchSamples,
			maxDirectReadsPerFrame = DefaultMaxDirectReadsPerFrame,
			maxQueuedLatencyMs = DefaultMaxQueuedLatencyMs,
			targetQueuedLatencyMs = DefaultTargetQueuedLatencyMs,
		};

		public void Normalize()
		{
			directReadScratchSamples = Mathf.Clamp(directReadScratchSamples, 256, 32768);
			directReadScratchSamples -= directReadScratchSamples % 2;
			maxDirectReadsPerFrame = Mathf.Clamp(maxDirectReadsPerFrame, 1, 64);
			maxQueuedLatencyMs = Mathf.Clamp(maxQueuedLatencyMs, 1, 500);
			targetQueuedLatencyMs = Mathf.Clamp(targetQueuedLatencyMs, 0, maxQueuedLatencyMs);
		}
	}

	public interface IPortailFeedPlayback
	{
		PortailFeed StreamSource { get; }
		MeshRenderer ScreenDisplaySurface { get; }
	}

	[DefaultExecutionOrder(10050)]
	[DisallowMultipleComponent]
	public sealed class PortailFeed : MonoBehaviour
	{
		Texture _streamTexture;
		PortailFeedInfo _streamDisplayInfo;
		PortailAudioSampleReader _directAudioReader;
		PortailAudioLatencySettings _audioLatencySettings = PortailAudioLatencySettings.Default;
		float[] _directAudioScratch = Array.Empty<float>();
		int _lastMipGenerationFrame = -1;

		public Texture StreamTexture => _streamTexture;
		public PortailFeedInfo CurrentStreamInfo => _streamDisplayInfo;
		public PortailFeedInfo CurrentFeedInfo => _streamDisplayInfo;
		public PortailAudioLatencySettings AudioLatency => _audioLatencySettings;
		public bool HasDirectAudioReader => _directAudioReader != null;

		public event Action<PortailFeed, float[], int> AudioSamplesPushed;

		public void SetDisplayOutput(Texture streamTexture, PortailFeedInfo streamInfo, string ignoredStatsText = null)
		{
			_streamTexture = streamTexture;
			_streamDisplayInfo = streamInfo;
		}

		public void SetDirectAudioReader(PortailAudioSampleReader reader)
		{
			_directAudioReader = reader;
		}

		public void SetAudioLatencySettings(PortailAudioLatencySettings settings)
		{
			settings.Normalize();
			_audioLatencySettings = settings;
		}

		void Update()
		{
			PumpDirectAudio();
		}

		void LateUpdate()
		{
			GenerateStreamTextureMips();
		}

		public void PushAudioSamples(float[] samples, int sampleCount)
		{
			if (samples == null || sampleCount <= 0)
				return;

			AudioSamplesPushed?.Invoke(this, samples, sampleCount);
		}

		void PumpDirectAudio()
		{
			PortailAudioSampleReader reader = _directAudioReader;
			if (reader == null || AudioSamplesPushed == null)
				return;

			PortailAudioLatencySettings settings = _audioLatencySettings;
			settings.Normalize();
			EnsureDirectAudioScratch(settings.directReadScratchSamples);

			for (int i = 0; i < settings.maxDirectReadsPerFrame; ++i)
			{
				int sampleCount = reader(_directAudioScratch);
				if (sampleCount <= 0)
					return;

				PushAudioSamples(_directAudioScratch, sampleCount);
				if (sampleCount < _directAudioScratch.Length)
					return;
			}
		}

		void EnsureDirectAudioScratch(int sampleCount)
		{
			sampleCount = Mathf.Clamp(sampleCount, 2, 32768);
			sampleCount -= sampleCount % 2;
			if (_directAudioScratch.Length == sampleCount)
				return;

			_directAudioScratch = new float[sampleCount];
		}

		public void ClearDisplayOutput()
		{
			_streamTexture = null;
			_streamDisplayInfo = default;
			_directAudioReader = null;
			_directAudioScratch = Array.Empty<float>();
			_lastMipGenerationFrame = -1;
		}

		void GenerateStreamTextureMips()
		{
			if (_lastMipGenerationFrame == Time.frameCount)
				return;

			RenderTexture renderTexture = _streamTexture as RenderTexture;
			if (renderTexture == null || !renderTexture.IsCreated())
				return;

			renderTexture.GenerateMips();
			_lastMipGenerationFrame = Time.frameCount;
		}
	}
}
