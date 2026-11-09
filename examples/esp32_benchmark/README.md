# ESP32 decoder benchmark

An ESP-IDF app that decodes real SSC frames (from Samsung's encoder, embedded in flash) and reports the time spent
in each decoder stage. It needs no Bluetooth and no DAC: any ESP32 board works.

| Embedded stream | Mode |
|---|---|
| `f44_192k.ssc` | SSC 44.1 kHz, 192 kbps |
| `hifi48_192k.ssc` | HiFi 48 kHz / 24-bit, 192 kbps |
| `uhq96_584k.ssc` | UHQ v2 96 kHz, 584 kbps |

## Run

```bash
. ../../firmware/esp-idf/export.sh        # Windows: ..\..\firmware\esp-idf\export.ps1
idf.py -p PORT build flash monitor
```

## Results (ESP32 at 240 MHz)

| Stream | Average / worst per frame | Frame duration | Share of one core |
|---|---|---|---|
| 44.1 kHz, 192 kbps | 4.7 / 6.9 ms | 19.6 ms | 24 % |
| HiFi 48 kHz, 192 kbps | 4.5 / 5.8 ms | 18 ms | 25 % |
| UHQ 96 kHz, 584 kbps | 4.4 / 5.5 ms | 9 ms | 49 % |

Stages reported: header + coarse energy, allocation, fine energy + PVQ, final energy + anti-collapse,
denormalisation, IMDCT and post-filter.
