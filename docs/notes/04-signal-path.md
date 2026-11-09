# D: SSC signal path (analysis in the encoder, and its exact inverse for the decoder)

> Research notes written while reverse-engineering SSC. Addresses are offsets in Samsung's libraries on a
> Galaxy S26 Ultra. References to `re/...` files, decompiler output and disassembly point to working files
> that are not part of this repository. The cleaned-up specification is [`../bitstream.md`](../bitstream.md).

Scope: `ssc_preemphasis`, `ssc_mdct_forward`, `ssc_fft`, the window, `ssc_comb_filter` (+ the `*_asm` kernels),
the prefilter decision code (`getPitchIndexGain`, `getPitchFilterThreshold`, `prefilter_gain_calculation`, `tone_detect`),
the band-energy functions, `ssc_amp2Log2`, `ssc_normalise_bands`, the last-band functions, and the headroom/DRC functions.

**Method.** I read the decompiles and disassembly. I then **ran the real library functions on the phone**
(SM-S948U1, `/system/lib64/libScalable_Encoder.so`, through `dlopen`), and compared them with Python and C models:
- MDCT impulse responses for all 972 (long) and 216 (short) inputs, random vectors, and the stride-8 interleave
- the comb filter for 7 (T0, T1, g0, g1) cases
- band energies, amp2Log2 and normalise on 40 random spectra
- headroom gain curves for 14 bitrates × 2 modes × {normal, uhq}

The decoder code in `D_decoder_snippets.c` was cross-compiled with the NDK and checked on the phone against the real
encoder functions (harness: `D_snip_test.c`; see §7).

## 0. Summary for the decoder writer

| Item | Encoder (SSC) | Decoder must do |
|---|---|---|
| Pre-emphasis | **None.** Pure shift: 16-bit `<<12`, 24-bit `<<4`, and **any hq bitrate `<<4`** | **No de-emphasis.** Output = sig/32768 (opus: `preemph={0,0,0,0}`) |
| Resampling | None. A 48 kHz input uses the 44.1k mode unchanged (only the `Fs` field reads 44100) | None. Output at the input rate |
| MDCT | Bit-matched to **opus 1.1 `clt_mdct_forward`** (float semantics), N=1728 (N2=864), overlap 108, shift 3 for 8×216 | opus 1.1 `clt_mdct_backward` or `ssc_synthesis_frame()`. No extra gain |
| Window | 108-tap **Vorbis power-complementary**, Q31 table @.rodata 0x3d80 | Same table (C array in the snippets) |
| Short blocks | 8 MDCTs of 216 at input offsets 108·b, coefficient `X[j*8+b]` (CELT B=8 interleave) | Same as CELT |
| Prefilter | CELT `comb_filter`, **tapset 0 hard-wired**, gains `-g`, crossfade with w² (`win_square`) | CELT postfilter with tapset 0, `+g`, crossfade at output offset 108 (LM≠0 path) |
| Pitch/gain | MINPERIOD 15, MAXPERIOD 1024, T ≤ 1022, g = 0.09375·(qg+1) | Same |
| Band energy | `sqrt(Σx²)+1` (fixed-point), `logE = log2(bandE/4096) - eMeans[i]` | `freq = shape · 2^(logE+eMeans)` (CELT float) |
| Headroom | Fixed HF attenuation (≤1.7 dB) of the MDCT **before** energy analysis | Baked into the coded energies. Leave it (optional inverse is provided) |
| DRC limiter | Signal-dependent gain on the input PCM | Nothing (encoder-only, cannot be undone) |

## 1. Pre-emphasis: none

`ssc_preemphasis` @0xade4 (never called: it has no callers in `enc.dis`) and the copy inlined in `ssc_encode`
(decompile L949-L1052) only do `in[i] = pcm[2*i+c] << shift`. There is **no `x - coef*mem` filter**, and the mode has
no `preemph[]` field. The shift is:

```
shift = 4;  if (!hq && bits_per_sample != 24) shift = 12;      // ssc_encode L948-L951
```

