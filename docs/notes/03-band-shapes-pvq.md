# C: Band shape quantisation (bands.c / vq.c / cwrs.c)

> Research notes written while reverse-engineering SSC. Addresses are offsets in Samsung's libraries on a
> Galaxy S26 Ultra. References to `re/...` files, decompiler output and disassembly point to working files
> that are not part of this repository. The cleaned-up specification is [`../bitstream.md`](../bitstream.md).

Scope: `ssc_quant_all_bands`, `FUN_00109038`, `FUN_00109460`, `FUN_00109ca4`, `ssc_alg_quant`, `ssc_encode_pulses`,
`ssc_stereo_itheta`, `ssc_haar1`, `ssc_spreading_decision`, `FUN_001167d0`, `ssc_bitexact_cos_asm`, `ssc_cos_norm`,
`ssc_rsqrt_norm`, `ssc_frac_div32`. Reference: libopus 1.1 `celt/bands.c, vq.c, cwrs.c, rate.h`.
Addresses below are file vaddrs (objdump). Ghidra names are vaddr + 0x100000 (FUN_00109460 = 0x9460, DAT_00102760 = 0x2760).

## 0. Verdict

The band-shape coder is **opus 1.1 `quant_all_bands` compiled with `encode=1` and without RESYNTH**, specialised for
`start=0, C=2, LM=3`. The bitstream syntax and all integer bit accounting match opus 1.1 exactly. I found no
change to theta coding, qn, rebalance, bits2pulses, PVQ search output format, or CWRS. The things the decoder
must do differently from stock opus 1.1 are:

1. **No `effEBands` redirect.** The mode has effEBands=21, but `ssc_encode` sets `st->end = mode->nbEBands = 22`
   on every frame (`ssc_encode`: `*(st+0x14) = *(mode+8)`). The encoder codes band 21 (bins 672..815) into the real X.
   Opus 1.1 would redirect `X=norm` for `i>=effEBands` and would also stop synthesis at effEBands. **Remove both.**
2. **Use Samsung's pulse-cache tables verbatim** (`mode+0x68` index, `mode+0x70` bits). In the 44.1k mode, band 20
   (eBands 72..84, N0=12) points at the N0=18 cache entry in all 5 LM rows (02-bit-allocation.md has the same finding). This
   changes `bits2pulses`, `pulses2bits` and the split test `b > cache[cache[0]]+12` for band 20. The PVQ itself still
   uses the true N. (96k cache: index and bits match `compute_pulse_cache` exactly.)
3. Always stereo (C=2, Y = X+864). Mono has no path through this code.

Everything else in the decoder (folding/norm buffer, lowband_out, update_lowband, noise fill, stereo_merge, inv,
anti-collapse, LCG) is resynth-only code. The encoder binary does not contain it, so it cannot be checked
against this binary. It does not affect parsing. Use opus 1.1 decoder behaviour; `C_decoder_snippets.c` does that.

## 1. Function map

| Samsung | vaddr | opus 1.1 | notes |
|---|---|---|---|
| `ssc_quant_all_bands` | 0x8898 | `quant_all_bands` | encode=1, start=0, C=2, LM=3 folded. `quant_band_stereo` and `compute_theta(stereo=1)`, `intensity_stereo`/`stereo_split` call sites inlined |
| `FUN_00109038` | 0x9038 | `quant_band` | args `(ctx, X, N, b, B, gain, lowband_scratch, fill)`. lowband/lowband_out/LM gone; 7th arg unused |
| `FUN_00109460` | 0x9460 | `quant_partition` | args `(ctx, X, N, b, B, LM, gain, fill)`, with `compute_theta(stereo=0)`, mono `stereo_itheta`, `bits2pulses`, `pulses2bits` and `get_pulses` inlined |
| `FUN_00109ca4` | 0x9ca4 | `intensity_stereo` | encoder only |
| `ssc_stereo_itheta` | 0x16554 | `stereo_itheta(X,Y,stereo=1,N)` | encoder only |
| `FUN_001167d0` | 0x167d0 | `exp_rotation1(X,len,stride=1,c,s)` | static, placed after stereo_itheta. The stride2 pass is inlined in alg_quant |
| `ssc_alg_quant` | 0x1571c | `alg_quant(X,N,K,spread,B,enc)` | real args x0..x5. No gain/resynth |
| `ssc_encode_pulses` | 0x9e38 | `encode_pulses` (table version) | `icwrs` + `ec_enc_uint(i, CELT_PVQ_V(N,K))` |
| `ssc_haar1` | 0x87fc | `haar1` | called by tf_analysis in ssc_encode. Also inlined in quant_band |
| `ssc_spreading_decision` | 0x85ac | `spreading_decision` | encoder heuristic. Only the result is transmitted |
| `ssc_bitexact_cos_asm` | 0x17400 | `bitexact_cos` ×2 | NEON: computes cos(itheta) and cos(16384-itheta) in one call. Bit-identical |
| `ssc_cos_norm` | 0x11670 | `celt_cos_norm` (fixed) | used only by exp_rotation |
| `ssc_rsqrt_norm`, `ssc_frac_div32` | 0x11558, 0x11370 | — | **not used by band code** (callers: `ssc_remove_doubling`, `_ssc_lpc`) |

