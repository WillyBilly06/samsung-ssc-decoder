# A: Range coder, Laplace, and energy quantisation (SSC vs opus 1.1)

> Research notes written while reverse-engineering SSC. Addresses are offsets in Samsung's libraries on a
> Galaxy S26 Ultra. References to `re/...` files, decompiler output and disassembly point to working files
> that are not part of this repository. The cleaned-up specification is [`../bitstream.md`](../bitstream.md).

Sources checked: the decompile in `re/fn/*.c`, checked line by line against the disassembly (`fn.sh`, `enc.dis`,
`ssc_encode.dis`). Tables were dumped with `elf.py`. Reference: `ref/opus-1.1/celt/{entenc,entdec,entcode,mfrngcod,laplace,quant_bands}.c/h`.

## TL;DR

There is **one real range-coder difference**: **`EC_CODE_BITS = 24`** (opus uses 32). Everything else in
entenc/laplace/quant_bands matches opus 1.1 **fixed point** with `DB_SHIFT = 10`, LM = 3, C = 2 and start = 0.

| item | opus 1.1 | SSC |
|---|---|---|
| EC_SYM_BITS | 8 | 8 |
| EC_CODE_BITS | 32 | **24** |
| EC_CODE_TOP | 2^31 | **2^23 (0x800000)** |
| EC_CODE_BOT | 2^23 | **2^15 (0x8000)** (normalise while `rng <= 0x8000`) |
| EC_CODE_SHIFT | 23 | **15** |
| EC_CODE_EXTRA (decoder) | 7 | 7 (=(24-2)%8+1) |
| initial nbits_total (enc) | 33 | **25** |
| decoder initial nbits_total | 33-24=9 | 25-16 = **9** (the same value; this is a coincidence) |
| EC_WINDOW_SIZE (raw bits) | 32 | 32 |
| EC_UINT_BITS | 8 | 8 |
| BITRES | 3 | 3 |
| raw bits (`ec_enc_bits`) | written backwards from end of buffer | identical |
| ec_enc_done | — | identical algorithm (with 24-bit constants) |
| LAPLACE_MINP / NMIN | 1 / 16 | 1 / 16 (`0x7fe0 = 32768-2*16`) |
| DB_SHIFT | 10 | 10 |
| pred_coef / beta_coef (LM=3) | 16384 / 6554 | 16384 / 6554 |
| beta_intra | 4915 | 4915 |
| e_prob_model | [LM][intra][42] | the LM=3 (960) row, each padded to 46 bytes; only the first 42 are used |

**Decoder consequence:** take opus 1.1 `entdec.c` unchanged and set `#define EC_CODE_BITS (24)` in `mfrngcod.h`.
The generic macros then produce the correct decoder. Everything else derives from `EC_CODE_BITS`, so nothing else
needs to change. `ec_tell()` behaves the same as in opus: it returns 1 right after init. The decoder state after
`ec_dec_init` is rng = 2^23 and nbits_total = 25.

**Budget caveat (affects coarse energy and everything else):** the encoder's `total_bits` is
`8*min(nbBytes,1275)`, but the range-coder storage is the full `nbBytes`. See §5.

---

## 1. Range coder (entenc.c / entcode.c)

The struct layout is the same as opus 1.1 `ec_ctx` (64-bit):
`+0 buf, +8 storage, +0xc end_offs, +0x10 end_window, +0x14 nend_bits, +0x18 nbits_total, +0x1c offs,
+0x20 rng, +0x24 val, +0x28 ext, +0x2c rem, +0x30 error`.

### ssc_ec_enc_init (0xf824)
It stores two 16-byte constants from .rodata 0x2750/0x2770 (`stur q1,[x0,#0xc]`, `stur q2,[x0,#0x1c]`) and the
8-byte constant `0x00000000ffffffff` at +0x2c. The resulting state is:
`end_offs=0, end_window=0, nend_bits=0, nbits_total=0x19 (25), offs=0, rng=0x00800000, val=0, ext=0, rem=-1, error=0`.
Opus has `nbits_total = EC_CODE_BITS+1 = 33` and `rng = EC_CODE_TOP = 2^31`. These values give **EC_CODE_BITS = 24**.

