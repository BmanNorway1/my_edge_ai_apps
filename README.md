# Axon NPU Demos

This repository contains a set of demonstrations built for the Nordic Axon NPU, created during the summer of 2026 for Nordic Semiconductor. Each entry below is a standalone project showing a different use case for on-device machine learning on the nRF54LM20 series.

## Contents

**image_classification**
Hand sign finger digit classification, comparing two training pipelines (Edge Impulse Studio and a local Python/Keras pipeline) across two color modes (RGB and greyscale). Includes firmware, training scripts, and documentation for reproducing or swapping in new models.

**wakeword/ww_please**
Wakeword spotting running on-device.

**Game Controller / Game Receiver**
These are currently submodules in this git repo, see git docs for info about how to download these if you clone this project. A two-part demo turning an nRF54LM20 board into a game controller driven by on-device Edge AI. The controller runs keyword spotting (over the DMIC microphone) and gesture recognition (over the BMI270 IMU) to drive a PC-side game, coordinated by an engine controller. The receiver runs on a separate nRF54LM20 DK acting as a Bluetooth LE central, bridging the controller's commands from the wireless dongle to the PC over UART.

These two live in their own repositories, since each is a full out-of-tree application built against the sdk-edge-ai add-on:
- [game_controller](https://github.com/aslakoi/game_controller): keyword spotting + gesture recognition, runs on the dongle
- [game_receiver](https://github.com/Aslakoi/game_receiver): BLE central bridge, runs on the DK, relays commands to the PC

## Hardware

All demos target the nRF54LM20 series. image_classification, wakeword/ww_please, and game_controller use the integrated Axon NPU for on-device inference. game_receiver runs no ML itself, it is a BLE bridge pairing with game_controller's dongle.

## Getting started

**image_classification** and **wakeword/ww_please** are self-contained folders in this repository. See the README inside each for prerequisites, building, and flashing.

**game_controller** and **game_receiver** are separate repositories, each requiring an NCS workspace with the [sdk-edge-ai](https://github.com/nrfconnect/sdk-edge-ai) add-on installed. See each repository's own README for setup and build instructions.
