# SSC decoder library

Portable C99 decoder for Samsung Scalable Codec / Samsung Seamless Codec frames. It has no dependencies beyond
`libm`. The tables are `const`, so they stay in flash on microcontrollers.

## Files

| File | Contents |
|---|---|
| `ssc_decoder.h` | Public API |
| `ssc_decoder.c` | Header parsing, frame control flow, output conversion |
| `ssc_ec.c`, `ssc_ec.h` | Range decoder (CELT entropy coder, symbols prefixed `ssc_ec_`) |
| `ssc_energy.c` | Coarse, fine and final band energy |
| `ssc_alloc.c` | Bit allocation |
| `ssc_bands.c`, `ssc_cwrs.c` | Band shapes: PVQ, stereo, split bands, CWRS index decoding |
| `ssc_synth.c`, `ssc_twiddle.c` | Denormalisation, IMDCT (radix 2/3/4 FFT), window, overlap-add, post-filter |
| `ssc_tables.c`, `ssc_tables.h` | Mode tables: bands, caps, pulse cache, allocation vectors |
| `ssc_internal.h` | Shared internal helpers |
| `COPYING.opus` | libopus license (parts are derived from libopus 1.1) |

## Use it in your project

Add the `.c` files to your build and put this folder on the include path. Recommended flags for speed:
`-O2 -fno-math-errno`, plus `-fsingle-precision-constant` on MCUs with a single-precision FPU.

```c
#include "ssc_decoder.h"

ssc_decoder *dec = ssc_decoder_create(96000);
if (!dec) { /* bad rate or out of memory */ }

/* One A2DP packet may hold two frames at 96 kHz. */
int pos = 0;
while (pos < payload_len) {
    int n = ssc_frame_bytes(payload + pos, payload_len - pos, 96000);
    if (n <= 0 || n > payload_len - pos) break;          /* not a valid frame */
    int32_t pcm[2 * SSC_FRAME_SAMPLES];
    if (ssc_decode_s32(dec, payload + pos, n, pcm) == SSC_FRAME_SAMPLES)
        output(pcm, SSC_FRAME_SAMPLES);
    pos += n;
}

ssc_decoder_destroy(dec);
```

## API reference

### Lifetime

```c
ssc_decoder *ssc_decoder_create(int sample_rate);
ssc_decoder *ssc_decoder_create_with_allocator(int sample_rate, ssc_alloc_fn alloc, ssc_free_fn release, void *ctx);
void ssc_decoder_destroy(ssc_decoder *dec);
void ssc_decoder_reset(ssc_decoder *dec);
int  ssc_decoder_size(void);
```

* `sample_rate`: `44100` or `48000` (both use the 44.1 kHz codec mode), or `96000`.
* The decoder allocates 6 blocks (about 44 KB in total, the largest about 15 KB) and nothing else.
  `ssc_decoder_size()` returns the total. Pass your own `alloc`/`release` to place them in a specific heap.
* `ssc_decoder_reset()` clears history. Use it after lost packets or a stream restart.

### Decoding

```c
int ssc_decode      (ssc_decoder *dec, const uint8_t *frame, int len, int16_t *pcm);
int ssc_decode_s32  (ssc_decoder *dec, const uint8_t *frame, int len, int32_t *pcm);
int ssc_decode_float(ssc_decoder *dec, const uint8_t *frame, int len, float   *pcm);
```

* `frame` points at the `0xFF` header byte, and `len` is the frame size.
* Output: 864 interleaved stereo samples (1728 values). Returns `864`, or a negative value on error, in which
  case the output is silence.
* `ssc_decode_s32` scales 16-bit full scale to `1 << 31` and keeps the extra precision of the 24-bit modes.
  `ssc_decode_float` outputs `[-1, 1)`.
* Output lags input by 108 samples.

### Frame helpers

```c
int ssc_frame_bytes(const uint8_t *frame, int avail, int sample_rate);
int ssc_parse_frame_info(const uint8_t *frame, int len, ssc_frame_info *info);
```

* `ssc_frame_bytes`: the size of the frame starting at `frame`, worked out from its header (frames carry no
  length field). Returns 0 for an invalid header.
* `ssc_parse_frame_info`: fills `bitrate` (bit/s), `hq`, `seq` and `end_band`. Returns 0 if the header is valid.

### Build options

| Define | Effect |
|---|---|
| `SSC_DEBUG_TELL` | Exposes `ssc_debug_tell` (bit position after each frame) for bitstream checks |
| `SSC_PROFILE` | Calls `ssc_prof_mark(stage)` at each decoder stage, which you implement (used by the ESP32 benchmark) |

The format itself is described in [../docs/bitstream.md](../docs/bitstream.md).