### FUN_0010f88c = ec_enc_normalize with ec_enc_carry_out inlined
```
while (rng <= 0x8000) {                // EC_CODE_BOT = 2^15
    c = val >> 15;                     // EC_CODE_SHIFT
    if (c != 0xFF) { carry=c>>8; if(rem>=0) write(rem+carry); while(ext) {write((0xFF+carry)&0xFF); ext--;} rem=c&0xFF; }
    else ext++;
    val = (val & 0x7FFF) << 8;         // (val<<8)&(EC_CODE_TOP-1)
    rng <<= 8; nbits_total += 8;
}
```
This is identical to opus, with the 24-bit constants (disassembly: `cmp w9,#0x8000; b.hi`, `ubfiz w8,w8,#8,#15`, `lsr #15`, `lsr #23`).

### ec_encode / ec_encode_bin / ec_enc_bit_logp / ec_enc_icdf / ec_enc_uint
These are formula-for-formula identical to opus 1.1:
- encode: `r=rng/ft; if(fl){val+=rng-r*(ft-fl); rng=r*(fh-fl);} else rng-=r*(ft-fh);`
- encode_bin: the same with `r=rng>>bits`.
- bit_logp: `s=rng>>logp; if(val) {val+=rng-s; rng=s;} else rng-=s`.
- icdf: `r=rng>>ftb; s>0 ? (val+=rng-r*icdf[s-1], rng=r*(icdf[s-1]-icdf[s])) : rng-=r*icdf[s]`.
- uint: `EC_UINT_BITS=8`. If `ilog(ft-1)>8`, it codes the top 8 bits with ec_encode and the remaining `ftb` bits as raw bits (ec_enc_bits is inlined).

### ec_tell_frac (0xf7c0)
This is the opus 1.1 3-iteration algorithm: `l=ilog(rng); r=rng>>(l-16); 3x {r=r*r>>15; b=r>>16; l=2l+b; r>>=b;}`
and it returns `nbits_total*8 - l`. Integer `ec_tell` is inlined everywhere as `nbits_total + clz(rng) - 32`
(= `nbits_total - ilog(rng)`).

### ec_enc_bits (0xfba4): raw bits
This is identical to opus: a 32-bit window. When `used+bits > 32` it flushes bytes with
`buf[storage - ++end_offs] = window&0xFF` until `used < 8`, then `window |= val<<used`.
**Raw bits go at the END of the buffer, LSB first, the same as CELT.**

### ec_enc_patch_initial_bits, ec_enc_shrink
- patch_initial_bits: identical. The checks are `offs>0`, then `rem>=0`, then `rng <= EC_CODE_TOP>>nbits` (0x800000>>n), and the val mask is `mask<<15` with the value shifted by `15+8-n`.
- shrink: `SSC_COPY8(buf+size-end_offs, buf+storage-end_offs, end_offs); storage=size`. This is identical. The only call in ssc_encode uses the same size as init, so it is a no-op.

### ec_enc_done (0xfd08)
This is identical to opus 1.1 with `EC_CODE_BITS=24`:
```
l = 24 - ilog(rng)  (= clz(rng)-8);  msk = 0x7FFFFF>>l;  end=(val+msk)&~msk;
if ((end|msk) >= val+rng) { l++; msk>>=1; end=(val+msk)&~msk; }
while (l>0) { carry_out(end>>15); end=(end<<8)&0x7FFFFF; l-=8; }
if (rem>=0 || ext>0) carry_out(0);
flush whole bytes of end_window to the end of the buffer;
if (!error) { clear buf[offs .. storage-end_offs); if (used>0) {... buf[storage-end_offs-1] |= window (masked if overlapping) } }
```

