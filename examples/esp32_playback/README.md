# ESP32 SSC playback example

The smallest complete use of the decoder on an ESP32. The app decodes SSC streams stored in flash and plays them
through a PCM5102A DAC, looping over the three main modes:

| Clip | Mode | Decoder | I2S |
|---|---|---|---|
| `mix_192000.ssc` | SSC 44.1 kHz, 192 kbps | `ssc_decoder_create(44100)` | 44.1 kHz, 32-bit |
| `hifi48_192000.ssc` | Seamless HiFi 48 kHz / 24-bit | `ssc_decoder_create(48000)` | 48 kHz, 32-bit |
| `uhq96_584000.ssc` | UHQ v2 96 kHz, 584 kbps | `ssc_decoder_create(96000)` | 96 kHz, 32-bit |

The clips are frames from Samsung's own encoder (from [`test_vectors/`](../../test_vectors)), embedded with
`EMBED_FILES`.

## Wiring

| ESP32 | PCM5102A |
|---|---|
| GPIO 27 | BCK |
| GPIO 25 | LRCK |
| GPIO 26 | DIN |
| GND | SCK, GND |
| 3V3 | VIN |

Change the pins at the top of [`main/playback.c`](main/playback.c).

## Build and run

```bash
. ../../firmware/esp-idf/export.sh            # Windows: ..\..\firmware\esp-idf\export.ps1
idf.py set-target esp32
idf.py -p PORT build flash monitor
```

The log shows each clip's bitrate, frame size, decoder memory and decode time:

```
I (...) ssc_playback: UHQ 96 kHz: 96000 Hz, decoder uses ... bytes, free heap ...
I (...) ssc_playback: UHQ 96 kHz: 584 kbps, 22 coded bands, 660-byte frames
I (...) ssc_playback: UHQ 96 kHz: 333 frames, 0 errors, decode avg ... us / worst ... us per 9000 us frame (...% of one core)
```

## How it works

1. `CMakeLists.txt` adds the decoder component with `EXTRA_COMPONENT_DIRS`, and `main/CMakeLists.txt` requires
   `ssc_decoder`.
2. For each clip: create a decoder for the clip's rate, set the I2S clock (APLL for exact 44.1/48/96 kHz), decode
   frame by frame with `ssc_decode_s32()`, and write to I2S. The blocking write paces decoding to real time.
3. Destroy the decoder after each clip, which frees all of its memory.

To receive SSC over Bluetooth instead of from flash, see [docs/integration.md](../../docs/integration.md).
