#include <Arduino.h>
#include <Preferences.h>
#include "esp_gap_bt_api.h"
#include "esp_avrc_api.h"
#include "esp_system.h"
#include "esp_heap_caps.h"
#include "AudioTools.h"
#include "BluetoothA2DPSink.h"
#include "esPod.h"
#include "EspDriveAudio.h"
#include "EspDriveMenu.h"

#ifndef INVERT_LED_LOGIC
#define INVERT_LED_LOGIC(stateBoolean) stateBoolean
#else
#undef INVERT_LED_LOGIC
#define INVERT_LED_LOGIC(stateBoolean) !stateBoolean
#endif
#ifndef INVERT_DCD_LOGIC
#define INVERT_DCD_LOGIC(stateBoolean) stateBoolean
#else
#undef INVERT_DCD_LOGIC
#define INVERT_DCD_LOGIC(stateBoolean) !stateBoolean
#endif
#if defined(ENABLE_ACTIVE_DCD) && !defined(DISABLE_ACTIVE_DCD)
#define ESPDRIVE_ACTIVE_DCD 1
#else
#define ESPDRIVE_ACTIVE_DCD 0
#endif
#if ESPDRIVE_ACTIVE_DCD && !defined(DCD_CTRL_PIN)
#define DCD_CTRL_PIN 5
#endif

constexpr size_t kMetadataLength = 256;
constexpr uint32_t kOverlayDefaultMs = 4000UL;
constexpr uint32_t kWelcomeOverlayMs = 15000UL;
constexpr uint32_t kPairingTimeoutMs = 90000UL;
constexpr uint32_t kManualDisconnectKeepCarMs = 30000UL;
constexpr uint32_t kConfirmTimeoutMs = 10000UL;
constexpr uint32_t kCarRecoveryDcdMs = 750UL;
constexpr uint32_t kCarRecoveryResultMs = 2500UL;
constexpr uint32_t kReliableAutoplayInitialMs = 500UL;
constexpr uint32_t kReliableAutoplayRetryMs = 1200UL;
constexpr uint8_t kReliableAutoplayMaxRetries = 3;
constexpr uint8_t kBoundedReconnectAttempts = 3;
constexpr uint32_t kPreferredReconnectWindowMs = 12000UL;
constexpr uint32_t kOutputCarLinkTimeoutMs = 10000UL;
constexpr uint32_t kStateTaskWakeMs = 100UL;
constexpr UBaseType_t kActionQueueLength = 12;
constexpr uint32_t kWorkerStackSize = 4096;

#ifndef A2DP_SINK_NAME
#define A2DP_SINK_NAME "espiPod"
#endif
#ifndef WS_PIN
#define WS_PIN 25
#endif
#ifndef DIN_PIN
#define DIN_PIN 26
#endif
#ifndef BCLK_PIN
#define BCLK_PIN 27
#endif

#ifdef AUDIOKIT
#include "AudioBoard.h"
#include "AudioTools/AudioLibs/I2SCodecStream.h"
AudioInfo info(44100, 2, 16);
DriverPins minimalPins;
AudioBoard minimalAudioKit(AudioDriverES8388, minimalPins);
I2SCodecStream i2s(minimalAudioKit);
BluetoothA2DPSink a2dp_sink(i2s);
#else
I2SStream i2s;
BluetoothA2DPSink a2dp_sink;
#endif

enum class AutoplayMode : uint8_t { Off, HeadUnit, Immediate, Reliable };
enum class CarRecoveryState : uint8_t { Idle, DcdReleased, WaitingHandshake };

struct AutoplayState
{
	AutoplayMode mode = AutoplayMode::HeadUnit;
	uint8_t retriesSent = 0;
	uint32_t nextAttemptAtMs = 0;
	bool waitingForAudio = false;
};

struct CarLinkRecovery
{
	CarRecoveryState state = CarRecoveryState::Idle;
	uint32_t deadlineMs = 0;
};

struct Overlay
{
	uint32_t expiresAtMs = 0;
	bool active = false;
};

struct RealMetadata
{
	char title[kMetadataLength] = "Unknown title";
	char artist[kMetadataLength] = "Unknown artist";
	char album[kMetadataLength] = "Unknown album";
	char genre[kMetadataLength] = {};
	char duration[kMetadataLength] = {};
	char trackNumber[kMetadataLength] = {};
	char totalTracks[kMetadataLength] = {};
	uint8_t pendingAttributes = 0;
};

enum MetadataPending : uint8_t
{
	TitlePending = 1U << 0,
	ArtistPending = 1U << 1,
	AlbumPending = 1U << 2,
	GenrePending = 1U << 3,
	DurationPending = 1U << 4,
	TrackNumberPending = 1U << 5,
	TotalTracksPending = 1U << 6,
};

struct BluetoothEventMailbox
{
	bool connected = false;
	bool connectionChanged = false;
	esp_a2d_audio_state_t audioState = ESP_A2D_AUDIO_STATE_REMOTE_SUSPEND;
	bool audioStateChanged = false;
	uint32_t playPositionMs = 0;
	bool playPositionChanged = false;
};

struct PhoneIdentity
{
	uint8_t address[6] = {};
	bool valid = false;
};

esPod espod(1, UART1_RX, UART1_TX, 19200);
Preferences espDrivePreferences;
EspDriveAudioSettings audioSettings;
AutoplayState autoplay;
CarLinkRecovery carRecovery;
Overlay overlay;
RealMetadata metadata;
BluetoothEventMailbox bluetoothEvents;
PhoneIdentity currentPhone;
PhoneIdentity preferredPhone;
portMUX_TYPE stateMux = portMUX_INITIALIZER_UNLOCKED;
QueueHandle_t espDriveActionQueue;
TaskHandle_t espDriveActionTaskHandle;
TaskHandle_t processAVRCTaskHandle;
volatile esp_a2d_audio_state_t latestAudioState = ESP_A2D_AUDIO_STATE_REMOTE_SUSPEND;
volatile bool avrcConnected = false;
volatile int latestRssi = 0;
volatile bool latestRssiAvailable = false;
volatile uint16_t negotiatedSampleRate = 0;
volatile uint8_t negotiatedChannels = 2;
uint32_t pairingExpiresAtMs = 0;
uint32_t reconnectSuppressedUntilMs = 0;
uint32_t keepCarLinkUntilMs = 0;
uint32_t clearPairingsConfirmUntilMs = 0;
uint32_t forgetPhoneConfirmUntilMs = 0;
uint32_t restartConfirmUntilMs = 0;
uint32_t outputCarLinkDeadlineMs = 0;
uint32_t preferredOnlyUntilMs = 0;
uint8_t avrcTransactionLabel = 0;