### Decoder
Use opus 1.1 `entdec.c` as is, with `EC_CODE_BITS 24`. The decoder then has `EC_CODE_EXTRA=7`,
`nbits_total` initially 9, `rng=1<<7`, `val=127-(b0>>1)`, and two normalise steps, giving `rng=2^23` and
`nbits_total=25`. Symbol arithmetic (`ec_decode`, `ec_decode_bin`, `ec_dec_update`, `ec_dec_icdf`,
`ec_dec_bit_logp`, `ec_dec_uint`, `ec_dec_bits`) needs no change. `A_decoder_snippets.c` contains an explicit
self-contained version.

**Verified by simulation:** a Python port of the Samsung encoder functions (decompile semantics) was run
against the 24-bit decoder over 3000 random streams. The streams mixed bit_logp, icdf, raw bits, uint,
Laplace and trim-icdf symbols, then ec_enc_done. Every symbol decoded correctly, and `ec_tell` matched between
encoder and decoder after every symbol.

Note: precision is lower than in opus. `ec_decode_bin(15)` can have `ext = rng>>15` as small as 1. The encoder
does exactly the same, so this is consistent.

## 2. Laplace (ssc_ec_laplace_encode @0x10d50)

This is identical to opus 1.1 `ec_laplace_encode`:
- `ec_laplace_get_freq1`: `(0x7FE0 - fs) * (0x4000 - decay) >> 15`, so `LAPLACE_MINP=1` and `LAPLACE_NMIN=16`.
- The decay loop is `fs*=2; fl+=fs+2; fs=fs*decay>>15` while `fs>0 && i<|val|`.
- In the tail case, `ndi_max = ((0x8000|sign) - fl)>>1`, then `di=min(|v|-i, ndi_max-1)`, `fl += 2di+1+s`, `fs = (fl!=32768)`, and the value is rewritten.
- It ends with `ec_encode_bin(fl, fl+fs, 15)`.

=> **`ec_laplace_decode` from opus 1.1 is used unchanged.**

## 3. Coarse energy

### ssc_quant_coarse_energy (0x13340), the wrapper
Arguments (as decompiled): `(mode, <unused>, end, eBands, oldEBands, budget, error, enc, nbAvailableBytes, force_intra(st+0x10), &delayedIntra(st+0x24))`.
Encoder-only logic, for information:
- `tell = ec_tell(enc)`. **two_pass is effectively always 1** when `tell+3 <= budget`. In that case the intra pass always runs first (intra prob table 0x5d91).
- If `force_intra` (st+0x10, init 0, never set by any visible code) is set, it keeps intra. Otherwise it reruns inter (table 0x5dbf) from the saved state. It picks intra when `badness1 < badness2 || (equal && tell_frac_inter > tell_frac_intra)` (intra_bias = 0, no loss_rate).
- If `tell+3 > budget`, it runs only inter, with intra = 0.
- `max_decay = min(nbAvailableBytes<<7, 16384)` (16 dB in Q10). The `end-start>10` test is folded.
- `delayedIntra`: intra gives `new_dist`; inter gives `new_dist + (delayedIntra>>2)` (pred_coef²=0.25). `loss_distortion` sums over `2*end` contiguous values, `min(200, dist>>14)`.

