# F_a2dp: SSC on the Bluetooth/A2DP side (Samsung `libbluetooth_jni.so`)

> Research notes written while reverse-engineering SSC. Addresses are offsets in Samsung's libraries on a
> Galaxy S26 Ultra. References to `re/...` files, decompiler output and disassembly point to working files
> that are not part of this repository. The cleaned-up specification is [`../bitstream.md`](../bitstream.md).

Source: `extracted/bt_apex/payload/lib64/libbluetooth_jni.so` (stripped). All addresses below are
**Ghidra addresses** (image base 0x100000). For the raw file, use `file_vaddr = ghidra_addr - 0x100000`.
Decompiles: `re/bt_ssc_decomp.c` (main), `re/bt_ssc_misc.c`, `re/bt_ssc_vendor.c`, `re/bt_ssc_vendor2.c`,
`re/bt_ssc_create.c`, `re/bt_codec_priority.c`. New Ghidra scripts: `tools/scripts/DecompileSscMisc.java` and
`tools/scripts/DecompileAt.java` (args: out file, then hex addresses).

## TL;DR for the ESP32 sink

* Codec Information Element: **vendor codec, Vendor ID 0x00000075 (Samsung), Codec ID 0x0103**, LOSC 9,
  with **one** codec-specific byte.
* Advertise `07 09 00 FF 75 00 00 00 03 01 28` in the AVDTP Media Codec capability: 44.1 kHz, no bitrate limit.
  The phone then sends SetConfiguration with the same 10 bytes `09 00 FF 75 00 00 00 03 01 28`. That is
  44.1 kHz, 16-bit, stereo, 864-sample frames, 192/192/128/88 kbps depending on the phone's link-busy level.
* Media packet = **RTP header (12 bytes) + SSC frame(s) concatenated**. There is **no media payload header byte**
  (no frame count, no fragmentation bits). The stack does **not fragment** frames. At 44.1/48 kHz there is exactly
  **one SSC frame per packet**. At 96 kHz there are 1 or 2 frames per packet.
* The RTP timestamp is a **packet counter** (+1 per packet, starting at 1), not a sample count.
* If (and only if) you advertise SCMS-T content protection and the phone activates it, there is 1 extra CP byte
  before the first SSC frame. Do not advertise CP.
* The phone has no feedback channel from the sink. Bitrate is chosen only from the phone's own "busy level"
  (controller link-feedback vendor event plus its TX-queue depth).

---

## 1. Codec Information Element (CIE)

### 1.1 Byte layout (A2DP_ParseInfoSsc @ 0xa90878)

`p[]` is the AOSP-style codec_info array, where `p[0]` is LOSC:

| idx | value | meaning / check in ParseInfoSsc |
|---|---|---|
| 0 | `0x09` | LOSC; must equal 9 (`*p != '\t'` → error 0x29 = A2DP_WRONG_CODEC/AVDTP UNSUPPORTED_CONFIGURATION) |
| 1 | `0x00` | media type (AUDIO<<4); must be `< 0x10` |
| 2 | `0xFF` | codec type = Non-A2DP (vendor) |
| 3..6 | `75 00 00 00` | Vendor ID, u32 LE = **0x00000075** (Samsung Electronics) |
| 7..8 | `03 01` | Codec ID, u16 LE = **0x0103** |
| 9 | flags | see 1.2 |

The CIE parses into this struct (12 bytes, stack copy `tA2DP_SSC_CIE`):

```c
typedef struct {
  uint32_t vendorId;      // +0  == 0x75
  uint16_t codecId;       // +4  == 0x0103
  uint8_t  sampleRate;    // +6  = p[9] & 0xF0
  uint8_t  noBitrateLimit;// +7  = p[9] & 0x08
  uint8_t  hifi;          // +8  = p[9] & 0x04
  uint8_t  uhqVer2;       // +9  = p[9] & 0x02
  uint8_t  bit0;          // +10 = p[9] & 0x01   (blocks UHQ v2 selection, meaning unknown)
} tA2DP_SSC_CIE;
```

The log string is `"codec info[..] codecid[..] samplerate[{:x}] limitation[{:x}]"` (high nibble / low nibble of p[9]).

### 1.2 The flags byte `p[9]`