struct EspDriveActionRequest { EspDriveMenuSelection selection; };

struct VirtualTrackSelectionGuard
{
	EspDriveMenuCategory category = EspDriveMenuCategory::None;
	uint32_t recordId = 0;
	uint32_t expiresAtMs = 0;
	bool active = false;
};

EspDriveMenuCategory selectedVirtualCategory = EspDriveMenuCategory::None;
VirtualTrackSelectionGuard virtualTrackSelectionGuard;
constexpr uint32_t kVirtualTrackSelectionGuardMs = 3000UL;

static bool elapsed(uint32_t now, uint32_t deadline) { return deadline != 0 && (int32_t)(now - deadline) >= 0; }
static bool confirmationPending(uint32_t now, uint32_t deadline) { return deadline != 0 && !elapsed(now, deadline); }

static const char *peerNameOr(const char *fallback)
{
	const char *peerName = a2dp_sink.get_peer_name();
	return peerName != nullptr && peerName[0] != '\0' ? peerName : fallback;
}

static const char *autoplayName(AutoplayMode value)
{
	switch (value) { case AutoplayMode::Off: return "Off"; case AutoplayMode::HeadUnit: return "Head unit"; case AutoplayMode::Immediate: return "Immediate"; default: return "Reliable"; }
}
static const char *channelName(ChannelMode value)
{
	switch (value) { case ChannelMode::Normal: return "Normal"; case ChannelMode::Swap: return "Swap L/R"; default: return "Mono"; }
}
static const char *gainName(GainPreset value)
{
	switch (value) { case GainPreset::Full: return "100%"; case GainPreset::Percent85: return "85%"; default: return "70%"; }
}
static const char *outputActivationName(OutputActivation value) { return value == OutputActivation::Immediate ? "Immediate" : "After car link"; }
static const char *menuCategoryName(EspDriveMenuCategory category)
{
	switch (category)
	{
	case EspDriveMenuCategory::Phone: return "Phone";
	case EspDriveMenuCategory::Audio: return "Audio";
	case EspDriveMenuCategory::Playback: return "Playback";
	case EspDriveMenuCategory::Diagnostics: return "Diagnostics";
	case EspDriveMenuCategory::System: return "System";
	default: return "None";
	}
}
static const char *a2dpStateName(esp_a2d_connection_state_t state)
{
	switch (state) { case ESP_A2D_CONNECTION_STATE_CONNECTED: return "Connected"; case ESP_A2D_CONNECTION_STATE_CONNECTING: return "Connecting"; case ESP_A2D_CONNECTION_STATE_DISCONNECTING: return "Disconnecting"; default: return "Disconnected"; }
}
static const char *audioStateName(esp_a2d_audio_state_t state)
{
	switch (state) { case ESP_A2D_AUDIO_STATE_STARTED: return "Started"; case ESP_A2D_AUDIO_STATE_REMOTE_SUSPEND: return "Paused"; default: return "Stopped"; }
}
static void copyText(char *destination, size_t destinationSize, const char *source)
{
	if (destinationSize == 0) return;
	size_t out = 0;
	for (size_t in = 0; source != nullptr && source[in] != '\0';)
	{
		const uint8_t c = static_cast<uint8_t>(source[in]);
		if (c < 0x20 || c == 0x7F) { if (out + 1 >= destinationSize) break; destination[out++] = ' '; ++in; continue; }
		size_t bytes = c < 0x80 ? 1 : ((c & 0xE0) == 0xC0 ? 2 : ((c & 0xF0) == 0xE0 ? 3 : ((c & 0xF8) == 0xF0 ? 4 : 1)));
		bool valid = bytes == 1;
		if (bytes > 1)
		{
			valid = true;
			for (size_t byteIndex = 1; byteIndex < bytes; ++byteIndex)
				if (source[in + byteIndex] == '\0' || (static_cast<uint8_t>(source[in + byteIndex]) & 0xC0) != 0x80) valid = false;
		}
		if (!valid) { if (out + 1 >= destinationSize) break; destination[out++] = '?'; ++in; continue; }
		if (out + bytes >= destinationSize) break;
		for (size_t byteIndex = 0; byteIndex < bytes; ++byteIndex) destination[out++] = source[in + byteIndex];
		in += bytes;
	}
	destination[out] = '\0';
}
static void updateOutputActivation(uint32_t now)
{
	if (audioSettings.activation == OutputActivation::Immediate || espod.extendedInterfaceModeActive || elapsed(now, outputCarLinkDeadlineMs))
		audioSettings.outputEnabled = true;
}

static void setEspodDisabled(bool disabled)
{
	espod.disabled = disabled;
#if ESPDRIVE_ACTIVE_DCD
	digitalWrite(DCD_CTRL_PIN, INVERT_DCD_LOGIC(disabled));
#endif
}

static size_t writeI2sPcm(const uint8_t *data, size_t length) { return i2s.write(data, length); }
void read_data_stream(const uint8_t *data, uint32_t length)
{
	EspDriveAudio::processPcm(data, length, audioSettings, millis(), writeI2sPcm);
}

void publishRealMetadata()
{
	RealMetadata snapshot;
	portENTER_CRITICAL(&stateMux);
	snapshot = metadata;
	portEXIT_CRITICAL(&stateMux);
	espod.updateTrackTitle(snapshot.title);
	espod.updateArtistName(snapshot.artist);
	espod.updateAlbumName(snapshot.album);
	espod.updateTrackGenre(snapshot.genre);
	espod.updateTrackNumber(static_cast<uint32_t>(strtoul(snapshot.trackNumber, nullptr, 10)));
	espod.updateTotalTrackCount(static_cast<uint32_t>(strtoul(snapshot.totalTracks, nullptr, 10)));
	espod.updateTrackDuration(static_cast<uint32_t>(strtoul(snapshot.duration, nullptr, 10)));
}
void showEspDriveOverlay(const char *title, const char *artist, const char *album, uint32_t durationMs = kOverlayDefaultMs)
{
	portENTER_CRITICAL(&stateMux);
	overlay.expiresAtMs = millis() + durationMs;
	overlay.active = true;
	portEXIT_CRITICAL(&stateMux);
	espod.updateTrackTitle(title);
	espod.updateArtistName(artist);
	espod.updateAlbumName(album);
}

