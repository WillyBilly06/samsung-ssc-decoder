# E: ssc_encode frame flow, from the decoder's point of view

> Research notes written while reverse-engineering SSC. Addresses are offsets in Samsung's libraries on a
> Galaxy S26 Ultra. References to `re/...` files, decompiler output and disassembly point to working files
> that are not part of this repository. The cleaned-up specification is [`../bitstream.md`](../bitstream.md).

Sources: `re/fn/ssc_encode.c` (Ghidra), `re/ssc_encode.dis` (llvm-objdump, @0xb6cc..0xf7bc).
Reference: opus-1.1 `celt/celt_encoder.c` `celt_encode_with_ec` and `celt_decoder.c` `celt_decode_with_ec`.
I checked the reference frames in `harness/ref_frames/*.ssc` against the header logic.
Line numbers `Lnnnn` refer to `re/fn/ssc_encode.c`. Addresses are in the disassembly.

## 0. Summary

The frame layout is CELT 1.1 with these fixed values: **start=0, LM=3 (M=8, N=864), C=2 always,
no tapset symbol, no preemphasis**. The decoder must change these things relative to CELT 1.1:

1. **total_bits = 8*min(payload_len, 1275)**, but the range decoder storage is the full payload_len.
   tf_decode's budget uses `storage*8`. Every other budget uses the clamped total_bits.
2. **Coded end band** `end = 22 - red`, from the header (§1.3): 22, 21, 20, 19, or 18 for the 152k rate.
   It is used for coarse/fine energy, dynalloc, allocation, PVQ, and finalise.
   After the frame, `oldBandE[c][end..21] = -28.0` (Q10 0x9000). CELT uses 0 there.
3. **hq rates (all E-family, plus 256k and 328k) code no TF symbols at all.** The final `tf_res[i]=0` for all bands, even
   when the frame is transient. tf_select is never coded.
4. dynalloc `quanta = max(48, (eBands[i+1]-eBands[i])<<4)`. This is the C=2 formula, hard-coded. `cap[]` is the
   precomputed int32 table at `mode+0x78`.
5. **Samsung dynalloc post-processing:** offsets[18..21] are propagated after decoding, using the coarse energies (§2.8).
6. The allocVectors table is chosen by bitrate: >192000 → `mode+0x30`, else `mode+0x28`. See 02-bit-allocation.md.
7. The postfilter has no tapset symbol, so tapset = 0 (gains 0.3066/0.2170/0.1296). Pitch, octave, and gain are the same as CELT.

The symbol order is the same as CELT 1.1: silence → postfilter → transient → [coarse energy: intra bit + Laplace] →
TF (non-hq only) → spread → dynalloc → trim → [allocation: skip / intensity / dual] → fine energy →
PVQ (quant_all_bands) → anti-collapse raw bit → energy finalise raw bits → ec_enc_done.

---

## 1. Frame header and the top-level variables

### 1.1 Header (4 bytes, written before the range coder; L392-L530, L1346-L1410)
```
b0 = 0xFF
b1 = 0xEE                              (F-family and 256k/328k etc.: "iVar99 == 0")
   = 0xE0 | ((seq >> 4) & 0xF)         (E-family: 291k,308k,442k,584k,886k,250k,152k)
b2 = (seq << 4) | (lb << 2) | (flag & 3)        flag = u16 st+0x8e (always 1; set by encoder_init via st+0x8c=0x10000, never changed)
b3 = rate code; for E-family (except 152k) the top 3 bits are replaced (see 1.3)
```
- `seq` is the u16 at st+0x8c. After every frame: `seq = (seq == 0xFF) ? 0 : seq+1` (@0xf640). It is an 8-bit counter. F-family
  carries only the low nibble (b2>>4). E-family carries the full 8 bits: `seq = ((b1&0xF)<<4) | (b2>>4)`.
- For E-family, b1 can also be 0xEE (seq 0xE0..0xEF). **Identify the rate from `b3 & 0x1F`**, not from b1:

| b3&0x1F | rate | family | hq | allocVectors (bitrate>192000) | end |
|---|---|---|---|---|---|
| 0x10 | 88000  | F | 0 | A (+0x28) | 22 |
| 0x11 | 96000  | F | 0 | A | 22 |
| 0x12 | 128000 | F | 0 | A | 22 |
| 0x13 | 192000 | F | 0 | A | 22 |
| 0x14 | 229000 | F | 0 | B (+0x30) | 22 |
| 0x15 | 328000 | F | 1 | B | 22 |
| 0x16 | 256000 | F | 1 | B | 22 |
| 0x00 | 291000 | E | 1 | B | 22-((b2>>2)&3) |
| 0x01 | 308000 | E | 1 | B | 22-((b2>>2)&3) |
| 0x02 | 442000 | E | 1 | B | 22-((b2>>2)&3) |
| 0x03 | 584000 | E | 1 | B | 22-((b2>>2)&3) |
| 0x04 | 886000 | E | 1 | B | 22-((b2>>2)&3) |
| 0x05 | 250000 | E | 1 | B | 22-((b2>>2)&3) |
| 0x06 | 152000 (b3 = 0xE6 exactly) | E | 1 | A | **18** |

Neither the channel count nor the sample rate is in the header. `mono_192000.ssc` has the same header
`ff ee xd f3` as the stereo file, and so does `mix96_192000.ssc`. The bitstream always codes **2 channels** (see §3).
The 44.1k/48k mode versus the 96k mode must be known out of band. The 48k input uses the 44.1k mode, and `st+0x60` is overwritten to 44100.

### 1.2 Payload size, total_bits, and the 1275 clamp (L531-L605, @0xbc8c-0xbec8)
```
base = (bitrate * 864) / (Fs_mode * 8)          Fs_mode = st+0x60 = 44100 (44.1k/48k) or 96000
if (!hq) base += (base*3) >> 7
base |= 1                                       (forced odd)
nbBytes = base - 1                              -> payload length; frame = nbBytes + 4
ec_enc_init(buf+4, nbBytes); ... ec_enc_shrink(enc, nbBytes)   (no-op: same size)
if (pcm==NULL || base < 3) return -1
nbCompressedBytes = min(nbBytes, 1275)          (@0xbe4c: cmp w23,#0x4fb; csel)
if (st+0x18 bitrate != -1) ...                  (VBR-style cap; st+0x18 is always -1 from encoder_init, so it is dead code)
effectiveBytes (uVar126) = nbCompressedBytes
total_bits (iVar90, [sp+0xdc]) = 8 * effectiveBytes
```
- **The clamp limits only the bit budget. The ec buffer stays at the full nbBytes.** For 442k/584k/886k (payload
  1082/1430/2168) the range coder writes from the front, ec_enc_done zero-fills the middle, and the raw bits
  (ec_enc_bits: fine-energy finalise, anti-collapse, PVQ raw bits) go at the very end of the **full**
  nbBytes buffer. This matches the coordinator's observation of a zero run followed by identical tail bytes.
- Every allocation and stop condition uses total_bits = 8*min(nbBytes,1275): silence, postfilter, transient, coarse
  budget, spread, dynalloc (`uVar126*64`), trim, the allocation `bits`, quant_all_bands `total_bits*8 - rsv`,
  and the finalise `total_bits - ec_tell`.
  **Exception: TF uses `enc->storage*8`**, which is the full nbBytes (L2003 `local_530*8`; local_530 = ec+8 = storage).
- The return value is `effectiveBytes + 4` (≤ 1279), or −3 if `enc->error` (@0xf650). For >1275 rates this is
  smaller than the real frame. The transport must send the full `nbBytes+4` bytes (the harness frames are 2172 bytes at 886k).
- Decoder: `ec_dec_init(&dec, frame+4, len-4)`, `effLen = min(len-4, 1275)`, `total_bits = effLen*8`.