| bit | mask | meaning | evidence |
|---|---|---|---|
| 7 | 0x80 | 96 kHz (legacy "UHQ", SSC type 0x200) | GetTrackSampleRate @0xa91464: 0x80→96000 |
| 6 | 0x40 | 32 kHz | 0x40→32000 |
| 5 | 0x20 | 44.1 kHz | 0x20→44100 |
| 4 | 0x10 | 48 kHz | 0x10→48000 |
| 3 | 0x08 | **no bitrate limitation** (set → 192 kbps table, clear → 128 kbps table) | GetTrackBitrateLimitationSsc @0xa91734 returns `(p[9]&8)==0` |
| 2 | 0x04 | **HiFi**: 48 kHz / 24-bit (SSC type 0x100) | GetTrackSampleRate: bit2→48000 (checked before the nibble); GetTrackBitsPerSample @0xa91620: →24 |
| 1 | 0x02 | **UHQ ver2**: 96 kHz / 32-bit container (SSC type 0x400) | GetTrackSampleRate logs "uhq ver2", →96000; bits→0x22 (4-byte samples) |
| 0 | 0x01 | if set, the phone will not pick UHQ v2 | setCodecConfig @0xa92400 |

Derived values from a *configuration* CIE:

* Sample rate (A2DP_VendorGetTrackSampleRateSsc @0xa91464):
  `if (b&2) 96000; else if (b&4) 48000; else switch(b&0xF0){0x10:48000;0x20:44100;0x40:32000;0x80:96000}`
* Bits per sample (A2DP_VendorGetTrackBitsPerSampleSsc @0xa91620):
  `(b&2 || (b&0xF0)==0x80) ? 0x22 /*4-byte container*/ : (b&4) ? 24 : 16`
* Channels: always 2 (A2DP_GetTrackChannelCount, SSC branch `mov w0,#2` at file 0x95feec)
* SSC type (A2DP_VendorGetSscType @0xa917f8), matching Java `CODEC_SSC*`:
  `b&2 → 0x400 (CODEC_SSC_UHQ2)`, `(b&0xF0)==0x80 → 0x200 (CODEC_SSC_UHQ)`, `b&4 → 0x100 (CODEC_SSC_HIFI)`,
  else `0x80 (CODEC_SSC)`

Validity: ParseInfoSsc(is_capability=true) accepts any flags byte. For a configuration (is_capability=false) the byte
must have the HiFi bit, the UHQv2 bit, or **exactly one** sample-rate bit (A2DP_BitsSet == single), otherwise 0xC3
(invalid sampling frequency).

### 1.3 The phone's own (source) capability

Built in A2DP_VendorInitCodecConfigSsc @0xa920dc: 8 bytes from 0x20ac08 (`09 00 FF 75 00 00 00 03`), then
`[8]=0x01`, `[9]=0x28 | hifi`. So the phone advertises:

```
09 00 FF 75 00 00 00 03 01 2C     (HiFi enabled: 44.1k + no-limit + HiFi)
09 00 FF 75 00 00 00 03 01 28     (HiFi disabled)
```

HiFi is disabled only if the IOP DB (`libbt-iopdb`, file `etc/bluetooth/iop_bt.db`) has a **device-global**
entry `FeatureChecker KEY_NAME="SSC_HIFI_DISABLED_1"`. That query is `iop_exception_with_string(10, "SSC_HIFI_DISABLED_1")`
@0x6aa5a8, where table index 10 = "FeatureChecker"/"Downloadable_FeatureChecker". The shipped `iop_bt.db` has only
`FeatureChecker KEY_NAME="dummy"`, so **HiFi is enabled by default**. If disabled, the log shows
"A2DPSM-SSC HiFi 1st is disabled". The check is not peer-specific.

A2dpCodecConfigSsc ctor @0xa9223c: codec index 8, codec_id 0x1030075ff, name "SSC".
Local capability: sample_rate = 44.1k (+48k if HiFi), bits = 16|24, channel = stereo.

### 1.4 Configuration selection (A2dpCodecConfigSsc::setCodecConfig @0xa92400)

This decompile has shifted parameters: `param_2` = peer codec info, `param_3` = is_capability,
`in_x3` = result config. Reconstructed logic:

```
peer = ParseInfoSsc(peer_caps); sr = peer.b9 & 0xF0
if sr == 0x80:              bps_caps = 32(enum 8)
elif (is_cap || sr == 0) && peer.hifi && local_hifi_enabled:
                            bps_caps = 24; sr_eff = 0x10 (48k); hifi_ok = 1
else:                       bps_caps = 16
(if peer.hifi also: hifi_ok = 1, and a user pref of 44.1k is upgraded to 48k/24-bit)
sample rate: user pref (96k needs peer.uhqVer2 && !peer.bit0) → audio-HAL pref → best = 44.1k if 0x20 else 48k if 0x10
bits: user pref → audio pref → best of bps_caps
result p[9] = (peer.b9 & 0x08) | ( UHQv2 path ? 0x02 : (hifi_ok && bits==24) ? 0x04 : chosen_sr_bit )
```