void avrc_metadata_callback(uint8_t id, const uint8_t *text)
{
	const char *value = text == nullptr ? "" : reinterpret_cast<const char *>(text);
	char *destination = nullptr;
	const char *fallback = "";
	uint8_t pendingFlag = 0;

	switch (id)
	{
	case ESP_AVRC_MD_ATTR_TITLE:
		destination = metadata.title; fallback = "Unknown title"; pendingFlag = TitlePending; break;
	case ESP_AVRC_MD_ATTR_ARTIST:
		destination = metadata.artist; fallback = "Unknown artist"; pendingFlag = ArtistPending; break;
	case ESP_AVRC_MD_ATTR_ALBUM:
		destination = metadata.album; fallback = "Unknown album"; pendingFlag = AlbumPending; break;
	case ESP_AVRC_MD_ATTR_GENRE:
		destination = metadata.genre; pendingFlag = GenrePending; break;
	case ESP_AVRC_MD_ATTR_PLAYING_TIME:
		destination = metadata.duration; pendingFlag = DurationPending; break;
	case ESP_AVRC_MD_ATTR_TRACK_NUM:
		destination = metadata.trackNumber; pendingFlag = TrackNumberPending; break;
	case ESP_AVRC_MD_ATTR_NUM_TRACKS:
		destination = metadata.totalTracks; pendingFlag = TotalTracksPending; break;
	default:
		ESP_LOGW("AVRCP", "Unhandled metadata attribute %u", id);
		return;
	}

	portENTER_CRITICAL(&stateMux);
	copyText(destination, kMetadataLength, value[0] != '\0' ? value : fallback);
	metadata.pendingAttributes |= pendingFlag;
	portEXIT_CRITICAL(&stateMux);
	xTaskNotifyGive(processAVRCTaskHandle);
}

static void processAVRCTask(void *)
{
	for (;;)
	{
		ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(kStateTaskWakeMs));
		bool changed = false;
		portENTER_CRITICAL(&stateMux);
		changed = metadata.pendingAttributes != 0;
		metadata.pendingAttributes = 0;
		const bool overlayExpired = overlay.active && elapsed(millis(), overlay.expiresAtMs);
		if (overlayExpired) overlay.active = false;
		const bool overlayActive = overlay.active;
		portEXIT_CRITICAL(&stateMux);
		if (changed && !overlayActive) publishRealMetadata();
		if (overlayExpired) publishRealMetadata();
	}
}

void connectionStateChanged(esp_a2d_connection_state_t state, void *)
{
	portENTER_CRITICAL(&stateMux);
	bluetoothEvents.connected = state == ESP_A2D_CONNECTION_STATE_CONNECTED;
	bluetoothEvents.connectionChanged = true;
	portEXIT_CRITICAL(&stateMux);
}
void audioStateChanged(esp_a2d_audio_state_t state, void *)
{
	portENTER_CRITICAL(&stateMux);
	bluetoothEvents.audioState = state;
	bluetoothEvents.audioStateChanged = true;
	portEXIT_CRITICAL(&stateMux);
}
void avrc_rn_play_pos_callback(uint32_t position)
{
	portENTER_CRITICAL(&stateMux);
	bluetoothEvents.playPositionMs = position;
	bluetoothEvents.playPositionChanged = true;
	portEXIT_CRITICAL(&stateMux);
}
void avrcConnectionChanged(bool connected) { avrcConnected = connected; }
void sampleRateChanged(uint16_t sampleRate) { negotiatedSampleRate = sampleRate; }
void rssiChanged(esp_bt_gap_cb_param_t::read_rssi_delta_param &rssi) { latestRssi = rssi.rssi_delta; latestRssiAvailable = true; }

static void sendSeekCommand(esp_avrc_pt_cmd_t command, esp_avrc_pt_cmd_state_t state)
{
	if (a2dp_sink.get_connection_state() != ESP_A2D_CONNECTION_STATE_CONNECTED) return;
	const esp_err_t result = esp_avrc_ct_send_passthrough_cmd(avrcTransactionLabel++ & 0x0F, command, state);
	if (result != ESP_OK) ESP_LOGW("AVRCP", "Seek command failed: %s", esp_err_to_name(result));
}
void playStatusHandler(PB_COMMAND command)
{
	switch (command)
	{
	case PB_CMD_TOGGLE: if (latestAudioState == ESP_A2D_AUDIO_STATE_STARTED) a2dp_sink.pause(); else a2dp_sink.play(); break;
	case PB_CMD_STOP: a2dp_sink.stop(); break;
	case PB_CMD_PLAY: a2dp_sink.play(); break;
	case PB_CMD_PAUSE: a2dp_sink.pause(); break;
	case PB_CMD_NEXT_TRACK: case PB_CMD_NEXT: a2dp_sink.next(); break;
	case PB_CMD_PREVIOUS_TRACK: case PB_CMD_PREV: a2dp_sink.previous(); break;
	case PB_CMD_SEEK_FF: sendSeekCommand(ESP_AVRC_PT_CMD_FAST_FORWARD, ESP_AVRC_PT_CMD_STATE_PRESSED); break;
	case PB_CMD_SEEK_RW: sendSeekCommand(ESP_AVRC_PT_CMD_REWIND, ESP_AVRC_PT_CMD_STATE_PRESSED); break;
	case PB_CMD_STOP_SEEK:
		sendSeekCommand(ESP_AVRC_PT_CMD_FAST_FORWARD, ESP_AVRC_PT_CMD_STATE_RELEASED);
		sendSeekCommand(ESP_AVRC_PT_CMD_REWIND, ESP_AVRC_PT_CMD_STATE_RELEASED);
		break;
	default: ESP_LOGW("AVRCP", "Unsupported iPod playback command %u", static_cast<unsigned>(command)); break;
	}
}

