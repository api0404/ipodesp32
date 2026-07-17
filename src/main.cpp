#include <Arduino.h>
#include <climits>
#include <Preferences.h>
#include "esp_gap_bt_api.h"
#include "esp_avrc_api.h"
#include "esp_system.h"
#include "esp_heap_caps.h"
#include "AudioTools.h"
#include "BluetoothA2DPSink.h"
#include "esPod.h"
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
#if defined(ENABLE_ACTIVE_DCD) && !defined(DCD_CTRL_PIN)
#define DCD_CTRL_PIN 5
#endif

constexpr size_t kMetadataLength = 256;
constexpr size_t kPeerNameLength = 64;
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
constexpr uint32_t kFadeDurationMs = 180UL;
constexpr uint32_t kStateTaskWakeMs = 100UL;

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
enum class ChannelMode : uint8_t { Normal, Swap, Mono };
enum class GainPreset : uint8_t { Full, Percent85, Percent70 };
enum class OutputActivation : uint8_t { Immediate, AfterCarLink };
enum class CarRecoveryState : uint8_t { Idle, DcdReleased, WaitingHandshake };

struct AudioSettings
{
	volatile ChannelMode channelMode = ChannelMode::Normal;
	volatile GainPreset gain = GainPreset::Full;
	volatile bool startupFade = true;
	volatile OutputActivation activation = OutputActivation::Immediate;
	volatile bool outputEnabled = true;
	volatile uint32_t fadeStartedAtMs = 0;
};

struct AutoplayState
{
	AutoplayMode mode = AutoplayMode::HeadUnit;
	uint8_t retriesSent = 0;
	uint32_t nextAttemptAtMs = 0;
	bool waitingForAudio = false;
	bool avrcReady = false;
};

struct CarLinkRecovery
{
	CarRecoveryState state = CarRecoveryState::Idle;
	uint32_t deadlineMs = 0;
};

struct Overlay
{
	char title[kMetadataLength] = {};
	char artist[kMetadataLength] = {};
	char album[kMetadataLength] = {};
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
	bool titlePending = false;
	bool artistPending = false;
	bool albumPending = false;
	bool genrePending = false;
	bool durationPending = false;
	bool trackNumberPending = false;
	bool totalTracksPending = false;
};

struct ConnectionMailbox
{
	bool connected = false;
	bool changed = false;
	char peerName[kPeerNameLength] = {};
};

struct PhoneIdentity
{
	uint8_t address[6] = {};
	bool valid = false;
};

esPod espod(1, UART1_RX, UART1_TX, 19200);
Preferences espDrivePreferences;
AudioSettings audioSettings;
AutoplayState autoplay;
CarLinkRecovery carRecovery;
Overlay overlay;
RealMetadata metadata;
ConnectionMailbox connectionMailbox;
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
volatile bool disconnectResetPending = false;
uint32_t pairingExpiresAtMs = 0;
uint32_t reconnectSuppressedUntilMs = 0;
uint32_t keepCarLinkUntilMs = 0;
uint32_t clearPairingsConfirmUntilMs = 0;
uint32_t forgetPhoneConfirmUntilMs = 0;
uint32_t restartConfirmUntilMs = 0;
uint32_t outputCarLinkDeadlineMs = 0;
uint32_t preferredOnlyUntilMs = 0;
uint8_t avrcTransactionLabel = 0;

struct EspDriveActionRequest { EspDriveActionId action; uint32_t recordId; };

static bool elapsed(uint32_t now, uint32_t deadline) { return deadline != 0 && (int32_t)(now - deadline) >= 0; }
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
static uint8_t gainPercent(GainPreset preset) { return preset == GainPreset::Full ? 100 : (preset == GainPreset::Percent85 ? 85 : 70); }
static int16_t saturate16(int32_t value) { return value > INT16_MAX ? INT16_MAX : (value < INT16_MIN ? INT16_MIN : static_cast<int16_t>(value)); }
static void updateOutputActivation()
{
	if (audioSettings.activation == OutputActivation::Immediate || espod.extendedInterfaceModeActive || elapsed(millis(), outputCarLinkDeadlineMs))
		audioSettings.outputEnabled = true;
}

