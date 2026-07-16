#include <Arduino.h>
#include <Preferences.h>
#include "esp_gap_bt_api.h"
#include "AudioTools.h"
#include "BluetoothA2DPSink.h"
#include "esPod.h"
#include "EspDriveMenu.h"

#pragma region Board IO Macros
// LED Logic inversion
#ifndef INVERT_LED_LOGIC
#define INVERT_LED_LOGIC(stateBoolean) stateBoolean
#else
#undef INVERT_LED_LOGIC
#define INVERT_LED_LOGIC(stateBoolean) !stateBoolean
#endif

// DCD Logic inversion
#ifndef INVERT_DCD_LOGIC
#define INVERT_DCD_LOGIC(stateBoolean) stateBoolean
#else
#undef INVERT_DCD_LOGIC
#define INVERT_DCD_LOGIC(stateBoolean) !stateBoolean
#endif

// DCD control pin to pretend there is a physical disconnect
#if defined(ENABLE_ACTIVE_DCD) && !defined(DCD_CTRL_PIN)
#define DCD_CTRL_PIN 5
#endif
#pragma endregion

#pragma region AVRC-related FreeRTOS tasks defines
#ifndef AVRC_METADATA_TEXT_MAX_LENGTH
#define AVRC_METADATA_TEXT_MAX_LENGTH 256
#endif
#ifndef BT_PEER_NAME_MAX_LENGTH
#define BT_PEER_NAME_MAX_LENGTH 64
#endif
#ifndef PROCESS_AVRC_TASK_STACK_SIZE
#define PROCESS_AVRC_TASK_STACK_SIZE 4096
#endif
#ifndef PROCESS_AVRC_TASK_PRIORITY
#define PROCESS_AVRC_TASK_PRIORITY 6
#endif
#pragma endregion

#pragma region A2DP Sink Configuration
// A2DP instance name
#ifndef A2DP_SINK_NAME
#define A2DP_SINK_NAME "espiPod"
#endif
#ifndef BT_CONNECTED_MESSAGE_DURATION_MS
#define BT_CONNECTED_MESSAGE_DURATION_MS 15000UL
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

#ifdef AUDIOKIT // Using the AiThink A1S AudioKit chip
#include "AudioBoard.h"
#include "AudioTools/AudioLibs/I2SCodecStream.h"
AudioInfo info(44100, 2, 16);
DriverPins minimalPins;
AudioBoard minimalAudioKit(AudioDriverES8388, minimalPins);
I2SCodecStream i2s(minimalAudioKit);
BluetoothA2DPSink a2dp_sink(i2s);
#else // Case not using the audiokit, like Sandwich Carrier Board
I2SStream i2s;
BluetoothA2DPSink a2dp_sink;
#endif

/// @brief Data stream reader callback
/// @param data Data buffer to pass to the I2S
/// @param length Length of the data buffer
void read_data_stream(const uint8_t *data, uint32_t length)
{
	i2s.write(data, length);
}

#pragma endregion

#pragma region Helper Functions declaration
void initializeA2DPSink();
esp_err_t initializeAVRCTask();
esp_err_t initializeEspDriveActionTask();
#pragma endregion

#pragma region A2DP/AVRC callbacks declaration
void connectionStateChanged(esp_a2d_connection_state_t state, void *ptr);
void audioStateChanged(esp_a2d_audio_state_t state, void *ptr);
void avrc_rn_play_pos_callback(uint32_t play_pos);
void avrc_metadata_callback(uint8_t id, const uint8_t *text);
void playStatusHandler(PB_COMMAND playCommand);
void showEspDriveOverlay(const char *title, const char *artist, const char *album, uint32_t durationMs = 4000);
uint32_t espDriveDatabaseCount(DB_CATEGORY category);
bool espDriveDatabaseRecord(DB_CATEGORY category, uint32_t recordId, char *recordName, size_t recordNameSize);
void espDriveDatabaseSelected(DB_CATEGORY category, uint32_t recordId);
#pragma endregion

// Declare the principal object... multiple syntaxes are available
// Autobaud syntax example
// esPod espod(1,UART1_RX,UART1_TX,0);
esPod espod(1, UART1_RX, UART1_TX, 19200);
bool pendingPlayReq = false; // Might use this to make sure play requests are not ignored.
Preferences espDrivePreferences;
bool autoplayEnabled = true;
bool playRequestedForConnection = false;
uint32_t pairingExpiresAtMs = 0;
uint32_t reconnectSuppressedUntilMs = 0;
uint32_t clearPairingsConfirmUntilMs = 0;
uint32_t restartConfirmUntilMs = 0;

