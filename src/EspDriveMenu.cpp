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
};
bool autoplayEnabled = true;
}

uint32_t EspDriveMenu::recordCount(DB_CATEGORY category)
{
    return category == DB_CAT_PLAYLIST ? sizeof(menuRecords) / sizeof(menuRecords[0]) :
        (category == DB_CAT_TRACK ? TOTAL_NUM_TRACKS : 1);
}

bool EspDriveMenu::recordName(DB_CATEGORY category, uint32_t recordId, char *name, size_t nameSize)
{
    if (category != DB_CAT_PLAYLIST || recordId >= sizeof(menuRecords) / sizeof(menuRecords[0]))
        return false;

    if (menuRecords[recordId].action == EspDriveActionId::ToggleAutoplay)
        snprintf(name, nameSize, "ESPDrive: Autoplay: %s", autoplayEnabled ? "On" : "Off");
    else
        snprintf(name, nameSize, "%s", menuRecords[recordId].label);
    return true;
}

void EspDriveMenu::setAutoplayEnabled(bool enabled)
{
    autoplayEnabled = enabled;
}

EspDriveActionId EspDriveMenu::actionForRecord(DB_CATEGORY category, uint32_t recordId)
{
    if (category != DB_CAT_PLAYLIST || recordId >= sizeof(menuRecords) / sizeof(menuRecords[0]))
        return EspDriveActionId::None;

    return menuRecords[recordId].action;
}
