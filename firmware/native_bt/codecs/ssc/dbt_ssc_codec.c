#include "dbt_ssc_codec.h"

#include "ssc_decoder.h"

/* Codec information element after LOSC (9 octets):
 *   [0] media type 0x00, [1] codec type 0xFF, [2..5] vendor 0x00000075 LE, [6..7] codec 0x0103 LE, [8] flags.
 * Flags: 0x80 96 kHz (legacy), 0x40 32 kHz, 0x20 44.1 kHz, 0x10 48 kHz, 0x08 unlimited bitrate (192 kbps table),
 *        0x04 HiFi (48 kHz / 24-bit), 0x02 UHQ v2 (96 kHz / 32-bit container), 0x01 UHQ v2 blocked.
 * A configuration has the UHQ v2 bit, the HiFi bit, or exactly one sample-rate bit (Samsung's
 * A2DP_VendorGetTrackSampleRateSsc checks them in that order). */
#define SSC_CIE_BYTES 9u
#define SSC_RATE_MASK 0xF0u
#define SSC_FLAG_UNLIMITED 0x08u
#define SSC_FLAG_HIFI 0x04u
#define SSC_FLAG_UHQ2 0x02u
#define SSC_FLAG_NO_UHQ2 0x01u

/* The adapter owns no PCM buffer. A decoded frame stays inside the decoder (its postfilter buffers) and is
 * converted piecewise into ssc_output_buffer(), decoder memory that is idle between frames. S16: the whole
 * frame (3456 bytes) is one view; S32: two views of 432 frames. The view is borrowed by the worker after a
 * PENDING pcm() until resume/reset/close, and the next piece or frame is only produced from resume(). */
#define SSC_VIEW_FRAMES(bytes) ((uint16_t)(SSC_OUTPUT_BYTES / (2u * (bytes))))
_Static_assert(SSC_FRAME_SAMPLES == DBT_SSC_FRAME_SAMPLES, "SSC frame size");
_Static_assert(DBT_SSC_FRAME_SAMPLES % (SSC_OUTPUT_BYTES / 8u) == 0 && DBT_SSC_FRAME_SAMPLES % (SSC_OUTPUT_BYTES / 4u) == 0,
               "SSC views must split a frame evenly");

typedef struct {
    ssc_decoder *codec;
    /* Packet being decoded. 96 kHz packets may carry two frames; each decoded frame is emitted on its own and
     * the next one is decoded from resume() once the worker has retired the previous PCM view. */
    const uint8_t *packet;
    uint32_t timestamp;
    uint16_t size, pos, index;
    uint16_t emitted; /* frames of the current decoded frame already emitted; 0 = decode the next frame */
} ssc_state_t;

static void copy(void *to, const void *from, unsigned n) {
    uint8_t *d = to;
    const uint8_t *s = from;
    for (unsigned i = 0; i < n; ++i) d[i] = s[i];
}

static bool header_ok(const uint8_t *p) {
    return p[0] == 0x75 && !p[1] && !p[2] && !p[3] && p[4] == 0x03 && p[5] == 0x01;
}

static bool one_bit(unsigned v) { return v && !(v & (v - 1u)); }

dbt_result_t dbt_ssc_configure(const uint8_t *p, uint16_t size, dbt_codec_config_t *out) {
    if (!p || !out || size != SSC_CIE_BYTES || p[0] || p[1] != 0xff || !header_ok(p + 2)) return DBT_UNSUPPORTED;
    uint8_t f = p[8], rate = f & SSC_RATE_MASK;
    dbt_codec_config_t c = {0};
    if (f & SSC_FLAG_UHQ2) { /* UHQ v2: 96 kHz codec mode, 24-bit content in a 32-bit container */
        c.rate = 96000u;
        c.sample_bytes = 4;
    } else if (f & SSC_FLAG_HIFI) { /* HiFi: 48 kHz / 24-bit, coded with the 44.1 kHz mode */
        c.rate = 48000u;
        c.sample_bytes = 4;
    } else if (one_bit(rate) && (rate == 0x20u || rate == 0x10u)) {
        c.rate = rate == 0x20u ? 44100u : 48000u;
        c.sample_bytes = 2;
    } else if (rate == 0x80u) { /* legacy UHQ: same 96 kHz codec mode (phones normally use UHQ v2 instead) */
        c.rate = 96000u;
        c.sample_bytes = 4;
    } else {
        return DBT_UNSUPPORTED; /* 32 kHz is never offered */
    }
    c.samples = DBT_SSC_FRAME_SAMPLES;
    c.channels = 2;
    c.cie_size = SSC_CIE_BYTES;
    copy(c.cie, p, SSC_CIE_BYTES);
    *out = c;
    return DBT_OK;
}