static void formatAddress(const PhoneIdentity &phone, char *output, size_t outputSize)
{
	if (!phone.valid) { snprintf(output, outputSize, "None"); return; }
	snprintf(output, outputSize, "%02X:%02X:%02X:%02X:%02X:%02X", phone.address[0], phone.address[1], phone.address[2], phone.address[3], phone.address[4], phone.address[5]);
}
static void loadPreferences()
{
	espDrivePreferences.begin("espdrive", false);
	if (espDrivePreferences.isKey("autoplayMode")) autoplay.mode = static_cast<AutoplayMode>(espDrivePreferences.getUChar("autoplayMode", static_cast<uint8_t>(AutoplayMode::HeadUnit)));
	else { autoplay.mode = espDrivePreferences.getBool("autoplay", true) ? AutoplayMode::Immediate : AutoplayMode::Off; espDrivePreferences.putUChar("autoplayMode", static_cast<uint8_t>(autoplay.mode)); }
	audioSettings.channelMode = static_cast<ChannelMode>(espDrivePreferences.getUChar("channel", 0));
	audioSettings.gain = static_cast<GainPreset>(espDrivePreferences.getUChar("gain", 0));
	audioSettings.startupFade = espDrivePreferences.getBool("fade", true);
	audioSettings.activation = static_cast<OutputActivation>(espDrivePreferences.getUChar("activation", 0));
	if (espDrivePreferences.getBytesLength("preferred") == sizeof(preferredPhone.address)) { espDrivePreferences.getBytes("preferred", preferredPhone.address, sizeof(preferredPhone.address)); preferredPhone.valid = true; }
}
static bool preferredAddressValidator(esp_bd_addr_t address)
{
	return !preferredPhone.valid || elapsed(millis(), preferredOnlyUntilMs) ||
		memcmp(address, preferredPhone.address, sizeof(preferredPhone.address)) == 0;
}
static void captureCurrentPhone()
{
	const esp_bd_addr_t *address = a2dp_sink.get_current_peer_address();
	if (address != nullptr)
	{
		memcpy(currentPhone.address, *address, sizeof(currentPhone.address));
		currentPhone.valid = true;
	}
}
static uint8_t bondCount() { const int count = esp_bt_gap_get_bond_device_num(); return count < 0 ? 0 : static_cast<uint8_t>(min(count, 255)); }
static bool currentPhoneIsBonded()
{
	if (!currentPhone.valid) return false;
	esp_bd_addr_t bonds[20]; int count = 20;
	if (esp_bt_gap_get_bond_device_list(&count, bonds) != ESP_OK) return false;
	for (int index = 0; index < count; ++index) if (memcmp(bonds[index], currentPhone.address, sizeof(currentPhone.address)) == 0) return true;
	return false;
}
static const char *actionName(EspDriveActionId action)
{
	switch (action) { case EspDriveActionId::ReconnectCar: return "ReconnectCar"; case EspDriveActionId::RequestForgetCurrent: return "ForgetCurrent"; case EspDriveActionId::SetPreferredPhone: return "SetPreferred"; case EspDriveActionId::ShowDiagnostic: return "Diagnostic"; default: return "ESPDrive"; }
}
static void fillMenuRuntimeForRecord(EspDriveMenuRuntime &runtime, char *storage, size_t storageSize,
	EspDriveMenuCategory category, uint32_t recordId)
{
#if ESPDRIVE_CATEGORIZED_MENU
	switch (category)
	{
	case EspDriveMenuCategory::Phone:
		if (recordId == 6)
		{
			formatAddress(preferredPhone, storage, storageSize);
			runtime.preferredPhone = storage;
		}
		break;

	case EspDriveMenuCategory::Audio:
		runtime.channelMode = channelName(audioSettings.channelMode);
		runtime.gainPreset = gainName(audioSettings.gain);
		runtime.startupFade = audioSettings.startupFade ? "On" : "Off";
		runtime.outputActivation = outputActivationName(audioSettings.activation);
		break;

	case EspDriveMenuCategory::Playback:
		runtime.autoplayMode = autoplayName(autoplay.mode);
		break;

	case EspDriveMenuCategory::Diagnostics:
		switch (recordId)
		{
		case 0: snprintf(storage, storageSize, "%s", peerNameOr("No phone")); runtime.connectedPhone = storage; break;
		case 1: runtime.bondedDeviceCount = bondCount(); break;
		case 2: runtime.a2dpState = a2dpStateName(a2dp_sink.get_connection_state()); break;
		case 3: runtime.avrcState = avrcConnected ? "Connected" : "Waiting"; break;
		case 4: runtime.audioState = audioStateName(latestAudioState); break;
		case 5: snprintf(storage, storageSize, "%ddBm", latestRssi); runtime.rssi = latestRssiAvailable ? storage : "Unavailable"; break;
		case 6: snprintf(storage, storageSize, "%u Hz, %u ch", negotiatedSampleRate, negotiatedChannels); runtime.audioFormat = negotiatedSampleRate ? storage : "Unavailable"; break;
		case 7: runtime.carLink = espod.extendedInterfaceModeActive ? "Extended active" : (espod.disabled ? "Disabled" : "Ready"); break;
		case 8: snprintf(storage, storageSize, "%lus", static_cast<unsigned long>(millis() / 1000UL)); runtime.uptime = storage; break;
		case 9: snprintf(storage, storageSize, "%lu / %lu", static_cast<unsigned long>(ESP.getFreeHeap()), static_cast<unsigned long>(heap_caps_get_minimum_free_size(MALLOC_CAP_8BIT))); runtime.heap = storage; break;
		case 10: snprintf(storage, storageSize, "%d", esp_reset_reason()); runtime.resetReason = storage; break;
		case 11: snprintf(storage, storageSize, "%s %s", VERSION_BRANCH, VERSION_STRING); runtime.build = storage; break;
		default: break;
		}
		break;

	default:
		break;
	}
#else
	(void)category;
	switch (recordId)
	{
	case 4: runtime.autoplayMode = autoplayName(autoplay.mode); break;
	case 10: formatAddress(preferredPhone, storage, storageSize); runtime.preferredPhone = storage; break;
	case 11: runtime.channelMode = channelName(audioSettings.channelMode); break;
	case 12: runtime.gainPreset = gainName(audioSettings.gain); break;
	case 13: runtime.startupFade = audioSettings.startupFade ? "On" : "Off"; break;
	case 14: runtime.outputActivation = outputActivationName(audioSettings.activation); break;
	case 15: snprintf(storage, storageSize, "%s", peerNameOr("No phone")); runtime.connectedPhone = storage; break;
	case 16: runtime.bondedDeviceCount = bondCount(); break;
	case 17: runtime.a2dpState = a2dpStateName(a2dp_sink.get_connection_state()); break;
	case 18: runtime.avrcState = avrcConnected ? "Connected" : "Waiting"; break;
	case 19: runtime.audioState = audioStateName(latestAudioState); break;
	case 20: snprintf(storage, storageSize, "%ddBm", latestRssi); runtime.rssi = latestRssiAvailable ? storage : "Unavailable"; break;
	case 21: snprintf(storage, storageSize, "%u Hz, %u ch", negotiatedSampleRate, negotiatedChannels); runtime.audioFormat = negotiatedSampleRate ? storage : "Unavailable"; break;
	case 22: runtime.carLink = espod.extendedInterfaceModeActive ? "Extended active" : (espod.disabled ? "Disabled" : "Ready"); break;
	case 23: snprintf(storage, storageSize, "%lus", static_cast<unsigned long>(millis() / 1000UL)); runtime.uptime = storage; break;
	case 24: snprintf(storage, storageSize, "%lu / %lu", static_cast<unsigned long>(ESP.getFreeHeap()), static_cast<unsigned long>(heap_caps_get_minimum_free_size(MALLOC_CAP_8BIT))); runtime.heap = storage; break;
	case 25: snprintf(storage, storageSize, "%d", esp_reset_reason()); runtime.resetReason = storage; break;
	case 26: snprintf(storage, storageSize, "%s %s", VERSION_BRANCH, VERSION_STRING); runtime.build = storage; break;
	default: break;
	}
#endif
}
static EspDriveMenuCategory activeVirtualCategory()
{
	portENTER_CRITICAL(&stateMux);
	const EspDriveMenuCategory category = selectedVirtualCategory;
	portEXIT_CRITICAL(&stateMux);
	return category;
}