void read_data_stream(const uint8_t *data, uint32_t length)
{
	if (!audioSettings.outputEnabled || data == nullptr || length < sizeof(int16_t) * 2) return;
	const uint32_t frames = length / (sizeof(int16_t) * 2);
	const uint8_t percent = gainPercent(audioSettings.gain);
	const uint32_t fadeElapsed = millis() - audioSettings.fadeStartedAtMs;
	const uint8_t fadePercent = !audioSettings.startupFade || fadeElapsed >= kFadeDurationMs ? 100 : static_cast<uint8_t>((fadeElapsed * 100UL) / kFadeDurationMs);
	constexpr uint32_t kPcmFramesPerChunk = 64;
	int16_t output[kPcmFramesPerChunk * 2];
	for (uint32_t offset = 0; offset < frames;)
	{
		const uint32_t chunkFrames = min(kPcmFramesPerChunk, frames - offset);
		for (uint32_t frame = 0; frame < chunkFrames; ++frame)
		{
			const int16_t *input = reinterpret_cast<const int16_t *>(data) + (offset + frame) * 2;
			int32_t left = input[0];
			int32_t right = input[1];
			if (audioSettings.channelMode == ChannelMode::Swap) { const int32_t tmp = left; left = right; right = tmp; }
			if (audioSettings.channelMode == ChannelMode::Mono) { const int32_t mono = (left + right) / 2; left = mono; right = mono; }
			output[frame * 2] = saturate16((left * percent * fadePercent) / 10000);
			output[frame * 2 + 1] = saturate16((right * percent * fadePercent) / 10000);
		}
		i2s.write(reinterpret_cast<const uint8_t *>(output), chunkFrames * sizeof(int16_t) * 2);
		offset += chunkFrames;
	}
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
	copyText(overlay.title, sizeof(overlay.title), title);
	copyText(overlay.artist, sizeof(overlay.artist), artist);
	copyText(overlay.album, sizeof(overlay.album), album);
	overlay.expiresAtMs = millis() + durationMs;
	overlay.active = true;
	portEXIT_CRITICAL(&stateMux);
	espod.updateTrackTitle(title);
	espod.updateArtistName(artist);
	espod.updateAlbumName(album);
}