`band_ctx` layout is the opus 1.1 one: `+0 encode, +8 m, +0x10 i, +0x14 intensity, +0x18 spread, +0x1c tf_change,
+0x20 ec, +0x28 remaining_bits, +0x30 bandE, +0x38 seed`.

## 2. Call from ssc_encode (@0xf2xx, decompile line 3763)

```
ssc_quant_all_bands(mode, end = st->end - k, X = &DAT_0012238c (static int16 norm buffer, Y = X+864),
    collapse_masks = &local_cc, bandE, pulses, shortBlocks (8 or 0), spread = st->spread (st+0x20),
    dual_stereo, intensity = st->intensity (st+0x50), tf_res,
    total_bits = nbBytes*8<<3 - anti_collapse_rsv, balance, ec, codedBands, &st->rng (st+0x1c), 1 /*unused*/)
```
- `st->end` = 22 (reset every frame), so `end = 22 - k`. k is computed from the "last band" sub-band energies
  (`ssc_compute_band_energies_for_lastband`): k=0 in the non-hq branch, k∈0..3 in the hq branch (written to header
  byte 2 as `+= k*4`), and **k=4 when header byte 3 == 0xE6 (152k)**. The header agent should confirm this.
  The same `end` is used for coarse/fine/finalise energy.
- After quant_all_bands: `if (anti_collapse_rsv) ec_enc_bits(consec_transient < 2, 1)`. anti_collapse_rsv = 8 when
  `isTransient && total_bits - tell - 1 >= 40` (LM=3: `(LM+2)<<3`). This matches opus.
- `st->rng` is passed as seed but is not changed. The encoder has no resynth, and the LCG constants do not appear in the binary.

## 3. quant_all_bands (0x8898), compared with opus 1.1 line by line

```
M=8; B = shortBlocks ? 8 : 1; fill0 = (1<<B)-1
for i in 0..end-1:
   tell = ec_tell_frac(ec)
   if (i != 0) balance -= tell                       # identical (start=0)
   remaining_bits = total_bits - tell - 1; ctx.remaining_bits = remaining_bits
   if (i < codedBands) b = max(0, min(16383, min(remaining_bits+1, pulses[i] + balance/min(3, codedBands-i))))
   else b = 0
   ctx.tf_change = tf_res[i]
   if (i == intensity) dual_stereo = 0               # persistent, as in opus
   if dual_stereo: x_cm = quant_band(X, N, b/2, B, fill0); y_cm = quant_band(Y, N, b/2, B, fill0)
   else:           x_cm = y_cm = quant_band_stereo(X, Y, N, b, B, fill0)   # inlined
   collapse_masks[2i] = x_cm; collapse_masks[2i+1] = y_cm
   balance += pulses[i] + tell                        # identical
```
- **Folding/lowband**: in the encoder, `lowband_offset` stays 0 (its update needs `resynth`), so opus also gives
  `effective_lowband=-1` and `x_cm=y_cm=(1<<B)-1`. Samsung is that code with the dead parts removed. The only
  pointer left is a constant scratch pointer (`Y + 8*eBands[nbEBands]`). It is NULL for `i==end-1` (opus's
  `lowband_scratch=NULL` on the last band) and quant_band ignores it.