uint32_t espDriveDatabaseCount(DB_CATEGORY category)
{
	const EspDriveMenuCategory activeCategory = activeVirtualCategory();
	const uint32_t count = EspDriveMenu::recordCount(category, activeCategory);
	if (count == 0 && category != DB_CAT_PLAYLIST && category != DB_CAT_TRACK)
		ESP_LOGW("ESPDriveMenu", "Unexpected DB count category=0x%02X active=%s", category, menuCategoryName(activeCategory));
	return count;
}
bool espDriveDatabaseRecord(DB_CATEGORY category, uint32_t recordId, char *recordName, size_t recordNameSize)
{
	const EspDriveMenuCategory activeCategory = activeVirtualCategory();
	char storage[128];
	EspDriveMenuRuntime runtime;
	if (category == DB_CAT_TRACK) fillMenuRuntimeForRecord(runtime, storage, sizeof(storage), activeCategory, recordId);
#if !ESPDRIVE_CATEGORIZED_MENU
	else fillMenuRuntimeForRecord(runtime, storage, sizeof(storage), activeCategory, recordId);
#endif
	const bool found = EspDriveMenu::recordName(category, activeCategory, recordId, runtime, recordName, recordNameSize);
	if (!found) ESP_LOGW("ESPDriveMenu", "Unexpected DB record category=0x%02X record=%lu active=%s", category,
		static_cast<unsigned long>(recordId), menuCategoryName(activeCategory));
	return found;
}
void espDriveDatabaseSelected(DB_CATEGORY category, uint32_t recordId)
{
	if (category == DB_CAT_PLAYLIST)
	{
		const EspDriveMenuCategory selectedCategory = EspDriveMenu::categoryForPlaylist(recordId);
		if (selectedCategory == EspDriveMenuCategory::None)
		{
			ESP_LOGW("ESPDriveMenu", "Unexpected playlist selection record=%lu", static_cast<unsigned long>(recordId));
			return;
		}
		portENTER_CRITICAL(&stateMux);
		selectedVirtualCategory = selectedCategory;
		portEXIT_CRITICAL(&stateMux);
		ESP_LOGI("ESPDriveMenu", "Virtual category selected: %s", menuCategoryName(selectedCategory));
		return;
	}

	const EspDriveMenuCategory activeCategory = activeVirtualCategory();
	const EspDriveMenuSelection selection = EspDriveMenu::selectionForRecord(category, activeCategory, recordId);
	if (selection.action == EspDriveActionId::None)
	{
		ESP_LOGW("ESPDriveMenu", "Unexpected virtual selection category=0x%02X record=%lu active=%s", category,
			static_cast<unsigned long>(recordId), menuCategoryName(activeCategory));
		return;
	}
	portENTER_CRITICAL(&stateMux);
	virtualTrackSelectionGuard.category = activeCategory;
	virtualTrackSelectionGuard.recordId = recordId;
	virtualTrackSelectionGuard.expiresAtMs = millis() + kVirtualTrackSelectionGuardMs;
	virtualTrackSelectionGuard.active = true;
	portEXIT_CRITICAL(&stateMux);
	const EspDriveActionRequest request = {selection};
	if (xQueueSend(espDriveActionQueue, &request, 0) != pdTRUE) ESP_LOGW("ESPDriveMenu", "Action queue full for %lu", static_cast<unsigned long>(recordId));
}
bool absorbVirtualTrackSelection(uint32_t trackIndex)
{
	bool absorb = false;
	EspDriveMenuCategory category = EspDriveMenuCategory::None;
	portENTER_CRITICAL(&stateMux);
	if (virtualTrackSelectionGuard.active && elapsed(millis(), virtualTrackSelectionGuard.expiresAtMs))
		virtualTrackSelectionGuard.active = false;
	if (virtualTrackSelectionGuard.active && virtualTrackSelectionGuard.recordId == trackIndex)
	{
		category = virtualTrackSelectionGuard.category;
		virtualTrackSelectionGuard.active = false;
		absorb = true;
	}
	portEXIT_CRITICAL(&stateMux);
	if (absorb) ESP_LOGI("ESPDriveMenu", "Absorbed PlayCurrentSelection record=%lu category=%s",
		static_cast<unsigned long>(trackIndex), menuCategoryName(category));
	return absorb;
}
static void showDiagnostic(const EspDriveMenuSelection &selection)
{
	char storage[128];
	EspDriveMenuRuntime runtime;
	fillMenuRuntimeForRecord(runtime, storage, sizeof(storage), selection.category, selection.recordId);
	char label[128] = {}; EspDriveMenu::recordName(selection.databaseCategory, selection.category, selection.recordId, runtime, label, sizeof(label));
	showEspDriveOverlay(label, "ESPDrive diagnostics", "Read only");
}
static void runCarRecovery()
{
	if (carRecovery.state != CarRecoveryState::Idle) { showEspDriveOverlay("Car recovery already running", "ESPDrive", "Please wait"); return; }
	ESP_LOGI("CarRecovery", "Starting car-link recovery");
	espod.resetState();
#if ESPDRIVE_ACTIVE_DCD
	setEspodDisabled(true);
	carRecovery.state = CarRecoveryState::DcdReleased;
	carRecovery.deadlineMs = millis() + kCarRecoveryDcdMs;
	showEspDriveOverlay("Reconnecting car", "DCD released", "Bluetooth stays connected");
#else
	showEspDriveOverlay("Car state reset", "No active DCD hardware", "Handshake may require USB reconnect");
	ESP_LOGW("CarRecovery", "Active DCD support is unavailable in this build");
#endif
}

