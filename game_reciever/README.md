# Game Receiver

A Zephyr / nRF Connect SDK application that turns an nRF54LM20 DK into a
Bluetooth LE central for the [game_controller](../game_controller) dongle.
It scans for the controller's Nordic UART Service (NUS), connects, subscribes
to notifications, and relays each received command onto its own console
UART as a `Command: <token>` line (e.g. `Command: UP`).

That console line lands on the same J-Link VCOM port that
`game_controller/game/controller.py` already auto-detects, so the DK acts as
the physical bridge between the wireless controller and the PC-side game.

## Prerequisites

This is an **out-of-tree application** for the
[sdk-edge-ai](https://github.com/nrfconnect/sdk-edge-ai) add-on. It must be
built from inside an NCS workspace that has the sdk-edge-ai add-on installed,
alongside the `game_controller` application.

- nRF Connect SDK (matching the sdk-edge-ai add-on version)
- A `nrf54lm20dk/nrf54lm20b/cpuapp` board, flashed with `game_controller`
  running on a paired `nrf54lm20dongle/nrf54lm20b/cpuapp`

## Building

From the application directory:

```bash
cd game_receiver
west build -b nrf54lm20dk/nrf54lm20b/cpuapp
west flash
```

## Layout

```
src/
  main.c
  ble/          BLE central: scan, connect, discover, subscribe to NUS
```

## License

Copyright (c) 2026 Nordic Semiconductor ASA

SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
