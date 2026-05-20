## BT-A2DP-Source-HiGrow

Standalone **Bluetooth A2DP Source (transmitter)** test for an **ESP32 (Classic BT)** board (e.g. LilyGo HiGrow).

### What it does
- Connects to a Bluetooth speaker/headphones (A2DP Sink) by name
- Streams audio as an **A2DP Source**:
  - Default: **I2S input → A2DP** bridge (expects 44.1kHz, 16-bit, stereo)
  - Optional fallback: generate a 440Hz sine wave (compile-time)
- Exposes a simple **UART control protocol** so another MCU (e.g. ProS3) can
  `CONNECT`/`DISCONNECT` and put the ESP32 into deep sleep cleanly.

### Requirements
- A board with **ESP32 (original)** that supports **Classic Bluetooth (BR/EDR)**.
  - **ESP32-S3 / ESP32-C6 won’t work** for A2DP.
- PlatformIO

### Setup
1. Open this folder in PlatformIO.
2. Edit `include/options.h` and set:
   - `#define A2DP_SINK_NAME "Your Speaker Name"`
   - (Bridge mode) set I2S pins:
     - `I2S_BCK_PIN`, `I2S_WS_PIN`, `I2S_DATA_IN_PIN`
3. Put your speaker/headphones into pairing mode.
4. Build + upload.

If it connects, you should hear a steady tone.

### Wiring (bridge mode)
Connect your audio source’s I2S output to the ESP32:
- **BCLK** → `I2S_BCK_PIN`
- **LRCLK/WS** → `I2S_WS_PIN`
- **DATA out** → `I2S_DATA_IN_PIN`
- **GND** → **GND**

The ESP32 is configured as **I2S SLAVE RX** (upstream device provides the clocks).

### UART control (recommended)
This firmware can be controlled by another MCU via UART (line-based, `\n` terminated).

Defaults (override with `#define`s in `include/options.h`):
- **CTRL UART**: `RX=GPIO39` (input-only), `TX=GPIO13`, `115200 baud`
- **Wake pin**: `GPIO38` (EXT0 wake, level high)

Commands:
- `PING` → `PONG`
- `STATUS` → prints connection/audio state + buffer stats (includes current upstream PCM `sr=...`)
- `CONNECT <name>` → scan + connect to the first matching sink name
- `DISCONNECT` → best-effort disconnect (keeps stack ready)
- `BT_OFF` → disconnect + `end(true)` (release memory)
- `BT_ON` → start again with last name
- `SR <hz>` → set the **upstream PCM sample rate** on the I2S lines (`44100` or `48000`)
  - A2DP output is fixed to **44.1kHz**; when `SR 48000` is set, the bridge resamples **48k → 44.1k** before encoding
- `SLEEP` → disconnect + deep sleep (wake on `WAKE_PIN`)

### Notes (upload/reset)
If upload fails or the board doesn’t auto-reset cleanly, `platformio.ini` uses explicit `esptool` upload flags (`--before=default_reset`, `--after=hard_reset`, `--baud=921600`) which were confirmed working on at least one HiGrow setup.