void avrc_metadata_callback(uint8_t id, const uint8_t *text)
{
	portENTER_CRITICAL(&stateMux);
	const char *value = text == nullptr ? "" : reinterpret_cast<const char *>(text);
	switch (id)
	{
	case ESP_AVRC_MD_ATTR_TITLE: copyText(metadata.title, sizeof(metadata.title), value[0] ? value : "Unknown title"); metadata.titlePending = true; break;
	case ESP_AVRC_MD_ATTR_ARTIST: copyText(metadata.artist, sizeof(metadata.artist), value[0] ? value : "Unknown artist"); metadata.artistPending = true; break;
	case ESP_AVRC_MD_ATTR_ALBUM: copyText(metadata.album, sizeof(metadata.album), value[0] ? value : "Unknown album"); metadata.albumPending = true; break;
	case ESP_AVRC_MD_ATTR_GENRE: copyText(metadata.genre, sizeof(metadata.genre), value); metadata.genrePending = true; break;
	case ESP_AVRC_MD_ATTR_PLAYING_TIME: copyText(metadata.duration, sizeof(metadata.duration), value); metadata.durationPending = true; break;
	case ESP_AVRC_MD_ATTR_TRACK_NUM: copyText(metadata.trackNumber, sizeof(metadata.trackNumber), value); metadata.trackNumberPending = true; break;
	case ESP_AVRC_MD_ATTR_NUM_TRACKS: copyText(metadata.totalTracks, sizeof(metadata.totalTracks), value); metadata.totalTracksPending = true; break;
	default: portEXIT_CRITICAL(&stateMux); ESP_LOGW("AVRCP", "Unhandled metadata attribute %u", id); return;
	}
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
		changed = metadata.titlePending || metadata.artistPending || metadata.albumPending || metadata.durationPending || metadata.genrePending || metadata.trackNumberPending || metadata.totalTracksPending;
		metadata.titlePending = metadata.artistPending = metadata.albumPending = metadata.durationPending = false;
		metadata.genrePending = metadata.trackNumberPending = metadata.totalTracksPending = false;
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
	const bool connected = state == ESP_A2D_CONNECTION_STATE_CONNECTED;
	portENTER_CRITICAL(&stateMux);
	connectionMailbox.changed = true;
	connectionMailbox.connected = connected;
	if (connected) copyText(connectionMailbox.peerName, sizeof(connectionMailbox.peerName), a2dp_sink.get_peer_name());
	portEXIT_CRITICAL(&stateMux);
	if (connected)
	{
		espod.disabled = false;
		audioSettings.fadeStartedAtMs = millis();
		outputCarLinkDeadlineMs = millis() + kOutputCarLinkTimeoutMs;
		audioSettings.outputEnabled = audioSettings.activation == OutputActivation::Immediate;
		autoplay.retriesSent = 0;
		autoplay.waitingForAudio = autoplay.mode == AutoplayMode::Reliable;
		autoplay.nextAttemptAtMs = millis() + kReliableAutoplayInitialMs;
	}
	else
	{
		autoplay.waitingForAudio = false;
		disconnectResetPending = true;
		espod.disabled = !elapsed(millis(), keepCarLinkUntilMs);
	}
#ifdef LED_BUILTIN
	digitalWrite(LED_BUILTIN, INVERT_LED_LOGIC(connected));
#endif
#ifdef ENABLE_ACTIVE_DCD
	digitalWrite(DCD_CTRL_PIN, INVERT_DCD_LOGIC(espod.disabled));
#endif
}
void audioStateChanged(esp_a2d_audio_state_t state, void *)
{
	latestAudioState = state;
	if (state == ESP_A2D_AUDIO_STATE_STARTED) { espod.play(true); autoplay.waitingForAudio = false; }
	else if (state == ESP_A2D_AUDIO_STATE_REMOTE_SUSPEND) espod.pause(true);
}
void avrc_rn_play_pos_callback(uint32_t position) { espod.updatePlayPosition(position); }
void avrcConnectionChanged(bool connected) { autoplay.avrcReady = connected; avrcConnected = connected; }
void sampleRateChanged(uint16_t sampleRate) { negotiatedSampleRate = sampleRate; }
void rssiChanged(esp_bt_gap_cb_param_t::read_rssi_delta_param &rssi) { latestRssi = rssi.rssi; latestRssiAvailable = true; }

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
	const uint8_t *address = a2dp_sink.get_current_peer_address();
	if (address != nullptr) { memcpy(currentPhone.address, address, sizeof(currentPhone.address)); currentPhone.valid = true; }
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
static void fillMenuRuntime(EspDriveMenuRuntime &runtime, char storage[][96])
{
	snprintf(storage[0], 96, "%s", autoplayName(autoplay.mode)); runtime.autoplayMode = storage[0];
	formatAddress(preferredPhone, storage[1], 96); runtime.preferredPhone = storage[1];
	runtime.channelMode = channelName(audioSettings.channelMode); runtime.gainPreset = gainName(audioSettings.gain);
	runtime.startupFade = audioSettings.startupFade ? "On" : "Off"; runtime.outputActivation = outputActivationName(audioSettings.activation);
	const char *peer = a2dp_sink.get_peer_name(); snprintf(storage[2], 96, "%s", peer != nullptr ? peer : "No phone"); runtime.connectedPhone = storage[2];
	runtime.bondedDeviceCount = bondCount(); runtime.a2dpState = a2dpStateName(a2dp_sink.get_connection_state()); runtime.avrcState = avrcConnected ? "Connected" : "Waiting"; runtime.audioState = audioStateName(latestAudioState);
	snprintf(storage[3], 96, "%ddBm", latestRssi); runtime.rssi = latestRssiAvailable ? storage[3] : "Unavailable";
	snprintf(storage[4], 96, "%u Hz, %u ch", negotiatedSampleRate, negotiatedChannels); runtime.audioFormat = negotiatedSampleRate ? storage[4] : "Unavailable";
	runtime.carLink = espod.extendedInterfaceModeActive ? "Extended active" : (espod.disabled ? "Disabled" : "Ready");
	snprintf(storage[5], 96, "%lus", static_cast<unsigned long>(millis() / 1000UL)); runtime.uptime = storage[5];
	snprintf(storage[6], 96, "%lu / %lu", static_cast<unsigned long>(ESP.getFreeHeap()), static_cast<unsigned long>(heap_caps_get_minimum_free_size(MALLOC_CAP_8BIT))); runtime.heap = storage[6];
	snprintf(storage[7], 96, "%d", esp_reset_reason()); runtime.resetReason = storage[7];
	snprintf(storage[8], 96, "%s %s", VERSION_BRANCH, VERSION_STRING); runtime.build = storage[8];
}
uint32_t espDriveDatabaseCount(DB_CATEGORY category) { return EspDriveMenu::recordCount(category); }
bool espDriveDatabaseRecord(DB_CATEGORY category, uint32_t recordId, char *recordName, size_t recordNameSize)
{
	char storage[9][96] = {}; EspDriveMenuRuntime runtime; fillMenuRuntime(runtime, storage);
	return EspDriveMenu::recordName(category, recordId, runtime, recordName, recordNameSize);
}
void espDriveDatabaseSelected(DB_CATEGORY category, uint32_t recordId)
{
	const EspDriveActionId action = EspDriveMenu::actionForRecord(category, recordId);
	if (action == EspDriveActionId::None) return;
	const EspDriveActionRequest request = {action, recordId};
	if (xQueueSend(espDriveActionQueue, &request, 0) != pdTRUE) ESP_LOGW("ESPDriveMenu", "Action queue full for %lu", static_cast<unsigned long>(recordId));
}
static void showDiagnostic(uint32_t recordId)
{
	char storage[9][96] = {}; EspDriveMenuRuntime runtime; fillMenuRuntime(runtime, storage);
	char label[128] = {}; EspDriveMenu::recordName(DB_CAT_PLAYLIST, recordId, runtime, label, sizeof(label));
	showEspDriveOverlay(label, "ESPDrive diagnostics", "Read only");
}
static void runCarRecovery()
{
	if (carRecovery.state != CarRecoveryState::Idle) { showEspDriveOverlay("Car recovery already running", "ESPDrive", "Please wait"); return; }
	ESP_LOGI("CarRecovery", "Starting car-link recovery");
	espod.resetState();
#ifdef ENABLE_ACTIVE_DCD
	espod.disabled = true;
	digitalWrite(DCD_CTRL_PIN, INVERT_DCD_LOGIC(true));
	carRecovery.state = CarRecoveryState::DcdReleased;
	carRecovery.deadlineMs = millis() + kCarRecoveryDcdMs;
	showEspDriveOverlay("Reconnecting car", "DCD released", "Bluetooth stays connected");
#else
	showEspDriveOverlay("Car state reset", "No active DCD hardware", "Handshake may require USB reconnect");
	ESP_LOGW("CarRecovery", "Active DCD support is unavailable in this build");
#endif
}
static void processAction(const EspDriveActionRequest &request)
{
	ESP_LOGI("ESPDriveAction", "Action %s record=%lu", actionName(request.action), static_cast<unsigned long>(request.recordId));
	switch (request.action)
	{
	case EspDriveActionId::ShowStatus: showEspDriveOverlay(a2dp_sink.get_connection_state() == ESP_A2D_CONNECTION_STATE_CONNECTED ? "Connected" : "No phone connected", a2dp_sink.get_peer_name() ?: "Bluetooth ready", espod.extendedInterfaceModeActive ? "Car link active" : "Car link waiting"); break;
	case EspDriveActionId::StartPairing:
		keepCarLinkUntilMs = millis() + kPairingTimeoutMs; espod.disabled = false; a2dp_sink.set_auto_reconnect(false); if (a2dp_sink.get_connection_state() == ESP_A2D_CONNECTION_STATE_CONNECTED) a2dp_sink.disconnect(); esp_bt_gap_set_scan_mode(ESP_BT_CONNECTABLE, ESP_BT_GENERAL_DISCOVERABLE); pairingExpiresAtMs = millis() + kPairingTimeoutMs; showEspDriveOverlay("Pairing mode", "ESPDrive", "Open phone Bluetooth"); break;
	case EspDriveActionId::ReconnectLast: preferredOnlyUntilMs = preferredPhone.valid ? millis() + kPreferredReconnectWindowMs : 0; a2dp_sink.set_auto_reconnect(true, kBoundedReconnectAttempts); showEspDriveOverlay("Reconnecting", preferredPhone.valid ? "Preferred phone first" : "Last known phone", "Bounded attempts"); break;
	case EspDriveActionId::DisconnectCurrent:
		if (a2dp_sink.get_connection_state() != ESP_A2D_CONNECTION_STATE_CONNECTED) showEspDriveOverlay("No phone connected", "ESPDrive", "Nothing to disconnect"); else { keepCarLinkUntilMs = millis() + kManualDisconnectKeepCarMs; reconnectSuppressedUntilMs = keepCarLinkUntilMs; a2dp_sink.set_auto_reconnect(false); a2dp_sink.disconnect(); showEspDriveOverlay("Phone disconnected", "Car menu remains available", "Bluetooth ready"); } break;
	case EspDriveActionId::ToggleAutoplay:
		autoplay.mode = static_cast<AutoplayMode>((static_cast<uint8_t>(autoplay.mode) + 1U) % 4U); autoplay.waitingForAudio = false; espDrivePreferences.putUChar("autoplayMode", static_cast<uint8_t>(autoplay.mode)); showEspDriveOverlay("Autoplay", autoplayName(autoplay.mode), "Saved"); break;
	case EspDriveActionId::RequestClearPairings:
		if (!elapsed(millis(), clearPairingsConfirmUntilMs)) { esp_bd_addr_t bonds[20]; int count = 20; if (esp_bt_gap_get_bond_device_list(&count, bonds) == ESP_OK) for (int i = 0; i < count; ++i) esp_bt_gap_remove_bond_device(bonds[i]); preferredPhone.valid = false; espDrivePreferences.remove("preferred"); clearPairingsConfirmUntilMs = 0; showEspDriveOverlay("Pairings cleared", "ESPDrive", "Pair a new phone"); } else { clearPairingsConfirmUntilMs = millis() + kConfirmTimeoutMs; showEspDriveOverlay("Select again to confirm", "Clear paired phones", "Expires in 10 seconds"); } break;
	case EspDriveActionId::RequestRestart:
		if (!elapsed(millis(), restartConfirmUntilMs)) { showEspDriveOverlay("Restarting", "ESPDrive", "Please wait", 800); vTaskDelay(pdMS_TO_TICKS(800)); esp_restart(); } else { restartConfirmUntilMs = millis() + kConfirmTimeoutMs; showEspDriveOverlay("Select again to confirm", "Restart ESPDrive", "Expires in 10 seconds"); } break;
	case EspDriveActionId::ReconnectCar: runCarRecovery(); break;
	case EspDriveActionId::RequestForgetCurrent:
		if (!currentPhone.valid) { showEspDriveOverlay("No current phone", "ESPDrive", "Nothing to forget"); break; }
		if (!elapsed(millis(), forgetPhoneConfirmUntilMs)) { esp_bt_gap_remove_bond_device(currentPhone.address); if (preferredPhone.valid && memcmp(preferredPhone.address, currentPhone.address, 6) == 0) { preferredPhone.valid = false; espDrivePreferences.remove("preferred"); } forgetPhoneConfirmUntilMs = 0; showEspDriveOverlay("Phone forgotten", "ESPDrive", "Other pairings kept"); } else { forgetPhoneConfirmUntilMs = millis() + kConfirmTimeoutMs; showEspDriveOverlay("Select again to confirm", "Forget current phone", "Expires in 10 seconds"); } break;
	case EspDriveActionId::SetPreferredPhone:
		if (!currentPhone.valid || !currentPhoneIsBonded()) showEspDriveOverlay("No bonded current phone", "ESPDrive", "Connect a paired phone first"); else { preferredPhone = currentPhone; espDrivePreferences.putBytes("preferred", preferredPhone.address, 6); showEspDriveOverlay("Preferred phone saved", a2dp_sink.get_peer_name() ?: "Current phone", "Used for auto reconnect"); } break;
	case EspDriveActionId::ShowPreferredPhone: { char address[32]; formatAddress(preferredPhone, address, sizeof(address)); showEspDriveOverlay("Preferred phone", address, preferredPhone.valid ? "Address identity" : "None saved"); } break;
	case EspDriveActionId::CycleChannelMode: audioSettings.channelMode = static_cast<ChannelMode>((static_cast<uint8_t>(audioSettings.channelMode) + 1U) % 3U); espDrivePreferences.putUChar("channel", static_cast<uint8_t>(audioSettings.channelMode)); showEspDriveOverlay("Channel mode", channelName(audioSettings.channelMode), "Local PCM processing"); break;
	case EspDriveActionId::CycleGainPreset: audioSettings.gain = static_cast<GainPreset>((static_cast<uint8_t>(audioSettings.gain) + 1U) % 3U); espDrivePreferences.putUChar("gain", static_cast<uint8_t>(audioSettings.gain)); showEspDriveOverlay("Output gain", gainName(audioSettings.gain), "Local only"); break;
	case EspDriveActionId::ToggleStartupFade: audioSettings.startupFade = !audioSettings.startupFade; espDrivePreferences.putBool("fade", audioSettings.startupFade); showEspDriveOverlay("Startup fade", audioSettings.startupFade ? "On" : "Off", "Saved"); break;
	case EspDriveActionId::ToggleOutputActivation: audioSettings.activation = audioSettings.activation == OutputActivation::Immediate ? OutputActivation::AfterCarLink : OutputActivation::Immediate; espDrivePreferences.putUChar("activation", static_cast<uint8_t>(audioSettings.activation)); showEspDriveOverlay("Output activation", outputActivationName(audioSettings.activation), "Saved"); break;
	case EspDriveActionId::ShowDiagnostic: showDiagnostic(request.recordId); break;
	default: showEspDriveOverlay("Not available", "ESPDrive", "Unsupported menu action"); break;
	}
}
static void processEspDriveActions(void *) { EspDriveActionRequest request; for (;;) if (xQueueReceive(espDriveActionQueue, &request, portMAX_DELAY) == pdTRUE) processAction(request); }

