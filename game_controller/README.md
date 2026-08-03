# Game Controller

A Zephyr / nRF Connect SDK application that turns an nRF54LM20 board into a
game controller driven by on-device Edge AI: keyword spotting (KWS) over the
DMIC microphone and gesture recognition over the BMI270 IMU, coordinated by an
engine controller.

## Prerequisites

This is an **out-of-tree application** for the
[sdk-edge-ai](https://github.com/nrfconnect/sdk-edge-ai) add-on. It depends on
the add-on for the nRF Edge AI / Axon libraries (`CONFIG_NRF_EDGEAI`,
`CONFIG_NRF_AXON`), so it must be built from inside an NCS workspace that has
the sdk-edge-ai add-on installed.

- nRF Connect SDK (matching the sdk-edge-ai add-on version)
- The `sdk-edge-ai` add-on checked out in your NCS workspace
- A supported board:
  - `nrf54lm20dk/nrf54lm20b/cpuapp`
  - `nrf54lm20dongle/nrf54lm20b/cpuapp`

## Getting it

Clone this repository into the `applications/` folder of your sdk-edge-ai
add-on:

```bash
cd <ncs-workspace>/sdk-edge-ai/applications
git clone https://github.com/aslakoi/game_controller.git
```

## Building

From the application directory:

```bash
cd game_controller
west build -b nrf54lm20dk/nrf54lm20b/cpuapp
west flash
```

For the dongle, use `-b nrf54lm20dongle/nrf54lm20b/cpuapp`.

## Configuration

Application-specific Kconfig options live in `Kconfig`, for example:

- `CONFIG_KWS_EMA_ALPHA` — exponential moving-average coefficient for class
  probability (units of 1/1000).
- `CONFIG_KWS_ONLY` — skip the gesture engine so the app boots without a
  BMI270 wired up. Useful for KWS-only development, since the IMU's only 5V
  source is the DK and it otherwise has to be jumper-wired in for the app to
  boot at all.

## Layout

```
src/
  main.c
  common.h
  engine/        Engine controller coordinating KWS and gesture engines
  kws/           Keyword spotting (DMIC capture + model)
  gesture/       Gesture recognition (BMI270 IMU + model)
  hw_modules/    Buttons, LEDs
boards/          Board overlays (DK and dongle)
```

Generated Edge AI model sources are vendored under
`src/**/nrf_edgeai_generated/`.

## License

Copyright (c) 2026 Nordic Semiconductor ASA

SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