- **tf_change**: passed through ctx, applied in quant_band (below). Identical.
- **intensity / dual_stereo**: identical semantics.
- **update_lowband, norm buffer, `norm[j]=HALF(norm+norm2)` at the intensity band**: resynth-only, absent. The decoder uses opus.
- **effEBands**: the `if (i>=m->effEBands) X=norm` branch is absent. See §0.1.
- **Last band**: the only special case is `lowband_scratch = NULL` (and `lowband_out = NULL` in the decoder).
  The `*LastBand*` energy functions only measure sub-band energies for header signalling. They never write X.
  `CalculateBandEnergyLastBand` and `last_band_info_calc` are not called anywhere in the encoder.

### Inlined quant_band_stereo + compute_theta(stereo=1)
```
pulse_cap = logN[i] + 24;  offset = (pulse_cap>>1) - 4          # QTHETA_OFFSET=4, N>=8 so never TWOPHASE
N2 = 2N-1; qb = min(b - pulse_cap - 32, (b + N2*offset)/N2); qb = min(64, qb)
qn = qb < 4 ? 1 : ((exp2_table8[qb&7] >> (14 - (qb>>3))) + 1) & ~1  # exp2_table8 @0x2760 = {16384,17866,19483,21247,23170,25267,27554,30048}
itheta = stereo_itheta(X,Y,1,N); tell = ec_tell_frac
if (i < intensity && qn != 1):
    x = (itheta*qn + 8192) >> 14                                  # rounding identical
    step pdf: x0=qn/2, ft=3*(x0+1)+x0, fl/fh as opus (p0=3)
    itheta = (x<<14)/qn
    itheta==0 ? intensity_stereo : stereo_split
else:
    inv = itheta > 8192; if inv Y=-Y; intensity_stereo
    if (b > 16 && remaining_bits > 16) ec_enc_bit_logp(inv, 2) else inv=0
    itheta = 0
qalloc = ec_tell_frac - tell; b -= qalloc; remaining_bits -= qalloc
itheta==0     : iside=0,     delta=-16384, fill unchanged ((1<<B)-1)
itheta==16384 : iside=32767, delta= 16384, fill = 0
else          : imid=bitexact_cos(itheta), iside=bitexact_cos(16384-itheta),
                delta = FRAC_MUL16((N-1)<<7, bitexact_log2tan(iside, imid))
mbits = max(0, min(b, (b-delta)/2)); sbits = b - mbits            # C division (code adds 1 when negative)
mbits>=sbits: mid(X, mbits, gain=32767, fill) ; reb=mbits-(rem0-rem); if(reb>24 && itheta!=0) sbits+=reb-24; side(Y, sbits, gain=iside, fill>>B)
else        : side first, then if(reb>24 && itheta!=16384) mbits+=reb-24; mid
```
This matches opus 1.1. `bitexact_log2tan` is inlined (`clz-17` for `15-EC_ILOG`, constants -2597 = -0xa25 and 7932 = 0x1efc).

## 4. quant_band (FUN_00109038)

Same as opus 1.1: `N_B=N/B`, `recombine=max(0,tf_change)`, `haar1(X,N>>k,1<<k)` and
`fill = bit_interleave_table[fill&15] | bit_interleave_table[fill>>4]<<2` (table @0x27c0 = `0,1,1,1,2,3,3,3,2,3,3,3,2,3,3,3`),
then the time-divide loop `while(!(N_B&1) && tf_change<0){haar1(X,N_B,B); fill|=fill<<B; B<<=1; N_B>>=1; tf_change++}`,
then `deinterleave_hadamard(X, N_B>>recombine, B<<recombine, hadamard = (B_orig==1))` with
`ordery_table` @0x2880 = `1,0, 3,0,2,1, 7,0,4,3,6,1,5,2, 15,0,8,7,12,3,11,4,14,1,9,6,13,2,10,5` (identical; stack tmp of 144 int16).
Then `quant_partition(ctx, X, N, b, B, LM=3, gain, fill)`. There is no N==1 case (N ≥ 8 always).