### 1.3 Last-band side information (L1346-L1410; `ssc_compute_band_energies_for_lastband` @0x7cb4)
`red` (Ghidra `uVar131`, asm w26) is the number of top bands that are not coded. **The coded end band is `22 - red`.**
The encoder uses `22 - red` as `end` for coarse energy, spreading, dynalloc, hysteresis, allocation, fine,
PVQ, and finalise. It also stores `red` in the global `previous_uhq_index`, which only the encoder's
transient detector uses.

- **non-hq** (F0-F4): `red = 0`. Bits 2-3 of b2 carry a 2-bit "last-band activity" code `lb`. The function splits band 21 (bins
  `eBands[21]*8 = 672`.. in 44.1k) into 4 parts with the end bins `{708,744,780,816}` (table @0x27f8). For each part it computes the
  L2 norm per channel. `lb = 3` if `E0[3]+E1[3] >= 30002`, else 2 if part 2 qualifies, else 1 if part 1 qualifies, else 0.
  The encoder computes this **after** the <192k zeroing of band 21 (§4), so lb=0 for 88k/96k/128k.
  **It does not change anything in the coded bitstream.** It is decoder side information only (probably the
  bandwidth of the last band). Observed values: 192k and 229k give `lb=3`, and 88k/96k/128k give 0.
- **hq F-family** (256k F6, 328k F5): `red = 0`. The header is not modified.
- **152k** (b3 == 0xE6): `red = 4` → end = 18. The header is not modified (bits 2-3 = 0).
- **Other E-family**: the encoder searches `red = 0..3`. For each value it splits band `21-red` into 8 parts using row `red` of
  the table @0x2840 (shown below). The parts are designed for the 96k band layout. The search stops at the first `red` where some part k (checked from k=7 down to 0)
  has `E0[k]+E1[k] > 12001`, and sets `sub = k+1`. If nothing qualifies, then `red = 3, sub = 0`.
  Header: `b2 += red<<2`; `b3 = (sub ? (sub-1)<<5 : 0) | (b3 & 0x1F)`.
  Decoder: `end = 22 - ((b2>>2)&3)`. `b3>>5` is side information (sub-1, where 0 can also mean sub=0).
```
@0x27f8 (non-hq, 4 parts):  708 744 780 816
@0x2840 row0: 738 756 774 792 810 828 846 864   (band 21: start eBands[21]*8)
        row1: 594 612 630 648 666 684 702 720   (band 20)
        row2: 450 468 486 504 522 540 558 576   (band 19)
        row3: 306 324 342 360 378 396 414 432   (band 18)
```
Every E-family reference frame has `b2&0xC = 0xC` and `b3>>5 = 0`, so red=3 and end=19. A likely reason:
**for hq rates the PCM is shifted left by only 4 bits, even for 16-bit input** (@0xc254: `shift = (hq || bps==24) ? 4 : 12`).
With 16-bit test input the hq signal is 48 dB smaller inside the encoder, so the 12001 threshold is never reached.
To test E-family with real bandwidth, use `bits_per_sample=24` and 24-bit-scaled samples.
The decoder's output scaling also depends on this shift.

### 1.4 Channels and LM
- The PCM is always read as interleaved stereo (L996-L1051: stride 2, channel 1 from `pcm+1`). Coarse energy
  (`FUN_00113734`), finalise, allocation, and quant_all_bands are all unrolled for 2 channels. **C=2 in the bitstream**,
  whatever `channels` was passed to create. `st->channels` (st+0xc) only affects encoder heuristics (15*C, 10*C,
  peak scan length) and the state layout.
- LM = 3 always (frame_size 864 = 108<<3). shortBlocks = 8 when transient.
- start = 0 (it is not even a parameter of the Samsung helpers).

---

## 2. Symbols in order (decoder pseudo-code with exact conditions)

`tell = ec_tell()`, `tellf = ec_tell_frac()`. `TB = total_bits = 8*min(len,1275)`.

