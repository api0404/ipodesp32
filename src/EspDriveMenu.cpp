#include "EspDriveMenu.h"

namespace
{
constexpr EspDriveMenuRecord rootRecords[] = {
    {0, EspDriveActionId::None, "ESPDrive: Phone"},
    {1, EspDriveActionId::None, "ESPDrive: Audio"},
    {2, EspDriveActionId::None, "ESPDrive: Playback"},
    {3, EspDriveActionId::None, "ESPDrive: Diagnostics"},
    {4, EspDriveActionId::None, "ESPDrive: System"},
};

constexpr EspDriveMenuRecord phoneRecords[] = {
    {0, EspDriveActionId::ShowStatus, "ESPDrive: Status"},
    {1, EspDriveActionId::StartPairing, "ESPDrive: Pair new phone"},
    {2, EspDriveActionId::ReconnectLast, "ESPDrive: Reconnect last"},
    {3, EspDriveActionId::DisconnectCurrent, "ESPDrive: Disconnect"},
    {4, EspDriveActionId::RequestForgetCurrent, "ESPDrive: Forget current phone"},
    {5, EspDriveActionId::SetPreferredPhone, "ESPDrive: Prefer current phone"},
    {6, EspDriveActionId::ShowPreferredPhone, "ESPDrive: Preferred phone"},
};

constexpr EspDriveMenuRecord audioRecords[] = {
    {0, EspDriveActionId::CycleChannelMode, "ESPDrive: Channel mode"},
    {1, EspDriveActionId::CycleGainPreset, "ESPDrive: Output gain"},
    {2, EspDriveActionId::ToggleStartupFade, "ESPDrive: Startup fade"},
    {3, EspDriveActionId::ToggleOutputActivation, "ESPDrive: Output activation"},
};

constexpr EspDriveMenuRecord playbackRecords[] = {
    {0, EspDriveActionId::ToggleAutoplay, "ESPDrive: Autoplay"},
};

constexpr EspDriveMenuRecord diagnosticRecords[] = {
    {0, EspDriveActionId::ShowDiagnostic, "ESPDrive: Phone"},
    {1, EspDriveActionId::ShowDiagnostic, "ESPDrive: Bonded devices"},
    {2, EspDriveActionId::ShowDiagnostic, "ESPDrive: A2DP"},
    {3, EspDriveActionId::ShowDiagnostic, "ESPDrive: AVRCP"},
    {4, EspDriveActionId::ShowDiagnostic, "ESPDrive: Audio"},
    {5, EspDriveActionId::ShowDiagnostic, "ESPDrive: RSSI"},
    {6, EspDriveActionId::ShowDiagnostic, "ESPDrive: Format"},
    {7, EspDriveActionId::ShowDiagnostic, "ESPDrive: Car link"},
    {8, EspDriveActionId::ShowDiagnostic, "ESPDrive: Uptime"},
    {9, EspDriveActionId::ShowDiagnostic, "ESPDrive: Heap"},
    {10, EspDriveActionId::ShowDiagnostic, "ESPDrive: Reset reason"},
    {11, EspDriveActionId::ShowDiagnostic, "ESPDrive: Firmware"},
};

constexpr EspDriveMenuRecord systemRecords[] = {
    {0, EspDriveActionId::ReconnectCar, "ESPDrive: Reconnect car"},
    {1, EspDriveActionId::RequestClearPairings, "ESPDrive: Clear pairings"},
    {2, EspDriveActionId::RequestRestart, "ESPDrive: Restart"},
};

constexpr EspDriveMenuRecord flatRecords[] = {
    {0, EspDriveActionId::ShowStatus, "ESPDrive: Status"},
    {1, EspDriveActionId::StartPairing, "ESPDrive: Pair new phone"},
    {2, EspDriveActionId::ReconnectLast, "ESPDrive: Reconnect last"},
    {3, EspDriveActionId::DisconnectCurrent, "ESPDrive: Disconnect"},
    {4, EspDriveActionId::ToggleAutoplay, "ESPDrive: Autoplay"},
    {5, EspDriveActionId::RequestClearPairings, "ESPDrive: Clear pairings"},
    {6, EspDriveActionId::RequestRestart, "ESPDrive: Restart"},
    {7, EspDriveActionId::ReconnectCar, "ESPDrive: Reconnect car"},
    {8, EspDriveActionId::RequestForgetCurrent, "ESPDrive: Forget current phone"},
    {9, EspDriveActionId::SetPreferredPhone, "ESPDrive: Prefer current phone"},
    {10, EspDriveActionId::ShowPreferredPhone, "ESPDrive: Preferred phone"},
    {11, EspDriveActionId::CycleChannelMode, "ESPDrive: Channel mode"},
    {12, EspDriveActionId::CycleGainPreset, "ESPDrive: Output gain"},
    {13, EspDriveActionId::ToggleStartupFade, "ESPDrive: Startup fade"},
    {14, EspDriveActionId::ToggleOutputActivation, "ESPDrive: Output activation"},
    {15, EspDriveActionId::ShowDiagnostic, "ESPDrive: Phone"},
    {16, EspDriveActionId::ShowDiagnostic, "ESPDrive: Bonded devices"},
    {17, EspDriveActionId::ShowDiagnostic, "ESPDrive: A2DP"},
    {18, EspDriveActionId::ShowDiagnostic, "ESPDrive: AVRCP"},
    {19, EspDriveActionId::ShowDiagnostic, "ESPDrive: Audio"},
    {20, EspDriveActionId::ShowDiagnostic, "ESPDrive: RSSI"},
    {21, EspDriveActionId::ShowDiagnostic, "ESPDrive: Format"},
    {22, EspDriveActionId::ShowDiagnostic, "ESPDrive: Car link"},
    {23, EspDriveActionId::ShowDiagnostic, "ESPDrive: Uptime"},
    {24, EspDriveActionId::ShowDiagnostic, "ESPDrive: Heap"},
    {25, EspDriveActionId::ShowDiagnostic, "ESPDrive: Reset reason"},
    {26, EspDriveActionId::ShowDiagnostic, "ESPDrive: Firmware"},
};

template <size_t N>
const EspDriveMenuRecord *recordAt(const EspDriveMenuRecord (&records)[N], uint32_t recordId)
{
    return recordId < N ? &records[recordId] : nullptr;
}

const EspDriveMenuRecord *recordsFor(EspDriveMenuCategory category, uint32_t recordId)
{
    switch (category)
    {
    case EspDriveMenuCategory::Phone: return recordAt(phoneRecords, recordId);
    case EspDriveMenuCategory::Audio: return recordAt(audioRecords, recordId);
    case EspDriveMenuCategory::Playback: return recordAt(playbackRecords, recordId);
    case EspDriveMenuCategory::Diagnostics: return recordAt(diagnosticRecords, recordId);
    case EspDriveMenuCategory::System: return recordAt(systemRecords, recordId);
    default: return nullptr;
    }
}

uint32_t countFor(EspDriveMenuCategory category)
{
    switch (category)
    {
    case EspDriveMenuCategory::Phone: return sizeof(phoneRecords) / sizeof(phoneRecords[0]);
    case EspDriveMenuCategory::Audio: return sizeof(audioRecords) / sizeof(audioRecords[0]);
    case EspDriveMenuCategory::Playback: return sizeof(playbackRecords) / sizeof(playbackRecords[0]);
    case EspDriveMenuCategory::Diagnostics: return sizeof(diagnosticRecords) / sizeof(diagnosticRecords[0]);
    case EspDriveMenuCategory::System: return sizeof(systemRecords) / sizeof(systemRecords[0]);
    default: return 0;
    }
}

void formatDynamicLabel(EspDriveMenuCategory category, uint32_t recordId, const EspDriveMenuRuntime &runtime,
                        const char *fallback, char *name, size_t nameSize)
{
    if (category == EspDriveMenuCategory::Phone && recordId == 6) snprintf(name, nameSize, "ESPDrive: Preferred: %s", runtime.preferredPhone);
    else if (category == EspDriveMenuCategory::Audio && recordId == 0) snprintf(name, nameSize, "ESPDrive: Channel: %s", runtime.channelMode);
    else if (category == EspDriveMenuCategory::Audio && recordId == 1) snprintf(name, nameSize, "ESPDrive: Gain: %s", runtime.gainPreset);
    else if (category == EspDriveMenuCategory::Audio && recordId == 2) snprintf(name, nameSize, "ESPDrive: Startup fade: %s", runtime.startupFade);
    else if (category == EspDriveMenuCategory::Audio && recordId == 3) snprintf(name, nameSize, "ESPDrive: Output: %s", runtime.outputActivation);
    else if (category == EspDriveMenuCategory::Playback && recordId == 0) snprintf(name, nameSize, "ESPDrive: Autoplay: %s", runtime.autoplayMode);
    else if (category == EspDriveMenuCategory::Diagnostics)
    {
        static const char *const values[] = {runtime.connectedPhone, nullptr, runtime.a2dpState, runtime.avrcState, runtime.audioState,
                                             runtime.rssi, runtime.audioFormat, runtime.carLink, runtime.uptime, runtime.heap,
                                             runtime.resetReason, runtime.build};
        if (recordId == 1) snprintf(name, nameSize, "ESPDrive: Bonded: %u", runtime.bondedDeviceCount);
        else if (recordId < sizeof(values) / sizeof(values[0])) snprintf(name, nameSize, "%s: %s", fallback, values[recordId]);
        else snprintf(name, nameSize, "%s", fallback);
    }
    else snprintf(name, nameSize, "%s", fallback);
}
}