So the phone **never configures 0x80 (legacy 96k)**. It configures only 0x20/0x10 (+0x08), 0x04 (+0x08), or
0x02 (+0x08). UHQ v2 is used only when the user enables "UHQ" (Java `setEnableScalableUhqMode`, user sample rate
96 kHz). Channel mode is always stereo.

Typical SetConfiguration bytes:

| sink advertises p[9] | phone configures | mode |
|---|---|---|
| 0x20 | `09 00 FF 75 00 00 00 03 01 20` | 44.1k/16, bitrate-limited (≤128 kbps) |
| **0x28** | `09 00 FF 75 00 00 00 03 01 28` | 44.1k/16, ≤192 kbps (**recommended**) |
| 0x10 or 0x18 | `…03 01 10` / `…03 01 18` | 48k/16 |
| 0x2C (HiFi) | `…03 01 0C` | 48k/24 "SSC HiFi" (sample-rate nibble is **0**) |
| 0x2E (+UHQv2), user UHQ on | `…03 01 0A` | 96k/32-container UHQ v2 |

### 1.5 Is SSC gated to Samsung peers?

Nothing in the native stack gates SSC by peer identity. A2DP_IsVendorPeerSinkCodecValidSsc @0xa90a44 only runs
ParseInfoSsc, and there is no name/OUI whitelist. The default codec priority is
`index*1000+1001 = 9001` (A2dpCodecs::setCodecPriority @0xa60d5c), the **highest** of all source codecs
(LDAC 5001, LHDC 8001). That means SSC wins whenever the peer advertises it. Caveats:

* The Java Bluetooth.apk passes `a2dp_source_codec_priority_ssc` and per-device metadata
  (`a2dpSupportsSscUhq`/`a2dpEnabledSscUhq`). It could override the priority, but that was not decompiled here.
* With HAL offload (`provider::supports_codec()` in createCodec @0xa5a32c), a generic A2dpCodecConfigExt is used
  instead, and the DSP encodes (see §2.6).
* SCMS-T content protection is **not required**. It is the standard AOSP behaviour (ContentProtectIsScmst
  @0x755618, AudioProtectHasScmst @0x755710).

---

## 2. Media packet format

### 2.1 RTP header: yes

A2DP_VendorUsesRtpHeader @0xa70eb0 → A2DP_VendorUsesRtpHeaderSsc @0xa90b14 returns **1**. AVDTP prepends the
standard 12-byte RTP header (V=2, PT as negotiated by AVDTP, usually 0x60, seq +1 per packet, SSRC).

### 2.2 No media payload header

* A2DP_BuildCodecHeader @0xa602ec → A2DP_VendorBuildCodecHeader (file 0x9711d8): for vendor 0x75 / codec 0x103
  it just returns `true` (`cmp w22,#0x75; ccmp ...#0x103; cset w0`) and **does not touch the buffer**. This is
  like aptX; LDAC/Opus instead prepend 1 byte.
* The encoder allocates `BT_HDR` with `offset = 0x17 = 23 = AVDT_MEDIA_OFFSET` (`*(u32*)(hdr+2)=0x170000`,
  in a2dp_vendor_ssc_send_frames @0xa94210). For comparison, SBC uses offset 0x18 = AVDT_MEDIA_OFFSET+1 for its
  frame-count byte. The SSC frames are written directly at `hdr + 8 + 23` (`ssc_encode(..., lVar52 + 0x1f + pos, ...)`).
* `layer_specific = 0` (no frame count) and `len = sum of ssc_encode() return values`.
* Content protection (bta_av_co_audio_source_data_path @0x751b48): if CP is supported and the peer's CP is active
  (`peer+0x448 == 1`), `offset--, len++, data[0] = SCMS-T header byte`. Only then is there 1 extra leading byte.

So the L2CAP media payload is:

```
[RTP 12 bytes][CP byte, only if SCMS-T active][SSC frame #1][SSC frame #2 (96 kHz modes only)]
```

Each SSC frame is self-delimiting: a 4-byte header `FF Ex ss bb` + range-coded payload (see ../bitstream.md). The
**bytes are exactly what `ssc_encode` wrote**. The stack does not rewrite header, sequence or bitrate fields.

