#include <Arduino.h>
#include <math.h>

#include "esp_log.h"
#include "driver/i2s.h"
#include "esp_sleep.h"
#include "driver/rtc_io.h"

#include "BluetoothA2DPSource.h"
#include "options.h"

static constexpr int kSampleRateHz = A2DP_SAMPLE_RATE_HZ;

// -------- Control UART (ProS3 <-> HiGrow) --------
// A simple line-based command protocol so the ProS3 can control BT connect/sleep.
// Default pins are chosen to work with your stated availability:
// - TX can be any output-capable pin (default 13)
// - RX can be an input-only pin (default 39)
static constexpr size_t kPcmRingBytes = (size_t)PCM_RING_BYTES;

BluetoothA2DPSource a2dp_source;

static const char *kTag = "BT_TX";

#if CTRL_UART_ENABLE
static HardwareSerial s_ctrlSerial(1);
static String s_ctrlLine;
#endif

static char s_sinkName[64] = A2DP_SINK_NAME;
static bool s_btStarted = false;
static uint32_t s_lastUseMs = 0;
static volatile bool s_shuttingDown = false;
static TaskHandle_t s_i2sTask = nullptr;
static volatile bool s_i2sTaskStopped = true;

// The ProS3 can switch stream sample rates (podcasts are often 48kHz).
// A2DP (SBC) output in this project is fixed to kSampleRateHz, so when the input
// rate differs we resample into the PCM ring buffer to avoid pitch shift/clicks.
static volatile uint32_t s_inPcmHz = (uint32_t)kSampleRateHz;

#if USE_I2S_INPUT
// Forward declaration: ring_write is defined below, but used by the resampler helper.
static void ring_write(const uint8_t *data, size_t len);

static uint32_t s_rs_inHz = 0;
static uint32_t s_rs_phase_q16 = 0;  // Q16.16 position in input frames (index 0 is prev sample)
static uint32_t s_rs_step_q16 = 0;   // Q16.16 input-frames per output-frame
static int16_t s_rs_prevL = 0;
static int16_t s_rs_prevR = 0;
static bool s_rs_hasPrev = false;

static inline void resamplerReset(uint32_t inHz) {
  s_rs_inHz = inHz;
  s_rs_phase_q16 = 0;
  s_rs_step_q16 = (uint32_t)(((uint64_t)inHz << 16) / (uint64_t)kSampleRateHz);
  s_rs_prevL = 0;
  s_rs_prevR = 0;
  s_rs_hasPrev = false;
}

static inline void ring_write_pcm_44k1(int16_t* interleavedLR, size_t frames, uint32_t inHz) {
  if (!interleavedLR || frames == 0) return;
  // Only handle the two rates we care about today.
  if (inHz != 44100u && inHz != 48000u) inHz = 44100u;

  // Fast path: already at the encoder rate.
  if (inHz == (uint32_t)kSampleRateHz) {
    ring_write(reinterpret_cast<const uint8_t*>(interleavedLR), frames * 4);
    return;
  }

  // Downsample 48k -> 44.1k with simple linear interpolation.
  if (s_rs_inHz != inHz || s_rs_step_q16 == 0) {
    resamplerReset(inHz);
  }

  // Initialize prev sample on first block so interpolation has a stable starting point.
  if (!s_rs_hasPrev) {
    s_rs_prevL = interleavedLR[0];
    s_rs_prevR = interleavedLR[1];
    s_rs_hasPrev = true;
  }

  const uint32_t maxPhase = (uint32_t)frames << 16; // idx in [0..frames-1] valid, idx+1 in [1..frames]
  size_t outFrames = 0;

  // Output buffer (int16 interleaved L/R). Sized for the largest chunk we read (4096 bytes => 1024 frames).
  static int16_t s_out[2048];

  while (s_rs_phase_q16 < maxPhase && (outFrames + 1) < frames && (outFrames * 2 + 1) < (sizeof(s_out) / sizeof(s_out[0]))) {
    const uint32_t idx = (s_rs_phase_q16 >> 16);
    const uint32_t frac = (s_rs_phase_q16 & 0xFFFFu);

    int16_t l0 = 0, r0 = 0, l1 = 0, r1 = 0;
    if (idx == 0) {
      l0 = s_rs_prevL;
      r0 = s_rs_prevR;
      l1 = interleavedLR[0];
      r1 = interleavedLR[1];
    } else {
      const size_t i0 = (size_t)(idx - 1) * 2;
      const size_t i1 = (size_t)(idx) * 2;
      l0 = interleavedLR[i0 + 0];
      r0 = interleavedLR[i0 + 1];
      l1 = interleavedLR[i1 + 0];
      r1 = interleavedLR[i1 + 1];
    }

    const int32_t dl = (int32_t)l1 - (int32_t)l0;
    const int32_t dr = (int32_t)r1 - (int32_t)r0;
    const int32_t lo = (int32_t)l0 + (int32_t)((dl * (int32_t)frac) >> 16);
    const int32_t ro = (int32_t)r0 + (int32_t)((dr * (int32_t)frac) >> 16);

    s_out[outFrames * 2 + 0] = (int16_t)lo;
    s_out[outFrames * 2 + 1] = (int16_t)ro;
    outFrames++;

    s_rs_phase_q16 += s_rs_step_q16;
  }

  // Carry phase forward to the next block and keep the last input frame as "prev".
  s_rs_phase_q16 = (s_rs_phase_q16 >= maxPhase) ? (s_rs_phase_q16 - maxPhase) : 0u;
  s_rs_prevL = interleavedLR[(frames - 1) * 2 + 0];
  s_rs_prevR = interleavedLR[(frames - 1) * 2 + 1];
  s_rs_hasPrev = true;

  ring_write(reinterpret_cast<const uint8_t*>(s_out), outFrames * 4);
}
#endif