static void startPairing()
{
	const uint32_t now = millis();
	keepCarLinkUntilMs = now + kPairingTimeoutMs;
	setEspodDisabled(false);
	a2dp_sink.set_auto_reconnect(false);
	if (a2dp_sink.get_connection_state() == ESP_A2D_CONNECTION_STATE_CONNECTED) a2dp_sink.disconnect();
	esp_bt_gap_set_scan_mode(ESP_BT_CONNECTABLE, ESP_BT_GENERAL_DISCOVERABLE);
	pairingExpiresAtMs = now + kPairingTimeoutMs;
	showEspDriveOverlay("Pairing mode", "ESPDrive", "Open phone Bluetooth");
}

static void reconnectLastPhone()
{
	preferredOnlyUntilMs = preferredPhone.valid ? millis() + kPreferredReconnectWindowMs : 0;
	a2dp_sink.set_auto_reconnect(true, kBoundedReconnectAttempts);
	showEspDriveOverlay("Reconnecting", preferredPhone.valid ? "Preferred phone first" : "Last known phone", "Bounded attempts");
}

static void disconnectCurrentPhone()
{
	if (a2dp_sink.get_connection_state() != ESP_A2D_CONNECTION_STATE_CONNECTED)
	{
		showEspDriveOverlay("No phone connected", "ESPDrive", "Nothing to disconnect");
		return;
	}

	keepCarLinkUntilMs = millis() + kManualDisconnectKeepCarMs;
	reconnectSuppressedUntilMs = keepCarLinkUntilMs;
	a2dp_sink.set_auto_reconnect(false);
	a2dp_sink.disconnect();
	showEspDriveOverlay("Phone disconnected", "Car menu remains available", "Bluetooth ready");
}

static void cycleAutoplayMode()
{
	autoplay.mode = static_cast<AutoplayMode>((static_cast<uint8_t>(autoplay.mode) + 1U) % 4U);
	autoplay.waitingForAudio = false;
	espDrivePreferences.putUChar("autoplayMode", static_cast<uint8_t>(autoplay.mode));
	showEspDriveOverlay("Autoplay", autoplayName(autoplay.mode), "Saved");
}

static void requestClearPairings()
{
	const uint32_t now = millis();
	if (!confirmationPending(now, clearPairingsConfirmUntilMs))
	{
		clearPairingsConfirmUntilMs = now + kConfirmTimeoutMs;
		showEspDriveOverlay("Select again to confirm", "Clear paired phones", "Expires in 10 seconds");
		return;
	}

	esp_bd_addr_t bonds[20];
	int count = 20;
	if (esp_bt_gap_get_bond_device_list(&count, bonds) == ESP_OK)
		for (int index = 0; index < count; ++index) esp_bt_gap_remove_bond_device(bonds[index]);
	preferredPhone.valid = false;
	espDrivePreferences.remove("preferred");
	clearPairingsConfirmUntilMs = 0;
	showEspDriveOverlay("Pairings cleared", "ESPDrive", "Pair a new phone");
}

static void requestRestart()
{
	const uint32_t now = millis();
	if (!confirmationPending(now, restartConfirmUntilMs))
	{
		restartConfirmUntilMs = now + kConfirmTimeoutMs;
		showEspDriveOverlay("Select again to confirm", "Restart ESPDrive", "Expires in 10 seconds");
		return;
	}

	restartConfirmUntilMs = 0;
	showEspDriveOverlay("Restarting", "ESPDrive", "Please wait", 800);
	vTaskDelay(pdMS_TO_TICKS(800));
	esp_restart();
}

static void requestForgetCurrentPhone()
{
	if (!currentPhone.valid)
	{
		showEspDriveOverlay("No current phone", "ESPDrive", "Nothing to forget");
		return;
	}

	const uint32_t now = millis();
	if (!confirmationPending(now, forgetPhoneConfirmUntilMs))
	{
		forgetPhoneConfirmUntilMs = now + kConfirmTimeoutMs;
		showEspDriveOverlay("Select again to confirm", "Forget current phone", "Expires in 10 seconds");
		return;
	}

	esp_bt_gap_remove_bond_device(currentPhone.address);
	if (preferredPhone.valid && memcmp(preferredPhone.address, currentPhone.address, sizeof(currentPhone.address)) == 0)
	{
		preferredPhone.valid = false;
		espDrivePreferences.remove("preferred");
	}
	forgetPhoneConfirmUntilMs = 0;
	showEspDriveOverlay("Phone forgotten", "ESPDrive", "Other pairings kept");
}

static void setPreferredPhone()
{
	if (!currentPhone.valid || !currentPhoneIsBonded())
	{
		showEspDriveOverlay("No bonded current phone", "ESPDrive", "Connect a paired phone first");
		return;
	}

	preferredPhone = currentPhone;
	espDrivePreferences.putBytes("preferred", preferredPhone.address, sizeof(preferredPhone.address));
	showEspDriveOverlay("Preferred phone saved", peerNameOr("Current phone"), "Used for auto reconnect");
}

static void showPreferredPhone()
{
	char address[32];
	formatAddress(preferredPhone, address, sizeof(address));
	showEspDriveOverlay("Preferred phone", address, preferredPhone.valid ? "Address identity" : "None saved");
}

static void cycleChannelMode()
{
	audioSettings.channelMode = static_cast<ChannelMode>((static_cast<uint8_t>(audioSettings.channelMode) + 1U) % 3U);
	espDrivePreferences.putUChar("channel", static_cast<uint8_t>(audioSettings.channelMode));
	showEspDriveOverlay("Channel mode", channelName(audioSettings.channelMode), "Local PCM processing");
}

static void cycleGainPreset()
{
	audioSettings.gain = static_cast<GainPreset>((static_cast<uint8_t>(audioSettings.gain) + 1U) % 3U);
	espDrivePreferences.putUChar("gain", static_cast<uint8_t>(audioSettings.gain));
	showEspDriveOverlay("Output gain", gainName(audioSettings.gain), "Local only");
}

