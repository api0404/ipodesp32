#pragma once

#include <Arduino.h>
#include "esPod_utils.h"

enum class EspDriveActionId : uint8_t
{
    None,
    ShowStatus,
    StartPairing,
    ReconnectLast,
    DisconnectCurrent,
    ToggleAutoplay,
    RequestClearPairings,
    RequestRestart
};

struct EspDriveMenuRecord
{
    uint32_t recordId;
    EspDriveActionId action;
    const char *label;
};

class EspDriveMenu
{
public:
    static void setAutoplayEnabled(bool enabled);
    static uint32_t recordCount(DB_CATEGORY category);
    static bool recordName(DB_CATEGORY category, uint32_t recordId, char *name, size_t nameSize);
    static EspDriveActionId actionForRecord(DB_CATEGORY category, uint32_t recordId);
};