static bool namePrefixMatchI(const char *ssid, const char *want) {
  if (!ssid || !ssid[0] || !want || !want[0]) return false;
  for (size_t i = 0; want[i]; i++) {
    const char a = (char)tolower((unsigned char)ssid[i]);
    const char b = (char)tolower((unsigned char)want[i]);
    if (a != b) return false;
  }
  return true;
}

// Default behavior: do NOT auto-connect on boot. Wait for UART command.
#ifndef AUTO_CONNECT_ON_BOOT
#define AUTO_CONNECT_ON_BOOT 0
#endif

// Auto-sleep if idle (no BT started, no UART activity) for this long.
#ifndef AUTO_SLEEP_IDLE_MS
#define AUTO_SLEEP_IDLE_MS 30000UL
#endif

static void logBoth(const char *msg) {
  Serial.println(msg);
#if CTRL_UART_ENABLE
  s_ctrlSerial.println(msg);
#endif
}

static void btStopGraceful(bool releaseMemory) {
  if (!s_btStarted) return;
  Serial.println("[BT] stopping...");
  // Best-effort disconnect. Some speakers will still show "connected" until their own timeout
  // if we hard-power-off without a clean disconnect.
  a2dp_source.set_connected(false);
  delay(250);
  a2dp_source.end(releaseMemory);
  s_btStarted = false;
  Serial.println("[BT] stopped");
  s_lastUseMs = millis();
}

static void btStart(const char *name) {
  if (!name || !name[0]) return;
  if (s_btStarted) {
    // If already running, do a clean restart to re-scan from scratch.
    btStopGraceful(false);
    delay(150);
  }
  Serial.printf("[BT] starting, target='%s'\n", name);
  a2dp_source.start(name);
  s_btStarted = true;
  s_lastUseMs = millis();
}

static void enterDeepSleep() {
  Serial.println("[SLEEP] preparing deep sleep");
  s_shuttingDown = true;
  btStopGraceful(true);

#if USE_I2S_INPUT
  // Stop I2S driver to reduce leakage and avoid surprises on wake.
  if (s_i2sTask) {
    i2s_stop(I2S_NUM_0);
    const uint32_t t0 = millis();
    while (!s_i2sTaskStopped && (millis() - t0) < 800) {
      delay(10);
    }
  }
  i2s_driver_uninstall(I2S_NUM_0);
#endif

  // Configure ext0 wake on WAKE_PIN.
  const gpio_num_t wakeGpio = (gpio_num_t)WAKE_PIN;
  rtc_gpio_deinit(wakeGpio);
  pinMode(WAKE_PIN, INPUT);
  // Keep it from floating.
  rtc_gpio_pulldown_en(wakeGpio);
  rtc_gpio_pullup_dis(wakeGpio);
  esp_sleep_enable_ext0_wakeup(wakeGpio, WAKE_LEVEL);

  Serial.printf("[SLEEP] deep sleep, wake on GPIO%d=%d\n", WAKE_PIN, WAKE_LEVEL);
  Serial.flush();
  delay(50);
  esp_deep_sleep_start();
}