struct MetadataOverlay
{
	char title[AVRC_METADATA_TEXT_MAX_LENGTH] = {};
	char artist[AVRC_METADATA_TEXT_MAX_LENGTH] = {};
	char album[AVRC_METADATA_TEXT_MAX_LENGTH] = {};
	uint32_t expiresAtMs = 0;
	bool active = false;
};

MetadataOverlay activeOverlay;
portMUX_TYPE overlayMux = portMUX_INITIALIZER_UNLOCKED;

struct EspDriveActionRequest
{
	EspDriveActionId action;
	uint32_t recordId;
	uint32_t timestampMs;
};

QueueHandle_t espDriveActionQueue;
TaskHandle_t espDriveActionTaskHandle;

void setup()
{

#ifdef LED_BUILTIN
	pinMode(LED_BUILTIN, OUTPUT);
	digitalWrite(LED_BUILTIN, INVERT_LED_LOGIC(LOW));
#endif

#ifdef ENABLE_ACTIVE_DCD
	pinMode(DCD_CTRL_PIN, OUTPUT);
	digitalWrite(DCD_CTRL_PIN, INVERT_DCD_LOGIC(HIGH)); // Logic is inverted
#endif

	ESP_LOGI(__func__, "setup() start");

	// Inform of possible errors that led to a reset
	ESP_LOGI("RESET", "Reset reason: %d", esp_reset_reason());

	// Publish build information
	ESP_LOGI("BUILD_INFO", "env:%s\t date: %s\t time: %s", PIOENV, __DATE__, __TIME__);
	ESP_LOGI("VERSION", "%s", VERSION_STRING);
	ESP_LOGI("BRANCH", "%s", VERSION_BRANCH);

	// Start AVRC Notifications handler
	if (initializeAVRCTask() != ESP_OK)
		esp_restart();
	if (initializeEspDriveActionTask() != ESP_OK)
		esp_restart();
	autoplayEnabled = espDrivePreferences.begin("espdrive", false) && espDrivePreferences.getBool("autoplay", true);
	EspDriveMenu::setAutoplayEnabled(autoplayEnabled);
	ESP_LOGI("ESPDriveAction", "Autoplay loaded: %s", autoplayEnabled ? "ON" : "OFF");

	// Start the A2DP Sink
	initializeA2DPSink();

	espod.attachPlayControlHandler(playStatusHandler);
	espod.attachDatabaseHandlers(espDriveDatabaseCount, espDriveDatabaseRecord, espDriveDatabaseSelected);
	ESP_LOGI(__func__, "Waiting for peer");
	while (a2dp_sink.get_connection_state() != ESP_A2D_CONNECTION_STATE_CONNECTED)
	{
		delay(10);
	}
	delay(50);
	ESP_LOGI(__func__, "Peer connected: %s", a2dp_sink.get_peer_name());
	ESP_LOGI(__func__, "Setup finished");
}

void loop()
{
	const uint32_t now = millis();
	if (pairingExpiresAtMs != 0 && (int32_t)(now - pairingExpiresAtMs) >= 0)
	{
		pairingExpiresAtMs = 0;
		a2dp_sink.set_auto_reconnect(true);
		showEspDriveOverlay("Pairing timed out", "ESPDrive", "Try again");
	}
	if (reconnectSuppressedUntilMs != 0 && (int32_t)(now - reconnectSuppressedUntilMs) >= 0)
	{
		reconnectSuppressedUntilMs = 0;
		a2dp_sink.set_auto_reconnect(true);
		ESP_LOGI("ESPDriveAction", "Auto-reconnect restored");
	}
	vTaskDelay(1); // Purely out of precaution
}

#pragma region AVRC Task and Mailbox declaration/definition
/// @brief Latest values received from AVRCP. Each attribute is coalesced so
/// display updates always use the newest available value.
struct avrcMetadataMailbox
{
	bool albumPending = false;
	bool artistPending = false;
	bool titlePending = false;
	bool playingTimePending = false;
	char album[AVRC_METADATA_TEXT_MAX_LENGTH] = {};
	char artist[AVRC_METADATA_TEXT_MAX_LENGTH] = {};
	char title[AVRC_METADATA_TEXT_MAX_LENGTH] = {};
	char playingTime[AVRC_METADATA_TEXT_MAX_LENGTH] = {};
};