### 2.3 Frames per packet and fragmentation

a2dp_ssc_get_num_frame (inside send_frames) accumulates PCM bytes per tick and schedules N packets. For each
packet, one PCM "chunk" is read and encoded, `frame by frame (864 samples)`, into the same buffer:

| mode | PCM per 864-sample frame (bytes) | chunk read per packet | SSC frames / packet |
|---|---|---|---|
| 44.1k or 48k, 16-bit (types 0x80) | 0xD80 = 864·2ch·2 | 0xD80 | **1** |
| 48k HiFi 24-bit packed (0x100) | 0x1440 = 864·2·3 | 0x1440 | **1** |
| 96k legacy (0x200) | 0x1B00 = 864·2·4 | 0x3600 | **2** |
| 96k UHQ v2 (0x400), busy level < 2 | 0x1B00 | 0x1B00 | **1** |
| 96k UHQ v2, busy level ≥ 2 | 0x1B00 | 0x3600 | **2** |

There is **no fragmentation code**. The packet is enqueued whole (`enqueue_cb(p_buf, 1, bytes_read)`). The MTU is
not checked by the encoder: `get_effective_frame_size` (vtable slot, file 0x9941dc) just returns the stored
peer MTU. Packet sizes are kept small by design (see §3.3). The largest is 2×372 = 744 bytes + 12 RTP, so the sink
should offer an L2CAP media MTU of at least about 800 (895/1005 are fine).

### 2.4 Timestamp

* `*(uint32_t*)(p_buf+1) = ++pkt_counter` in send_frames (`DAT_01043e64 = DAT_01043e64 + 1; *(int*)(hdr+8) = ...`).
  The counter is zeroed by the memset in a2dp_vendor_ssc_encoder_init @0xa93944.
* A2DP_VendorGetPacketTimestampSsc @0xa91918: `*ts = *(uint32_t*)p_data`, i.e. that value becomes the
  **RTP timestamp**.
* So RTP timestamp = 1, 2, 3, … (+1 per packet, regardless of frames in the packet). Do not use it as a sample
  clock. Use the frame header's 12-bit/8-bit sequence and RTP seq to detect loss.
* In dual-play, the second link has its own counter (`DAT_01043e68`), and its BT_HDR has `layer_specific |= 0x100`.

### 2.5 Dual play

* `btif_av_need_dual_play_for_ssc` @0x79f704 iterates connected peers and returns true if **two** peers
  currently use SSC with the same type: two of 0x100 (HiFi) or two of 0x80.
* `a2dp_ssc_encode_frames_on_dual_play` (inside send_frames) then encodes the same PCM with a second encoder
  instance (`ssc_encoder_create` is called twice in A2DP_VendorLoadEncoderSsc @0xa93300). It sends separate
  packets to each headset.
* This is Samsung **"Dual Audio"** (two separate headsets), not the left/right buds of one product. It is
  irrelevant to a single sink.

### 2.6 Offload path (FYI)

When the Bluetooth Audio HAL is in hardware mode, software send_frames logs
"### offload is enabled, discarded frame ###". Instead, `setup_codec` @0x900d74 / 0x95637c sends the DSP:

* `SehSscConfiguration{sampleRate, channelMode, bitsPerSample}` (A2dpSscToHalConfig @0x9764c8; bits enum table at
  0x252840: 16/24/32/34)
* `encodedAudioBitrate = SscBitrateByBusyLevel(type, busy, limit)` (same tables as §3.2)
* `peerMtu = min(effective_frame_size, peer_mtu) ≤ 1005`

The packetization done by the DSP firmware cannot be checked from this library. The Buds must decode both paths,
so it is presumably identical.

---

## 3. Encoder parameters used by the stack (a2dp_vendor_ssc_encoder.cc)

### 3.1 Creation and PCM

* `ssc_encoder_create(Fs, 2, bits, &err)` (A2DP_VendorLoadEncoderSsc @0xa93300):
  * `Fs = 96000` if the track rate is 96000, otherwise **44100**. 48 kHz HiFi still creates the encoder with 44100,
    and the codec mode is the same anyway.
  * `channels = 2`.
  * `bits` is set in a2dp_vendor_ssc_set_current_encoder_info @0xa93dc0 from the btav bits enum:
    1→16, 2→24, 4→32, 8→**0x22**.
  * The library is loaded from `libScalable_Encoder.so` with `ssc_encoder_create`, `ssc_encoder_destroy` and
    `ssc_encode`.