- 16-bit, non-hq: `x<<12` (full scale 2^27). 24-bit: `x<<4` (full scale 2^27). These are the same internal scale as
  opus fixed point (SIG_SHIFT=12).
- **hq bitrates** (E-family, 256k, 328k) always use `<<4`. The DRC threshold is also switched to 24-bit units for hq
  (`limit<<8`), so hq expects 24-bit PCM. A 16-bit stream at an hq rate would be coded 48 dB low. The decoder cannot
  detect this; it is an encoder-side issue.

**Decoder: no de-emphasis filter.** Treat the float synthesis as 16-bit PCM units:
pcm_float = sig/32768, int16 = sig, int24 = sig·256. With opus `deemphasis()`, set `coef[0]=0`, which leaves only the
`1/32768` scaling.

Other input processing:
- `Biquad_Narrow_int2int_stereo` and `powercalc` write only to `_pcmpower`. They feed `CalculateLimitGain`, and the
  PCM is untouched.
- `SSC_DRC_Exe` is a peak limiter applied in place to the input PCM: 8 chunks of 108 samples, Q22 gain ramp, gain
  reduced when more than 24 samples exceed the threshold. It is encoder-only and cannot be undone.

## 2. MDCT

### 2.1 Forward transform (verified)

`ssc_mdct_forward(mdct_lookup *l /*mode+0x40*/, int32 *in, int32 *out, int overlap, int shift, int stride)`.
The window is not a parameter: it is hard-coded to .rodata 0x3d80.

- Structure: identical to opus 1.1 `clt_mdct_forward` (fold/window, pre-rotation, N/4 complex FFT, post-rotation).
  - Trig table at mode+0x58 is `cos(2πi/1728)` Q31, i = 0..432 (max error 4.7e-10).
  - The "sine" approximation is replaced by an exact rotation by θ = π/(4N): `(0x7fffff22, 0xee4bb)` for N=1728 and
    `(0x7fffc88c, 0x7725c6)` for N=216.
- `ssc_fft` is a hand-written fixed-point FFT (radix 3 stages multiply by 0x2aaaaaab = 1/3). kfft structs:
  - @0x1bac0 = {nfft-ish 144, shift 0, bitrev @0x4ac4, twiddles @0x4be4}: the 432-point FFT done as 3×144
  - @0x1bad8 = {18, 3, @0x5964, @0x4be4}: the 54-point FFT
  - `kfft[shift>>1]` is used, so only shifts 0 and 3 exist.

  The overall scaling is 1/nfft, the same as opus.
- Empirical check (device output vs a float transliteration of opus 1.1 `clt_mdct_forward` with this window):
  - max |error| ≤ 3 LSB on ~1e6-magnitude outputs, for all 972 long impulses, all 216 short impulses and random
    vectors, in both modes
  - stride 8 puts block b in lanes `b, b+8, …` (other lanes untouched)

Closed form (verified to 5e-7 relative):

```
X[k] = (4/N) * Σ_{n=0}^{N-1} wf[n]·xf[n]·cos(2π/N·(n + 1/2 + N/4)·(k + 1/2)),   k = 0..N/2-1
N = 1728 (long) / 216 (short)
xf = the N2+108 input samples [108 old | N2 new] placed at offset pad = (N/2-108)/2 (378 long, 0 short) in a length-N frame
wf = 0 (pad) | w[0..107] rising | 1 (N2-108 samples) | w[107..0] falling | 0 (pad)
```

The scaling is the same as opus. With fixed signal = float signal·4096, the coefficients are 4096× the opus-float
coefficients.

### 2.2 Window (dumped)

Q31, .rodata 0x3d80, 108 entries (`win_square` @.data 0x1ff80 = w² in Q31, max error 6.6e-10, used only by the comb filter):