static void initializeA2DPSink()
{
#ifdef AUDIOKIT
	minimalPins.addI2C(PinFunction::CODEC, 32, 33); minimalPins.addI2S(PinFunction::CODEC, 0, BCLK_PIN, WS_PIN, DIN_PIN, 35); auto cfg = i2s.defaultConfig(); cfg.copyFrom(info); i2s.begin(cfg);
#else
	a2dp_sink.set_stream_reader(read_data_stream, false); auto cfg = i2s.defaultConfig(TX_MODE); cfg.pin_ws = WS_PIN; cfg.pin_data = DIN_PIN; cfg.pin_bck = BCLK_PIN; cfg.sample_rate = 44100; cfg.i2s_format = I2S_LSB_FORMAT; i2s.begin(cfg);
#endif
	a2dp_sink.set_auto_reconnect(true, kBoundedReconnectAttempts);
	a2dp_sink.set_address_validator(preferredAddressValidator);
	a2dp_sink.set_on_connection_state_changed(connectionStateChanged);
	a2dp_sink.set_on_audio_state_changed(audioStateChanged);
	a2dp_sink.set_avrc_connection_state_callback(avrcConnectionChanged);
	a2dp_sink.set_avrc_metadata_callback(avrc_metadata_callback);
	a2dp_sink.set_avrc_metadata_attribute_mask(ESP_AVRC_MD_ATTR_TITLE | ESP_AVRC_MD_ATTR_ARTIST | ESP_AVRC_MD_ATTR_ALBUM | ESP_AVRC_MD_ATTR_PLAYING_TIME | ESP_AVRC_MD_ATTR_GENRE | ESP_AVRC_MD_ATTR_TRACK_NUM | ESP_AVRC_MD_ATTR_NUM_TRACKS);
	a2dp_sink.set_avrc_rn_play_pos_callback(avrc_rn_play_pos_callback, 1);
	a2dp_sink.set_rssi_callback(rssiChanged); a2dp_sink.set_rssi_active(true);
	a2dp_sink.set_sample_rate_callback(sampleRateChanged);
	a2dp_sink.start(A2DP_SINK_NAME);
}
static void serviceStateMachines()
{
	const uint32_t now = millis();
	if (disconnectResetPending) { disconnectResetPending = false; espod.resetState(); }
	if (pairingExpiresAtMs && elapsed(now, pairingExpiresAtMs)) { pairingExpiresAtMs = 0; a2dp_sink.set_auto_reconnect(true, kBoundedReconnectAttempts); showEspDriveOverlay("Pairing timed out", "ESPDrive", "Try again"); }
	if (reconnectSuppressedUntilMs && elapsed(now, reconnectSuppressedUntilMs)) { reconnectSuppressedUntilMs = 0; a2dp_sink.set_auto_reconnect(true, kBoundedReconnectAttempts); }
	if (preferredOnlyUntilMs && elapsed(now, preferredOnlyUntilMs)) { preferredOnlyUntilMs = 0; a2dp_sink.set_auto_reconnect(true, kBoundedReconnectAttempts); ESP_LOGI("ESPDrive", "Preferred reconnect window expired; allowing bonded-phone fallback"); }
	if (keepCarLinkUntilMs && elapsed(now, keepCarLinkUntilMs)) { keepCarLinkUntilMs = 0; if (a2dp_sink.get_connection_state() != ESP_A2D_CONNECTION_STATE_CONNECTED) espod.disabled = true; }
	if (carRecovery.state == CarRecoveryState::DcdReleased && elapsed(now, carRecovery.deadlineMs)) { espod.resetState(); espod.disabled = false; digitalWrite(DCD_CTRL_PIN, INVERT_DCD_LOGIC(false)); carRecovery.state = CarRecoveryState::WaitingHandshake; carRecovery.deadlineMs = now + kCarRecoveryResultMs; ESP_LOGI("CarRecovery", "DCD reasserted; waiting for handshake"); }
	if (carRecovery.state == CarRecoveryState::WaitingHandshake && (espod.extendedInterfaceModeActive || elapsed(now, carRecovery.deadlineMs))) { showEspDriveOverlay(espod.extendedInterfaceModeActive ? "Car reconnected" : "Car handshake pending", "ESPDrive", espod.extendedInterfaceModeActive ? "Ready" : "Check USB/head unit"); carRecovery.state = CarRecoveryState::Idle; }
	updateOutputActivation();
	if (autoplay.waitingForAudio && autoplay.mode == AutoplayMode::Reliable && autoplay.avrcReady && a2dp_sink.get_connection_state() == ESP_A2D_CONNECTION_STATE_CONNECTED && elapsed(now, autoplay.nextAttemptAtMs)) { if (autoplay.retriesSent < kReliableAutoplayMaxRetries) { a2dp_sink.play(); ++autoplay.retriesSent; autoplay.nextAttemptAtMs = now + kReliableAutoplayRetryMs; } else autoplay.waitingForAudio = false; }
	if (autoplay.mode == AutoplayMode::Immediate && autoplay.avrcReady && autoplay.retriesSent == 0 && a2dp_sink.get_connection_state() == ESP_A2D_CONNECTION_STATE_CONNECTED) { a2dp_sink.play(); autoplay.retriesSent = 1; }
	if (connectionMailbox.changed) { portENTER_CRITICAL(&stateMux); const bool connected = connectionMailbox.connected; connectionMailbox.changed = false; portEXIT_CRITICAL(&stateMux); if (connected) { captureCurrentPhone(); showEspDriveOverlay("Connected", a2dp_sink.get_peer_name() ?: "Phone", "ESPDrive", kWelcomeOverlayMs); } }
}
void setup()
{
#ifdef LED_BUILTIN
	pinMode(LED_BUILTIN, OUTPUT); digitalWrite(LED_BUILTIN, INVERT_LED_LOGIC(LOW));
#endif
#ifdef ENABLE_ACTIVE_DCD
	pinMode(DCD_CTRL_PIN, OUTPUT); digitalWrite(DCD_CTRL_PIN, INVERT_DCD_LOGIC(HIGH));
#endif
	ESP_LOGI("ESPDrive", "Build %s %s", VERSION_BRANCH, VERSION_STRING);
	loadPreferences();
	preferredOnlyUntilMs = preferredPhone.valid ? millis() + kPreferredReconnectWindowMs : 0;
	espDriveActionQueue = xQueueCreate(12, sizeof(EspDriveActionRequest));
	xTaskCreatePinnedToCore(processAVRCTask, "processAVRC", 4096, nullptr, 6, &processAVRCTaskHandle, ARDUINO_RUNNING_CORE);
	xTaskCreatePinnedToCore(processEspDriveActions, "espDriveAction", 4096, nullptr, 4, &espDriveActionTaskHandle, ARDUINO_RUNNING_CORE);
	if (espDriveActionQueue == nullptr || processAVRCTaskHandle == nullptr || espDriveActionTaskHandle == nullptr) esp_restart();
	initializeA2DPSink();
	espod.attachPlayControlHandler(playStatusHandler);
	espod.attachDatabaseHandlers(espDriveDatabaseCount, espDriveDatabaseRecord, espDriveDatabaseSelected);
}
void loop() { serviceStateMachines(); vTaskDelay(pdMS_TO_TICKS(10)); }