* `ssc_encode(enc, int32 *pcm, 864, out, bitrate)` returns the frame length in bytes (`nbBytes + 4`).
* PCM given to ssc_encode is **interleaved L,R int32 at native bit depth (not left-justified)**:
  * 16-bit input: sign-extended int16 → int32 (NEON `sshll #8 | byte`, file ~0x996518).
  * 24-bit packed (3 bytes/sample) input: sign-extended 24 → int32 (`b0 | b1<<8 | b2<<16`, top byte = sign,
    file ~0x99641c).
  * 96 kHz: the 32-bit PCM is passed through unchanged (memcpy, then direct call at file 0x996300).
  * So a decoder should output the same scale: ±2^15 for 16-bit modes, ±2^23 for HiFi.
* Encoder tick: `get_encoder_interval_ms` = 20 ms if `DAT_0101f684` (a feature flag) is set, else 10 ms.
  It is forced to 10 ms for UHQ v2.

### 3.2 Bitrate vs busy level

The busy level is `A2dpCurrentEncodeInfo[0]`, clamped to 0..3. The bitrate is chosen per packet, both in
send_frames and in SscBitrateByBusyLevel @0x94b63c / @0x975920 (HAL path). Tables are in .rodata:

| busy level → | 0 | 1 | 2 | 3 | table |
|---|---|---|---|---|---|
| type 0x80/0x100, bit3 set (no limit) | 192000 | 192000 | 128000 | 88000 | 0x207650 |
| type 0x80/0x100, bit3 clear (limited) | 128000 | 128000 | 128000 | 88000 | 0x209770 |
| type 0x200 (96k legacy) | 328000 | 328000 | 328000 | 328000 | 0x20a320 (sw path hard-codes 0x50140) |
| type 0x400 (UHQ v2) | 584000 | 584000 | 291000 | 152000 | 0x207800 |

When the bitrate changes, the stack logs "new bitrate {}, previous bitrate {}", writes it to
`A2dpCurrentEncodeInfo+0x28`, and calls the codec-info callback `(0, 8)` to notify the UI. Nothing is sent over the
air. The sink sees the change only through the bitrate code in each frame header (b3).

### 3.3 Resulting frame and packet sizes

These follow the ../bitstream.md formula `base=bitrate*864/(FsMode*8)`, `+base*3>>7` unless hq, `|1`,
`nbBytes = base-1`, frame = nbBytes+4. They were confirmed against `ssc_encode` lines 531-542 and its return
`uVar126+4`.

| bitrate | FsMode | hq | frame bytes | frames/pkt | SSC payload/pkt |
|---|---|---|---|---|---|
| 88k (F0) | 44100 | 0 | 224 | 1 | 224 |
| 128k (F2) | 44100 | 0 | 324 | 1 | 324 |
| 192k (F3) | 44100 | 0 | 484 | 1 | 484 |
| 328k (F5) | 96000 | 1 | 372 | 2 | 744 |
| 584k (E3) | 96000 | 1 | 660 | 1 | 660 |
| 291k (E0) | 96000 | 1 | 330 | 2 | 660 |
| 152k (E6) | 96000 | 1 | 174 | 2 | 348 |

### 3.4 Header fields

The stack never writes into the SSC frame. The `FF EE`/`FF Ex`, sequence and bitrate code are produced inside
`ssc_encode` (header write is in `fn/ssc_encode.c`, e.g. the `*param_4 = 0xeeff` branch for 128k).

### 3.5 Encoder interface vtable (0xfa0808)

init 0xa93944, cleanup 0xa93f4c, feeding_reset 0xa93ff4, feeding_flush 0xa94138, get_interval_ms 0xa941cc,
get_effective_frame_size 0xa941dc, send_frames 0xa94210, set_transmit_queue_length NULL,
set_current_encoder_info 0xa93dc0, and an unknown getter 0xa941ec (returns 324 if Fs==96000 else 353).

---

## 4. Return path / feedback

* **None from the sink.** There is no SSC-specific AVDTP vendor command, no RTCP and no HCI command to change
  bitrate. Rate adaptation is phone-local:
  * Controller vendor event "ALF" (adaptive link feedback, value 0..3). In btif_a2dp_source_vendor_event_handler
    @0x790c74, `p[2]` → btif_a2dp_source_busy_level @0x7901f0. It is applied only if the codec is non-SBC or the
    MTU > 663 and the peer is not a carkit.
  * TX-queue monitoring: btif_a2dp_source_audio_handle_timer @0x793a64 and
    btif_a2dp_source_busy_level_monitoring @0x790790, using a smoothed queue depth (≥6 → level 3).
    btif_a2dp_source_enqueue_callback @0x79317c can also force level 2.
  * HAL offload: `ISehBluetoothAudioOffload::sehBusyLevelChanged`.
  * For UHQ v2 with dual connection or 20 ms mode, busy level is forced to ≥2.
