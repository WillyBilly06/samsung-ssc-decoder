# B: Bit allocation (rate.c) in SSC

> Research notes written while reverse-engineering SSC. Addresses are offsets in Samsung's libraries on a
> Galaxy S26 Ultra. References to `re/...` files, decompiler output and disassembly point to working files
> that are not part of this repository. The cleaned-up specification is [`../bitstream.md`](../bitstream.md).

Sources: `ssc_compute_allocation` @0x149d8 (decompile `re/fn/ssc_compute_allocation.c`, disassembly
`sh re/fn.sh ssc_compute_allocation`), call site `ssc_encode` @0xf238, pulse-cache use in
`FUN_00109460` (quant_band). Reference: opus-1.1 `celt/rate.c` and `rate.h`.

**Summary.** The allocation is opus-1.1 `clt_compute_allocation` with `interp_bits2pulses` inlined,
specialised to **start=0, C=2, LM=3**. The arithmetic is the same as CELT. All the CELT constants
are unchanged. These parts differ:
- The allocVectors tables (11×22): two tables per mode, chosen by bitrate.
- The `cap[]` source: a precomputed int32 table in the mode, with no init_caps.
- The pulse-cache index for 44.1k band 20.

The decoder version is in `B_decoder_snippets.c`. It was syntax-checked with NDK clang
`-std=c99 -Wall -Wextra -pedantic`, and the only warnings are about unused statics.

---

## 1. Signature and argument mapping

```
int ssc_compute_allocation(CELTEncoder *st /*x0*/, AllocCtx *a /*x1*/, ec_enc *ec /*x2*/,
                           int end /*w3*/, int signalBandwidth /*w4*/);   // returns codedBands
```
Call (ssc_encode @0xf20c–0xf238):
```
a->total = (nbBytes*64) + ~ec_tell_frac(ec) - anti_collapse_rsv;   // = bits - tell - 1 - rsv
codedBands = ssc_compute_allocation(st, a, ec, st->end(0x14) - lastband_red,
                                               st->end(0x14) - lastband_red - 1);
```
`st+0x14` is set to `mode->nbEBands` (22) at frame start. `lastband_red` is `uVar131`/w26, which is 0..4 (see §6).
anti_collapse_rsv = 8 if `isTransient && (bits-tell-1) > 39`, which is `(LM+2)<<3 = 40` with LM=3.

`AllocCtx` is a stack block in ssc_encode at sp+0x3e0 (Ghidra `local_2b0`). It has int offsets:

| a-offset | int idx | CELT parameter | ssc_encode local |
|---|---|---|---|
| 0x000 | 0 | `total` (in) | local_2b0[0] |
| 0x004 | 1..22 | `ebits[]` (out, = fine_quant) | passed as `local_2b0|4` to quant_fine_energy |
| 0x05c | 0x17.. | `pulses[]` (out) | auStack_254 |
| 0x0b8 | 0x2e (ptr) | `cap` (int*) | local_1f8 = `mode->cache.caps` (+0x78) **raw pointer** |
| 0x0c0 | 0x30.. | `offsets[]` (dynalloc) | local_1f0 |
| 0x118 | 0x46.. | `fine_priority[]` (out) | auStack_198 |
| 0x170 | 0x5c | `alloc_trim` | local_140 |
| 0x174 | 0x5d | `*dual_stereo` (in/out) | local_13c |
| 0x178 | 0x5e | `*balance` (out) | local_138 |
| 0x180 | 0x60 | **table select** | local_130 = `bitrate > 192000` |

The function reads these fields from `st`: `*st` = mode, `st+0x2c` = `prev` (lastCodedBands), and
`st+0x50` = `*intensity` (in/out).
After the call, ssc_encode updates `lastCodedBands` with the CELT rule:
`if (last) codedBands' = clamp(codedBands, last-1, last+1)`.

Mapping to `clt_compute_allocation(m,start,end,offsets,cap,alloc_trim,intensity,dual_stereo,total,balance,pulses,ebits,fine_priority,C,LM,ec,encode,prev,signalBandwidth)`:
m=st->mode, **start=0**, end=w3, offsets=a+0xc0, cap=mode+0x78 table, alloc_trim=a[0x5c],
intensity=&st[0x50], dual_stereo=&a[0x5d], total=a[0], balance=&a[0x5e], pulses=a+0x5c,
ebits=a+4, fine_priority=a+0x118, **C=2**, **LM=3**, ec=x2, encode=1, prev=st[0x2c],
signalBandwidth=w4 (= end-1).