uint32_t EspDriveMenu::recordCount(DB_CATEGORY databaseCategory, EspDriveMenuCategory selectedCategory)
{
#if ESPDRIVE_CATEGORIZED_MENU
    if (databaseCategory == DB_CAT_PLAYLIST) return sizeof(rootRecords) / sizeof(rootRecords[0]);
    if (databaseCategory == DB_CAT_TRACK) return countFor(selectedCategory);
    return 0;
#else
    return databaseCategory == DB_CAT_PLAYLIST ? sizeof(flatRecords) / sizeof(flatRecords[0]) :
        (databaseCategory == DB_CAT_TRACK ? TOTAL_NUM_TRACKS : 0);
#endif
}

bool EspDriveMenu::recordName(DB_CATEGORY databaseCategory, EspDriveMenuCategory selectedCategory, uint32_t recordId,
                              const EspDriveMenuRuntime &runtime, char *name, size_t nameSize)
{
#if ESPDRIVE_CATEGORIZED_MENU
    if (databaseCategory == DB_CAT_PLAYLIST)
    {
        const EspDriveMenuRecord *record = recordAt(rootRecords, recordId);
        if (record == nullptr) return false;
        snprintf(name, nameSize, "%s", record->label);
        return true;
    }
    if (databaseCategory != DB_CAT_TRACK) return false;
    const EspDriveMenuRecord *record = recordsFor(selectedCategory, recordId);
    if (record == nullptr) return false;
    formatDynamicLabel(selectedCategory, recordId, runtime, record->label, name, nameSize);
    return true;
#else
    if (databaseCategory != DB_CAT_PLAYLIST) return false;
    const EspDriveMenuRecord *record = recordAt(flatRecords, recordId);
    if (record == nullptr) return false;
    switch (recordId)
    {
    case 4: snprintf(name, nameSize, "ESPDrive: Autoplay: %s", runtime.autoplayMode); break;
    case 10: snprintf(name, nameSize, "ESPDrive: Preferred: %s", runtime.preferredPhone); break;
    case 11: snprintf(name, nameSize, "ESPDrive: Channel: %s", runtime.channelMode); break;
    case 12: snprintf(name, nameSize, "ESPDrive: Gain: %s", runtime.gainPreset); break;
    case 13: snprintf(name, nameSize, "ESPDrive: Startup fade: %s", runtime.startupFade); break;
    case 14: snprintf(name, nameSize, "ESPDrive: Output: %s", runtime.outputActivation); break;
    default:
        formatDynamicLabel(EspDriveMenuCategory::Diagnostics, recordId >= 15 ? recordId - 15 : 99,
                           runtime, record->label, name, nameSize);
        break;
    }
    return true;
#endif
}