#if USE_I2S_INPUT
static uint8_t s_ring[kPcmRingBytes];
static size_t s_ring_r = 0;
static size_t s_ring_w = 0;
static size_t s_ring_fill = 0;
static portMUX_TYPE s_ring_mux = portMUX_INITIALIZER_UNLOCKED;

static uint32_t s_i2s_read_calls = 0;
static uint32_t s_i2s_read_bytes = 0;
static uint32_t s_i2s_zero_reads = 0;
static uint32_t s_i2s_err_reads = 0;
static int s_i2s_last_err = 0;
static uint32_t s_ring_overrun_bytes = 0;
static uint32_t s_ring_underrun_bytes = 0;
static uint32_t s_a2dp_cb_calls = 0;
static uint32_t s_pcm_peak = 0;
static uint32_t s_pcm_dc_abs = 0;

static void ring_write(const uint8_t *data, size_t len) {
  if (len == 0) return;
  portENTER_CRITICAL(&s_ring_mux);

  // If overflow, drop oldest data to make room.
  if (len > kPcmRingBytes) {
    data += (len - kPcmRingBytes);
    len = kPcmRingBytes;
  }
  if (len > (kPcmRingBytes - s_ring_fill)) {
    const size_t need = len - (kPcmRingBytes - s_ring_fill);
    s_ring_r = (s_ring_r + need) % kPcmRingBytes;
    s_ring_fill -= need;
    s_ring_overrun_bytes += need;
  }

  const size_t first = min(len, kPcmRingBytes - s_ring_w);
  memcpy(&s_ring[s_ring_w], data, first);
  const size_t rem = len - first;
  if (rem) memcpy(&s_ring[0], data + first, rem);

  s_ring_w = (s_ring_w + len) % kPcmRingBytes;
  s_ring_fill += len;

  portEXIT_CRITICAL(&s_ring_mux);
}

static size_t ring_read(uint8_t *out, size_t len) {
  if (len == 0) return 0;
  size_t got = 0;

  portENTER_CRITICAL(&s_ring_mux);
  got = min(len, s_ring_fill);

  const size_t first = min(got, kPcmRingBytes - s_ring_r);
  memcpy(out, &s_ring[s_ring_r], first);
  const size_t rem = got - first;
  if (rem) memcpy(out + first, &s_ring[0], rem);

  s_ring_r = (s_ring_r + got) % kPcmRingBytes;
  s_ring_fill -= got;
  portEXIT_CRITICAL(&s_ring_mux);

  if (got < len) {
    memset(out + got, 0, len - got);
    s_ring_underrun_bytes += (len - got);
  }
  return len;
}