### allocVectors selection
```
ldr w9,[x1,#0x180]; cmp w9,#0; mov w9,#0x30; mov w10,#0x28; csel x9,x10,x9,eq; ldr x4,[mode,x9]
```
In ssc_encode (@0xb70c–0xb734): `flag = (bitrate_arg > 0x2ee00)`, i.e. bitrate > 192000.
- bitrate ≤ 192000 (88k, 96k, 128k, 152k, 192k) → `mode+0x28` (table A)
- bitrate > 192000 (229k, 250k, 256k, 291k, 308k, 328k, 442k, 584k, 886k) → `mode+0x30` (table B)

The selection does **not** use the "hq" flag. The decoder gets the bitrate from header byte 3.

---

## 2. Differences from opus-1.1 rate.c

The constants are all checked in the disassembly.

| constant | CELT | SSC | evidence |
|---|---|---|---|
| BITRES | 3 | 3 | skip_rsv/dual_rsv = 8, the skip bit costs 8 |
| ALLOC_STEPS | 6 | 6 | interp loop: hi=64, 6 iterations, `>>6` |
| FINE_OFFSET | 21 | 21 | `mov w14,#-0x15` |
| MAX_FINE_BITS | 8 | 8 | `cmp w9,#8; csel ...,w15(=8)` |
| LOG_MAX_PSEUDO | 6 | 6 | bits2pulses in quant_band unrolls 6 bisection steps |
| QTHETA_OFFSET | 4 | 4 | quant_all_bands:160 / FUN_00109460:171 `((logN+0x18)>>1) + -4` |
| logM | LM<<3 | 24 | `add w3,w4,#0x18` |
| alloc_floor = C<<BITRES | 8C | 16 | |
| nbAllocVectors | 11 | 11 | mode+0x20; bisection lo=1, hi=10 |

These parts are specialised. Each is equivalent to CELT with C=2, LM=3, start=0:
- **thresh[j]** = `max(16, 12*N0)`, which is `max(C<<3, (3*N0<<LM<<3)>>4)`.
- **trim_offset[j]** = `(2*alloc_trim-16) * (end-j-1) * N0`. This is CELT's
  `C*N0*(trim-5-LM)*(end-j-1)*(1<<(LM+3))>>6`, and the `*64>>6` is exact. There is no
  single-coefficient adjustment because `N0<<3` is never 1.
- **bits per allocVector entry** = `4*N0*alloc[mid*22+j]` (`C*N*alloc<<LM>>2`).
- **First bisection** (allocVectors): the same as CELT. The `psum += min(bitsj, cap)` uses a saturating
  add clamped to ±0x7fffffff, which has no effect in practice. `psum > total → hi=mid-1 else lo=mid+1`.
- **bits1/bits2**: the same, including `bits2=cap[j]` when `hi >= 11`, offsets added to bits1 only if `lo>0`,
  and `skip_start` = the last j with `offsets[j]>0`.
- **Interp bisection**: the same (lo=0, hi=64, 6 steps, alloc_floor 16).
- **Skip loop**: the same. The skip-flag coding threshold is `max(thresh[j], 24)`.
  The encoder's decision is `codedBands<=2 || (band_bits > bw*(codedBands<=prev ? 28 : 36) && j<=signalBandwidth)`,
  and 28/36 are `(7 or 9)*bw<<3<<3>>4`. If true it codes `ec_enc_bit_logp(1,1)` and stops, otherwise it codes `0` and the band is
  skipped (psum += 8, band_bits -= 8). For a decoder this is simply
  `if (band_bits >= max(thresh,24)) { if (ec_dec_bit_logp(ec,1)) break; ... }`.
  If the loop ends at `j<=skip_start`, skip_rsv is added back to total.
- **intensity_rsv handling** in the skip loop: `intensity_rsv = LOG2_FRAC_TABLE[j]` (start=0). This is the same as CELT.
- **Reservations**: `skip_rsv = total>7 ? 8 : 0`, `intensity_rsv = LOG2_FRAC_TABLE[end]` (0 if it exceeds
  total), and `dual_stereo_rsv = (total-intensity_rsv >= 8) ? 8 : 0`. These are always applied because C is fixed at 2.
- **Intensity**: `*intensity = min(*intensity, codedBands); ec_enc_uint(*intensity, codedBands+1)`.
  For the decoder: `intensity = ec_dec_uint(ec, codedBands+1)`. If intensity_rsv is 0, intensity = 0.
  If intensity ≤ 0, total += dual_stereo_rsv and dual_stereo = 0. Otherwise, if dual_stereo_rsv > 0, read
  `dual_stereo = ec_dec_bit_logp(ec,1)`.
  `ssc_ec_enc_uint` is standard (EC_UINT_BITS=8, and ft ≤ 23 is always a single symbol).
