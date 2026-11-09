# SSC on the ESP32 sink

How the firmware in [`firmware/native_bt/pure_sink`](../firmware/native_bt/pure_sink) receives and plays SSC. For
the overall firmware architecture (tasks, buffering, drift handling, pairing) see the
[pure_sink README](../firmware/native_bt/pure_sink/README.md).

## Where the code is

| File | Role |
|---|---|
| [`ssc_decoder/`](../ssc_decoder) | The decoder library (shared with the host tools) |
| [`firmware/native_bt/codecs/ssc/dbt_ssc_codec.c`](../firmware/native_bt/codecs/ssc/dbt_ssc_codec.c) | Codec adapter: A2DP configuration, packet splitting, decoder lifetime |
| [`firmware/native_bt/esp32_probe/components/ssc_decoder/`](../firmware/native_bt/esp32_probe/components/ssc_decoder) | ESP-IDF component that builds the two above |
| [`firmware/native_bt/pure_sink/main/a2dp_sink.c`](../firmware/native_bt/pure_sink/main/a2dp_sink.c) | Registers the SSC endpoint and capability `… 03 01 3E` |

## Mode → output format

| Configured flags | Mode | Decoder mode | PCM to I2S |
|---|---|---|---|
| `0x20` (+`0x08`) | SSC 44.1 kHz | 44.1 kHz | 44.1 kHz, 16-bit |
| `0x10` (+`0x08`) | SSC 48 kHz | 44.1 kHz mode, played at 48 kHz | 48 kHz, 16-bit |
| `0x04` (+`0x08`) | HiFi | 44.1 kHz mode, played at 48 kHz | 48 kHz, 32-bit |
| `0x02` (+`0x08`) | UHQ v2 | 96 kHz | 96 kHz, 32-bit |
| `0x80` | legacy UHQ | 96 kHz | 96 kHz, 32-bit |

## Packets

Each media packet holds one or two frames. The adapter checks that the payload is a whole number of valid frames
(sizes from `ssc_frame_bytes()`), then decodes and hands out one frame at a time. If the audio output is still
holding the previous frame's PCM, the adapter returns `PENDING` and continues with the second frame when called
again. A malformed packet is dropped and the decoder is reset.

## Memory

Nothing is allocated until the phone opens an SSC stream. Then:

* the decoder state: about 44 KB in 6 blocks, the largest about 15 KB, so it fits a fragmented heap;
* one frame of PCM (up to 6.9 KB for 32-bit stereo);
* the queue of received packets.

Together this is the codec's working set: **65,572 bytes** for SSC. All tables are `const` and stay in flash.
Everything is freed when the stream closes or the phone switches codec. Heap measured on the board (original
ESP32, no PSRAM):

| Moment | Free heap | Largest free block |
|---|---|---|
| Connected, no codec open | 119,692 B | 110,592 B |
| SSC opened | 54,088 B | 47,104 B |
| Streaming UHQ v2 96 kHz, 584 kbps | 45,648 B | 43,008 B |

## CPU

The decoder runs on core 0 at priority 19, and the I2S writer runs on core 1. Measured with
[`examples/esp32_benchmark`](../examples/esp32_benchmark) at 240 MHz:

| Stream | Average / worst frame | Real-time share |
|---|---|---|
| 44.1 kHz, 192 kbps | 4.7 / 6.9 ms | 24 % |
| HiFi 48 kHz, 192 kbps | 4.5 / 5.8 ms | 25 % |
| UHQ 96 kHz, 584 kbps | 4.4 / 5.5 ms | 49 % |

## Console output

```
I (39562) a2dp: configured: SSC 48000 Hz S32 2 ch, mode HiFi (48 kHz/24-bit), unlimited bitrate, flags 0x0c
I (39562) a2dp: codec opened: SSC, decoder working set 65572 bytes
I (39635) a2dp: reconfigured: SSC 96000 Hz S32 2 ch, mode UHQ v2 (96 kHz/24-bit), unlimited bitrate, flags 0x0a
I (59267) a2dp: stream start: SSC 96000 Hz S32 2 ch, mode UHQ v2 (96 kHz/24-bit)
I (60324) a2dp: SSC stream: bitrate 584 kbps (index 0x03)
I (69272) pure_sink: heap free 45648 min 44964 largest 43008 | ... | codec SSC 65572 B | ring 74/85 ms | underruns 0
```
