# Test vectors

Real SSC frames produced by **Samsung's own encoder** (`libScalable_Encoder.so` on a Galaxy S26 Ultra) from the
test signals in `input/`. Use them to test a decoder without a phone.

## File format

`ssc/*.ssc`: a sequence of `[u16 little-endian length][SSC frame]`. Inputs are raw little-endian 16-bit PCM.

| Input | Signal |
|---|---|
| `input/mix.s16le` | Mixed test signal, 44.1 kHz stereo |
| `input/mix48k.s16le` | Mixed test signal, 48 kHz stereo |
| `input/mix96k.s16le` | Mixed test signal, 96 kHz stereo |
| `input/sine.s16le` | Sine tones, 44.1 kHz stereo |
| `input/mono.s16le` | Mono signal, 44.1 kHz |

## Streams

| Files | Source | Encoder setting | Decode with |
|---|---|---|---|
| `mix_<bitrate>.ssc` | `mix.s16le` | 44.1 kHz, 16-bit, every bitrate from 88 000 to 886 000 | `44100` |
| `mix24_<bitrate>.ssc` | `mix.s16le` | 44.1 kHz, 24-bit input path | `44100` |
| `mix48_192000.ssc` | `mix.s16le` | 48 kHz encoder setting, 16-bit (44.1 kHz input) | `48000` |
| `hifi48_192000.ssc` | `mix48k.s16le` | HiFi: 48 kHz, 24-bit | `48000` |
| `mix96_192000.ssc` | `mix.s16le` | 96 kHz mode, 192 kbps (44.1 kHz input) | `96000` |
| `uhq96_291000.ssc`, `uhq96_584000.ssc` | `mix96k.s16le` | UHQ v2: 96 kHz, 24-bit | `96000` |
| `sine_<bitrate>.ssc` | `sine.s16le` | 44.1 kHz, 16-bit | `44100` |
| `mono_192000.ssc` | `mono.s16le` | mono input | `44100` |

A few frames in the `uhq96_*` files are one byte shorter than their header says. The capture tool trimmed a trailing
`0xA5` byte, so expect one size mismatch per affected frame. The audio still decodes.

```bash
build/ssc_dec_tool test_vectors/ssc/uhq96_584000.ssc out.s16le 96000
```
