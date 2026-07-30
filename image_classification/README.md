# Finger Digit Classifier

Firmware for classifying hand sign finger digits (zero through five, plus
unknown) on an nRF54LM20 DK, using the Axon NPU. Four variants are provided,
covering two training pipelines times two color modes.

| Variant | Pipeline | Color |
|---|---|---|
| `main_ei_rgb.c` | Edge Impulse Studio | RGB |
| `main_ei_greyscale.c` | Edge Impulse Studio | Greyscale |
| `main_manual_rgb.c` | `train.py` + `model_converter.py` | RGB |
| `main_manual_greyscale.c` | `train.py` + `model_converter.py` | Greyscale |

The manual variants also send predictions over BLE (Nordic UART Service) and
expect a paired data-collection firmware plus `collect_dataset.py` to build
the training set.

## Prerequisites

- nRF Connect SDK with the
  [sdk-edge-ai](https://github.com/nrfconnect/sdk-edge-ai) add-on installed
- An `nrf54lm20dk/nrf54lm20b/cpuapp` board with an Arducam Mega camera
- For the Edge Impulse variants: a model exported from Studio as a
  "Nordic Axon NPU library"
- For the manual variants: a model trained with `train.py`, converted with
  `model_converter.py`, and compiled through the Axon compiler

## Building

Each variant is its own app. From inside the app directory:

```bash
west build -b nrf54lm20dk/nrf54lm20b/cpuapp .
west flash
```

For the Edge Impulse variants, patch a fresh export before building:

```bash
python3 patch_ei_axon_export.py --project-root . --model-name <name> --dry-run
```

Check the diff, then re-run without `--dry-run` to apply it.

## Layout

```
main_ei_rgb.c              Edge Impulse SDK, RGB model
main_ei_greyscale.c        Edge Impulse SDK, greyscale model
main_manual_rgb.c          manual Axon driver, RGB model
main_manual_greyscale.c    manual Axon driver, greyscale model
ble_nus.c / ble_nus.h      BLE NUS sender, used by the manual variants
patch_ei_axon_export.py    patches known bugs in a fresh Edge Impulse export
train.py                   trains the model (manual pipeline)
keras_model.py             model architectures used by train.py
model_converter.py         converts the trained model to int8 TFLite
collect_dataset.py         builds the training set from device camera frames
```

## Swapping in a new model

**Edge Impulse variants:**

1. Export from Studio as a "Nordic Axon NPU library" and unzip into
   `ei-model/`.
2. Run `patch_ei_axon_export.py` against the fresh export.
3. Update the model-specific names in `main.c`: the `#include` for the test
   vectors header, and the array names inside `axon_selftest()`.
4. Build and confirm the self-test passes before trusting live inference.

**Manual driver variants:**

1. Train with `train.py`, convert with `model_converter.py`, then compile
   through the Axon compiler.
2. Rename the `#include` and the model struct name (`model_finger_digits`)
   to match what the compiler generated. Both RGB and greyscale variants use
   this same struct name, since each is a separate build project with no
   naming collision between them.
3. If the model changed significantly, recheck the LUT assumption in
   `init_lut()`: it assumes one shared scale/zero-point for the whole input
   tensor, which is normal for image inputs but worth confirming against the
   generated model header.
4. There is no self-test in these variants. If something looks wrong, check
   the pixel conversion first, before assuming the model is broken.

## Keeping training and firmware in sync

These are the recurring sources of train/serve mismatch on this project.

- **Resolution.** `CAM_WIDTH`/`CAM_HEIGHT` in firmware, `IMG_SIZE` in
  `train.py`, and the data-collection firmware's capture resolution must all
  match. All three are 96 right now.
- **Warmup frames.** `WARMUP_FRAMES` must match the data-collection firmware
  exactly (currently 8), or auto-exposure settles differently between
  training and inference frames.
- **Pixel expansion arithmetic.** The RGB565 to RGB888 formula
  (`value * 255 // 31` for 5-bit channels, `value * 255 // 63` for 6-bit
  channels) must be identical across `collect_dataset.py`,
  `train.py`'s `rgb888_to_camera_gray()`, and the firmware's own conversion
  functions. Already verified exact across all three; re-verify if any of
  them changes.
- **Color mode.** `COLOR_MODE` in `train.py` must match the firmware
  variant you deploy to. An RGB model will not run correctly on a
  greyscale build, or vice versa.
- **HWC to CHW layout.** RGB models need a transpose somewhere in the
  pipeline, since the camera produces interleaved pixel data but the
  Axon-compiled model expects planar data. The Edge Impulse path handles
  this via the patch script; the manual RGB firmware handles it directly in
  `capture_one_frame()`. Greyscale models have no such issue.
- **Interlayer buffer size** (Edge Impulse variants only). Must fit whatever
  model is loaded. `patch_ei_axon_export.py` sets this automatically from
  the model's own generated header.

## License

Copyright (c) 2026 Nordic Semiconductor ASA

SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
