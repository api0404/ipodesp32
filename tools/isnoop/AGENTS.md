# iSnoop notes

This directory is a standalone PlatformIO project for inspecting iPod serial
traffic. Run its commands from this directory; its dependencies and platform
version are intentionally separate from the root firmware project.

## Commands

```powershell
pio run -e withESPLog
pio run -e frugal
```

Use `withESPLog` for protocol investigation and `frugal` when diagnostic log
volume must be minimized. Both targets use the `nodemcu-32s` board and a
115200-baud monitor.

## Runtime model

- `src/main.cpp` configures two UARTs at 19200 baud. Defaults are UART1
  RX/TX 18/19 and UART2 RX/TX 16/17; override them with build flags rather
  than editing defaults for a one-off harness.
- `snooper.cpp` frames packets (`FF 55`, length, payload, checksum), forwards
  validated traffic through FreeRTOS queues, and logs decoded Lingo 0x00 and
  0x04 packets.
- The snooper allocates queued packet payloads dynamically. Every new queue
  path must release ownership on both successful processing and queue failure.
- Treat captures as hardware-sensitive evidence. Do not alter timing, queue
  sizes, serial timeouts, or packet forwarding semantics without validating on
  the connected interface.
