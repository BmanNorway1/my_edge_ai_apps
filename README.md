# Axon NPU Demos

This repository contains a set of demonstrations built for the Nordic Axon NPU, created during the summer of 2026 for Nordic Semiconductor. Each folder is a standalone project showing a different use case for on-device machine learning on the nRF54LM20 series.

## Contents

**capture_photo**
Firmware and tooling for collecting camera image datasets directly from the device, used as the data source for the image classification models below.

**image_classification**
Hand sign finger digit classification, comparing two training pipelines (Edge Impulse Studio and a local Python/Keras pipeline) across two color modes (RGB and greyscale). Includes firmware, training scripts, and documentation for reproducing or swapping in new models.

**wakeword/ww_please**
Wakeword spotting running on-device.

## Hardware

All demos target the nRF54LM20 series and its integrated Axon NPU.

## Getting started

Each folder is self-contained and includes its own build instructions. See the README inside each folder for details on prerequisites, building, and flashing.