## 5. quant_partition (FUN_00109460)

- Cache: `cache = bits + index[(LM+1)*nbEBands + i]` with mode `+0x68/+0x70`. Split if `LM != -1 && N >= 3 && b > cache[cache[0]] + 12`.
- Split: `N>>=1; Y=X+N; LM--; if(B==1) fill=(fill&1)|(fill<<1); B=(B+1)>>1`. Mono compute_theta:
  `pulse_cap = logN[i] + 8*LM` (new LM), offset `(pulse_cap>>1)-4`, same qn. Coding: **uniform** `ec_enc_uint(x, qn+1)`
  if `B0>1` (B before halving), otherwise **triangular** with `ft=(qn/2+1)^2` and `fl,fs` as in opus. `itheta=(x<<14)/qn`.
  Then the same imid/iside/delta/fill rules as above. Then
  `if (B0>1 && (itheta&0x3fff)) { if (itheta>8192) delta -= delta>>(4-LM); else delta = min(0, delta + (N<<3>>(5-LM))); }`
  (coded as `N>>(3-LMold)` and `>>(5-LMold)`, which gives the same result), `mbits = max(0,min(b,(b-delta)/2))`
  (coded as `>>1`, which is the same after the max(0)), same rebalance (+24 threshold),
  `gain*mid`/`gain*side` with MULT16_16_P15, `cm = cm_mid | cm_side << (B0>>1)`.
- Leaf: `bits2pulses` is the 6-step bisection on `bits-1` with the final `lo/hi` choice (`lo==0 ? -1 : cache[lo]`).
  `pulses2bits = q ? cache[q]+1 : 0`. Then `while (remaining_bits < 0 && q > 0)` backoff, `K = get_pulses(q)`
  (`q<8 ? q : (8+(q&7))<<((q>>3)-1)`), and `alg_quant(X, N, K, spread, B, ec)` if q>0. If q==0 the encoder returns cm=0.
  All identical.

## 6. alg_quant / exp_rotation / encode_pulses (vq.c, cwrs.c)

- `exp_rotation(X, N, dir=+1, stride=B, K, spread)`: skip if `2K >= N || spread == 0`. `SPREAD_FACTOR` @0x5ebc = **{15,10,5}**.
  `gain = celt_div(32767*N, N + factor*K)` (`rcp * (N<<15 - N) >> 31`), `theta = gain*gain >> 16`,
  `c = cos_norm(theta)`, `s = cos_norm(32767-theta)` (`theta^0x7fff`). If `N >= 8B`: `stride2` = first s where
  `(s*s+s)*B + (B>>2) >= N`. For each of the B blocks: `exp_rotation1(.., 1, c, -s)` then (if stride2)
  `exp_rotation1(.., stride2, s, -c)`. The second pass is inlined and I checked its operands. Its backward loop
  starts at `len-2*stride-1`, the same as opus 1.1. It truncates with `>>15` and no rounding (also opus 1.1).
  Decoder: `dir=-1` does `exp_rotation1(stride2, s, c)` then `exp_rotation1(1, c, s)`.
- The PVQ search is opus 1.1's (projection pre-search when `K > N/2`, then greedy). Sign handling and
  `if (N+3 < pulsesLeft) iy[0] += pulsesLeft` are also opus 1.1's. The search only affects encoder quality.
- Collapse mask: `B<=1 ? 1 : OR over blocks of (iy[block*N0+j]!=0)<<block`. Identical.
- `encode_pulses`: `icwrs` (table version) + `ec_enc_uint(i, U(N,K)+U(N,K+1))` with
  `U(n,k) = ROW[min(n,k)][max(n,k)]`. Row pointer table @0x1b948 (15 rows) → data @0x28f8, offsets
  0,176,351,525,698,870,1041,1131,1178,1207,1226,1240,1248,1254,1257. The data is **bit-identical to opus 1.1's
  non-CUSTOM_MODES `CELT_PVQ_U_DATA[1272]`** (verified entry by entry, `re/elf.py`). It is not computed at run time.
