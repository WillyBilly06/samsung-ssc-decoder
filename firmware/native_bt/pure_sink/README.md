# pure_sink — BDK SPEAKER pure playback firmware

A minimal Bluetooth Classic A2DP speaker for the original ESP32 (4 MB flash, no
PSRAM, PCM5102A DAC) built on the in-house `native_bt` host stack. It only
plays audio: there is no DSP, BLE, sound effects, diagnostics or JSON output.

## Architecture

```
phone ──BR/EDR──> ESP32 controller (VHCI, core 1)
                     │ HCI
                     ▼
          host owner task (app_main, core 1, prio 20)  ── single event loop
          ├─ native_bt runtime: HCI, ACL, L2CAP, HCI flow control
          ├─ GAP: "BDK SPEAKER", CoD 0x240414, discoverable/connectable
          ├─ Classic security: SSP just works, link keys in NVS ("native_bonds")
          ├─ SDP server: A2DP Sink + AVRCP CT/TG records
          ├─ AVRCP: passthrough keys, absolute volume (target), metadata/play status
          └─ A2DP/AVDTP acceptor ── encoded media queue (codec worker)
                                          │
          decoder task (core 0, prio 19)  ▼  dbt_codec_worker_step()
          SBC / AAC / aptX / aptX HD / aptX LL / LDAC / Opus / Opus05 / SSC
                                          │ PCM (S16/S24/S32, mono/stereo), copied
                                          ▼
                          PCM ring buffer (64 KB, source format)
                                          │
          I2S writer task (core 1, prio 21)  ▼  convert to 32-bit stereo slots
                              × volume (one multiply, skipped at 100 %)
                              ± 1-frame drift slip
                                          ▼
                 I2S1 std Philips, master, APLL → PCM5102A
                 BCLK GPIO27, LRCK GPIO25, DATA GPIO26 (no MCLK)
```

* **Back-pressure**: the worker runs in deferred-PCM mode. The PCM hook copies
  one ≤256-frame chunk into the ring or returns `DBT_BUSY`; the writer wakes the
  decoder when it frees space. HCI receive credits are limited to free encoded
  queue slots while streaming, so a full ring
  throttles the phone instead of dropping packets.
* **Stream control**: the worker's stream hook (START/SUSPEND/CLOSE, decoder
  task) only publishes a request; the writer applies it (flush, re-create the
  I2S channel when the sample rate changes, 44.1/48/88.2/96 kHz) and
  acknowledges, and the producer gets `BUSY` until then.
* **Prefill**: playback (re)starts when 60 ms are buffered (ring + I2S DMA).
  An empty ring is an underrun: the DMA plays zeros (`auto_clear`) and the
  writer re-buffers 60 ms. This 60 ms is also the AVDTP delay report.
* **Clock drift**: no resampler. The writer keeps a slow moving average of the
  ring level (~2.5 s time constant, so Bluetooth bursts are ignored). Only when
  the average is more than 12 ms off its set-point does it drop or repeat one
  stereo frame, at most once per 4 I2S chunks (≈50 frames/s capacity, ~1000 ppm,
  versus <100 ppm real drift). Typical result: one inaudible single-sample slip
  every few seconds to minutes, no oscillation, no audible pitch change.
* **Volume**: AVRCP absolute volume 0..127 maps to mute / −50 dB … 0 dB
  (dB-linear), applied as one integer multiply per sample.
* **Pairing**: discoverable whenever no phone holds an encrypted link; always
  connectable so bonded phones reconnect. New pairing is accepted while no
  other phone is connected. Up to 8 bonds; the 9th replaces the oldest slot.
* **Button (GPIO0/BOOT)**: short press = AVRCP play/pause; hold 3 s = pairing
  mode for 120 s (disconnects the current phone and stays discoverable).

## Files

| File | Purpose |
| --- | --- |
| `CMakeLists.txt` | Project; pulls the shared components from `../esp32_probe/components` via `EXTRA_COMPONENT_DIRS`, sets `DBT_PROBE_SSC_BUILD=ON` |
| `sdkconfig.defaults` | Controller-only BTDM, core layout, 4 MB flash, UART0 921600, `I2S_ISR_IRAM_SAFE` |
| `partitions.csv` | nvs 0x9000/0x6000 (unchanged → bonds survive), phy_init 0xf000, factory app 0x10000 (3.9 MB) |
| `main/main.c` | Controller + VHCI transport, runtime/GAP setup, host owner loop, visibility policy, button, logging |
| `main/bt_security.c` | Classic SSP policy and NVS bond store (`native_bonds` namespace) |
| `main/bt_sdp.c` | SDP records (A2DP Sink, AVRCP CT/TG) |
| `main/a2dp_sink.c` | Endpoints, validators and codec ops, codec worker, decoder task, codec logging, credit policy, delay reports |
| `main/sink_avrcp.c` | AVRCP controller/target: keys, absolute volume, metadata |
| `main/audio_out.c` | PCM ring, I2S writer, volume, drift handling |
| `main/button.c` | GPIO0 short/long press |


## Build and flash

From the repository root (Windows PowerShell):

```powershell
firmware\esp-idf\install.ps1 esp32        # once
tools\build_pure_sink.ps1                  # builds and checks the image fits the factory partition
tools\flash_pure_sink.ps1 -Port COM7       # keeps NVS, so pairings survive
```

Linux / macOS:

```bash
./firmware/esp-idf/install.sh esp32        # once
. ./firmware/esp-idf/export.sh
idf.py -C firmware/native_bt/pure_sink build
idf.py -C firmware/native_bt/pure_sink -p /dev/ttyUSB0 flash monitor
```

The shared `native_bt` component points the SDK's size check at an `ota_1` slot this layout does not have. That
prints a harmless warning; the build script checks the image against the factory partition instead.

Do **not** use `erase_flash` if you want to keep pairings: NVS is at 0x9000–0xEFFF. There is no `otadata`
partition, so the bootloader always starts the factory app at 0x10000. Console: 921600 baud
(`idf.py monitor -b 921600`, or `python tools/serial_log.py COM7 60 log.txt`).

## Log lines

* boot / BT ready / codec open / codec close: `heap[...]: free, min, largest`
* connect, disconnect, pairing, bond stored, discoverability changes
* `configured:` / `codec changed:` / `reconfigured:` / `stream start:` with codec,
  rate, S16/S24/S32, channels, decoded fields and the raw CIE in hex
* in-band: SBC bitpool, LDAC frame size → kbps/quality, SSC bitrate index → kbps
  (logged on change, at most once per second)
* AVDTP state changes, I2S start/stop, underruns (rate limited)
* AVRCP connect, absolute volume, play status, now playing
* every 10 s while streaming (60 s idle): heap, decoder/I2S/host stack free,
  codec working set, ring level/target, volume, underruns, drift slips

## Intentionally left out

BLE (advertising, GATT, LE SMP, BLE control/OTA), RFCOMM/SPP, HFP/SCO, LHDC,
EQ/limiter/FIR/dither/volume ramps, DSP assist, startup sounds, LEDs, SPIFFS,
OTA and rollback, signal analysis, profilers, ingress/dispatch traces, JSON
diagnostics, speaker-initiated reconnection to the last phone (the phone
reconnects to the bonded speaker; tap it in the phone's Bluetooth list or let
the phone auto-connect), multi-point, and a resampler (drift is handled by
single-frame slips).

The controller still runs in dual mode (`BTDM`) because the host's controller
bring-up requires it (`DBT_FAULT_NOT_DUAL_MODE`) and the GAP setup writes LE
advertising data/parameters; advertising is never enabled.