static void toggleStartupFade()
{
	audioSettings.startupFade = !audioSettings.startupFade;
	espDrivePreferences.putBool("fade", audioSettings.startupFade);
	showEspDriveOverlay("Startup fade", audioSettings.startupFade ? "On" : "Off", "Saved");
}

static void toggleOutputActivation()
{
	audioSettings.activation = audioSettings.activation == OutputActivation::Immediate ?
		OutputActivation::AfterCarLink : OutputActivation::Immediate;
	espDrivePreferences.putUChar("activation", static_cast<uint8_t>(audioSettings.activation));
	showEspDriveOverlay("Output activation", outputActivationName(audioSettings.activation), "Saved");
}

static void processAction(const EspDriveActionRequest &request)
{
	const EspDriveMenuSelection &selection = request.selection;
	ESP_LOGI("ESPDriveAction", "Action %s category=%s record=%lu", actionName(selection.action),
		menuCategoryName(selection.category), static_cast<unsigned long>(selection.recordId));
	switch (selection.action)
	{
	case EspDriveActionId::ShowStatus: showEspDriveOverlay(a2dp_sink.get_connection_state() == ESP_A2D_CONNECTION_STATE_CONNECTED ? "Connected" : "No phone connected", peerNameOr("Bluetooth ready"), espod.extendedInterfaceModeActive ? "Car link active" : "Car link waiting"); break;
	case EspDriveActionId::StartPairing: startPairing(); break;
	case EspDriveActionId::ReconnectLast: reconnectLastPhone(); break;
	case EspDriveActionId::DisconnectCurrent: disconnectCurrentPhone(); break;
	case EspDriveActionId::ToggleAutoplay: cycleAutoplayMode(); break;
	case EspDriveActionId::RequestClearPairings: requestClearPairings(); break;
	case EspDriveActionId::RequestRestart: requestRestart(); break;
	case EspDriveActionId::ReconnectCar: runCarRecovery(); break;
	case EspDriveActionId::RequestForgetCurrent: requestForgetCurrentPhone(); break;
	case EspDriveActionId::SetPreferredPhone: setPreferredPhone(); break;
	case EspDriveActionId::ShowPreferredPhone: showPreferredPhone(); break;
	case EspDriveActionId::CycleChannelMode: cycleChannelMode(); break;
	case EspDriveActionId::CycleGainPreset: cycleGainPreset(); break;
	case EspDriveActionId::ToggleStartupFade: toggleStartupFade(); break;
	case EspDriveActionId::ToggleOutputActivation: toggleOutputActivation(); break;
	case EspDriveActionId::ShowDiagnostic: showDiagnostic(selection); break;
	default: showEspDriveOverlay("Not available", "ESPDrive", "Unsupported menu action"); break;
	}
}
static void processEspDriveActions(void *)
{
	EspDriveActionRequest request;
	for (;;)
	{
		if (xQueueReceive(espDriveActionQueue, &request, portMAX_DELAY) == pdTRUE) processAction(request);
	}
}

static void initializeWorkerTasks()
{
	espDriveActionQueue = xQueueCreate(kActionQueueLength, sizeof(EspDriveActionRequest));
	if (espDriveActionQueue == nullptr)
	{
		ESP_LOGE("ESPDrive", "Failed to create action queue");
		esp_restart();
	}

	const BaseType_t avrcTaskResult = xTaskCreatePinnedToCore(processAVRCTask, "processAVRC",
		kWorkerStackSize, nullptr, 6, &processAVRCTaskHandle, ARDUINO_RUNNING_CORE);
	const BaseType_t actionTaskResult = xTaskCreatePinnedToCore(processEspDriveActions, "espDriveAction",
		kWorkerStackSize, nullptr, 4, &espDriveActionTaskHandle, ARDUINO_RUNNING_CORE);
	if (avrcTaskResult != pdPASS || actionTaskResult != pdPASS)
	{
		ESP_LOGE("ESPDrive", "Failed to create worker tasks");
		esp_restart();
	}
}

static void initializeA2DPSink()
{
#ifdef AUDIOKIT
	minimalPins.addI2C(PinFunction::CODEC, 32, 33);
	minimalPins.addI2S(PinFunction::CODEC, 0, BCLK_PIN, WS_PIN, DIN_PIN, 35);
	auto cfg = i2s.defaultConfig();
	cfg.copyFrom(info);
	i2s.begin(cfg);
#else
	a2dp_sink.set_stream_reader(read_data_stream, false);
	auto cfg = i2s.defaultConfig(TX_MODE);
	cfg.pin_ws = WS_PIN;
	cfg.pin_data = DIN_PIN;
	cfg.pin_bck = BCLK_PIN;
	cfg.sample_rate = 44100;
	cfg.i2s_format = I2S_LSB_FORMAT;
	i2s.begin(cfg);
#endif
	a2dp_sink.set_auto_reconnect(true, kBoundedReconnectAttempts);
	a2dp_sink.set_address_validator(preferredAddressValidator);
	a2dp_sink.set_on_connection_state_changed(connectionStateChanged);
	a2dp_sink.set_on_audio_state_changed(audioStateChanged);
	a2dp_sink.set_avrc_connection_state_callback(avrcConnectionChanged);
	a2dp_sink.set_avrc_metadata_callback(avrc_metadata_callback);
	a2dp_sink.set_avrc_metadata_attribute_mask(
		ESP_AVRC_MD_ATTR_TITLE | ESP_AVRC_MD_ATTR_ARTIST | ESP_AVRC_MD_ATTR_ALBUM |
		ESP_AVRC_MD_ATTR_PLAYING_TIME | ESP_AVRC_MD_ATTR_GENRE |
		ESP_AVRC_MD_ATTR_TRACK_NUM | ESP_AVRC_MD_ATTR_NUM_TRACKS);
	a2dp_sink.set_avrc_rn_play_pos_callback(avrc_rn_play_pos_callback, 1);
	a2dp_sink.set_rssi_callback(rssiChanged);
	a2dp_sink.set_rssi_active(true);
	a2dp_sink.set_sample_rate_callback(sampleRateChanged);
	a2dp_sink.start(A2DP_SINK_NAME);
}

