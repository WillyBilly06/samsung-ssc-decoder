# SSC over Bluetooth A2DP

How a Galaxy phone negotiates and streams SSC, and what a sink has to do to receive it. The source behaviour was
read from Samsung's Bluetooth stack on a Galaxy S26 Ultra and confirmed with the ESP32 firmware in this repository.
Full research notes: [notes/06-a2dp-stack.md](notes/06-a2dp-stack.md).

## Codec information element

SSC is an A2DP **vendor-specific codec** (codec type `0xFF`).

```
Media Codec capability (AVDTP service category 7), LOSC = 9
  00          media type: audio
  FF          codec type: vendor specific
  75 00 00 00 vendor ID 0x00000075 (Samsung)
  03 01       codec ID 0x0103 (SSC)
  xx          flags
```

| Flag bit | Meaning | Phone codec type |
|---|---|---|
| `0x80` | 96 kHz, legacy UHQ | `CODEC_SSC_UHQ` |
| `0x40` | 32 kHz (never used by phones) | – |
| `0x20` | 44.1 kHz | `CODEC_SSC` |
| `0x10` | 48 kHz | `CODEC_SSC` |
| `0x08` | unlimited bitrate (up to 192 kbps for 16-bit; without it the phone caps at 128 kbps) | – |
| `0x04` | **HiFi**: 48 kHz / 24-bit ("Samsung Seamless Codec") | `CODEC_SSC_HIFI` |
| `0x02` | **UHQ v2**: 96 kHz / 24-bit in a 32-bit container | `CODEC_SSC_UHQ2` |
| `0x01` | the phone must not pick UHQ v2 | – |

A **capability** may set any combination of bits. A **configuration** (SetConfiguration from the phone) has the HiFi
bit, the UHQ v2 bit, or exactly one sample-rate bit, plus optionally `0x08`. The phone rejects anything else.

## What to advertise

| Sink flags | Capability bytes | What the phone configures |
|---|---|---|
| `0x28` | `07 09 00 FF 75 00 00 00 03 01 28` | 44.1 kHz / 16-bit, up to 192 kbps |
| `0x2C` | `… 03 01 2C` | HiFi 48 kHz / 24-bit (`… 03 01 0C`) |
| `0x3E` | `… 03 01 3E` | HiFi, or UHQ v2 96 kHz (`… 03 01 0A`) when the phone's UHQ option is on |

The ESP32 firmware advertises `0x3E` (every mode). Keep an SBC endpoint as well (it is mandatory), and do not add
a Content Protection capability.

The phone advertises `… 03 01 2C` itself (44.1 kHz + unlimited + HiFi). Nothing gates SSC to Samsung devices: any sink
that advertises the element above is offered SSC, and SSC has the highest priority of the phone's codecs.

## Configuration rules on the phone

1. If the sink offers UHQ v2 (`0x02`, without `0x01`) and the user has enabled UHQ → UHQ v2, flags `0x0A`.
2. Otherwise, if the sink offers HiFi (`0x04`) → HiFi 48 kHz / 24-bit, flags `0x0C`.
3. Otherwise 44.1 kHz if offered, else 48 kHz, 16-bit.

`0x08` is copied from the sink's capability. The phone never configures legacy `0x80`.

**Developer options:** the phone reports only one selectable sample rate and bit depth for SSC, namely those of the
current mode. All other choices appear greyed out. This is Samsung's design, not a sink problem. Modes change
with the UHQ setting.

## Media packets

```
RTP header (12 bytes) | SSC frame | [second SSC frame]
```

* No codec payload header. Frames are back to back, and their sizes come from their headers (see
  [bitstream.md](bitstream.md#bitrates-and-frame-sizes)).
* 44.1 / 48 kHz: one frame per packet.
* 96 kHz: one or two frames per packet, depending on the bitrate (584 kbps: 1; 291, 328 and 152 kbps: 2).
* The phone's encoder runs on a 10 ms tick for UHQ v2 (20 or 10 ms otherwise).

## Bitrate adaptation

The phone picks the bitrate per packet from its link "busy level" (0 – 3). Nothing is signalled to the sink:
the sink only sees the bitrate code change in the frame header.

| Mode | Level 0 | Level 1 | Level 2 | Level 3 |
|---|---|---|---|---|
| SSC / HiFi with `0x08` | 192 | 192 | 128 | 88 kbps |
| SSC / HiFi without `0x08` | 128 | 128 | 128 | 88 kbps |
| Legacy UHQ (`0x80`) | 328 | 328 | 328 | 328 kbps |
| UHQ v2 | 584 | 584 | 291 | 152 kbps |

A sink must accept a different bitrate (and frame size) on every frame. The decoder state does not need a reset
when the bitrate changes.