enum class btConnectionEvent : uint8_t
{
	None,
	Connected,
	Disconnected
};

struct btConnectionMailbox
{
	btConnectionEvent event = btConnectionEvent::None;
	char peerName[BT_PEER_NAME_MAX_LENGTH] = {};
};

avrcMetadataMailbox pendingMetadata;
btConnectionMailbox pendingConnection;
portMUX_TYPE metadataMailboxMux = portMUX_INITIALIZER_UNLOCKED;
TaskHandle_t processAVRCTaskHandle;

/// @brief Low priority task to process coalesced metadata updates.
/// @param pvParameters
static void processAVRCTask(void *pvParameters)
{
	char deferredTrackTitle[AVRC_METADATA_TEXT_MAX_LENGTH] = {};
	bool hasDeferredTrackTitle = false;
	uint32_t welcomeMessageStartedAt = 0;
	bool welcomeMessageActive = false;

#ifdef STACK_HIGH_WATERMARK_LOG
	UBaseType_t uxHighWaterMark;
	UBaseType_t minHightWaterMark = PROCESS_AVRC_TASK_STACK_SIZE;
#endif

	// Main loop
	while (true)
	{
		const uint32_t welcomeMessageElapsed = (uint32_t)(millis() - welcomeMessageStartedAt);
		const TickType_t receiveTimeout = welcomeMessageActive &&
			welcomeMessageElapsed < BT_CONNECTED_MESSAGE_DURATION_MS
			? pdMS_TO_TICKS(BT_CONNECTED_MESSAGE_DURATION_MS - welcomeMessageElapsed)
			: portMAX_DELAY;
// Stack high watermark logging
#ifdef STACK_HIGH_WATERMARK_LOG
		uxHighWaterMark = uxTaskGetStackHighWaterMark(NULL);
		if (uxHighWaterMark < minHightWaterMark)
		{
			minHightWaterMark = uxHighWaterMark;
			ESP_LOGI("HWM", "Process AVRC Task High Watermark: %d, used stack: %d", minHightWaterMark,
					 PROCESS_AVRC_TASK_STACK_SIZE - minHightWaterMark);
		}
#endif
		// Wait for coalesced metadata, a connection event, or welcome-message expiry.
		ulTaskNotifyTake(pdTRUE, receiveTimeout);

		avrcMetadataMailbox metadata;
		btConnectionMailbox connection;
		portENTER_CRITICAL(&metadataMailboxMux);
		metadata = pendingMetadata;
		pendingMetadata.albumPending = false;
		pendingMetadata.artistPending = false;
		pendingMetadata.titlePending = false;
		pendingMetadata.playingTimePending = false;
		connection = pendingConnection;
		pendingConnection.event = btConnectionEvent::None;
		portEXIT_CRITICAL(&metadataMailboxMux);

		if (connection.event == btConnectionEvent::Connected)
		{
			welcomeMessageStartedAt = millis();
			welcomeMessageActive = true;
			hasDeferredTrackTitle = false;
			char welcomeMessage[AVRC_METADATA_TEXT_MAX_LENGTH];
			snprintf(welcomeMessage, sizeof(welcomeMessage), "Welcome! Connected to: %s", connection.peerName);
			espod.updateTrackTitle(welcomeMessage);
		}
		else if (connection.event == btConnectionEvent::Disconnected)
		{
			welcomeMessageActive = false;
			hasDeferredTrackTitle = false;
		}

		if (connection.event != btConnectionEvent::Disconnected && metadata.albumPending)
			espod.updateAlbumName(metadata.album);
		if (connection.event != btConnectionEvent::Disconnected && metadata.artistPending)
			espod.updateArtistName(metadata.artist);
		if (connection.event != btConnectionEvent::Disconnected && metadata.playingTimePending)
			espod.updateTrackDuration(atoi(metadata.playingTime));
		if (connection.event != btConnectionEvent::Disconnected && metadata.titlePending)
		{
			if (welcomeMessageActive &&
				(uint32_t)(millis() - welcomeMessageStartedAt) < BT_CONNECTED_MESSAGE_DURATION_MS)
			{
				snprintf(deferredTrackTitle, sizeof(deferredTrackTitle), "%s", metadata.title);
				hasDeferredTrackTitle = true;
			}
			else
			{
				espod.updateTrackTitle(metadata.title);
			}
		}

		if (welcomeMessageActive &&
			(uint32_t)(millis() - welcomeMessageStartedAt) >= BT_CONNECTED_MESSAGE_DURATION_MS)
		{
			welcomeMessageActive = false;
			if (hasDeferredTrackTitle)
			{
				espod.updateTrackTitle(deferredTrackTitle);
				hasDeferredTrackTitle = false;
			}
		}
	}
}
#pragma endregion