- Reachable (N, Kmax) pairs from both modes' caches (including the band-20 quirk):
  (2,128) (3,128) (4,128) (6,88) (8,36) (9,26) (12,16) (16,12) (18,11) (24,9) (32,7) (36,7) (48,6) (64,5) (72,5) (96,5) (144,4).
  All of them are inside the opus table bounds.
- **Check:** a Python port of the SMALL_FOOTPRINT `ncwrs_urow`/`cwrsi` (used in the snippets) decoded 2340 random
  vectors that were encoded with Samsung's `icwrs` + U table. All pairs above were covered, with 0 mismatches,
  and `ncwrs_urow` V(N,K) equalled the table V for every pair.

## 7. Helpers

- `ssc_bitexact_cos_asm(a, b, &ca, &cb)`: `x2=(x*x+4096)>>13; t=(-626*x2 +16384)>>15; t=(x2*(8277+t)+16384)>>15;
  t=(x2*(-7651+t)+16384)>>15; out = t - x2 + 32768`. This equals opus `1 + (32767-x2) + FRAC_MUL16(...)`. The result is
  never more than 32767 for reachable itheta (itheta ≥ 64 because qn ≤ 256).
- `ssc_cos_norm` = opus fixed `celt_cos_norm` (L1..L4 = 32767, -7651, 8277, -626, MULT16_16_P15).
- `ssc_haar1` = haar1 with `(x*0xb504)>>16`. Since 0xb504/65536 = 23170/32768, this is `MULT16_16_Q15(23170,x)`.
  `stereo_split` in quant_all_bands uses the same form.
- `ssc_stereo_itheta`: opus stereo version (`(x>>1)±(y>>1)` energies, `sqrt`, atan2 approximation with 0x2690 / 0x6488 /
  0x517d). Encoder only.
- `intensity_stereo` (FUN_00109ca4): opus fixed version (`shift = ilog2(max(El,Er))-13`, `a = (v<<14)/(1+sqrt(...))`,
  `X = (a1*l>>14) + (a2*r>>14)`). Encoder only. The decoder does not need it: with itheta=0 the side gets 0 bits and
  stereo_merge copies mid to Y.

## 8. Spreading

- `spread_icdf` @0x3d14 = **{25,23,2,0}**, coded with `ec_enc_icdf(.., 5)` only when `tell+4 <= total_bits`.
  Otherwise nothing is coded. The opus decoder then uses SPREAD_NORMAL (2). The encoder instead keeps `st->spread`
  from the previous frame (it starts at 2). This can only differ at extremely small budgets and only changes
  exp_rotation in reconstruction, not parsing.
- Encoder choice: `!transient && nbAvailableBytes >= 10*C` → `ssc_spreading_decision`, else 2. The heuristic loops
  bands 8..end with widths >1 (same as opus skipping N<=8). Thresholds 0x80/0x200/0x800 per Q15 x² · N, and the
  hf_sum uses the last 4 bands. The decoder does not need it.

## 9. Anti-collapse, seed

Not present in the encoder (it is resynth/decoder-only in opus). The only bitstream element is the
anti-collapse raw bit after quant_all_bands (§2). Decoder: opus 1.1 `anti_collapse` with C=2, LM=3, size=864,
`seed = ctx.seed` after quant_all_bands, LCG `seed = 1664525*seed + 1013904223`.

## 10. Decoder checklist

1. Before quant_all_bands, zero X[0..2*864).
2. Use `end = 22 - k`. Bands up to 21 are real bands: no effEBands redirect, and synthesis/denormalise
   covers `eBands[end]*8` (up to 816).
3. Use the dumped `cache.index` / `cache.bits` / `logN` (from B_decoder_snippets.c tables) for bits2pulses, the split test, and qn.
4. Range decoder: SSC's modified constants (see 01-range-coder-and-energy.md). The API is the opus one
   (`ec_tell_frac, ec_decode, ec_dec_update, ec_dec_uint, ec_dec_bit_logp`).
5. Everything else follows `C_decoder_snippets.c` (opus 1.1 decode path, float signal math, exact integer accounting).
