#pragma once

// Central configuration for BT-A2DP-Source-HiGrow.
//
// You can edit this file, or override any macro via PlatformIO `build_flags`:
//   -D A2DP_SINK_NAME=\"My Speaker\"
//   -D I2S_BCK_PIN=17 ...

// -------- Board pin map --------
// Default (no board flag): LilyGo HiGrow wired to the yoRadio ProS3 (pins in the defaults below).
// -DCOMPANION_BOARD_NODEMCU32S: generic ESP32-WROOM devkit (NodeMCU-32S) for April's Speaker.
//   Avoids flash pins 6-11, strapping pins 0/2/5/12/15 and UART0 1/3. GPIO38 (HiGrow wake) is not
//   broken out on these boards, so wake moves to GPIO33 (RTC-capable for EXT0).
//   Pins chosen 2026-10-24 (Cain: "you choose"); see yoRadio docs/APRILS_SPEAKER.md + wiring PDF.
//   Node EN <- FeatherS3 GPIO9 and Node IO0 <- FeatherS3 GPIO8 (open-drain, for reset + flashing via the S3).
#if defined(COMPANION_BOARD_NODEMCU32S)
  #ifndef CTRL_UART_TX_PIN
  #define CTRL_UART_TX_PIN 17   // -> FeatherS3 UART1 RX GPIO16 (yoRadio BT_COMPANION_UART_RX)
  #endif
  #ifndef CTRL_UART_RX_PIN
  #define CTRL_UART_RX_PIN 16   // <- FeatherS3 UART1 TX GPIO15 (BT_COMPANION_UART_TX)
  #endif
  #ifndef WAKE_PIN
  #define WAKE_PIN 33           // <- FeatherS3 GPIO7 wake output (EXT0, active high, pulled down)
  #endif
  #ifndef I2S_BCK_PIN
  #define I2S_BCK_PIN 26        // shared with Pirate Audio BCK (FeatherS3 I2S_BCLK GPIO17)
  #endif
  #ifndef I2S_WS_PIN
  #define I2S_WS_PIN 25         // shared with Pirate Audio LRCK (FeatherS3 I2S_LRC GPIO18)
  #endif
  #ifndef I2S_DATA_IN_PIN
  #define I2S_DATA_IN_PIN 22    // shared with Pirate Audio DIN (FeatherS3 I2S_DOUT GPIO14)
  #endif
  #ifndef A2DP_RX_LOCAL_NAME
  #define A2DP_RX_LOCAL_NAME "April's Speaker"
  #endif
#endif

// -------- Target Bluetooth sink name --------
#ifndef A2DP_SINK_NAME
#define A2DP_SINK_NAME "HiFi-IIS"
#endif

// -------- Logging controls (defaults: quiet) --------
#ifndef HIGROW_LOG_TELEMETRY
#define HIGROW_LOG_TELEMETRY 0
#endif
#ifndef HIGROW_LOG_DISCOVERY
#define HIGROW_LOG_DISCOVERY 0
#endif
#ifndef HIGROW_LOG_CTRL
#define HIGROW_LOG_CTRL 1
#endif

// -------- Audio source mode --------
// 1 = I2S input -> A2DP bridge, 0 = generated sine tone.
#ifndef USE_I2S_INPUT
#define USE_I2S_INPUT 1
#endif

// A2DP (SBC) expects PCM typically as 44.1kHz, 16-bit, stereo.
// For the bridge, your upstream I2S source should match this.
#ifndef A2DP_SAMPLE_RATE_HZ
#define A2DP_SAMPLE_RATE_HZ 44100
#endif

// Ring buffer size (bytes). Needs to cover BT scheduling jitter.
#ifndef PCM_RING_BYTES
#define PCM_RING_BYTES (64 * 1024)
#endif

// -------- Control UART (ProS3 <-> HiGrow) --------
#ifndef CTRL_UART_ENABLE
#define CTRL_UART_ENABLE 1
#endif
#ifndef CTRL_UART_BAUD
#define CTRL_UART_BAUD 115200
#endif
#ifndef CTRL_UART_TX_PIN
#define CTRL_UART_TX_PIN 13
#endif
#ifndef CTRL_UART_RX_PIN
#define CTRL_UART_RX_PIN 39
#endif

// -------- Deep sleep wake pin (HiGrow) --------
// Must be RTC-capable GPIO for EXT0.
#ifndef WAKE_PIN
#define WAKE_PIN 38
#endif
#ifndef WAKE_LEVEL
#define WAKE_LEVEL 1
#endif

// Default behavior: do NOT auto-connect on boot. Wait for UART command.
#ifndef AUTO_CONNECT_ON_BOOT
#define AUTO_CONNECT_ON_BOOT 0
#endif

// Auto-sleep if idle (no BT started, no UART activity) for this long (ms).
// Set to 0 to disable.
#ifndef AUTO_SLEEP_IDLE_MS
#define AUTO_SLEEP_IDLE_MS 30000UL
#endif

// -------- I2S input pins (bridge mode) --------
#ifndef I2S_BCK_PIN
#define I2S_BCK_PIN 17
#endif
#ifndef I2S_WS_PIN
#define I2S_WS_PIN 23
#endif
#ifndef I2S_DATA_IN_PIN
#define I2S_DATA_IN_PIN 19
#endif

// I2S input slot width. Many sources output 32-bit slots for 16-bit audio.
#ifndef I2S_IN_BITS
#define I2S_IN_BITS 32
#endif

// When I2S_IN_BITS=32, choose where the 16-bit audio lives inside the 32-bit slot.
// Most common is left-justified (MSB 16 bits). If audio is wrong, try 0.
#ifndef I2S_32BIT_USE_MSB16
#define I2S_32BIT_USE_MSB16 1
#endif

// -------- A2DP sink (RX) mode: phone -> HiGrow -> I2S master -> DAC --------
#ifndef A2DP_RX_LOCAL_NAME
#define A2DP_RX_LOCAL_NAME "yoRadio Fusion"
#endif

// I2S master TX data pin (shared DIN net with ProS3 / MAX98357).
#ifndef I2S_DATA_OUT_PIN
#define I2S_DATA_OUT_PIN I2S_DATA_IN_PIN
#endif

