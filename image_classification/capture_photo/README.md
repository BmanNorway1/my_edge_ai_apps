# Capture Photo

This application captures JPEG images from an Arducam Mega camera and streams
them to a host PC over SEGGER RTT, for collecting image datasets.

## Application overview

The application captures JPEG images from an Arducam Mega camera module
connected over SPI, and sends each image to the host over a SEGGER RTT
up-channel. A companion Python script on the host splits the RTT byte stream
back into individual `.jpg` files.

It has two capture modes, selected with the buttons on the development kit:

- **Single shot** - captures one image per button press and sends it on RTT
  channel 1.
- **Stream mode** - captures a burst of up to a configurable number of images
  at a fixed interval and sends them on RTT channel 2. The burst can be stopped
  early with a button.

The two modes use separate RTT channels so the host can sort the images into
separate folders.

The camera's auto-exposure and gain need a few frames to settle, otherwise the
first image comes out nearly black. The application therefore discards a number
of warmup frames before keeping an image. In stream mode the camera keeps
streaming for the whole burst, so the warmup happens only once, before the
first image.

## Requirements

The application supports the **nRF54LM20 DK** (`nrf54lm20dk/nrf54lm20b/cpuapp`).

It also requires an **Arducam Mega 3MP (B0400) camera** connected over SPI.
Configure the development kit using Board Configurator to provide 3.3V to power
the camera. The application may also work for other Arducams such as the
Arducam Mega 5MP, but this is not tested and confirmed at this time.

### Pin mapping

| Description         | Arducam Mega Pin | nRF54LM20 DK Pin |
| ------------------- | ---------------- | ---------------- |
| Power supply (3.3V) | `VCC`            | `VDD:IO`         |
| Ground              | `GND`            | `GND`            |
| Chip select         | `CS`             | `P1.7`           |
| SPI MOSI            | `MOSI`           | `P1.6`           |
| SPI MISO            | `MISO`           | `P1.5`           |
| SPI Clock           | `SCK`            | `P1.4`           |

For detailed pin configuration, refer to the device tree overlay
`boards/nrf54lm20dk_nrf54lm20b_cpuapp.overlay` file.

## User interface

### Buttons

- **Button 1 (sw0)** - Captures a single image and sends it on RTT channel 1
  (single-shot mode).
- **Button 2 (sw1)** - Starts stream mode, capturing a burst of images and
  sending them on RTT channel 2.
- **Button 3 (sw2)** - Stops a running stream. The stream stops between images,
  never mid-capture, so the JPEG being collected is never truncated.

### LEDs

- **LED0 (capture LED)** - Turns on while the application is capturing and
  sending an image, and turns off when it is idle and waiting for a button
  press. In stream mode it stays on for the whole burst.

## Customizing the application

- **Output folders** - Change where images are saved by editing `SINGLE_OUTDIR`
  (single-shot photos) and `STREAM_OUTDIR` (stream-mode photos) at the top of
  `collect_dataset.py`.
- **Number of images in stream mode** - Set `STREAM_MAX_IMAGES` in `src/main.c`
  to change how many images a single stream-mode burst captures.
  `STREAM_INTERVAL_MS` sets the delay between images.
- **Number of warmup frames** - Set `WARMUP_FRAMES` in `src/main.c` to change
  how many frames are discarded before an image is kept, giving the camera's
  auto-exposure and gain time to settle.
- **Image resolution** - Set `CAM_WIDTH` and `CAM_HEIGHT` in `src/main.c` to
  change the resolution of the captured images. The default is the camera's
  maximum, 2048x1536.

## Building and running

Build and flash the application for the nRF54LM20 DK:

```console
west build -b nrf54lm20dk/nrf54lm20b/cpuapp
west flash
```

## Collecting images on the host

The `collect_dataset.py` script connects to the target over J-Link, reads both
RTT channels, and writes single-shot and stream-mode images into separate
folders. It depends on the `pylink-square` package, which is installed in a
virtual environment.

Run all of the following from the project folder
(`path_to_project/capture_photo`):

```console
cd path_to_project/capture_photo
```

Set up the virtual environment once:

```console
python3 -m venv .venv
source .venv/bin/activate
pip install pylink-square
```

After that, activate the environment and run the script each time you want to
collect images:

```console
source .venv/bin/activate
python3 collect_dataset.py
```

Press **Button 1** for a single photo or **Button 2** for stream mode. Press
**Ctrl+C** to stop. The script must be started *after* the board has booted or
been reset.

## Testing

> **Before you start:** Power up the board *without* the camera connected, then
> connect the camera and press the reset button. Start `collect_dataset.py`
> only after the board has booted or been reset. See
> [Known issues and workarounds](#known-issues-and-workarounds) for details.

1. Connect the development kit to the host with a USB cable.
2. Connect to the kit's serial port with a terminal emulator.
3. Start `collect_dataset.py` on the host.
4. Press **Button 1** on the development kit.
5. Observe **LED0** turning on while the image is captured and sent, then off.
6. Check that a new `.jpg` file appears in the single-shot output folder.
7. Press **Button 2** to capture a burst of images in stream mode, and
   optionally **Button 3** to stop the burst early.
8. Check that the images appear in the stream-mode output folder.

## Application output

The application shows the following output:

```console
[00:00:01.052,298] <inf> main: Idle: sw0 = single photo, sw1 = stream mode, sw2 = stop stream
[00:00:15.183,679] <inf> main: Captured JPEG: 280503 bytes
[00:00:17.107,788] <inf> main: Sent 280503-byte JPEG, ovmain: Idle: sw0 = single photo, sw1 = stream mode, sw2 = stop stream
[00:00:25.931,097] <inf> main: Stream mode: capturing 5 images
[00:00:27.795,760] <inf> main: Stream 1/5: sent 204951 bytes over RTT channel 2
[00:00:30.104,125] <inf> main: Stream 2/5: sent 162655 bytes over RTT channel 2
[00:00:32.915,706] <inf> main: Stream 3/5: sent 223365 bytes over RTT channel 2
[00:00:35.961,072] <inf> main: Stream 4/5: sent 252659 bytes over RTT channel 2
[00:00:35.961,086] <inf> main: Stream stopped by user after 4 image(s)
[00:00:35.961,097] <inf> main: Stream mode done
[00:00:35.961,102] <inf> main: Idle: sw0 = single photo, sw1 = stream mode, sw2 = stop stream
```

> **Interpreting the output:** If a log line in the terminal looks interrupted
> or cut off mid-message, the application has **not**
> crashed, this is just the log output being preempted while images are
> streamed over RTT.
>
> If **LED0 does not turn off** *and* no `Sent ... JPEG` line appears, the
> capture is stuck. Most often this just means the host is not connected:
> images are sent over RTT in blocking mode, so the firmware waits with the LED
> on until `collect_dataset.py` starts draining the channel.
> Start the host script and it continues. It is only a real crash if the LED
> stays on while the host is connected and reading.

## Known issues and workarounds

For the camera to work correctly, flash or reset the board after it
receives its power supply, and start the host script (`collect_dataset.py`)
only after the board has booted or been reset.

### Issue 1: Board not detected on power up with camera connected

**Problem:** When powering up the board, it is not detected by the host as long
as the camera is connected.

**Workaround:** Unplug the power cord to the camera to resolve detection. Power up the board
without the camera connected, then connect the camera afterward.

**Suggested fix:** Add a switch to the camera's power supply line. This lets the
camera be powered off during board detection without physically rewiring the
connection on every power-up.

### Issue 2: Camera initialization

**Problem:** The camera does not initialize properly if connected after the
board has already booted.

**Workaround:** Press the reset button on the board after connecting the camera.
This forces a re-initialization sequence that initializes the camera correctly. After this, run `collect_dataset.py` to start receiving images.

### Issue 3: Green tint

**Problem:** The first few frames have a green/colored tint because the sensor's
auto white balance and auto exposure have not converged yet.

**Workaround:** Discard the first few frames so the sensor's settings can settle
(see `WARMUP_FRAMES`). Otherwise the captured image comes out tinted or very
dark.

**Note:** It might be worth looking into the camera settings and disabling the
auto settings. This has not been explored yet.