uint8_t dbt_a2dp_ssc_validate(const dbt_avdtp_endpoint_t *endpoint, const dbt_avdtp_cap_t *codec) {
    if (!endpoint || !codec || codec->category != 7 || !codec->data) return 0x18;
    dbt_codec_config_t c;
    dbt_avdtp_caps_t local;
    dbt_avdtp_cap_error_t error;
    if (dbt_ssc_configure(codec->data, codec->size, &c) != DBT_OK ||
        dbt_avdtp_caps_validate(endpoint->capabilities, endpoint->size, DBT_AVDTP_CAPABILITIES, &local, &error) !=
            DBT_OK ||
        local.codec.size != SSC_CIE_BYTES)
        return 0x29;
    uint8_t want = codec->data[8], have = local.codec.data[8];
    /* Every selected rate/mode bit must be one we advertised (HiFi/UHQ configurations carry no rate bit). */
    if ((want & (SSC_RATE_MASK | SSC_FLAG_HIFI | SSC_FLAG_UHQ2 | SSC_FLAG_UNLIMITED)) & ~have) return 0x29;
    return 0;
}

static int decoder_rate(const dbt_codec_config_t *c) { return c->rate == 96000u ? 96000 : 44100; }

/* Number of whole frames in the payload, 0 if it is not a clean sequence of SSC frames. */
static unsigned packet_frames(const dbt_codec_config_t *c, const uint8_t *p, uint16_t size) {
    unsigned pos = 0, frames = 0;
    if (!p || size < 6 || size > DBT_SSC_FRAME_MAX * 2u) return 0;
    while (pos < size) {
        int n = ssc_frame_bytes(p + pos, (int)(size - pos), decoder_rate(c));
        if (n <= 0 || (unsigned)n > size - pos) return 0;
        pos += (unsigned)n;
        if (++frames > 2) return 0;
    }
    return frames;
}

uint32_t dbt_ssc_packet_frames(const dbt_codec_config_t *config, const dbt_avdtp_media_t *p) {
    if (!config || !p || !p->payload || p->payload_type == DBT_AVDTP_RAW_PAYLOAD_TYPE) return 0;
    return packet_frames(config, p->payload, p->size) * DBT_SSC_FRAME_SAMPLES;
}

/* Decoder memory goes through the worker's accounted allocator, so it only exists while an SSC stream is
 * open and is charged to the codec. The library allocates 3 blocks of at most ~8 KB (fragmented heap);
 * all of its tables are const and stay in flash. */
static void *ssc_alloc(void *ctx, unsigned size) { return dbt_codec_allocate((dbt_codec_decoder_t *)ctx, size); }
static void ssc_free(void *ctx, void *ptr) { dbt_codec_release((dbt_codec_decoder_t *)ctx, ptr); }

static void clear_packet(ssc_state_t *s) {
    s->packet = 0;
    s->size = s->pos = s->index = s->emitted = 0;
}

static void close_decoder(dbt_codec_decoder_t *d) {
    ssc_state_t *s = d->state;
    if (!s) return;
    if (s->codec) ssc_decoder_destroy(s->codec);
    dbt_codec_release(d, s);
    d->state = 0;
}