static void i2s_reader_task(void *) {
  s_i2sTaskStopped = false;
  static uint8_t tmp[4096];
  uint32_t last_nonzero_ms = millis();
  bool warned_no_i2s = false;
  while (true) {
    if (s_shuttingDown) {
      s_i2sTaskStopped = true;
      s_i2sTask = nullptr;
      vTaskDelete(nullptr);
    }
    size_t bytes_read = 0;
    esp_err_t err = i2s_read(I2S_NUM_0, tmp, sizeof(tmp), &bytes_read, pdMS_TO_TICKS(100));
    s_i2s_read_calls++;
    if (err != ESP_OK) {
      s_i2s_err_reads++;
      s_i2s_last_err = (int)err;
    }
    if (bytes_read == 0) {
      s_i2s_zero_reads++;
      const uint32_t now = millis();
      if (!warned_no_i2s && (now - last_nonzero_ms) > 2000) {
        warned_no_i2s = true;
        Serial.println("[I2S] No I2S data (likely missing BCLK/WS). Check wiring/pins/ground and that the source is outputting I2S.");
      }
    } else {
      last_nonzero_ms = millis();
      if (warned_no_i2s) {
        warned_no_i2s = false;
        Serial.println("[I2S] I2S data resumed.");
      }
    }
    if (err == ESP_OK && bytes_read) {
      if (I2S_IN_BITS == 16) {
        bytes_read &= ~((size_t)3); // 4 bytes per stereo 16-bit frame
        if (bytes_read) {
          s_i2s_read_bytes += bytes_read;

          const int16_t *s = reinterpret_cast<const int16_t *>(tmp);
          const size_t n = bytes_read / 2;
          int32_t acc = 0;
          int16_t peak = 0;
          for (size_t i = 0; i < n; i++) {
            const int16_t v = s[i];
            const int16_t av = (v < 0) ? (int16_t)-v : v;
            if (av > peak) peak = av;
            acc += v;
          }
          const int32_t mean = (n ? (acc / (int32_t)n) : 0);
          s_pcm_peak = (uint32_t)peak;
          s_pcm_dc_abs = (uint32_t)((mean < 0) ? -mean : mean);

          ring_write_pcm_44k1(reinterpret_cast<int16_t*>(tmp), bytes_read / 4, s_inPcmHz);
        }
      } else {
        bytes_read &= ~((size_t)7); // 8 bytes per stereo 32-bit frame
        if (bytes_read) {
          const int32_t *in = reinterpret_cast<const int32_t *>(tmp);
          const size_t frames = bytes_read / 8;
          int16_t *out = reinterpret_cast<int16_t *>(tmp); // in-place shrink

          int32_t acc = 0;
          int16_t peak = 0;
          for (size_t i = 0; i < frames * 2; i++) {
#if I2S_32BIT_USE_MSB16
            const int16_t v = (int16_t)(in[i] >> 16);
#else
            const int16_t v = (int16_t)(in[i] & 0xFFFF);
#endif
            out[i] = v;
            const int16_t av = (v < 0) ? (int16_t)-v : v;
            if (av > peak) peak = av;
            acc += v;
          }
          const size_t out_bytes = frames * 4;
          const int32_t mean = ((frames * 2) ? (acc / (int32_t)(frames * 2)) : 0);
          s_pcm_peak = (uint32_t)peak;
          s_pcm_dc_abs = (uint32_t)((mean < 0) ? -mean : mean);

          s_i2s_read_bytes += out_bytes;
          ring_write_pcm_44k1(out, out_bytes / 4, s_inPcmHz);
        }
      }
    }
    // If upstream clock is absent, i2s_read will often time out with 0 bytes.
    vTaskDelay(pdMS_TO_TICKS(1));
  }
}
#else
static constexpr float kToneHz = 440.0f;
static constexpr float kAmplitude = 0.20f;  // 0..1
static float s_phase = 0.0f;
#endif

// Callback to provide PCM bytes to the encoder.
static int32_t get_sound_data(uint8_t *data, int32_t byteCount) {
#if USE_I2S_INPUT
  // The A2DP stack may call this during shutdown; be defensive.
  if (!data || byteCount <= 0 || s_shuttingDown || !s_btStarted) return 0;
  s_a2dp_cb_calls++;
  ring_read(data, (size_t)byteCount);
  return byteCount;
#else
  // 16-bit stereo frames = 4 bytes per frame.
  const int32_t frames = byteCount / 4;
  int16_t *out = reinterpret_cast<int16_t *>(data);

  const float phase_inc =
      2.0f * static_cast<float>(M_PI) * kToneHz / static_cast<float>(kSampleRateHz);

  for (int32_t i = 0; i < frames; i++) {
    const float v = sinf(s_phase) * kAmplitude;
    const int16_t s = static_cast<int16_t>(v * 32767.0f);
    out[i * 2 + 0] = s;
    out[i * 2 + 1] = s;
    s_phase += phase_inc;
    if (s_phase > 2.0f * static_cast<float>(M_PI)) s_phase -= 2.0f * static_cast<float>(M_PI);
  }

  return frames * 4;
#endif
}

