#include "EspDriveAudio.h"

namespace
{
constexpr uint32_t kFadeDurationMs = 180UL;
constexpr uint32_t kPcmFramesPerChunk = 64;
constexpr size_t kSamplesPerFrame = 2;
constexpr size_t kBytesPerFrame = sizeof(int16_t) * kSamplesPerFrame;
constexpr int32_t kFullScale = 10000;

uint8_t gainPercent(GainPreset preset)
{
	return preset == GainPreset::Full ? 100 : (preset == GainPreset::Percent85 ? 85 : 70);
}

int16_t scaleSample(int32_t sample, int32_t scale)
{
	// Gain and fade only attenuate, so an int16_t input cannot overflow here.
	return static_cast<int16_t>((sample * scale) / kFullScale);
}

void transformChunk(const int16_t *input, int16_t *output, uint32_t frames,
	ChannelMode channelMode, int32_t scale)
{
	switch (channelMode)
	{
	case ChannelMode::Normal:
		for (uint32_t sample = 0; sample < frames * kSamplesPerFrame; ++sample)
			output[sample] = scaleSample(input[sample], scale);
		break;

	case ChannelMode::Swap:
		for (uint32_t frame = 0; frame < frames; ++frame)
		{
			output[frame * 2] = scaleSample(input[frame * 2 + 1], scale);
			output[frame * 2 + 1] = scaleSample(input[frame * 2], scale);
		}
		break;

	case ChannelMode::Mono:
		for (uint32_t frame = 0; frame < frames; ++frame)
		{
			const int32_t mono = (static_cast<int32_t>(input[frame * 2]) + input[frame * 2 + 1]) / 2;
			const int16_t scaled = scaleSample(mono, scale);
			output[frame * 2] = scaled;
			output[frame * 2 + 1] = scaled;
		}
		break;
	}
}
}

void EspDriveAudio::processPcm(const uint8_t *data, uint32_t length, const EspDriveAudioSettings &settings,
                               uint32_t nowMs, PcmWriter writer)
{
	if (data == nullptr || writer == nullptr || length < kBytesPerFrame) return;

	// Snapshot volatile settings once so the real-time loop does not reload them
	// for every sample and each buffer is processed consistently.
	const bool outputEnabled = settings.outputEnabled;
	const ChannelMode channelMode = settings.channelMode;
	const GainPreset gain = settings.gain;
	const bool startupFade = settings.startupFade;
	const uint32_t fadeStartedAtMs = settings.fadeStartedAtMs;
	if (!outputEnabled) return;

	const uint32_t frames = length / kBytesPerFrame;
	const size_t processedBytes = frames * kBytesPerFrame;
	const uint32_t fadeElapsed = nowMs - fadeStartedAtMs;
	const uint8_t fadePercent = !startupFade || fadeElapsed >= kFadeDurationMs ? 100 :
		static_cast<uint8_t>((fadeElapsed * 100UL) / kFadeDurationMs);
	const int32_t scale = gainPercent(gain) * fadePercent;

	if (channelMode == ChannelMode::Normal && scale == kFullScale)
	{
		writer(data, processedBytes);
		return;
	}

	int16_t output[kPcmFramesPerChunk * 2];
	const int16_t *input = reinterpret_cast<const int16_t *>(data);

	for (uint32_t offset = 0; offset < frames;)
	{
		const uint32_t chunkFrames = min(kPcmFramesPerChunk, frames - offset);
		transformChunk(input + offset * kSamplesPerFrame, output, chunkFrames, channelMode, scale);
		writer(reinterpret_cast<const uint8_t *>(output), chunkFrames * kBytesPerFrame);
		offset += chunkFrames;
	}
}