```
    178392,    1605298,    4457900,    8733769,   14429242,   21539396,   30058014,   39977549,   51289074,
  63982232,   78045176,   93464506,  110225207,  128310575,  147702149,  168379639,  190320859,  213501651,
 237895830,  263475113,  290209071,  318065076,  347008262,  377001497,  408005355,  439978115,  472875759,
 506651990,  541258267,  576643844,  612755839,  649539307,  686937337,  724891158,  763340273,  802222596,
 841474613,  881031555,  920827591,  960796024, 1000869511, 1040980285, 1081060395, 1121041950, 1160857368,
1200439638, 1239722574, 1278641082, 1317131417, 1355131443, 1392580889, 1429421593, 1465597745, 1501056112,
1535746257, 1569620740, 1602635307, 1634749059, 1665924606, 1696128194, 1725329824, 1753503338, 1780626489,
1806680987, 1831652523, 1855530771, 1878309364, 1899985853, 1920561641, 1940041898, 1958435451, 1975754665,
1992015292, 2007236320, 2021439791, 2034650612, 2046896359, 2058207060, 2068614970, 2078154351, 2086861227,
2094773147, 2101928942, 2108368480, 2114132420, 2119261967, 2123798631, 2127783991, 2131259459, 2134266055,
2136844183, 2139033424, 2140872326, 2142398211, 2143646989, 2144652984, 2145448765, 2146064996, 2146530292,
2146871084, 2147111505, 2147273279, 2147375625, 2147435171, 2147465888, 2147479021, 2147483048, 2147483641
```

Formula: `w[i] = sin(π/2 · sin²(π(i+½)/216))`, max error 2.3e-10. Power-complementary: w[i]² + w[107-i]² = 1 (error 5.6e-10).
This is the standard CELT window for overlap 108.

### 2.3 Framing, overlap and short blocks (ssc_encode L1170-L1252)