### FUN_00113734 = quant_coarse_energy_impl (C=2, LM=3 and start=0 are constant-folded)
Arguments: `(mode, end, eBands, oldEBands, budget, tell, prob_model, error, enc, intra, max_decay)`.
- **Intra flag:** `if (tell+3 <= budget) ec_enc_bit_logp(enc, intra, 3);`. The flag is coded **once per frame, at the start of coarse energy**, with logp=3 (P(1)=1/8). `tell` is the ec_tell value on entry to quant_coarse_energy. Decoder: `intra = (ec_tell(dec)+3 <= total_bits) ? ec_dec_bit_logp(dec,3) : 0;` (same as opus).
- `coef = intra ? 0 : 16384` (pred_coef[3]=0.5 Q15). `beta = intra ? 4915 : 6554` (the code has the constants `-0x1333` / `-0x199a`).
- Per band i (0..end-1), channel c (0,1); the two channels are unrolled:
  - `oldE = max(oldEBands[i+c*22], -9216)` (−9.0 Q10)
  - `f = (x<<7) - ((coef*oldE)>>8) - prev[c]` (Q17; the shift has no rounding, which gives the same result as PSHR32 because coef ∈ {0,16384})
  - `qi = (f + 0x10000) >> 17`
  - decay bound: `if (f < -0x10000 /*qi<0*/) { db = max(oldEBands_raw - max_decay, -28672); if (x < (int16)db) { qi += (db-x)>>10; qi = min(qi,0);} }`
  - `tell = nbits_total + clz(rng) - 32`; `bits_left = budget - tell - 6*(end-i)`.
    `if (i!=0 && bits_left<30) { if(bits_left<24) qi=min(qi,1); if(bits_left<16) qi=max(qi,-1); }` (no LFE)
  - **Coding:** `budget-tell >= 15` → `ec_laplace_encode(qi, prob[2*min(i,20)]<<7, prob[2*min(i,20)+1]<<6)`;
    `>= 2` → `qi=clamp(qi,-1,1); ec_enc_icdf(2*qi ^ -(qi<0), small_energy_icdf, 2)`;
    `== 1` → `qi=min(qi,0); ec_enc_bit_logp(-qi, 1)`; otherwise `qi = -1` (nothing coded).
  - `error = ((f+64)>>7) - (qi<<10)`.
  - qi is then saturated to `[-2^23, 2^23-1]` (`0xff800000`/`0x7fffff`). This is a harmless saturate inside SHL32 and has no practical effect, because Laplace values are far smaller.
  - `tmp = ((coef*oldE)>>8) + prev[c] + (qi<<17); tmp = max(tmp, -0x380000 /* -28<<17 */); oldEBands = (int16)((tmp+64)>>7)`
  - `prev[c] += (qi<<17) - beta*(int16)(qi<<2)`
  - badness += |qi0 - qi|
- **Probability index with 22 bands and LM=3:** `pi = 2*min(i,20)` (disassembly: `cmp x26,#0x14; csel`). Bands 20 and 21 both use pair 20. **The padding entries at [42..45] are never read.**
- **Budget:** `budget` is the encoder's `total_bits` = `[sp,#0xdc]` = `8*nbCompressedBytes`. See §5.
- The `end` passed in is `st->end - k`, where `k` (0..3) is the number of dropped top bands (`uVar131` in ssc_encode, also written into header byte 1 as `+= k*4`). That is outside this area, but the decoder must use the same `end` for coarse, fine and finalise.

### Tables (dumped from .rodata)
`e_prob_model` intra @ **0x5d91** (46 bytes), inter @ **0x5dbf** (46 bytes), `small_energy_icdf` @ **0x5ded** = `{2,1,0}`.
```
intra: 22,178, 63,114, 74,82, 84,83, 92,82, 103,62, 96,72, 96,67, 101,73, 107,72, 113,55,
       118,52, 125,52, 118,52, 117,55, 135,49, 137,39, 157,32, 145,29, 97,33, 77,40, | 68,45, 62,49 (unused)
inter: 42,121, 96,66, 108,43, 111,40, 117,44, 123,32, 120,36, 119,33, 127,33, 134,34, 139,21,
       147,23, 152,20, 158,25, 154,26, 166,21, 173,16, 184,13, 184,10, 150,13, 139,15, | 130,17, 125,19 (unused)
```
The first 42 bytes of each table are exactly opus 1.1 `e_prob_model[3][1]` (intra) and `e_prob_model[3][0]` (inter), the 960-sample row.
`eMeans` @ 0x5d78 (`ssc_eMeans_tmp`, 25 entries) = 103,100,92,85,81,77,72,70,78,75,73,71,78,74,69,72,70,74,76,71,60,60,60,60,60 (same as CELT).