- **Remaining bits**: percoeff and the remainder are spread over eBands widths, the same as CELT.
- **Fine bits**: the `N>1` branch is always taken because N = 8*N0.
  - `den = 16*N0 + (!dual_stereo && j<intensity)`
  - `NClogN = den*(logN[j]+24)`, `offset = (NClogN>>1) - 21*den`
  - `+NClogN>>2` if `bits+offset < 16*den`, else `+NClogN>>3` if `< 24*den`
  - `ebits = max(0,(bits+offset+4*den)/(8*den))`, and `ebits = bits>>4` if `2*ebits > bits>>3`, then
    `min(ebits, 8)`
  - `fine_priority = ebits*8*den >= bits+offset`, `bits -= 16*ebits`
  - The excess / extra_fine logic is the same: `extra_fine = min(excess>>4, 8-ebits)` and
    `fine_priority = 16*extra_fine >= excess-balance`. The N==2 offset and N==1 branch are absent (unreachable).
- **Skipped bands** (j ≥ codedBands): `ebits = bits>>4`, `bits = 0`, `fine_priority = (bits_before < 16)`.
- **cap[]** comes from the mode, not from init_caps (see §3).

There are **no functional differences** from CELT apart from the tables, the cap source, and the fixed
start/C/LM values.

**Mono note:** `ssc_encoder_init` accepts 1 or 2 channels, but the allocation always assumes C=2.
That includes the intensity and dual-stereo reservation and coding, and `alloc_floor=16`.

---

## 3. Pulse cache and caps (mode +0x60..+0x78)

| | 44.1k/48k mode @0x1b9c0 | 96k mode @0x1ba40 |
|---|---|---|
| cache.size (+0x60) | 346 | **334** |
| cache.index (+0x68) | @0x4834, 110 × int16 (5 rows LM=-1..3, ×22) | @0x5ad4 |
| cache.bits (+0x70) | @0x4910, 346 B (+2 pad) | @0x5bb0, 334 B (+2 pad) |
| cache.caps (+0x78) | @0x4a6c, **22 × int32** | @0x5d00, **22 × int32** |

**cache.bits** is a run of entries `[K, b(1)..b(K)]`.
- 44.1k entry starts are 0,41,82,123,164,200,222,248,265,277,290,300,308,316,323,329,335,341, and the last entry ends at 346.
- Entry N values (for 44.1k) are 1,2,3,4,6,9,8,12,18,16,24,36,32,48,72,64,96,144.
- The table matches opus-1.1 `compute_pulse_cache` for both modes, as the coordinator's phone run also found.

**cache.index.** I re-ran CELT's index algorithm against these eBands:
- **96k**: identical to CELT.
- **44.1k**: exactly 5 entries differ, all for **band 20** (eBands 72..84, N0=12), one in every LM row:

| row (LM) | CELT (N0=12 entry) | Samsung |
|---|---|---|
| -1 | 164 (N=6) | 200 (N=9) |
| 0 | 248 (N=12) | 265 (N=18) |
| 1 | 290 (N=24) | 300 (N=36) |
| 2 | 316 (N=48) | 323 (N=72) |
| 3 | 335 (N=96) | 341 (N=144) |

  Band 20 therefore uses the **N0=18 cache entries**, the same ones as band 21.
  This is exactly what CELT produces if the eBands end `…,60,72,90,108` instead of `…,60,72,84,102`. In that case bands 20 and 21 are both 18 wide, and the
  cache.bits contents and order are unchanged because the N=9 entry is still created at the same point. The cache was
  most likely built from an older band layout and never regenerated.

  What this means: in quant_band, `bits2pulses`/`pulses2bits` and the split test
  `b > cache[cache[0]]+12` for band 20 use the pulse-count/bit table of an 18-wide band. The PVQ
  itself still runs on the real N (12<<LM, from eBands; the bands agent should confirm this). The decoder must use the
  dumped index **as-is** to choose the same K. `logN[20]=29` (N0=12) and `cap[20]` follow the real eBands.
- Index lookup in quant_band (FUN_00109460:80) is the CELT one: `cache.bits + cache.index[(LM+1)*22 + i]`.
  bits2pulses and pulses2bits are the CELT versions.