- Per channel, `in_shared + c*(N+108)` = [108 samples of `in_mem` (the previous frame's **prefiltered** tail) | 864
  prefiltered new samples]. This is CELT's `in` layout.
- **Long:** `mdct(in, X + c*864, overlap=108, shift=0, stride=1)`. It is always computed first, and used for the
  energies and the transient decision.
- **Transient** (`ssc_patch_transient_decision`): recomputed as `mdct(in + b*108, X + c*864 + b, 108, shift=3, stride=8)`
  for b = 0..7, i.e. **CELT shortBlocks with B=8, coefficient index j*8+b**. The overlap and window are the same
  (108) as for long blocks, so long and short frames can follow each other in any order.
- The decoder output of a frame (after OLA) is the encoder's `in[0..864)`, i.e. delayed by 108 samples.

### 2.4 Decoder inverse (verified)

```
y[m] = wf[pad+m] · Σ_k X[k]·cos(2π/N·(pad+m + 1/2 + N/4)·(k + 1/2)),   m = 0 .. N/2+107
```

Overlap-add with hop N/2 (long), or put short block b at offset 108·b, then add the 108-sample tail of the previous frame.
**No scale factor** is needed: opus 1.1 float `clt_mdct_backward` (no FFT scaling) is equivalent.
On the phone, the real `ssc_mdct_forward` → `ssc_synthesis_frame()` gives perfect reconstruction:

| Frame types | SNR |
|---|---|
| long | 121 dB |
| short | 129 dB |
| mixed long/short | 123 dB |

The SNR is limited by the encoder's fixed-point rounding.

## 3. Prefilter → decoder postfilter

### 3.1 What the encoder does (ssc_encode L1053-L1168)

This is the same as CELT 1.1 `run_prefilter`, with these specifics:

- **Pitch:** `ssc_pitch_downsample`, then `ssc_pitch_search(…, len=864, max_pitch=979=1024-3·15)`, giving
  `T = 1024 - idx`. Then `ssc_remove_doubling(…, maxperiod=1024, minperiod=15, …)` and `T = min(T, 1022)`.
  `gain = rd·0.7` (×0x599a>>15).
  - The analysis is skipped (T=15, g=0) when the available bytes are < 25 or the frame is silent.
  - `tone_detect` is called from inside `ssc_pitch_search` (encoder-only pitch-candidate logic).
  - `getPitchIndexGain`, `getPitchFilterThreshold` and `prefilter_gain_calculation` are dead, out-of-line copies of
    this inlined code (no callers).
- **Threshold:** the same as CELT `pf_threshold`, then the CELT quantizer:
  - start at 0.2; +0.2 if |ΔT|·10 > T; +0.1 if < 25 bytes; +0.1 if < 35 bytes; −0.1 if prev g > 0.4; −0.1 if prev g > 0.55; floor at 0.2
  - keep the previous g if |g − g_prev| ≤ 0.1
  - `qg = clamp(((g+0x600)>>10)/3 − 1, ≤7)` (Samsung also forces qg=0 when the kept gain is < 0x600)
  - `g1 = 0.09375·(qg+1)` (exact Q15 3072·(qg+1)), or 0 if off
- **State:** the encoder stores the *quantized* gain and the pitch (`st+0x3c`, `st+0x38`, written at the end of
  `ssc_encode`), and clamps the old period to ≥ 15. These are the same values the decoder sees.
- **Filter:** `ssc_comb_filter(y = in+108, x = pre_mem+1024, T0 = T_prev, T1 = T, N = 864, g0 = −g_prev, g1 = −g1, overlap = 108)`.
  This is FIR on the unfiltered input (prefilter history of 1024 samples). It is bit-exactly
  (0 LSB error, integer model on device):

```
taps(T, n) = 0.3066406250·x[n-T] + 0.2170410156·(x[n-T+1]+x[n-T-1]) + 0.1296386719·(x[n-T+2]+x[n-T-2])
y[n] = x[n] + (1-f[n])·g0·taps(T0,n) + f[n]·g1·taps(T1,n),   f[n] = w[n]² for n<108, then 1 (g0 terms gone)
```

  - The gain multiplication is fixed-point, with truncated taps (`g*0x2740>>15`, `g*0x1bc8>>15`, `g*0x1098>>15`) and
    f as Q31 `win_square`.
  - **Only tapset 0 exists.** The asm kernels hard-code 0x2740/0x1bc8/0x1098 (= 10048/7112/4248 in Q15), and the
    bitstream has no tapset symbol.
  - Dispatch: function1 (g0==0), function2 (g1==0, then copy), function3 (T0==T1), function4 (general), then
    `const_asm` for n ≥ 108. They are equivalent to CELT `comb_filter`.
- **Bitstream:** `bit_logp(1,1)`, `uint(octave = ilog2(T+1)−4, 6)`, `bits(T+1−(16<<oct), 4+oct)`, `bits(qg, 3)`.
  When off: `bit_logp(0,1)` (only when budget allows).

### 3.2 What the decoder does

This is the CELT 1.1 decoder postfilter for LM≠0, with tapset 0, run in place (IIR) on the synthesized output, using
history (1024+2 samples) of postfiltered output:

```
T = (16<<octave) + bits(4+octave) − 1;   g = 0.09375·(qg+1);   off → T=0, g=0
out[0..108)   : comb_filter(T_old, T_old, g_old, g_old)       // steady; the samples are from the previous encoder frame
out[108..864) : comb_filter(T_old→T_new, g_old→g_new, crossfade f=w[i]² over out[108..216))
T_old = max(T_new, 15); g_old = g_new
```

The crossfade sits at offset 108 (= shortMdctSize = overlap) because the decoder output lags the encoder input by 108.
On the phone, the chain real-prefilter → real-MDCT → `ssc_synthesis_frame` → `ssc_postfilter_frame` reconstructs the
original input with **91.5 dB SNR** (40 frames, random T/qg, on/off and long/short switching). The residual comes from
the encoder's truncated tap gains. Negative controls:

| Variant | SNR |
|---|---|
| No postfilter | 13.9 dB |
| Crossfade at offset 0 instead of 108 | 20.1 dB |

So the 108 offset is required, and correct.

## 4. Band energies, log domain, normalisation and the last band

- **`ssc_compute_band_energies(mode, X, bandE, end)`** is CELT 1.1 fixed `compute_band_energies`:
  - `shift = clz(max)−21`
  - `bandE = sqrt(Σ EXTRACT16(x<<shift)²) >> shift`, **+1** (EPSILON)
  - channel 1 at `bandE[22+i]`

  Device check: within 0.4–0.7% of `sqrt(Σx²)+1`, which is the 16-bit truncation.
- **`ssc_amp2Log2(mode, end, bandE, logE)`** is the CELT 1.1 fixed `amp2Log2` without the effEnd split:
  - `logE[i] = celt_log2(bandE<<2) − (eMeans[i]<<6)` in Q10 (DB_SHIFT 10). Same polynomial: C = {−6801+8, 15746, −5217, 2545, −1401}.
  - `eMeans` = `ssc_eMeans_tmp` @0x5d78 = CELT's table (103, 100, 92, 85, 81, 77, 72, 70, 78, 75, 73, 71, 78, 74, 69, 72, 70, 74, 76, 71, 60…) in Q4
  - So `logE = log2(bandE_fixed/4096) − eMeans[i]/16` (device error 6e-4 log2 units). Given SIG_SHIFT=12, this is
    exactly the CELT float definition `log2(bandE_float) − eMeans`. Channel 1 is at +22.
- **`ssc_normalise_bands`** is CELT 1.1 fixed `normalise_bands`: `Xn = X/bandE` as int16 Q14 (device error 1.2e-4).
- **Decoder `denormalise_bands`:** `freq[j] = Xn[j]·2^(logE[i]+eMeans[i])` for bins [8·eBands[i], 8·eBands[i+1]),
  i ∈ [start, end); zero elsewhere (including 8·eBands[end]..863). This is unchanged CELT float.
  Device: lib normalise → `ssc_denormalise_bands` matches X/4096 to 1e-3 (the Q14 limit).

### 4.1 Which bands are coded

- `st+0x14` is set to `mode->nbEBands` = **22** every frame (ssc_encode L354). `effEBands` = 21 (mode+0xc) is not used
  as the coded end.
- Energies and logs are computed for all 22 bands.
- **Coded end = 22 − red**, which agrees with agents B and E:
  - non-hq: red = 0, so **band 21 is coded** (bins 672..815 at 44.1k, 720..863 at 96k)
  - 152k (0xE6): red = 4
  - E-family hq: red = 0..3 from the last-band search below

  Bins 816..863 (44.1k mode) are above eBands[22] and are always zero in the decoder.
- **Bitrate < 192000** (both modes): the encoder zeroes MDCT bins `8·eBands[21] .. 863` for both channels before the
  energies. In transient frames this only happens if !hq. So band 21 is coded as silence (the bandwidth is ~17.1 kHz at
  44.1k). The decoder just decodes it; there is nothing to undo.

### 4.2 `ssc_compute_band_energies_for_lastband` / `CalculateBandEnergyLastBand` / `last_band_info_calc`

`ssc_compute_band_energies_for_lastband(mode, X, out, nsub, hq, idx)` computes CELT-style sub-band amplitudes
(`sqrt(Σx²)+1`) over sub-bands of the top region:
- non-hq: start `8·eBands[21]`, edges @0x27f8 = {708, 744, 780, 816} (4 × 36 bins of band 21)
- hq: start `8·eBands[21−idx]`, 8 × 18-bin sub-bands from @0x2840+16·idx (rows {738..864}, {594..720}, {450..576}, {306..432})

The outputs are ch0 in `out[0..nsub)` and ch1 in `out[nsub..2nsub)`.
- **non-hq:** `b2 += 4·lb`, where lb is the highest sub-band s ∈ {3, 2, 1} with `out[s]+out[s+4] > 30001` (≈7.3 in
  16-bit units), else 0. This is side information only.
- **hq (E-family):** for idx = 0..3, find the highest sub-band c with `out[c]+out[c+8] > 12001`.
  - If found: `b2 += 4·idx`, `b3 = (c−1)<<5 | (b3 & 0x1f)` (with c counted from 1), and red = idx.
  - If none found: red = 3, c = 0.
- `CalculateBandEnergy`, `CalculateBandEnergyLastBand` and `last_band_info_calc` (= "find the highest sub-band whose
  2-channel mean > threshold") are dead out-of-line helpers (no callers).
- None of this modifies the spectrum. For the decoder it only defines `end` (and optional bandwidth side information).

## 5. Headroom and DRC: encoder-only

- **`SSC_DRC_Exe`** (input PCM limiter): signal-dependent and not transmitted. The decoder ignores it.
- **`normal_headroom` (non-hq) / `uhq_headroom` (hq)** multiply MDCT bins *before* the energy computation, so the
  attenuation is part of the coded signal. The gain is a fixed function of (mode, bitrate, hq), and the same per-index
  gain is applied in long and interleaved short spectra. Measured on device:

| Function / condition | Effect |
|---|---|
| normal, rate ≥ 96000 | `X[j] *= headroom_gain[j − 8·eBands[20]]/32768` for j ∈ [8·eBands[20]+68, 8·eBands[22]). Table @.data 0x20140 has 240 Q15 entries, 31308→26990. 44.1k: bins 644..815, −0.97…−1.68 dB |
| normal, rate < 96000 (88k) | ramp `32767−3(j−i0)` on [i0=8·eBands[17], i1=8·eBands[20]−15), then `0x7767−3(i1−i0)−5(j−i1)` up to 8·eBands[22]. 44.1k: 0…−1.19 dB over bins 320..815 |
| uhq, 44.1k mode | bins 350/354..863: −0.56…−1.09 dB, piecewise constant; the ≥270k class differs slightly |
| uhq, 96k mode | bins 288..863: −0.5…−0.59 dB |

  **Recommendation:** do not invert it. It is a deliberate ≤1.7 dB high-frequency headroom tilt, and Samsung's
  decoder almost certainly plays it as is. If wanted, `ssc_headroom_gain()` in the snippets returns the exact per-bin
  gain; it matched 47,760/47,760 bins on device, and a decoder could divide `freq` by it before the IMDCT.
- **Encoder bug:** with the **96k mode and a non-hq bitrate ≥ 96k**, `normal_headroom` indexes `headroom_gain` up to
  entry 287 and reads past the table into .bss (DRC state, scratch). Bins 816..863 therefore get garbage gains, which
  can be negative or zero. This is not invertible, and it is harmless for decoding.

## 6. Other encoder quirks found

- `ssc_comb_filter_function3_asm` / `function4_asm` use v8–v11 without saving d8–d11. This violates AAPCS64 (those
  registers are callee-saved). It corrupted my optimized test caller's FP locals, so the harness is built at -O0.
  It is an encoder-side hazard only.
- hq + 16-bit PCM: shift 4, so the signal is coded 48 dB low (see §1).
- The transient path adds 0x600 (1.5 in log2 units = LM/2) to a copy of the long-block bandLogE (`bandLogE2` in CELT).
  This is encoder analysis only.

## 7. Verification artifacts

- `re/findings/D_decoder_snippets.c`:
  - window table, eBands, eMeans
  - `ssc_denormalise_bands`
  - `ssc_imdct_init` / `ssc_synthesis_frame` (IMDCT via an N/4 mixed-radix FFT: 432 = 4·4·3·3·3, 54 = 2·3·3·3; ~12 KB tables, no big stack arrays)
  - `ssc_comb_filter` / `ssc_postfilter_frame` (1026+864 floats of state per channel)
  - `ssc_output_float`
  - optional `ssc_headroom_gain`

  It compiles clean with `-std=c99 -pedantic -Wall -Wextra -Wdouble-promotion`.
- `re/findings/D_snip_test.c`: the on-device test, which dlopens the real library. Build it with NDK clang at **-O0**
  and run it as `./snip_test /system/lib64/libScalable_Encoder.so`. Last results:
  - window vs formula: 2.9e-8
  - PR long/short/mixed: 121.1 / 129.1 / 122.6 dB
  - prefilter→postfilter: 91.5 dB (controls 13.9 / 20.1 dB)
  - denormalise: relative error 7.6e-4
  - headroom: 0 mismatches
