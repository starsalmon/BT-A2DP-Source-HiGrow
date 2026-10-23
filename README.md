## BT-A2DP-Source-HiGrow: Bluetooth companion for yoRadio Fusion

Classic-Bluetooth ESP32 firmware that gives an ESP32-S3 yoRadio (no Classic BT) A2DP in both directions:

- **TX / send**: I2S slave input from the radio -> A2DP source to BT headphones/speaker (`CONNECT <name>`).
  A2DP is fixed at 44.1 kHz; with `SR 48000` the bridge resamples 48k -> 44.1k.
- **RX / receive**: A2DP sink for a phone (`MODE RX <name>`), I2S master into the radio's DAC after
  the bus handover. Title/artist go back to the radio as `META` lines.

Controlled by the radio over UART. Command set and handover sequence:
[`docs/BT_COMPANION.md`](https://github.com/starsalmon/yoRadio-Fusion-ProS3/blob/cursor/multi-device/docs/BT_COMPANION.md)
in yoRadio-Fusion-ProS3 (moves to `main` with that branch). Send `HELP` for the list on the device.

Needs an original ESP32 (BR/EDR). ESP32-S3 / C3 / C6 can't do A2DP.

### Boards / envs

| Env | Board | Pins | Used with |
|-----|-------|------|-----------|
| `higrow_a2dp_source` (default pin map) | LilyGo HiGrow (`esp32dev`) | UART RX 39 / TX 13, wake 38, I2S BCK 17 / WS 23 / DATA 19 | ProS3 radio |
| `nodemcu32s_bt_companion` | NodeMCU-32S (`-DCOMPANION_BOARD_NODEMCU32S=1`) | UART TX 17 / RX 16, wake 33, I2S BCK 26 / WS 25 / DATA 22 (**proposed, TBD** pending wiring) | April's Speaker |

Pins live in [`include/options.h`](include/options.h); the Node block only overrides them when
`COMPANION_BOARD_NODEMCU32S` is set, so the HiGrow build is unchanged. Bluetooth receive name: "yoRadio Fusion"
(HiGrow) / "April's Speaker" (Node). Default send target `A2DP_SINK_NAME` "HiFi-IIS" (the radio sends
its own with `CONNECT`). UART 115200; wake is EXT0, level high.

### Build / upload

```bash
pio run -e higrow_a2dp_source
pio run -e higrow_a2dp_source -t upload --upload-port <PORT>
pio run -e nodemcu32s_bt_companion
```

- **HiGrow uploads at 115200** (`upload_speed` and `--baud`): 921600 fails on this board/cable.
  The Node env uses 460800.
- This project shares the pioarduino framework under `~/.platformio` with yoRadio. Building it
  reinstalls the stock framework, so the next yoRadio build re-applies its patches. Check yoRadio's
  `[fw-guard] ... (OK)` lines before flashing the radio.
- Keep `src/idf_component.yml` local (not committed).

### Memory (fix10)

The TX PCM ring (`PCM_RING_BYTES`, 64 KB) used to be a static array, i.e. 64 KB of DRAM held
permanently. In RX mode that starved Bluedroid/the A2DP sink (`host_recv_pkt_cb` malloc assert,
reboot ~4 s after the phone connected). Since fix10 the ring is malloc'd only while TX runs and freed
when it stops. BLE controller memory is released at boot (Classic only), and `[HEAP]` lines are
logged at boot, after sink start, and in the 1 s `[RX]` line.

### Pending

- **fix11** (`6760f4b`, local branch `cursor/bt-a2dp-sink-rx-fe2d`, not on main): album and track
  length metadata, AVRCP play status + position, UART `META album=` and `POS <elapsed_ms> <total_ms> <playing>`.
- Node ESP32 pins for April's Speaker are a proposal until the wiring is confirmed.

### Other options (`include/options.h`)

`USE_I2S_INPUT 0` = test sine instead of the I2S bridge; `AUTO_SLEEP_IDLE_MS` (30 s in TX with BT off
and no UART activity); `AUTO_CONNECT_ON_BOOT`; `HIGROW_LOG_*` log switches.
