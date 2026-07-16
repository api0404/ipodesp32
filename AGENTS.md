# iPodESP32 contributor notes

## Project map

- `src/main.cpp` owns firmware startup and the Bluetooth/iPod integration. It
  starts the A2DP sink, translates AVRCP callbacks into iPod metadata and
  playback updates, and runs the ESPDrive menu action worker.
- `src/EspDriveMenu.cpp` defines the virtual iPod playlist used as the
  ESPDrive settings menu. Keep record IDs stable: the head unit requests them
  by index.
- `include/` contains public, project-local headers.
- `lib/espod` is a Git submodule that implements the iPod Accessory Protocol
  emulator. Initialise it before building or changing its APIs:
  `git submodule update --init --recursive`.
- `platformio.ini` contains the sole supported firmware environment,
  `SBC_NodeMCU32S`. Historical AudioKit and AiO-DAC targets are retained in
  `platformio.archived.ini` and should stay outside normal builds and CI.
  Custom board definitions are in `hardware/boards/`.
- `tools/isnoop/` is a separate PlatformIO project for observing iPod serial
  traffic; it is not part of the main firmware build.
- `hardware/` and `docs/reference/` are hardware/reference artifacts. Avoid
  changing generated PCB, BOM, PDF, or binary files unless the task requires
  it.

## Build and validation

Run commands from the repository root:

```powershell
pio run -e SBC_NodeMCU32S
```

CI builds exactly that environment. Do not reactivate or routinely validate
the archived AudioKit/AiO-DAC targets unless the user explicitly changes the
supported hardware. `version.py` is a pre-build script that embeds the current
Git branch, revision, tag, and dirty state into the firmware.

## Firmware constraints

- This targets original ESP32 variants with Bluetooth Classic/EDR. Do not
  switch a target to an ESP32-S3/C-series board without re-evaluating the
  Bluetooth design.
- Preserve UART, I2S, LED, and DCD pin macros supplied by each PlatformIO
  environment. Board-specific configuration belongs in `platformio.ini` or a
  board definition, not unguarded in `main.cpp`.
- Callbacks may run in Bluetooth/FreeRTOS contexts. Keep them short; use the
  existing mailbox, task-notification, and queue patterns for blocking or
  user-visible work. Protect shared state with the existing critical sections.
- AVRCP metadata strings are bounded by `AVRC_METADATA_TEXT_MAX_LENGTH`.
  Preserve bounded copies and null termination when touching this path.
- Keep the iPod emulator disabled while Bluetooth is disconnected, and retain
  its reset/disconnect behavior. It is what keeps the car head unit's state
  aligned with the phone.
- ESPDrive actions that clear pairings or restart require a second selection.
  Maintain that confirmation safeguard for destructive menu actions.

## Change hygiene

- Keep changes focused; do not commit `.pio/` build output or local IDE files.
- Prefer the project's existing Arduino/ESP-IDF logging (`ESP_LOG*`) and tabs
  in the main firmware files.
- Hardware-dependent behavior needs a build plus, when feasible, a bench or
  in-car check. State explicitly when physical verification was not possible.