void setup() {
  Serial.begin(115200);
  delay(300);
  s_lastUseMs = millis();

  Serial.println();
  Serial.println("A2DP Source test (ESP32 Classic BT required)");
  Serial.print("Default target sink name: ");
  Serial.println(A2DP_SINK_NAME);
  Serial.println("Put your speaker/headphones in pairing mode now (or control via UART).");

#if CTRL_UART_ENABLE
  s_ctrlSerial.begin(CTRL_UART_BAUD, SERIAL_8N1, CTRL_UART_RX_PIN, CTRL_UART_TX_PIN);
  Serial.printf("[CTRL] UART1 RX=%d TX=%d baud=%d\n", CTRL_UART_RX_PIN, CTRL_UART_TX_PIN, CTRL_UART_BAUD);
#endif

  // Ensure ESP-IDF logs are visible on Serial (UART0).
  esp_log_level_set("*", ESP_LOG_INFO);
  // Make the A2DP library’s own logs visible (it uses ESP_LOGx with BT_AV_TAG etc).
  esp_log_level_set("BT_AV", ESP_LOG_INFO);
  esp_log_level_set("BT_API", ESP_LOG_INFO);
  esp_log_level_set("RCCT", ESP_LOG_INFO);

#if USE_I2S_INPUT
  Serial.println("Mode: I2S input -> A2DP (bridge)");
  Serial.printf("I2S pins: BCK=%d WS=%d DIN=%d\n", I2S_BCK_PIN, I2S_WS_PIN, I2S_DATA_IN_PIN);
  Serial.printf("Expected PCM (A2DP): %d Hz, 16-bit stereo\n", kSampleRateHz);
  Serial.printf("I2S input: %d-bit slots (32-bit will be downconverted)\n", (int)I2S_IN_BITS);

  // Configure I2S as SLAVE RX (clocks provided by upstream device).
  i2s_config_t cfg = {};
  cfg.mode = (i2s_mode_t)(I2S_MODE_SLAVE | I2S_MODE_RX);
  cfg.sample_rate = kSampleRateHz;
  cfg.bits_per_sample = (I2S_IN_BITS == 32) ? I2S_BITS_PER_SAMPLE_32BIT : I2S_BITS_PER_SAMPLE_16BIT;
  cfg.channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT;
  cfg.communication_format = I2S_COMM_FORMAT_STAND_I2S;
  cfg.intr_alloc_flags = ESP_INTR_FLAG_LEVEL1;
  cfg.dma_buf_count = 6;
  cfg.dma_buf_len = 256;
  cfg.use_apll = false;
  cfg.tx_desc_auto_clear = false;
  cfg.fixed_mclk = 0;

  const esp_err_t i2s_ok = i2s_driver_install(I2S_NUM_0, &cfg, 0, nullptr);
  if (i2s_ok != ESP_OK) {
    Serial.printf("I2S install failed: %d\n", (int)i2s_ok);
    while (true) delay(1000);
  }

  i2s_pin_config_t pins = {};
  pins.bck_io_num = I2S_BCK_PIN;
  pins.ws_io_num = I2S_WS_PIN;
  pins.data_out_num = I2S_PIN_NO_CHANGE;
  pins.data_in_num = I2S_DATA_IN_PIN;
  const esp_err_t pin_ok = i2s_set_pin(I2S_NUM_0, &pins);
  if (pin_ok != ESP_OK) {
    Serial.printf("I2S set_pin failed: %d\n", (int)pin_ok);
    while (true) delay(1000);
  }

  s_i2sTaskStopped = false;
  xTaskCreatePinnedToCore(i2s_reader_task, "i2s_reader", 4096, nullptr, 2, &s_i2sTask, 1);
#else
  Serial.println("Mode: generated sine test tone -> A2DP");
#endif

  // Provide PCM
  a2dp_source.set_data_callback(get_sound_data);

  // Connection/audio state callbacks (high-signal breadcrumbs).
  a2dp_source.set_on_connection_state_changed([](esp_a2d_connection_state_t st, void *) {
    // Values are esp_a2d_connection_state_t:
    // 0=DISCONNECTED, 1=CONNECTING, 2=CONNECTED, 3=DISCONNECTING
    Serial.printf("[BT] conn_state=%d\n", (int)st);
  });
  a2dp_source.set_on_audio_state_changed([](esp_a2d_audio_state_t st, void *) {
    // Values are esp_a2d_audio_state_t:
    // 0=REMOTE_SUSPEND, 1=STOPPED, 2=STARTED
    Serial.printf("[BT] audio_state=%d\n", (int)st);
  });

  // Log discovery mode transitions (helpful when debugging pairing/connect).
  a2dp_source.set_discovery_mode_callback([](esp_bt_gap_discovery_state_t st) {
#if HIGROW_LOG_DISCOVERY
    Serial.printf("[BT] discovery_mode=%d\n", (int)st);
#else
    (void)st;
#endif
  });

  // Optional: print discovered device names + RSSI (can be noisy).
  a2dp_source.set_ssid_callback([](const char *ssid, esp_bd_addr_t, int rssi) -> bool {
    if (ssid && ssid[0]) {
      // IMPORTANT: match against the *runtime* target name (s_sinkName), not the compile-time
      // default (A2DP_SINK_NAME). ProS3 can send CONNECT <name> to change it.
      const bool match = namePrefixMatchI(ssid, s_sinkName);
#if HIGROW_LOG_DISCOVERY
      Serial.printf("[BT] found: '%s' rssi=%d%s\n", ssid, rssi, match ? " [MATCH]" : "");
#else
      (void)rssi;
#endif
      // IMPORTANT: returning true here means “this is the target, connect to it”.
      // If we return true for every device, we’ll connect to the first compatible one.
      return match;
    }
    return false;
  });

  // Try connecting (the library actively scans and connects to the sink name)
  if (AUTO_CONNECT_ON_BOOT) {
    btStart(s_sinkName);
  } else {
    Serial.println("[BT] idle (waiting for UART CONNECT). Will auto-sleep if unused.");
  }
}

