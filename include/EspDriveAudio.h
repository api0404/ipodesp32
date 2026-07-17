#pragma once

#include <Arduino.h>

enum class ChannelMode : uint8_t { Normal, Swap, Mono };
enum class GainPreset : uint8_t { Full, Percent85, Percent70 };
enum class OutputActivation : uint8_t { Immediate, AfterCarLink };

struct EspDriveAudioSettings
{
	volatile ChannelMode channelMode = ChannelMode::Normal;
	volatile GainPreset gain = GainPreset::Full;
	volatile bool startupFade = true;
	volatile OutputActivation activation = OutputActivation::Immediate;
	volatile bool outputEnabled = true;
	volatile uint32_t fadeStartedAtMs = 0;
};

namespace EspDriveAudio
{
using PcmWriter = size_t (*)(const uint8_t *data, size_t length);

void processPcm(const uint8_t *data, uint32_t length, const EspDriveAudioSettings &settings,
                uint32_t nowMs, PcmWriter writer);
}