#pragma region Helper Function Definitions

static const char *espDriveActionName(EspDriveActionId action)
{
	switch (action)
	{
	case EspDriveActionId::ShowStatus: return "ShowStatus";
	case EspDriveActionId::StartPairing: return "StartPairing";
	case EspDriveActionId::ReconnectLast: return "ReconnectLast";
	case EspDriveActionId::DisconnectCurrent: return "DisconnectCurrent";
	case EspDriveActionId::ToggleAutoplay: return "ToggleAutoplay";
	case EspDriveActionId::RequestClearPairings: return "RequestClearPairings";
	case EspDriveActionId::RequestRestart: return "RequestRestart";
	default: return "None";
	}
}

void showEspDriveOverlay(const char *title, const char *artist, const char *album, uint32_t durationMs)
{
	portENTER_CRITICAL(&overlayMux);
	snprintf(activeOverlay.title, sizeof(activeOverlay.title), "%s", title);
	snprintf(activeOverlay.artist, sizeof(activeOverlay.artist), "%s", artist);
	snprintf(activeOverlay.album, sizeof(activeOverlay.album), "%s", album);
	activeOverlay.expiresAtMs = millis() + durationMs;
	activeOverlay.active = true;
	portEXIT_CRITICAL(&overlayMux);
	espod.updateTrackTitle(title);
	espod.updateArtistName(artist);
	espod.updateAlbumName(album);
	ESP_LOGI("ESPDriveOverlay", "Showing %s for %lu ms", title, (unsigned long)durationMs);
}