static dbt_result_t open_decoder(dbt_codec_decoder_t *d) {
    dbt_codec_config_t checked;
    if (dbt_ssc_configure(d->config.cie, d->config.cie_size, &checked) != DBT_OK || checked.rate != d->config.rate ||
        checked.channels != d->config.channels || checked.samples != d->config.samples ||
        checked.sample_bytes != d->config.sample_bytes)
        return DBT_INVALID;
    ssc_state_t *s = (ssc_state_t *)dbt_codec_allocate(d, sizeof(ssc_state_t));
    if (!s) return DBT_NO_SPACE;
    s->codec = 0;
    clear_packet(s);
    d->state = s;
    s->codec = ssc_decoder_create_with_allocator(decoder_rate(&checked), ssc_alloc, ssc_free, d);
    return s->codec ? DBT_OK : DBT_NO_SPACE; /* dbt_codec_open() calls close() on failure */
}

static dbt_result_t reset_decoder(dbt_codec_decoder_t *d) {
    ssc_state_t *s = d->state;
    if (!s || !s->codec) return DBT_INVALID;
    clear_packet(s);
    ssc_decoder_reset(s->codec);
    return DBT_OK;
}

/* Decode and emit the frames of the current packet, one view at a time, until done or a view is borrowed
 * (PENDING). */
static dbt_result_t resume(dbt_codec_decoder_t *d, uint32_t now) {
    (void)now;
    ssc_state_t *s = d->state;
    if (!s) return DBT_INVALID;
    const bool wide = d->config.sample_bytes == 4;
    const uint16_t chunk = SSC_VIEW_FRAMES(wide ? 4u : 2u);
    while (s->packet && (s->emitted || s->pos < s->size)) {
        if (!s->emitted) {
            const uint8_t *f = s->packet + s->pos;
            int n = ssc_frame_bytes(f, (int)(s->size - s->pos), decoder_rate(&d->config));
            if (n <= 0 || ssc_decode_frame(s->codec, f, n) != (int)DBT_SSC_FRAME_SAMPLES) {
                clear_packet(s);
                return DBT_INVALID;
            }
            s->pos = (uint16_t)(s->pos + n);
        }
        void *view = ssc_output_buffer(s->codec);
        int r = wide ? ssc_read_s32(s->codec, s->emitted, chunk, (int32_t *)view)
                     : ssc_read_s16(s->codec, s->emitted, chunk, (int16_t *)view);
        if (r != (int)chunk) {
            clear_packet(s);
            return DBT_INVALID;
        }
        uint32_t ts = s->timestamp + (uint32_t)s->index * DBT_SSC_FRAME_SAMPLES + s->emitted;
        dbt_result_t result = d->hooks.pcm(d->hooks.context, view, chunk, &d->config, ts);
        if (result != DBT_OK && result != DBT_PENDING) {
            clear_packet(s);
            return result;
        }
        s->emitted = (uint16_t)(s->emitted + chunk);
        if (s->emitted == DBT_SSC_FRAME_SAMPLES) {
            s->emitted = 0;
            ++s->index;
            ++d->frames;
        }
        if (result == DBT_PENDING) return result;
    }
    clear_packet(s);
    return DBT_OK;
}

static dbt_result_t media(dbt_codec_decoder_t *d, const dbt_avdtp_media_t *p, uint32_t now) {
    ssc_state_t *s = d->state;
    if (!s || !s->codec || !p || !p->payload) return DBT_INVALID;
    if (s->packet) return DBT_BUSY;
    if (!packet_frames(&d->config, p->payload, p->size)) return DBT_INVALID; /* worker drops it and resets */
    s->packet = p->payload;
    s->size = p->size;
    s->pos = s->index = s->emitted = 0;
    s->timestamp = p->timestamp;
    return resume(d, now);
}

const dbt_codec_ops_t dbt_ssc_codec = {"SSC", dbt_ssc_configure, open_decoder, close_decoder, reset_decoder,
                                       media, 0, resume, 0};
