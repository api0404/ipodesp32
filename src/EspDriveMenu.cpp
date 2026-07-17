#include "EspDriveMenu.h"

namespace
{
constexpr EspDriveMenuRecord menuRecords[] = {
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
}

uint32_t EspDriveMenu::recordCount(DB_CATEGORY category)
{
    return category == DB_CAT_PLAYLIST ? sizeof(menuRecords) / sizeof(menuRecords[0]) :
        (category == DB_CAT_TRACK ? TOTAL_NUM_TRACKS : 1);
}

bool EspDriveMenu::recordName(DB_CATEGORY category, uint32_t recordId, const EspDriveMenuRuntime &runtime,
                              char *name, size_t nameSize)
{
    if (category != DB_CAT_PLAYLIST || recordId >= sizeof(menuRecords) / sizeof(menuRecords[0]))
        return false;

    switch (recordId)
    {
    case 4: snprintf(name, nameSize, "ESPDrive: Autoplay: %s", runtime.autoplayMode); break;
    case 10: snprintf(name, nameSize, "ESPDrive: Preferred: %s", runtime.preferredPhone); break;
    case 11: snprintf(name, nameSize, "ESPDrive: Channel: %s", runtime.channelMode); break;
    case 12: snprintf(name, nameSize, "ESPDrive: Gain: %s", runtime.gainPreset); break;
    case 13: snprintf(name, nameSize, "ESPDrive: Startup fade: %s", runtime.startupFade); break;
    case 14: snprintf(name, nameSize, "ESPDrive: Output: %s", runtime.outputActivation); break;
    case 15: snprintf(name, nameSize, "ESPDrive: Phone: %s", runtime.connectedPhone); break;
    case 16: snprintf(name, nameSize, "ESPDrive: Bonded: %u", runtime.bondedDeviceCount); break;
    case 17: snprintf(name, nameSize, "ESPDrive: A2DP: %s", runtime.a2dpState); break;
    case 18: snprintf(name, nameSize, "ESPDrive: AVRCP: %s", runtime.avrcState); break;
    case 19: snprintf(name, nameSize, "ESPDrive: Audio: %s", runtime.audioState); break;
    case 20: snprintf(name, nameSize, "ESPDrive: RSSI: %s", runtime.rssi); break;
    case 21: snprintf(name, nameSize, "ESPDrive: Format: %s", runtime.audioFormat); break;
    case 22: snprintf(name, nameSize, "ESPDrive: Car link: %s", runtime.carLink); break;
    case 23: snprintf(name, nameSize, "ESPDrive: Uptime: %s", runtime.uptime); break;
    case 24: snprintf(name, nameSize, "ESPDrive: Heap: %s", runtime.heap); break;
    case 25: snprintf(name, nameSize, "ESPDrive: Reset: %s", runtime.resetReason); break;
    case 26: snprintf(name, nameSize, "ESPDrive: Firmware: %s", runtime.build); break;
    default: snprintf(name, nameSize, "%s", menuRecords[recordId].label); break;
    }
    return true;
}

EspDriveActionId EspDriveMenu::actionForRecord(DB_CATEGORY category, uint32_t recordId)
{
    if (category != DB_CAT_PLAYLIST || recordId >= sizeof(menuRecords) / sizeof(menuRecords[0]))
        return EspDriveActionId::None;

    return menuRecords[recordId].action;
}