### 2.1 Silence (L927-L940, @0xc27c)
```
tell = ec_tell()                       // = 1 on a fresh coder
if (tell == 1) silence = ec_dec_bit_logp(15); else silence = 0;   // the encoder always takes the tell==1 branch
if (silence) { dec->nbits_total += TB - ec_tell(); tell = TB; }   // L930: nbits_total = TB - clz(rng) + 32
```
Encoder side: silence = (peak over the previous frame's tail and this frame == 0).
CELT's `if (tell >= total_bits) silence = 1` can be kept. It never triggers here.
If silent, every later budget check fails, so no other range-coded symbols are read. The anti-collapse bit
cannot be coded (isTransient=0), and finalise gets `TB - tell = 0`. After the frame, oldBandE is set to −28 for all bands (L3774-L3804).

### 2.2 Postfilter (L1155-L1168, @0xc850-0xc8b0)
```
if (tell + 16 <= TB) {                       // the encoder writes '0' only under this condition; it writes '1' only if pf_on,
    if (ec_dec_bit_logp(1)) {                // which needs nbAvailableBytes >= 25 && !silence, so the condition holds
        octave = ec_dec_uint(6);             // enc: octave = EC_ILOG(pitch+1) - 5  (0x1b - clz)
        pitch  = (16 << octave) + ec_dec_bits(4 + octave) - 1;   // pitch in [15, 1022]
        qg     = ec_dec_bits(3);
        gain   = 3072 * (qg + 1);            // Q15, = 0.09375*(qg+1)
        tapset = 0;                          // NO tapset symbol (CELT: if tell+2<=TB icdf(tapset_icdf,2))
    }
    tell = ec_tell();
}
```
The comb filter (`ssc_comb_filter`) has fixed tapset-0 taps: `g*0x4e80>>16, g*0x1bc8>>15, g*0x1098>>15` = 0.3066/0.2170/0.1296.
The encoder prefilter applies `-gain` like CELT. The crossfade uses `win_square` over the overlap (synthesis team).

### 2.3 Transient (L1221-L1345, @0xcf0c)
```
isTransient = 0;
if (tell + 3 <= TB) isTransient = ec_dec_bit_logp(3);    // LM=3>0
shortBlocks = isTransient ? 8 : 0;
```
The encoder decides with `ssc_patch_transient_decision` on band log energies (opus-master style). This is encoder-only.

### 2.4 Coarse energy (L1991, `ssc_quant_coarse_energy`)
Arguments: `(mode, 22, end, bandLogE, oldBandE, budget=TB, error, enc, nbAvailableBytes, force_intra=st+0x10(=0), &delayedIntra)`.
Only `end` (the 3rd argument) is used. Decoder: `intra = (ec_tell()+3 <= TB) ? ec_dec_bit_logp(3) : 0;` then
`unquant_coarse_energy(start=0, end, C=2, LM=3)` with **budget = TB (not dec->storage*8)**.
The tables, coefficients, and the min(i,20) prob index are the same as CELT LM=3. See 01-range-coder-and-energy.md.

### 2.5 TF (L1997-L2061, @0xd918-0xdad8). Only when **hq == 0**
```
if (!hq) {
    budget = dec->storage * 8;               // FULL payload, not TB
    tell   = ec_tell();
    logp   = isTransient ? 2 : 4;
    tf_select_rsv = (tell + logp + 1 <= budget);      // LM>0
    budget -= tf_select_rsv;
    curr = tf_changed = 0;
    for (i = 0; i < 22; i++) {               // loop bound = st->end = 22 (red=0 for non-hq)
        if (tell + logp <= budget) { curr ^= ec_dec_bit_logp(logp); tell = ec_tell(); tf_changed |= curr; }
        tf_res[i] = curr;
        logp = isTransient ? 4 : 5;
    }
    tf_select = 0;
    if (tf_select_rsv && tf_select_table[4*isTransient + 0 + tf_changed] != tf_select_table[4*isTransient + 2 + tf_changed])
        tf_select = ec_dec_bit_logp(1);
    for (i = 0; i < 22; i++) tf_res[i] = tf_select_table[4*isTransient + 2*tf_select + tf_res[i]];
} else {
    for (i = 0; i < 22; i++) tf_res[i] = 0;  // FINAL value 0, not mapped through the table, and no symbols
}
```
`ssc_tf_select_table` @0x2838 = `{0,-2,0,-3, 3,0,1,-1}`, which is CELT's LM=3 row. The encoder's first-band logp arg (w2) is
`isTransient?2:4` (@0xd968), and later bands use `isTransient?4:5` (@0xd978). This is identical to CELT.
(Encoder TF analysis runs only if `!hq && effectiveBytes >= 15*C`. Otherwise tf_res=isTransient and tf_select=0 are still
**coded** for non-hq, as in CELT.)

### 2.6 Spread (L2062-L2071, @0xdb54)
```
spread = 2 /*SPREAD_NORMAL*/;
if (ec_tell() + 4 <= TB) spread = ec_dec_icdf(spread_icdf, 5);    // @0x3d14 = {25,23,2,0}, identical to CELT
```
(Encoder: `ssc_spreading_decision` if `!isTransient && nbAvailableBytes >= 10*C`, else 2.)

### 2.7 Dynalloc (L2716-L2785, @0xe590-0xe718)
```
cap[i]   = ((int32*)mode->cache.caps /*mode+0x78*/)[i]        // precomputed, int32
quanta_i = max(48, (eBands[i+1]-eBands[i]) << 4)               // asm: lsl #4; cmp #0x30; csel; and #0xfff0
TB8 = TB << 3;  total_boost = 0;  dynalloc_logp = 6;  tellf = ec_tell_frac();
for (i = 0; i < end; i++) {
    loop_logp = dynalloc_logp; boost = 0;
    while (tellf + (loop_logp << 3) < TB8 - total_boost && boost < cap[i]) {
        flag = ec_dec_bit_logp(loop_logp); tellf = ec_tell_frac();
        if (!flag) break;
        boost += quanta_i; total_boost += quanta_i; loop_logp = 1;
    }
    if (boost > 0) dynalloc_logp = max(2, dynalloc_logp - 1);
    offsets[i] = boost;
}
for (i = end; i < 22; i++) offsets[i] = 0;
```
The control flow is identical to CELT. Samsung checks the budget once before the first bit and again after each `1`.
cap tables (int32 from the mode). They are the stereo LM=3 values:
```
44.1k @0x4a6c: 1072x8, 2120x4, 4192x3, 6024x2, 7648, 9792x3, 9360
96k   @0x5d00: 1072x12, 2120x2, 4192x2, 6024x2, 9360x4
```
CELT uses `width = C*N<<LM`, so `quanta = min(width<<3, max(48,width))`. With C=2 this is the same formula. For a real mono stream it
would differ, but the bitstream is always C=2.

### 2.8 Samsung offsets post-processing (L2786-L2802, @0xe718-0xe808). **Must be mirrored**
This runs after all dynalloc symbols and before trim. `E[k]` = `oldBandE[channel 0][k]` (int16, Q10). At this point it holds
the **coarse-quantised** energies. The encoder passes this same array to quant_coarse_energy, so the decoder has the same
values after unquant_coarse_energy and before fine energy.
```
if (offsets[18] > 1000 && offsets[19] == 0) {
    if (E[19] > 7000 || (E[18] >> 1) < E[19]) offsets[19] = offsets[18];
} else if (offsets[19] > 1000 && offsets[20] == 0) {
    if (E[20] > 7000 || (E[19] >> 1) < E[20]) offsets[20] = offsets[19];
} else if (offsets[20] > 1000 && offsets[21] == 0) {
    if (E[21] > 7000 || (E[20] >> 1) < E[21]) offsets[21] = offsets[20];
}
```
(It runs even when no dynalloc loop ran. Values at or above `end` are ignored by the allocation.)

### 2.9 Alloc trim (L2925-L3737, @0xe998-0xe9c4, 0xf1d8)
```
trim = 5;
if (tellf + 48 <= TB8 - total_boost) trim = ec_dec_icdf(trim_icdf, 7);   // @0x3d74 = {126,124,119,109,87,41,19,9,4,2,0}, identical to CELT
```
`tellf` here is the value after the last dynalloc symbol. The encoder does not call tell_frac again in between.

### 2.10 Allocation (L3738-L3747, @0xf1ec-0xf238)
```
bits = TB8 - ec_tell_frac() - 1;
anti_collapse_rsv = (isTransient && bits > 39) ? 8 : 0;        // (LM+2)<<3 = 40
bits -= anti_collapse_rsv;
codedBands = clt_compute_allocation(mode, 0, end, offsets, cap, trim, &intensity, &dual_stereo, bits, &balance,
                                    pulses, fine_quant, fine_priority, C=2, LM=3, dec, 0, 0, 0,
                                    allocVectors = (bitrate > 192000) ? mode+0x30 : mode+0x28);
```
The skip bits, intensity (`ec_uint(codedBands+1)`) and dual-stereo bit are coded inside, as in CELT for C=2. The intensity
reservation is unconditional (C=2). See 02-bit-allocation.md.
The encoder's intensity choice is `hysteresis_decision(510, @0x3d18, @0x3d46, 22-red, st->intensity)`, clamped to `end`. The argument 510 is a constant
"kbps", so it always selects `end`. This is encoder-only, and the decoder reads it from the stream.
```
@0x3d18 intensity thresholds (int16 x23): 1 2 3 4 5 6 7 8 16 24 36 44 50 56 62 67 72 77 83 89 95 102 109
@0x3d46 hysteresis           (int16 x23): 1 1 1 1 1 1 1 2 2 2 2 2 2 2 3 3 3 4 4 5 5 7 7
```
dual_stereo: the encoder runs an inline CELT `stereo_analysis` over 13 bands (L2803-L2915). This is encoder-only.

### 2.11 Fine energy, PVQ, anti-collapse, finalise (L3760-L3773, @0xf294-0xf36c)
```
unquant_fine_energy(mode, 0, end, oldBandE, fine_quant, dec, C=2);
quant_all_bands(decode, mode, 0, end, X, Y, collapse_masks, NULL, pulses, shortBlocks, spread,
                dual_stereo, intensity, tf_res, TB8 - anti_collapse_rsv, balance, dec, LM=3, codedBands, &st->rng);
anti_collapse_on = (anti_collapse_rsv > 0) ? ec_dec_bits(1) : 0;   // enc: ec_enc_bits(st->consec_transient(st+0x40) < 2, 1)
unquant_energy_finalise(mode, 0, end, oldBandE, fine_quant, fine_priority, TB - ec_tell(), dec, C=2);
```
Encoder call arguments:
`ssc_quant_fine_energy(mode, end, oldBandE, error, fine_quant, enc)`;
`ssc_quant_all_bands(mode, end, X(both ch), collapse_masks, bandE, pulses, shortBlocks(0/8), st->spread_decision(st+0x20),
dual_stereo, st->intensity(st+0x50), tf_res, TB8 - rsv, balance, enc, codedBands, &st->rng(st+0x1c))`;
`ssc_quant_energy_finalise(mode, end, oldBandE, error, fine_quant, fine_priority, TB - ec_tell, enc)`.
The seed st+0x1c is set to `enc->rng` (L3953) before ec_enc_done, as in CELT.

### 2.12 End of frame state
- silence: `oldBandE[0..43] = -28<<10 (0x9000)` (L3774-L3804).
- `if (red != 0) oldBandE[c][22-red .. 21] = 0x9000` for both channels (L3806-L3860). **The decoder must do the same.** It affects
  the next frame's coarse-energy prediction when `end` changes. CELT sets oldBandE to 0 there instead.
- The encoder keeps oldLogE = oldBandE (non-transient) or the minimum (transient). It has no oldLogE2. The decoder's anti_collapse uses its
  own oldLogE/oldLogE2 as in CELT.
- `ec_enc_done`, then seq++.

---

## 3. Samsung vs CELT 1.1 differences (the complete list I found in ssc_encode)

Bitstream / decoder-visible:
1. 4-byte header outside the range coder (§1.1). The rate code determines hq, the allocVectors table, and the end reduction.
2. The `1275` clamp applies to total_bits only. The ec storage (and the raw-bit tail) is the full payload. TF budget = storage*8.
3. end = 22 - red, set per frame from the header. effEBands=21 is **not** used as the coded end: non-hq codes all 22 bands, including band 21
   (bins 672..815 in 44.1k, 720..863 in 96k).
4. hq: no TF symbols, and tf_res=0 (final). Non-hq: CELT TF over 22 bands.
5. No tapset symbol (tapset 0).
6. dynalloc quanta/caps are hard-coded for C=2, LM=3 (int32 caps table in the mode).
7. Offsets propagation for bands 18→19→20→21 based on oldBandE (§2.8).
8. allocVectors table choice by bitrate (>192000).
9. oldBandE = −28 for bands at or above end (when red>0), not 0.
10. C=2 always, and the channel count is not signalled. Fs mode is not signalled. There is no pre-emphasis (the PCM is copied with a plain shift, and
    `ssc_preemphasis` is never called), so the decoder must not apply de-emphasis.

Encoder-only (no decoder impact, but useful for understanding):
- DRC/biquad limiter (`SSC_DRC_Exe`, `CalculateLimitGain`, `Biquad_Narrow_int2int_stereo`) before encoding.
- PCM scaling `<<12` (non-hq, 16-bit) or `<<4` (24-bit, or **any** hq). This affects the absolute output level.
- `normal_headroom` vs `uhq_headroom` on the MDCT.
- For bitrate < 192000, MDCT bins `eBands[21]*8 .. 863` are zeroed. In transient frames this applies only when !hq. Band 21 is still coded.
- Transient detection by `ssc_patch_transient_decision` (band energies), using `previous_uhq_index`.
- Prefilter enable: `nbAvailableBytes >= 25 && !silence`. The gain thresholds are CELT-like, and pitch is clamped to ≤1022.
- TF analysis only for !hq and effBytes ≥ 15*C. The lambda is 24/12/8/6 by effBytes <40/<60/≤99/else.
- Intensity forced to `end` (hysteresis at a constant 510).
- consec_transient (st+0x40): `++` if transient. If not transient, it is reset to 0 when `tell+3 <= TB` at the transient point,
  and otherwise **also** `++` (L3905). It only drives the anti-collapse bit value.
- TF budget in silence frames: the TF budget is storage*8, but non-hq payloads are always ≤ 576 bytes (≤1275), so after silence
  `tell == storage*8` and no TF bit is read. hq frames have no TF. The two budgets therefore never disagree in practice.
- Dynalloc analysis only if effBytes ≥ 51, with extra boosts for offsets[0..1] (L2668-L2714).

---

## 4. Rodata dumped (addresses as file vaddr; Ghidra shows them +0x100000)
```
spread_icdf     @0x3d14: 25 23 2 0
trim_icdf       @0x3d74: 126 124 119 109 87 41 19 9 4 2 0
tf_select_table @0x2838: 0 -2 0 -3 3 0 1 -1
intensity thr   @0x3d18, hysteresis @0x3d46: see 2.10
lastband parts  @0x27f8, @0x2840: see 1.3
e_prob inter    @0x5dbf: CELT LM=3 inter (+2 extra bytes 130,17); intra @0x5d91: CELT LM=3 intra (+68,45). Index is min(i,20), so the extras are unused.
small_energy_icdf @0x5ded: 2 1 0
LOG2_FRAC_TABLE @0x5df0: 0 8 13 16 19 21 23 24 26 27 28 29 30 31 32 32 33 34 34 35 36 36 37 37
eBands 44.1k @0x3f30: 0 1 2 3 4 5 6 7 8 10 12 14 16 20 24 28 34 40 48 60 72 84 102
eBands 96k   @0x5988: 0 1 2 3 4 5 6 7 8 9 10 11 12 14 16 20 24 30 36 54 72 90 108
allocVectors A (both modes) @0x3f5e; B 44.1k @0x4050; B 96k @0x59b6 (11x22 u8; see 02-bit-allocation.md)
caps (int32[22]) 44.1k @0x4a6c, 96k @0x5d00: see 2.7
```