static BluetoothEventMailbox takeBluetoothEvents()
{
	portENTER_CRITICAL(&stateMux);
	const BluetoothEventMailbox pending = bluetoothEvents;
	bluetoothEvents.connectionChanged = false;
	bluetoothEvents.audioStateChanged = false;
	bluetoothEvents.playPositionChanged = false;
	portEXIT_CRITICAL(&stateMux);
	return pending;
}

static void serviceBluetoothEvents(uint32_t now)
{
	const BluetoothEventMailbox pending = takeBluetoothEvents();
	if (pending.connectionChanged)
	{
		if (pending.connected)
		{
			setEspodDisabled(false);
			audioSettings.fadeStartedAtMs = now;
			outputCarLinkDeadlineMs = now + kOutputCarLinkTimeoutMs;
			audioSettings.outputEnabled = audioSettings.activation == OutputActivation::Immediate;
			autoplay.retriesSent = 0;
			autoplay.waitingForAudio = autoplay.mode == AutoplayMode::Reliable;
			autoplay.nextAttemptAtMs = now + kReliableAutoplayInitialMs;
			captureCurrentPhone();
			showEspDriveOverlay("Connected", peerNameOr("Phone"), "ESPDrive", kWelcomeOverlayMs);
		}
		else
		{
			autoplay.waitingForAudio = false;
			espod.resetState();
			setEspodDisabled(!elapsed(now, keepCarLinkUntilMs));
		}

#ifdef LED_BUILTIN
		digitalWrite(LED_BUILTIN, INVERT_LED_LOGIC(pending.connected));
#endif
	}

	if (pending.audioStateChanged)
	{
		latestAudioState = pending.audioState;
		if (pending.audioState == ESP_A2D_AUDIO_STATE_STARTED)
		{
			espod.play(true);
			autoplay.waitingForAudio = false;
		}
		else if (pending.audioState == ESP_A2D_AUDIO_STATE_REMOTE_SUSPEND)
		{
			espod.pause(true);
		}
	}

	if (pending.playPositionChanged) espod.updatePlayPosition(pending.playPositionMs);
}

static void serviceConnectionTimers(uint32_t now)
{
	if (elapsed(now, pairingExpiresAtMs))
	{
		pairingExpiresAtMs = 0;
		a2dp_sink.set_auto_reconnect(true, kBoundedReconnectAttempts);
		showEspDriveOverlay("Pairing timed out", "ESPDrive", "Try again");
	}
	if (elapsed(now, reconnectSuppressedUntilMs))
	{
		reconnectSuppressedUntilMs = 0;
		a2dp_sink.set_auto_reconnect(true, kBoundedReconnectAttempts);
	}
	if (elapsed(now, preferredOnlyUntilMs))
	{
		preferredOnlyUntilMs = 0;
		a2dp_sink.set_auto_reconnect(true, kBoundedReconnectAttempts);
		ESP_LOGI("ESPDrive", "Preferred reconnect window expired; allowing bonded-phone fallback");
	}
	if (elapsed(now, keepCarLinkUntilMs))
	{
		keepCarLinkUntilMs = 0;
		if (a2dp_sink.get_connection_state() != ESP_A2D_CONNECTION_STATE_CONNECTED) setEspodDisabled(true);
	}
}

static void serviceCarRecovery(uint32_t now)
{
#if ESPDRIVE_ACTIVE_DCD
	if (carRecovery.state == CarRecoveryState::DcdReleased && elapsed(now, carRecovery.deadlineMs))
	{
		espod.resetState();
		setEspodDisabled(false);
		carRecovery.state = CarRecoveryState::WaitingHandshake;
		carRecovery.deadlineMs = now + kCarRecoveryResultMs;
		ESP_LOGI("CarRecovery", "DCD reasserted; waiting for handshake");
	}
#endif
	if (carRecovery.state == CarRecoveryState::WaitingHandshake &&
		(espod.extendedInterfaceModeActive || elapsed(now, carRecovery.deadlineMs)))
	{
		showEspDriveOverlay(espod.extendedInterfaceModeActive ? "Car reconnected" : "Car handshake pending",
			"ESPDrive", espod.extendedInterfaceModeActive ? "Ready" : "Check USB/head unit");
		carRecovery.state = CarRecoveryState::Idle;
	}
}

static void serviceAutoplay(uint32_t now)
{
	const bool phoneConnected = a2dp_sink.get_connection_state() == ESP_A2D_CONNECTION_STATE_CONNECTED;
	if (autoplay.waitingForAudio && autoplay.mode == AutoplayMode::Reliable && avrcConnected &&
		phoneConnected && elapsed(now, autoplay.nextAttemptAtMs))
	{
		if (autoplay.retriesSent >= kReliableAutoplayMaxRetries)
		{
			autoplay.waitingForAudio = false;
			return;
		}
		a2dp_sink.play();
		++autoplay.retriesSent;
		autoplay.nextAttemptAtMs = now + kReliableAutoplayRetryMs;
	}

	if (autoplay.mode == AutoplayMode::Immediate && avrcConnected &&
		autoplay.retriesSent == 0 && phoneConnected)
	{
		a2dp_sink.play();
		autoplay.retriesSent = 1;
	}
}

static void serviceStateMachines()
{
	const uint32_t now = millis();
	serviceBluetoothEvents(now);
	serviceConnectionTimers(now);
	serviceCarRecovery(now);
	updateOutputActivation(now);
	serviceAutoplay(now);
}
void setup()
{
#ifdef LED_BUILTIN
	pinMode(LED_BUILTIN, OUTPUT);
	digitalWrite(LED_BUILTIN, INVERT_LED_LOGIC(LOW));
#endif
#if ESPDRIVE_ACTIVE_DCD
	pinMode(DCD_CTRL_PIN, OUTPUT);
	digitalWrite(DCD_CTRL_PIN, INVERT_DCD_LOGIC(HIGH));
#endif
	ESP_LOGI("ESPDrive", "Build %s %s", VERSION_BRANCH, VERSION_STRING);
	loadPreferences();
	preferredOnlyUntilMs = preferredPhone.valid ? millis() + kPreferredReconnectWindowMs : 0;
	initializeWorkerTasks();
	initializeA2DPSink();
	espod.attachPlayControlHandler(playStatusHandler);
	espod.attachDatabaseHandlers(espDriveDatabaseCount, espDriveDatabaseRecord, espDriveDatabaseSelected);
	espod.attachVirtualTrackSelectionHandler(absorbVirtualTrackSelection);
}
void loop() { serviceStateMachines(); vTaskDelay(pdMS_TO_TICKS(10)); }
