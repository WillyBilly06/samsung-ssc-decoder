# Firmware

| Folder | Contents |
|---|---|
| [`native_bt/pure_sink/`](native_bt/pure_sink) | **The speaker firmware.** A2DP sink + AVRCP + I2S output, Classic Bluetooth only |
| [`native_bt/src`](native_bt/src), [`native_bt/include`](native_bt/include) | The Bluetooth host stack: HCI, L2CAP, GAP, SDP, AVDTP/A2DP, AVCTP/AVRCP, security |
| [`native_bt/codecs/`](native_bt/codecs) | Codec adapters: SSC, LDAC, aptX, AAC, SBC, Opus, plus the shared codec worker |
| [`native_bt/esp32_probe/components/`](native_bt/esp32_probe/components) | ESP-IDF components that build the stack and codecs |
| [`bt_audio_sink/main/dsp/`](bt_audio_sink/main/dsp) | Fixed-point helpers the stack's audio module compiles |
| [`esp-idf/`](esp-idf) | Espressif ESP-IDF v6.1 (Apache-2.0) |

The firmware talks to Espressif's Bluetooth controller directly over HCI (VHCI). It does not use Bluedroid or
NimBLE.

## Build

Windows (PowerShell), from the repository root:

```powershell
firmware\esp-idf\install.ps1 esp32
tools\build_pure_sink.ps1
tools\flash_pure_sink.ps1 -Port COM7
```

Linux / macOS:

```bash
./firmware/esp-idf/install.sh esp32
. ./firmware/esp-idf/export.sh
idf.py -C firmware/native_bt/pure_sink build
idf.py -C firmware/native_bt/pure_sink -p /dev/ttyUSB0 flash monitor
```

The bundled ESP-IDF has one local fix: `tools/cmake/ldgen.cmake` no longer splits linker-fragment paths that
contain spaces, so the repository can live in a folder like `My Projects`.

Details of the firmware design: [native_bt/pure_sink/README.md](native_bt/pure_sink/README.md). How SSC is
plugged in: [../docs/esp32-sink.md](../docs/esp32-sink.md).