EspDriveMenuCategory EspDriveMenu::categoryForPlaylist(uint32_t recordId)
{
#if ESPDRIVE_CATEGORIZED_MENU
    return recordId < sizeof(rootRecords) / sizeof(rootRecords[0]) ? static_cast<EspDriveMenuCategory>(recordId + 1) : EspDriveMenuCategory::None;
#else
    (void)recordId;
    return EspDriveMenuCategory::None;
#endif
}

EspDriveMenuSelection EspDriveMenu::selectionForRecord(DB_CATEGORY databaseCategory,
                                                       EspDriveMenuCategory selectedCategory, uint32_t recordId)
{
    EspDriveMenuSelection selection;
    selection.category = selectedCategory;
    selection.databaseCategory = databaseCategory;
    selection.recordId = recordId;
#if ESPDRIVE_CATEGORIZED_MENU
    if (databaseCategory == DB_CAT_TRACK)
    {
        const EspDriveMenuRecord *record = recordsFor(selectedCategory, recordId);
        if (record != nullptr) selection.action = record->action;
    }
#else
    if (databaseCategory == DB_CAT_PLAYLIST)
    {
        const EspDriveMenuRecord *record = recordAt(flatRecords, recordId);
        if (record != nullptr) selection.action = record->action;
    }
#endif
    return selection;
}