static void processEspDriveActions(void *pvParameters)
{
	EspDriveActionRequest request;
	while (true)
	{
		if (xQueueReceive(espDriveActionQueue, &request, portMAX_DELAY) == pdTRUE)
		{
			ESP_LOGI("ESPDriveAction", "Action started: record=%lu action=%s", (unsigned long)request.recordId,
					 espDriveActionName(request.action));
			switch (request.action)
			{
			case EspDriveActionId::ShowStatus:
				if (a2dp_sink.get_connection_state() == ESP_A2D_CONNECTION_STATE_CONNECTED)
					showEspDriveOverlay("Connected", a2dp_sink.get_peer_name() != nullptr ? a2dp_sink.get_peer_name() : "Known phone", espod.playStatus == PB_STATE_PLAYING ? "Audio playing" : "Audio paused");
				else
					showEspDriveOverlay("No phone connected", "Bluetooth ready", "Select Pair new phone");
				break;
			case EspDriveActionId::ToggleAutoplay:
				autoplayEnabled = !autoplayEnabled;
				if (espDrivePreferences.putBool("autoplay", autoplayEnabled) == 0)
					showEspDriveOverlay("Autoplay save failed", "ESPDrive", "Try again");
				else
				{
					EspDriveMenu::setAutoplayEnabled(autoplayEnabled);
					showEspDriveOverlay(autoplayEnabled ? "Autoplay enabled" : "Autoplay disabled", "ESPDrive", "Saved");
				}
				break;
			case EspDriveActionId::DisconnectCurrent:
				if (a2dp_sink.get_connection_state() != ESP_A2D_CONNECTION_STATE_CONNECTED)
					showEspDriveOverlay("No phone connected", "ESPDrive", "Nothing to disconnect");
				else
				{
					reconnectSuppressedUntilMs = millis() + 30000UL;
					a2dp_sink.set_auto_reconnect(false);
					a2dp_sink.disconnect();
					showEspDriveOverlay("Phone disconnected", "ESPDrive", "Bluetooth ready");
				}
				break;
			case EspDriveActionId::ReconnectLast:
				if (a2dp_sink.get_connection_state() == ESP_A2D_CONNECTION_STATE_CONNECTED)
					showEspDriveOverlay("Connected", a2dp_sink.get_peer_name() != nullptr ? a2dp_sink.get_peer_name() : "Known phone", "Already connected");
				else
				{
					a2dp_sink.set_auto_reconnect(true, 1);
					showEspDriveOverlay("Reconnecting", "ESPDrive", "Last known phone");
				}
				break;
			case EspDriveActionId::StartPairing:
				a2dp_sink.set_auto_reconnect(false);
				if (a2dp_sink.get_connection_state() == ESP_A2D_CONNECTION_STATE_CONNECTED)
					a2dp_sink.disconnect();
				esp_bt_gap_set_scan_mode(ESP_BT_CONNECTABLE, ESP_BT_GENERAL_DISCOVERABLE);
				pairingExpiresAtMs = millis() + 90000UL;
				showEspDriveOverlay("Pairing mode", "ESPDrive", "Open phone Bluetooth");
				break;
			case EspDriveActionId::RequestClearPairings:
				if ((int32_t)(millis() - clearPairingsConfirmUntilMs) <= 0)
				{
					int deviceCount = esp_bt_gap_get_bond_device_num();
					if (deviceCount > 0)
					{
						esp_bd_addr_t bondedDevices[10];
						int count = min(deviceCount, 10);
						if (esp_bt_gap_get_bond_device_list(&count, bondedDevices) == ESP_OK)
							for (int i = 0; i < count; ++i) esp_bt_gap_remove_bond_device(bondedDevices[i]);
					}
					showEspDriveOverlay("Pairings cleared", "ESPDrive", "Pair a new phone");
					clearPairingsConfirmUntilMs = 0;
				}
				else
				{
					clearPairingsConfirmUntilMs = millis() + 10000UL;
					showEspDriveOverlay("Select again to confirm", "Clear paired phones", "Expires in 10 seconds");
				}
				break;
			case EspDriveActionId::RequestRestart:
				if ((int32_t)(millis() - restartConfirmUntilMs) <= 0)
				{
					showEspDriveOverlay("Restarting", "ESPDrive", "Please wait", 1000);
					vTaskDelay(pdMS_TO_TICKS(1000));
					esp_restart();
				}
				else
				{
					restartConfirmUntilMs = millis() + 10000UL;
					showEspDriveOverlay("Select again to confirm", "Restart ESPDrive", "Expires in 10 seconds");
				}
				break;
			default:
				showEspDriveOverlay("Not available", "ESPDrive", "Coming soon");
				break;
			}
		}
	}
}

esp_err_t initializeEspDriveActionTask()
{
	espDriveActionQueue = xQueueCreate(8, sizeof(EspDriveActionRequest));
	if (espDriveActionQueue == nullptr)
		return ESP_FAIL;
	xTaskCreatePinnedToCore(processEspDriveActions, "espDriveAction", 3072, NULL, 4,
							&espDriveActionTaskHandle, ARDUINO_RUNNING_CORE);
	return espDriveActionTaskHandle == nullptr ? ESP_FAIL : ESP_OK;
}

uint32_t espDriveDatabaseCount(DB_CATEGORY category)
{
	return EspDriveMenu::recordCount(category);
}

bool espDriveDatabaseRecord(DB_CATEGORY category, uint32_t recordId, char *recordName, size_t recordNameSize)
{
	const bool found = EspDriveMenu::recordName(category, recordId, recordName, recordNameSize);
	if (found)
		ESP_LOGI("ESPDriveMenu", "Requested record=%lu name=%s", (unsigned long)recordId, recordName);
	return found;
}

void espDriveDatabaseSelected(DB_CATEGORY category, uint32_t recordId)
{
	const EspDriveActionId action = EspDriveMenu::actionForRecord(category, recordId);
	ESP_LOGI("ESPDriveMenu", "Selected record=%lu action=%s", (unsigned long)recordId, espDriveActionName(action));
	if (action == EspDriveActionId::None)
		return;
	const EspDriveActionRequest request = {action, recordId, millis()};
	if (xQueueSend(espDriveActionQueue, &request, 0) != pdTRUE)
		ESP_LOGW("ESPDriveMenu", "Action queue full; discarded record=%lu", (unsigned long)recordId);
	else
		ESP_LOGI("ESPDriveMenu", "Action enqueued: %s", espDriveActionName(action));
}