static void handleCtrlLine(const String &line) {
#if !CTRL_UART_ENABLE
  (void)line;
  return;
#else
  String s = line;
  s.trim();
  if (s.isEmpty()) return;
  s_lastUseMs = millis();

#if HIGROW_LOG_CTRL
  Serial.print("[CTRL] ");
  Serial.println(s);
#endif

  // Tokenize: CMD [arg...]
  int sp = s.indexOf(' ');
  String cmd = (sp >= 0) ? s.substring(0, sp) : s;
  String arg = (sp >= 0) ? s.substring(sp + 1) : String();
  cmd.toUpperCase();
  arg.trim();

  if (cmd == "PING") {
    s_ctrlSerial.println("PONG");
    return;
  }

  if (cmd == "HELP") {
    s_ctrlSerial.println("OK cmds: PING, STATUS, CONNECT <name>, DISCONNECT, BT_ON, BT_OFF, SR <hz>, SLEEP");
    return;
  }

  if (cmd == "STATUS") {
    s_ctrlSerial.printf("STATUS conn=%d audio=%d bt=%d sr=%u ring=%u i2sB=%u underrunB=%u overrunB=%u peak=%u dc=%u\n",
                        (int)a2dp_source.get_connection_state(),
                        (int)a2dp_source.get_audio_state(),
                        (int)(s_btStarted ? 1 : 0),
                        (unsigned)s_inPcmHz,
#if USE_I2S_INPUT
                        (unsigned)s_ring_fill,
                        (unsigned)s_i2s_read_bytes,
                        (unsigned)s_ring_underrun_bytes,
                        (unsigned)s_ring_overrun_bytes,
                        (unsigned)s_pcm_peak,
                        (unsigned)s_pcm_dc_abs
#else
                        0u, 0u, 0u, 0u, 0u, 0u
#endif
    );
    return;
  }

  if (cmd == "SR") {
    if (arg.isEmpty()) {
      s_ctrlSerial.println("ERR missing hz");
      return;
    }
    const uint32_t hz = (uint32_t)arg.toInt();
    if (hz != 44100u && hz != 48000u) {
      s_ctrlSerial.println("ERR hz must be 44100 or 48000");
      return;
    }
    s_inPcmHz = hz;
#if USE_I2S_INPUT
    resamplerReset(hz);
#endif
    s_ctrlSerial.printf("OK sr=%u\n", (unsigned)hz);
    return;
  }

  if (cmd == "CONNECT") {
    if (arg.isEmpty()) {
      s_ctrlSerial.println("ERR missing name");
      return;
    }
    arg.toCharArray(s_sinkName, sizeof(s_sinkName));
    s_ctrlSerial.printf("OK connect '%s'\n", s_sinkName);
    btStart(s_sinkName);
    return;
  }

  if (cmd == "DISCONNECT") {
    s_ctrlSerial.println("OK disconnect");
    btStopGraceful(false);
    return;
  }

  if (cmd == "BT_OFF") {
    s_ctrlSerial.println("OK bt_off");
    btStopGraceful(true);
    return;
  }

  if (cmd == "BT_ON") {
    s_ctrlSerial.printf("OK bt_on '%s'\n", s_sinkName);
    btStart(s_sinkName);
    return;
  }

  if (cmd == "SLEEP") {
    s_ctrlSerial.println("OK sleep");
    delay(50);
    enterDeepSleep();
    return;
  }

  s_ctrlSerial.println("ERR unknown cmd (send HELP)");
#endif
}