### Q format
**`DB_SHIFT = 10`.** Energies (`oldEBands`, `error`) are int16 in **Q10** (log2 units, 1.0 = 1024). The evidence is the constants -9216 (−9<<10), -28672 (−28<<10), -0x380000 (−28<<17), the fine offset 0x200 (0.5), `10 - fine_quant` shifts, max_decay 16384 and `nbBytes<<7`. The prediction state `prev[]` and `f` are Q17 (DB_SHIFT+7). This is the same as opus 1.1 fixed point (1.1 already uses DB_SHIFT 10; the Q8 claim applies to older CELT, not this build).

## 4. Fine energy and finalise

### ssc_quant_fine_energy (0x13c9c): `(mode, end, oldEBands, error, fine_quant, enc)`, start=0, C=2
```
for i in 0..end-1: if (fq=fine_quant[i]) <= 0 continue;
  for c in 0,1: q2 = (error + 512) >> (10-fq); clamp to [0, (1<<fq)-1]; ec_enc_bits(q2, fq);
                offset = ((q2<<10 | 512) >> fq) - 512;  oldE += offset; error -= offset;
```
This is identical to opus. **Decoder:** `q2 = ec_dec_bits(dec, fq); oldE += (((q2<<10)+512)>>fq) - 512;`

### ssc_quant_energy_finalise (0x13e14): `(mode, end, oldEBands, error, fine_quant, fine_priority, bits_left, enc)`
`bits_left = total_bits - ec_tell(enc)` (computed in ssc_encode after the anti-collapse bit).
```
for prio in 0,1: for (i=0; i<end && bits_left>=2; i++) {
   if (fine_quant[i] >= 8 /*MAX_FINE_BITS*/ || fine_priority[i] != prio) continue;
   for c in 0,1: q2 = (error>=0); ec_enc_bits(q2,1); oldE += ((q2<<10) - 512) >> (fine_quant[i]+1); bits_left--;
}
```
This is identical to opus 1.1. **Decoder:** `q2 = ec_dec_bits(dec,1); oldE += ((q2<<10)-512) >> (fq+1);`

## 5. Things the decoder integrator must know (outside this area, found incidentally)

1. **total_bits / coarse budget ≠ storage*8 for large frames.** In ssc_encode, the range coder is initialised with
   storage `nbBytes` (header-derived payload size). Then `nbCompressedBytes = min(nbBytes, 1275)`
   (`cmp w23,#0x4fb`). The `st->bitrate` branch (st+0x18) is dead because the field is initialised to -1 and
   never written. `total_bits = nbCompressedBytes*8` is used as the coarse-energy budget, as the finalise
   `bits_left` base, and (presumably) for allocation. The **ec storage stays `nbBytes`**: shrink is called with
   the same `nbBytes`, so raw bits sit at the end of the full `nbBytes` region.
   The decoder must call `ec_dec_init(payload, nbBytes)`, but pass `budget = 8*min(nbBytes,1275)` to
   `unquant_coarse_energy`. Stock opus uses `dec->storage*8`. This only matters for E3 (584k) and E4 (886k) at
   44.1/48k (nbBytes 1430 / ~2169) and some 96k rates. The encode-flow analysis should confirm this.
2. The energy functions hard-code **C=2** (both channels unrolled; stride = nbEBands = 22) and **start=0**.
3. `end` = `22 - k`, where k (0..3) is in the frame header (see the ssc_encode / header analysis).

## 6. Bottom line for decoding
- Range decoder: opus 1.1 entdec with `EC_CODE_BITS=24`.
- Laplace: opus 1.1 unchanged.
- Coarse: opus 1.1 `unquant_coarse_energy` with LM=3 (coef 16384, beta 6554 / intra 4915), the tables above, C=2, `budget = 8*min(nbBytes,1275)`.
- Fine and finalise: opus 1.1 unchanged (DB_SHIFT 10, MAX_FINE_BITS 8).
- See `A_decoder_snippets.c`.
