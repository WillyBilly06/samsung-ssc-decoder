# SSC bitstream specification

This page describes the Samsung Scalable Codec (SSC) frame format as implemented by [`ssc_decoder/`](../ssc_decoder). It is
written for someone building a decoder or encoder. Everything here was checked against Samsung's encoder: our
decoder ends every frame at the same bit position the encoder wrote.

**Contents:** [Overview](#overview) · [Codec modes](#codec-modes) · [Frame layout](#frame-layout) ·
[Bitrates and frame sizes](#bitrates-and-frame-sizes) · [Payload](#payload) ·
[Differences from Opus CELT](#differences-from-opus-celt-11) · [Output](#decoder-output)

## Overview

SSC is a transform codec derived from **CELT as found in Opus 1.1** (the fixed-point build). It keeps CELT's
toolbox: MDCT, band energies, pyramid vector quantisation (PVQ) of band shapes, a range coder and a pitch
post-filter. Samsung changed the frame size, band layout, allocation tables, header and bitrate range, so the
stream is not Opus-compatible.

| Property | Value |
|---|---|
| Samples per frame | 864 per channel |
| Frame duration | 19.6 ms at 44.1 kHz · 18 ms at 48 kHz · 9 ms at 96 kHz |
| Channels | always coded as stereo (C = 2) |
| Transform | MDCT, 864 coefficients, overlap 108; transient frames use 8 short MDCTs of 108 |
| Bands | 22 |
| Entropy coding | CELT range coder (`EC_CODE_BITS = 24`) |
| Bitrates | 88 – 886 kbps (14 fixed rates) |
| Algorithmic delay | 108 samples |

## Codec modes

Two static modes exist. 48 kHz streams use the 44.1 kHz mode unchanged; only the playback rate differs.

| Mode | Used for | Band edges (`eBands`, in units of 8 MDCT bins) |
|---|---|---|
| 44.1 kHz | 44.1 kHz and 48 kHz | 0 1 2 3 4 5 6 7 8 10 12 14 16 20 24 28 34 40 48 60 72 84 102 |
| 96 kHz | 96 kHz | 0 1 2 3 4 5 6 7 8 9 10 11 12 14 16 20 24 30 36 54 72 90 108 |

`LM` is always 3 (8 short blocks). The band energy means (`eMeans`) are CELT's. Each mode has its own caps table,
pulse cache and **two** 11 × 22 allocation-vector tables. Frames above 192 kbps use the second table. The tables
are in [`ssc_decoder/ssc_tables.c`](../ssc_decoder/ssc_tables.c).

## Frame layout

```
 byte 0    byte 1    byte 2    byte 3    byte 4 ...
+--------+---------+---------+---------+-------------------------------+
|  0xFF  | family  | seq/side|  rate   |  range-coded payload          |
+--------+---------+---------+---------+-------------------------------+
```

There is no length field. The frame size follows from the bitrate code (see below), so a packet holding several
frames can be split with `ssc_frame_bytes()`.

| Byte | F-family rates (88 – 256, 328 kbps) | E-family rates (152, 250, 291 – 886 kbps) |
|---|---|---|
| 0 | `0xFF` | `0xFF` |
| 1 | `0xEE` | `0xE0` + `seq[7:4]` |
| 2 | `seq[3:0]` in bits 7–4, side bits in 3–0 | `seq[3:0]` in bits 7–4, `reduce` in bits 3–2, side bits in 1–0 |
| 3 | rate code, `& 0x1F` = `0x10`…`0x16` | rate code, `& 0x1F` = `0x00`…`0x06` |

* **Sequence number:** 4 bits for the F-family, 8 bits for the E-family. It increments per frame.
* **End band:** F-family frames code all 22 bands. E-family frames code `22 − reduce` bands, except 152 kbps
  (`0xE6`), which always codes 18. Bands above the end band are silent.
* The top three bits of byte 3 are `0xE0`/`0xF0` in most streams, and `0x00` in some 16-bit E-family streams.
  Decode with `byte3 & 0x1F`.

## Bitrates and frame sizes

```
base   = bitrate × 864 / (Fs_mode × 8)        integer division; Fs_mode = 44100 or 96000
base  += base × 3 >> 7                        only for rates that are not "hq"
base  |= 1                                    forced odd
frame  = base + 3 bytes                       (4-byte header + base − 1 payload bytes)
```

| Code | Bitrate | hq | Frame bytes (44.1/48 kHz mode) | Frame bytes (96 kHz mode) | Typical use |
|---|---|---|---|---|---|
| `F0` | 88 kbps | – | 224 | 104 | SSC, congested link |
| `F1` | 96 kbps | – | 244 | 114 | |
| `F2` | 128 kbps | – | 324 | 150 | SSC, bitrate-limited sinks |
| `F3` | 192 kbps | – | 484 | 224 | SSC / HiFi default |
| `F4` | 229 kbps | – | 576 | 266 | |
| `F6` | 256 kbps | ✓ | 630 | 292 | |
| `F5` | 328 kbps | ✓ | 806 | 372 | legacy UHQ |
| `E6` | 152 kbps | ✓ | 376 | 174 | UHQ v2, busy link |
| `E5` | 250 kbps | ✓ | 616 | 284 | |
| `E0` | 291 kbps | ✓ | 716 | 330 | UHQ v2 |
| `E1` | 308 kbps | ✓ | 758 | 350 | |
| `E2` | 442 kbps | ✓ | 1086 | 500 | |
| `E3` | 584 kbps | ✓ | 1434 | 660 | UHQ v2 default |
| `E4` | 886 kbps | ✓ | 2172 | 1000 | |

The encoder may change the bitrate on any frame. The phone does this when the radio link gets busy (see
[a2dp.md](a2dp.md#bitrate-adaptation)).

## Payload

The payload is one range-coded block. The range decoder is initialised over all `frame − 4` bytes. The bit budget
used by allocation is `total_bits = 8 × min(payload_bytes, 1275)`, so 886 kbps frames are coded with the
same budget ceiling as Opus.

Symbols, in order (CELT 1.1 names):

| # | Symbol | Notes |
|---|---|---|
| 1 | silence | `logp 15`, only when `tell == 1` |
| 2 | post-filter | on/off bit; octave `uint(6)`, period `4 + octave` bits, gain 3 bits. **No tapset symbol** (tapset 0) |
| 3 | transient | `logp 3`; selects 8 short MDCTs |
| 4 | intra flag + coarse energy | Laplace-coded, CELT's probability model |
| 5 | TF resolution | **only for non-hq rates**; for hq rates every band has `tf_res = 0` |
| 6 | spreading | `icdf` with CELT's table |
| 7 | dynamic allocation boosts | CELT rules |
| 8 | allocation trim | `icdf` |
| 9 | allocation | CELT `compute_allocation` with SSC's tables, caps and pulse cache |
| 10 | fine energy | |
| 11 | band shapes | `quant_all_bands`: PVQ (CWRS), stereo, intensity / dual stereo, split bands |
| 12 | anti-collapse bit | when reserved |
| 13 | final energy bits | from remaining bits |

The details of each stage are in the notes: [range coder and energy](notes/01-range-coder-and-energy.md),
[bit allocation](notes/02-bit-allocation.md), [band shapes / PVQ](notes/03-band-shapes-pvq.md),
[signal path](notes/04-signal-path.md), [frame flow](notes/05-frame-flow.md).

## Differences from Opus CELT 1.1

| Area | Opus CELT 1.1 | SSC |
|---|---|---|
| Frame size | 120 / 240 / 480 / 960 at 48 kHz | 864 (fixed) |
| Modes | one 48 kHz mode | 44.1 kHz mode (shared by 48 kHz) and a 96 kHz mode, each with its own bands |
| Packet header | Opus TOC byte | 4-byte SSC header with sequence number and bitrate code |
| Speech coding | SILK / hybrid | none |
| Max bitrate | 510 kbps | 886 kbps |
| Pre-emphasis | yes | none |
| Post-filter | 3 tapsets | tapset 0 only, no tapset symbol |
| TF analysis | always coded | not coded for hq rates |
| Allocation | one table | two tables, chosen by bitrate (> 192 kbps uses the second) |
| Pulse cache | CELT layout | own cache; the band-20 cache index differs from the regular pattern |
| Band offsets | dynalloc only | Samsung post-processes `offsets[18..21]` |
| Coded bands | all | end band from header (18 – 22); energies above it are set to −28 |

## Decoder output

* Output is 864 stereo samples per frame, delayed by 108 samples relative to the encoder input.
* 16-bit modes reach 16-bit full scale. The 24-bit modes (HiFi, UHQ) carry extra precision. Use
  `ssc_decode_s32()`, which scales 16-bit full scale to `1 << 31`.