**caps.** Samsung's `cache.caps` is **not** CELT's `unsigned char caps[(maxLM+1)*2*nbEBands]`. It is
**22 int32 values that are already the final `cap[]`** for C=2, LM=3, in 1/8-bit units. ssc_encode stores the
pointer `mode+0x78` directly into the cap slot (local_1f8) and uses it both in the dynalloc loop (`boost < cap[i]`)
and in compute_allocation. Nothing else reads +0x78, and **init_caps does not exist**.

The coordinator saw "48 at caps[0]" because they read the table as bytes: `48,4,0,0` is int32 1072.
- 44.1k: `1072×8, 2120×4, 4192×3, 6024×2, 7648, 9792×3, 9360`
- 96k: `1072×12, 2120×2, 4192×2, 6024×2, 9360×4`

Under the CELT formula `cap = (c+64)*C*N>>2 = 4*N0*(c+64)`, the implied 8-bit caps are 204 (N0=1), 201 (2), 198 (4),
187 (6), 175 (8), 140 (12), 66 (18). They hold for both modes, so the caps were computed from N0 and not tuned
per band. For 44.1k this is computed from the real eBands, so band 20 gets 9792 = 48·204.
Bytes after the 22 ints, such as 0x4ac4… for 44.1k and `ssc_eMeans_tmp` @0x5d78 for 96k, are unrelated data.

---

## 4. Other tables

- `LOG2_FRAC_TABLE` @0x5df0 (24 bytes) is identical to CELT:
  `0,8,13,16,19,21,23,24,26,27,28,29,30,31,32,32,33,34,34,35,36,36,37,37`.
- eBands, logN, and both allocVectors for both modes are dumped in `B_decoder_snippets.c`. Pointers:
  - 44.1k: eBands 0x3f30, allocA **0x3f5e**, allocB 0x4050, logN 0x4142
  - 96k: eBands 0x5988, allocA **0x3f5e** (the same table as 44.1k), allocB 0x59b6, logN 0x5aa8
  - logN is consistent with `log2_frac(N0,3)` of the real eBands.
- **44.1k allocB vs allocA**: rows 0–7 are identical. The differences are:
  - row 8: band 14 is 116→106 (A has a non-monotonic 116) and band 20 is 23→21
  - row 9: bands 0–7 and 16–20 differ (A is `182,180,173,166,158,151,145,139,…,120,110,98,85,28`, B is `180,173,166,160,152,148,142,140,…,124,115,108,93,29`)
  - row 10: band 20 is 113→100
- **96k allocB** is a completely different table with a steep high-band roll-off. For example, row 1 is
  `90,73,59,43,31,24,18,14,10,5,0…`, and rows 5–8 end in `…,1,1,1,1`.
- All allocVectors differ from CELT's `band_allocation`. Row 1 starts at 118, versus 90 in CELT.

---

## 5. Decoder recipe

```
alloc_trim  = (decoded trim icdf, as in celt_decode)
bits        = (nbBytes*8 << 3) - ec_tell_frac(ec) - 1
anti_collapse_rsv = (isTransient && bits >= 40) ? 8 : 0;  bits -= anti_collapse_rsv
codedBands  = ssc_compute_allocation_dec(mode, end, offsets, alloc_trim,
                                         bitrate > 192000, bits, &balance,
                                         pulses, fine_quant, fine_priority,
                                         &intensity, &dual_stereo, ec)
```

---

## 6. `end` is not always 22 (found at the call site; belongs to the header area)

`end = 22 - lastband_red` (`uVar131`) is used for allocation, dynalloc, fine energy, quant_all_bands and finalise.
It is set in ssc_encode (decompile lines ~355–420 and 1346–1409):
- `iVar93` ("uhq") = 1 for 152k, 250k, 256k, 291k, 308k, 328k, 442k, 584k and 886k. `iVar99` = 1 for the same set
  except 256k and 328k.
- If iVar93=0, lastband_red = 0. A 2-bit energy class (0/4/8/12) is still added into header byte 2,
  `*local_570 = param_4+2`.
- If iVar93=1 and header byte 3 == 0xE6 (152k), lastband_red = **4**, so end=18, and the header is not modified.
- If iVar93=1, iVar99=1 and the frame is not 152k: lastband_red ∈ {0..3} is found by an energy search. It is written as
  **header byte 2 += lastband_red*4** (bits 2–3), and the **top 3 bits of header byte 3** are replaced
  by `(cVar96-1)<<5` (or 0).
- 256k and 328k use lastband_red = 0.

The decoder therefore needs header byte 2 bits 2–3 (for iVar99 rates), or the 152k rule, to get `end`.
The header agent should confirm this. ../bitstream.md's description of b2/b3 does not include these fields yet.