/// @brief Configures the CODEC or DAC and starts the A2DP Sink
void initializeA2DPSink()
{
#ifdef AUDIOKIT
	minimalPins.addI2C(PinFunction::CODEC, 32, 33);
	minimalPins.addI2S(PinFunction::CODEC, 0, BCLK_PIN, WS_PIN, DIN_PIN, 35);
	// a2dp_sink.set_stream_reader(read_data_stream, false); // Might need commenting out
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

	a2dp_sink.set_auto_reconnect(true, 10000);
	a2dp_sink.set_on_connection_state_changed(connectionStateChanged);
	a2dp_sink.set_on_audio_state_changed(audioStateChanged);
	a2dp_sink.set_avrc_metadata_callback(avrc_metadata_callback);
	a2dp_sink.set_avrc_metadata_attribute_mask(ESP_AVRC_MD_ATTR_TITLE | ESP_AVRC_MD_ATTR_ARTIST |
											   ESP_AVRC_MD_ATTR_ALBUM | ESP_AVRC_MD_ATTR_PLAYING_TIME);
	a2dp_sink.set_avrc_rn_play_pos_callback(avrc_rn_play_pos_callback, 1);

	a2dp_sink.start(A2DP_SINK_NAME);

	ESP_LOGI(__func__, "a2dp_sink started: %s", A2DP_SINK_NAME);
	delay(5);
}

/// @brief Starts the task that processes coalesced AVRCP metadata.
/// @return ESP_FAIL if the task could not be created, ESP_OK otherwise
esp_err_t initializeAVRCTask()
{
	xTaskCreatePinnedToCore(processAVRCTask, "processAVRCTask", PROCESS_AVRC_TASK_STACK_SIZE, NULL,
							PROCESS_AVRC_TASK_PRIORITY, &processAVRCTaskHandle, ARDUINO_RUNNING_CORE);
	if (processAVRCTaskHandle == nullptr)
	{
		ESP_LOGE(__func__, "Failed to create processAVRCTask");
		return ESP_FAIL;
	}

	return ESP_OK;
}
#pragma endregion

#pragma region A2DP/AVRC callbacks Definitions
/// @brief Callback on changes of A2DP connection and AVRCP connection. On
/// disconnect the esPod becomes silent.
/// @param state New state passed by the callback.
/// @param ptr Not used.
void connectionStateChanged(esp_a2d_connection_state_t state, void *ptr)
{
	switch (state)
	{
	case ESP_A2D_CONNECTION_STATE_CONNECTED:
	{
		ESP_LOGD(__func__, "ESP_A2D_CONNECTION_STATE_CONNECTED, espod enabled");
		espod.disabled = false;
		char peerName[BT_PEER_NAME_MAX_LENGTH];
		const char *connectedPeerName = a2dp_sink.get_peer_name();
		snprintf(peerName, sizeof(peerName), "%s", connectedPeerName != nullptr ? connectedPeerName : "Unknown device");
		portENTER_CRITICAL(&metadataMailboxMux);
		pendingConnection.event = btConnectionEvent::Connected;
		memcpy(pendingConnection.peerName, peerName, sizeof(pendingConnection.peerName));
		portEXIT_CRITICAL(&metadataMailboxMux);
		xTaskNotifyGive(processAVRCTaskHandle);
		playRequestedForConnection = false;
		if (autoplayEnabled)
		{
			ESP_LOGI(__func__, "Attempting autoplay request.");
			a2dp_sink.play();
			playRequestedForConnection = true;
		}
#ifdef LED_BUILTIN
		digitalWrite(LED_BUILTIN, INVERT_LED_LOGIC(HIGH));
#endif
		break;
	}
	case ESP_A2D_CONNECTION_STATE_DISCONNECTED:
		ESP_LOGD(__func__, "ESP_A2D_CONNECTION_STATE_DISCONNECTED, espod disabled");
		espod.resetState();
		espod.disabled = true;
		playRequestedForConnection = false;
		portENTER_CRITICAL(&metadataMailboxMux);
		pendingConnection.event = btConnectionEvent::Disconnected;
		portEXIT_CRITICAL(&metadataMailboxMux);
		xTaskNotifyGive(processAVRCTaskHandle);
#ifdef LED_BUILTIN
		digitalWrite(LED_BUILTIN, INVERT_LED_LOGIC(LOW));
#endif
		break;
	}
#ifdef ENABLE_ACTIVE_DCD
	digitalWrite(DCD_CTRL_PIN, INVERT_DCD_LOGIC(espod.disabled)); // Logic inversion by MACRO
#endif
}

