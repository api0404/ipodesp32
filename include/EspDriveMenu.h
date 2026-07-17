#pragma once

#include <Arduino.h>
#include "esPod_utils.h"

#ifndef ESPDRIVE_CATEGORIZED_MENU
#define ESPDRIVE_CATEGORIZED_MENU 1
#endif

enum class EspDriveMenuCategory : uint8_t
{
    None,
    Phone,
    Audio,
    Playback,
    Diagnostics,
    System
};

enum class EspDriveActionId : uint8_t
{
    None,
    ShowStatus,
    StartPairing,
    ReconnectLast,
    DisconnectCurrent,
    ToggleAutoplay,
    RequestClearPairings,
    RequestRestart,
    ReconnectCar,
    RequestForgetCurrent,
    SetPreferredPhone,
    ShowPreferredPhone,
    CycleChannelMode,
    CycleGainPreset,
    ToggleStartupFade,
    ToggleOutputActivation,
    ShowDiagnostic
};

struct EspDriveMenuRuntime
{
    const char *autoplayMode = "Off";
    const char *preferredPhone = "None";
    const char *channelMode = "Normal";
    const char *gainPreset = "100%";
    const char *startupFade = "On";
    const char *outputActivation = "Immediate";
    const char *connectedPhone = "No phone";
    const char *a2dpState = "Unavailable";
    const char *avrcState = "Unavailable";
    const char *audioState = "Unavailable";
    const char *rssi = "Unavailable";
    const char *audioFormat = "Unavailable";
    const char *carLink = "Unavailable";
    const char *uptime = "0s";
    const char *heap = "Unavailable";
    const char *resetReason = "Unavailable";
    const char *build = "Unavailable";
    uint8_t bondedDeviceCount = 0;
};

struct EspDriveMenuRecord
{
    uint32_t recordId;
    EspDriveActionId action;
    const char *label;
};

struct EspDriveMenuSelection
{
    EspDriveMenuCategory category = EspDriveMenuCategory::None;
    DB_CATEGORY databaseCategory = DB_CAT_PLAYLIST;
    uint32_t recordId = 0;
    EspDriveActionId action = EspDriveActionId::None;
};

class EspDriveMenu
{
public:
    static uint32_t recordCount(DB_CATEGORY databaseCategory, EspDriveMenuCategory selectedCategory);
    static bool recordName(DB_CATEGORY databaseCategory, EspDriveMenuCategory selectedCategory, uint32_t recordId,
                           const EspDriveMenuRuntime &runtime,
                           char *name, size_t nameSize);
    static EspDriveMenuCategory categoryForPlaylist(uint32_t recordId);
    static EspDriveMenuSelection selectionForRecord(DB_CATEGORY databaseCategory,
                                                    EspDriveMenuCategory selectedCategory, uint32_t recordId);
};