* The only way a sink influences this is indirectly: if it is slow to accept packets, ACL buffers back up, the TX
  queue grows and the bitrate drops.

---

## 5. ESP32 (ESP-IDF Bluedroid) sink guidance

### 5.1 SEP capability

Register a sink SEP with media type Audio and codec type 0xFF, and this capability element (AVDTP service
category 0x07):

```
07 09 | 00 FF | 75 00 00 00 | 03 01 | 28
cat LOSC mt ct    vendor id    codec  flags: 0x20 (44.1k) | 0x08 (no bitrate limit)
```

* For 24-bit output, use `2C` (adds HiFi): the phone then configures `0C` = 48 kHz/24-bit, same bitrate table.
* Avoid 0x80/0x02 (96 kHz) on ESP32. They are heavier, and the phone uses them only with the user's UHQ toggle.
* Do **not** add a Content Protection capability. Keep an SBC SEP too (mandatory).
* In the SetConfiguration handler, accept only LOSC==9, vendor 0x75, codec 0x0103, and a flags byte with exactly
  one sample-rate bit or the 0x04/0x02 bit. Derive sr/bits with the rules in §1.2.

ESP-IDF master has `esp_a2d_sink_register_stream_endpoint(seid, const esp_a2d_mcc_t*)` and
`esp_a2d_sink_register_audio_data_callback()` (Kconfig `BT_A2DP_USE_EXTERNAL_CODEC`), which deliver undecoded
media buffers. However, `esp_a2d_mcc_t` only has SBC/M12/M24/ATRAC CIE members, and the docs say only SBC/AAC are
supported. Expect to patch bluedroid as follows:

* Add a vendor CIE (7 bytes after the codec type: vendor u32, codec u16, flags u8) to the union / `A2D_` codec
  checks, and to `bta_av_co` sink codec matching (the getcap/setconfig paths).
* Make sure nothing strips a "media payload header" byte for this codec type (the SBC path skips 1 byte; SSC
  must skip 0).
* Ensure the AVDTP media L2CAP MTU is at least ~800 (BTA_AV_MAX_A2DP_MTU / L2CAP config).

### 5.2 Depacketizer

```c
// buf/len: L2CAP payload of an AVDTP media packet (if your stack already stripped RTP, skip step 1)
int ssc_depacketize(const uint8_t *buf, int len, bool cp_active, int fs_mode /*44100 or 96000*/) {
  // 1. RTP: 12 bytes + 4*CC (+ extension if X bit) ; padding if P bit
  int cc = buf[0] & 0x0F, off = 12 + 4*cc;
  if (buf[0] & 0x10) off += 4 + 4 * ((buf[off+2] << 8) | buf[off+3]);
  if (buf[0] & 0x20) len -= buf[len-1];
  uint16_t rtp_seq = (buf[2] << 8) | buf[3];      // +1 per packet; RTP timestamp is also +1 per packet
  if (cp_active) off += 1;                           // SCMS-T byte, only if CP was negotiated
  // 2. one or more SSC frames, back to back, no length prefix
  while (len - off >= 4) {
    const uint8_t *f = buf + off;
    if (f[0] != 0xFF || (f[1] & 0xF0) != 0xE0) break;    // resync / drop rest of packet
    int flen = 4 + ssc_payload_bytes(f[3] /*bitrate code*/, fs_mode);  // §3.3 / CONTEXT formula
    if (off + flen > len) break;                     // never happens: no fragmentation
    ssc_decode_frame(f, flen);                       // 864 stereo samples out
    off += flen;
  }
  return off;
}
```

Expected payload length per packet at 44.1k/16 (`…28` config): 484 B (192k, level 0/1), 324 B (128k, level 2)
or 224 B (88k, level 3). Each packet carries one frame = 864 samples = 19.6 ms. Packet loss shows up as RTP seq
gaps and gaps in the frame sequence field. Apply PLC for 864 samples per lost packet.

Decoder output scale: int32 in 16-bit range (or 24-bit range for HiFi), interleaved stereo, at 44100 Hz (or
48000 Hz for HiFi). The internal codec mode is the same for both rates.