/// @brief Callback for the change of playstate after connection. Aligns the
/// state of the esPod to the state of the phone. Play should be called by the
/// espod interaction
/// @param state The A2DP Stream to align to.
/// @param ptr Not used.
void audioStateChanged(esp_a2d_audio_state_t state, void *ptr)
{
	switch (state)
	{
	case ESP_A2D_AUDIO_STATE_STARTED:
		espod.play(true);
		break;
	case ESP_A2D_AUDIO_STATE_REMOTE_SUSPEND:
		espod.pause(true);
		break;
		// case ESP_A2D_AUDIO_STATE_STOPPED:
		//  espod.stop();
		// 	break;
	}
}

/// @brief Play position callback returning the ms spent since start on every
/// interval - normally 1s
/// @param play_pos Playing Position in ms
void avrc_rn_play_pos_callback(uint32_t play_pos)
{
	espod.updatePlayPosition(play_pos);
	ESP_LOGV(__func__, "PlayPosition called");
}

/// @brief Catch callback for the AVRC metadata. There can be duplicates !
/// @param id Metadata attribute ID : ESP_AVRC_MD_ATTR_xxx
/// @param text Text data passed around, sometimes it's a uint32_t disguised as
/// text
void avrc_metadata_callback(uint8_t id, const uint8_t *text)
{
	// Guard checks
	if (text == NULL)
	{
		ESP_LOGW(__func__, "Received empty pointer for ID %d", id);
		return;
	}
	if ((id != ESP_AVRC_MD_ATTR_PLAYING_TIME) && (text[0] == '\0'))
	{
		ESP_LOGW(__func__, "Empty string received for ID %d", id);
		return;
	}

	char payload[AVRC_METADATA_TEXT_MAX_LENGTH];
	snprintf(payload, sizeof(payload), "%s", (const char *)text);

	portENTER_CRITICAL(&metadataMailboxMux);
	switch (id)
	{
	case ESP_AVRC_MD_ATTR_ALBUM:
		pendingMetadata.albumPending = true;
		memcpy(pendingMetadata.album, payload, sizeof(pendingMetadata.album));
		break;
	case ESP_AVRC_MD_ATTR_ARTIST:
		pendingMetadata.artistPending = true;
		memcpy(pendingMetadata.artist, payload, sizeof(pendingMetadata.artist));
		break;
	case ESP_AVRC_MD_ATTR_TITLE:
		pendingMetadata.titlePending = true;
		memcpy(pendingMetadata.title, payload, sizeof(pendingMetadata.title));
		break;
	case ESP_AVRC_MD_ATTR_PLAYING_TIME:
		pendingMetadata.playingTimePending = true;
		memcpy(pendingMetadata.playingTime, payload, sizeof(pendingMetadata.playingTime));
		break;
	}
	portEXIT_CRITICAL(&metadataMailboxMux);
	xTaskNotifyGive(processAVRCTaskHandle);
}

/// @brief Callback function that passes intended playback operations from the
/// esPod to the A2DP player (i.e. the phone)
/// @param playCommand
void playStatusHandler(PB_COMMAND playCommand)
{
	switch (playCommand)
	{
	case PB_CMD_STOP:
		a2dp_sink.stop();
		ESP_LOGD(__func__, "A2DP_STOP");
		break;
	case PB_CMD_PLAY:
		a2dp_sink.play();
		ESP_LOGD(__func__, "A2DP_PLAY");
		break;
	case PB_CMD_PAUSE:
		a2dp_sink.pause();
		ESP_LOGD(__func__, "A2DP_PAUSE");
		break;
	case PB_CMD_PREVIOUS_TRACK:
		a2dp_sink.previous();
		ESP_LOGD(__func__, "A2DP_REWIND");
		break;
	case PB_CMD_NEXT_TRACK:
		a2dp_sink.next();
		ESP_LOGD(__func__, "A2DP_NEXT");
		break;
	case PB_CMD_NEXT:
		a2dp_sink.next();
		ESP_LOGD(__func__, "A2DP_NEXT");
		break;
	case PB_CMD_PREV:
		a2dp_sink.previous();
		ESP_LOGD(__func__, "A2DP_PREV");
		break;
	}
}

#pragma endregion
