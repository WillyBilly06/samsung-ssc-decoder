# Using the SSC decoder in your ESP-IDF project

This page shows how to add the decoder to an ESP-IDF project, decode frames, and feed it from a Bluetooth A2DP sink.
Two working examples go with it:

* [`examples/esp32_playback`](../examples/esp32_playback): decode SSC from flash and play it over I2S (smallest example).
* [`firmware/native_bt/pure_sink`](../firmware/native_bt/pure_sink): a complete A2DP speaker that receives SSC from a
  Galaxy phone.

## 1. Add the component

The [`ssc_decoder/`](../ssc_decoder) folder is an ESP-IDF component (`CMakeLists.txt` + `idf_component.yml`). Pick one
of these:

**Component manager** (`main/idf_component.yml` of your project):

```yaml
dependencies:
  ssc_decoder:
    git: https://github.com/WillyBilly06/samsung-ssc-decoder.git
    path: ssc_decoder
```

**Local copy or git submodule** (top-level `CMakeLists.txt`, before `include(project.cmake)`):

```cmake
set(EXTRA_COMPONENT_DIRS "path/to/samsung-ssc-decoder/ssc_decoder")
```

**Copy the folder** into your project's `components/` directory.

Then require it from the component that uses it:

```cmake
idf_component_register(SRCS "main.c" PRIV_REQUIRES ssc_decoder)
```

The component compiles with `-O2 -fsingle-precision-constant -fno-math-errno`. The ESP32 FPU is single precision,
so keep those flags if you compile the sources another way.

## 2. Decode frames

```c
#include "ssc_decoder.h"

ssc_decoder *dec = ssc_decoder_create(48000);            // 44100/48000 (one mode) or 96000
static int32_t pcm[2 * SSC_FRAME_SAMPLES];               // 864 stereo samples, 6912 bytes

int n = ssc_decode_s32(dec, frame, frame_len, pcm);      // returns 864, or < 0 (pcm = silence)
i2s_channel_write(tx, pcm, sizeof pcm, &written, portMAX_DELAY);   // 32-bit I2S slots
```

* `ssc_decode_s32()` puts 16-bit full scale at `1 << 31`, so the output goes straight to a 32-bit I2S slot and keeps
  the 24-bit detail of HiFi/UHQ. For 16-bit output use `ssc_decode()`.
* Memory: `ssc_decoder_create()` allocates about 44 KB in 6 blocks (the largest about 15 KB). Use
  `ssc_decoder_create_with_allocator()` to choose the heap, e.g. `heap_caps_malloc(size, MALLOC_CAP_INTERNAL)`.
  Create the decoder when a stream opens and destroy it when it closes, so it only uses RAM while SSC is playing.
* CPU on an ESP32 at 240 MHz: about 4.5 ms per frame in every mode (25 % of a core at 44.1/48 kHz, 49 % at
  96 kHz). Run the decoder in its own task, and let the I2S write block to pace it.

## 3. Feed it from an A2DP sink

Your Bluetooth stack must let you register a **vendor-specific stream endpoint** (codec type `0xFF`). The
`native_bt` stack in [`firmware/`](../firmware) does, and the steps below are what it does.

### Advertise SSC

Add a sink endpoint whose Media Codec capability is:

```
00 FF 75 00 00 00 03 01 3E      media type, codec type 0xFF, vendor 0x00000075, codec 0x0103, flags
```

Flags `0x3E` offer every mode. Include the Delay Reporting capability, keep your SBC endpoint, and do not add Content
Protection. Details: [a2dp.md](a2dp.md).

### Map the configuration to decoder and output

When the phone sends SET_CONFIGURATION, look at the flags byte (the 9th byte of the codec element):

| Flags | Mode | `ssc_decoder_create()` | Output | Decode with |
|---|---|---|---|---|
| `0x02` set | UHQ v2 | `96000` | 96 kHz, 32-bit | `ssc_decode_s32` |
| `0x04` set | HiFi | `48000` | 48 kHz, 32-bit | `ssc_decode_s32` |
| `0x20` | SSC | `44100` | 44.1 kHz, 16-bit | `ssc_decode` |
| `0x10` | SSC | `48000` | 48 kHz, 16-bit | `ssc_decode` |
| `0x80` | legacy UHQ | `96000` | 96 kHz, 32-bit | `ssc_decode_s32` |

Check them in that order: HiFi and UHQ configurations have no sample-rate bit.

### Decode each media packet

A media packet is an RTP header followed by one or two SSC frames, with no codec header in between. Frames have no
length field, so split them with `ssc_frame_bytes()`:

```c
void on_media_packet(const uint8_t *pkt, int len) {
    int rtp = 12 + 4 * (pkt[0] & 0x0F);                 // RTP header + CSRCs
    const uint8_t *p = pkt + rtp;
    int left = len - rtp;
    while (left > 0) {
        int n = ssc_frame_bytes(p, left, decoder_rate);  // size from the frame header
        if (n <= 0 || n > left) {                        // corrupt: drop the rest, reset history
            ssc_decoder_reset(dec);
            return;
        }
        if (ssc_decode_s32(dec, p, n, pcm) == SSC_FRAME_SAMPLES)
            pcm_ring_write(pcm, SSC_FRAME_SAMPLES);      // your PCM buffer -> I2S task
        p += n;
        left -= n;
    }
}
```

* The bitrate (and frame size) can change on any frame. That is normal: the phone lowers it when the radio link
  is busy. No reset is needed.
* Decode in a separate task from the Bluetooth host. Queue the received packets (they are 8–10× smaller than
  PCM, so queue compressed data rather than PCM when you need more buffering). Keep enough PCM ahead of the I2S DMA
  to cover one frame of decode time.
* Report your buffer delay with AVDTP Delay Reporting so video stays in sync.

The full implementation is in
[`firmware/native_bt/codecs/ssc/dbt_ssc_codec.c`](../firmware/native_bt/codecs/ssc/dbt_ssc_codec.c) (configuration,
packet splitting, decoder lifetime) and
[`firmware/native_bt/pure_sink/main/`](../firmware/native_bt/pure_sink/main) (endpoint, decoder task, I2S output).