void loop() {
#if CTRL_UART_ENABLE
  while (s_ctrlSerial.available()) {
    const char c = (char)s_ctrlSerial.read();
    if (c == '\r') continue;
    if (c == '\n') {
      handleCtrlLine(s_ctrlLine);
      s_ctrlLine = "";
      continue;
    }
    if (s_ctrlLine.length() < 160) s_ctrlLine += c;
  }
#endif

  // Auto-sleep when idle (default internal speaker mode).
  if (!s_btStarted && AUTO_SLEEP_IDLE_MS > 0) {
    const uint32_t now = millis();
    if ((uint32_t)(now - s_lastUseMs) >= AUTO_SLEEP_IDLE_MS) {
      Serial.println("[SLEEP] idle timeout");
      enterDeepSleep();
    }
  }

  static uint32_t last_stats_ms = 0;
  static uint32_t last_pcm_ms = 0;
  static uint32_t last_i2s_read_bytes = 0;
  static uint32_t last_a2dp_cb_calls = 0;
  static uint32_t last_ring_underrun_bytes = 0;
  static uint32_t last_ring_overrun_bytes = 0;
  const uint32_t now = millis();

  const uint32_t stats_period_ms = s_btStarted ? 1000UL : 10000UL;
  const uint32_t pcm_period_ms = s_btStarted ? 1000UL : 30000UL;

  if (HIGROW_LOG_TELEMETRY && (uint32_t)(now - last_stats_ms) >= stats_period_ms) {
    last_stats_ms = now;
#if USE_I2S_INPUT
    size_t fill = 0;
    portENTER_CRITICAL(&s_ring_mux);
    fill = s_ring_fill;
    portEXIT_CRITICAL(&s_ring_mux);

    const uint32_t i2s_read_bytes = s_i2s_read_bytes;
    const uint32_t a2dp_cb_calls = s_a2dp_cb_calls;
    const uint32_t underrun_bytes = s_ring_underrun_bytes;
    const uint32_t overrun_bytes = s_ring_overrun_bytes;

    const uint32_t di2s = i2s_read_bytes - last_i2s_read_bytes;
    const uint32_t dcb = a2dp_cb_calls - last_a2dp_cb_calls;
    const uint32_t dunder = underrun_bytes - last_ring_underrun_bytes;
    const uint32_t dover = overrun_bytes - last_ring_overrun_bytes;

    last_i2s_read_bytes = i2s_read_bytes;
    last_a2dp_cb_calls = a2dp_cb_calls;
    last_ring_underrun_bytes = underrun_bytes;
    last_ring_overrun_bytes = overrun_bytes;

    Serial.printf("[STATS] bt=%d conn=%d audio=%d ring=%u/%u i2s=+%uB (%u calls, %u zero, %u err, lastErr=%d) a2dp_cb=+%u underrun=+%uB overrun=+%uB\n",
                  (int)s_btStarted,
                  (int)a2dp_source.get_connection_state(),
                  (int)a2dp_source.get_audio_state(),
                  (unsigned)fill,
                  (unsigned)kPcmRingBytes,
                  (unsigned)di2s,
                  (unsigned)s_i2s_read_calls,
                  (unsigned)s_i2s_zero_reads,
                  (unsigned)s_i2s_err_reads,
                  (int)s_i2s_last_err,
                  (unsigned)dcb,
                  (unsigned)dunder,
                  (unsigned)dover);

    const bool audio_active = (a2dp_source.get_audio_state() == ESP_A2D_AUDIO_STATE_STARTED);
    if (audio_active && (uint32_t)(now - last_pcm_ms) >= pcm_period_ms) {
      last_pcm_ms = now;
      Serial.printf("[PCM] peak=%u dc_abs=%u\n",
                    (unsigned)s_pcm_peak,
                    (unsigned)s_pcm_dc_abs);
    }
#else
    Serial.printf("[STATS] conn=%d audio=%d running (tone)\n",
                  (int)a2dp_source.get_connection_state(),
                  (int)a2dp_source.get_audio_state());
#endif
  }
  delay(10);
}

