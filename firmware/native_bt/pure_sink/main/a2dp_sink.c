/* A2DP sink for pure playback: endpoint/capability tables and validators
 * copied from esp32_probe/main/probe_audio.c, the shared codec worker
 * (dbt_codec_worker_*) in deferred-PCM mode, and one decoder task on core 0.
 * PCM goes straight to audio_out (ring buffer + I2S writer). */
#include "a2dp_sink.h"

#include <inttypes.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

#include "audio_out.h"
#include "dbt_aac.h"
#include "dbt_aptx.h"
#include "dbt_codec.h"
#include "dbt_ldac.h"
#include "dbt_opus.h"
#include "dbt_sbc_codec.h"
#include "dbt_ssc_codec.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "pure_sink.h"

#define A2DP_PSM 0x19u
#define A2DP_RX_MTU 1800u /* full media SDU incl. RTP/codec headers */
#define A2DP_MEDIA_SLOTS 8u
#define DELAY_REPORT_PERIOD_MS 5000u
#define INFO_LOG_SPACING_MS 1000u

static const char *TAG = "a2dp";

/* ---- Endpoints (identical to probe_audio.c, all codecs of the SSC Classic build) ---- */
static const uint8_t sbc_capabilities[] = {1, 0, 7, 6, 0, 0, 0x3f, 0xff, 2, 53, 8, 0};
static const uint8_t android_capabilities[] = {1, 0, 7, 9, 0, 0xff, 0xe0, 0, 0, 0, 1, 0, 0x9b, 8, 0};
static const uint8_t opus05_capabilities[] = {1, 0, 7, 26, 0, 0xff, 0xf1, 5, 0, 0, 5, 0x10, 2, 1, 3, 0, 0, 0,
                                              0x1f, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 8, 0};
/* MPEG-2/4 AAC-LC, 44.1/48 kHz mono/stereo, VBR, up to 320 kbit/s. */
static const uint8_t aac_capabilities[] = {1, 0, 7, 8, 0, 2, 0xc0, 1, 0x8c, 0x84, 0xe2, 0, 8, 0};
/* Only stereo, 44.1/48 kHz and no aptX LL reverse channel/extensions. */
static const uint8_t aptx_capabilities[] = {1, 0, 7, 9, 0, 0xff, 0x4f, 0, 0, 0, 1, 0, 0x32, 8, 0};
static const uint8_t aptx_hd_capabilities[] = {1, 0, 7, 13, 0, 0xff, 0xd7, 0, 0, 0, 0x24, 0, 0x32, 0, 0, 0, 0, 8, 0};
static const uint8_t aptx_ll_capabilities[] = {1, 0, 7, 10, 0, 0xff, 0x0a, 0, 0, 0, 2, 0, 0x32, 0, 8, 0};
static const uint8_t ldac_capabilities[] = {1, 0, 7, 10, 0, 0xff, 0x2d, 1, 0, 0, 0xaa, 0, 0x3c, 7, 8, 0};
/* Samsung Scalable/Seamless Codec: vendor 0x00000075, codec 0x0103. Flags 0x3E = 44.1 + 48 kHz + unlimited
 * bitrate + HiFi (48 kHz/24-bit) + UHQ v2 (96 kHz/24-bit in 32-bit). */
static const uint8_t ssc_capabilities[] = {1, 0, 7, 9, 0, 0xff, 0x75, 0, 0, 0, 0x03, 0x01, 0x3e, 8, 0};
static const dbt_avdtp_endpoint_t endpoints[] = {
    {sbc_capabilities, sizeof(sbc_capabilities), 1, dbt_a2dp_sbc_validate, 0, 0},
    {android_capabilities, sizeof(android_capabilities), 2, dbt_a2dp_opus_validate, 0, 0},
    {opus05_capabilities, sizeof(opus05_capabilities), 3, dbt_a2dp_opus_validate, 0, 0},
    {aac_capabilities, sizeof(aac_capabilities), 4, dbt_a2dp_aac_validate, 0, 0},
    {aptx_capabilities, sizeof(aptx_capabilities), 5, dbt_a2dp_aptx_validate, 4, 4},
    {aptx_hd_capabilities, sizeof(aptx_hd_capabilities), 6, dbt_a2dp_aptx_validate, 0, 0},
    {aptx_ll_capabilities, sizeof(aptx_ll_capabilities), 7, dbt_a2dp_aptx_validate, 4, 4},
    {ldac_capabilities, sizeof(ldac_capabilities), 8, dbt_a2dp_ldac_validate, 0, 0},
    {ssc_capabilities, sizeof(ssc_capabilities), 10, dbt_a2dp_ssc_validate, 0, 0},
};
static const dbt_codec_ops_t *const codecs[] = {&dbt_sbc_codec, &dbt_opus_codec, &dbt_aac_codec,
                                                &dbt_aptx_codec, &dbt_ldac_codec, &dbt_ssc_codec};

typedef enum { FAM_NONE, FAM_SBC, FAM_AAC, FAM_APTX, FAM_LDAC, FAM_OPUS, FAM_SSC } family_t;

static dbt_a2dp_acceptor_t profile;
static dbt_codec_worker_t worker;
static TaskHandle_t owner, decoder_task;
static bool initialized;
static uint32_t worker_faults_seen, delay_binding, delay_at, resident_seen;
static dbt_stream_state_t state_seen;
static bool active_seen;

/* Host-owner view of the negotiated codec, for logging and delay reports. */
static dbt_codec_config_t host_config;
static bool host_config_valid;
static family_t family;
static dbt_codec_config_t pending_config;
static const char *pending_prefix;
static char previous_label[16];
/* In-band parameters observed by the media hook (host owner). */
static int32_t info_value = -1, info_logged = -1;
static uint32_t info_logged_at;

static uint32_t now_ms(void) { return (uint32_t)(esp_timer_get_time() / 1000); }

/* ------------------------------------------------------------ logging */
static const char *codec_label(const dbt_codec_config_t *c) {
    if (!c || !c->ops) return "none";
    if (c->ops == &dbt_sbc_codec) return "SBC";
    if (c->ops == &dbt_aac_codec) return "AAC";
    if (c->ops == &dbt_aptx_codec) return c->cie_size == 9 ? "aptX" : c->cie_size == 13 ? "aptX HD" : "aptX LL";
    if (c->ops == &dbt_ldac_codec) return "LDAC";
    if (c->ops == &dbt_opus_codec) return c->cie_size == 9 ? "Opus (Android)" : "Opus05";
    if (c->ops == &dbt_ssc_codec) return "SSC";
    return c->ops->name ? c->ops->name : "?";
}
static family_t codec_family(const dbt_codec_config_t *c) {
    if (!c || !c->ops) return FAM_NONE;
    if (c->ops == &dbt_sbc_codec) return FAM_SBC;
    if (c->ops == &dbt_aac_codec) return FAM_AAC;
    if (c->ops == &dbt_aptx_codec) return FAM_APTX;
    if (c->ops == &dbt_ldac_codec) return FAM_LDAC;
    if (c->ops == &dbt_opus_codec) return FAM_OPUS;
    if (c->ops == &dbt_ssc_codec) return FAM_SSC;
    return FAM_NONE;
}
static void describe(const dbt_codec_config_t *c, char *out, size_t size) {
    const uint8_t *p = c->cie;
    out[0] = 0;
    switch (codec_family(c)) {
    case FAM_SBC: {
        unsigned m = p[2] & 15u, bl = p[3] >> 4, sb = (p[3] >> 2) & 3u;
        snprintf(out, size, "%s, %u blocks, %u subbands, %s allocation, bitpool %u..%u",
                 m & 8 ? "mono" : m & 4 ? "dual channel" : m & 2 ? "stereo" : "joint stereo",
                 bl & 8 ? 4u : bl & 4 ? 8u : bl & 2 ? 12u : 16u, sb & 2 ? 4u : 8u, p[3] & 2 ? "SNR" : "loudness",
                 p[4], p[5]);
        break;
    }
    case FAM_AAC: {
        uint32_t bitrate = ((uint32_t)(p[5] & 0x7fu) << 16) | ((uint32_t)p[6] << 8) | p[7];
        const char *object = p[2] & 0x80 ? "MPEG-2 AAC-LC" : p[2] & 0x40 ? "MPEG-4 AAC-LC" : p[2] & 0x20 ? "MPEG-4 AAC-LTP"
                             : p[2] & 0x10 ? "MPEG-4 AAC scalable" : "object?";
        snprintf(out, size, "%s, %s, max bitrate %" PRIu32 " kbps", object, p[5] & 0x80 ? "VBR" : "CBR", bitrate / 1000u);
        break;
    }
    case FAM_APTX:
        snprintf(out, size, "%s, vendor 0x%02x%02x codec 0x%02x%02x, stereo", codec_label(c), p[3], p[2], p[7], p[6]);
        break;
    case FAM_LDAC:
        snprintf(out, size, "%s, quality (HQ/SQ/MQ/ABR) follows from the stream",
                 p[9] == 4 ? "mono" : p[9] == 2 ? "dual channel" : "stereo");
        break;
    case FAM_OPUS:
        snprintf(out, size, "%s, %u-sample frames", codec_label(c), c->samples);
        break;
    case FAM_SSC: {
        uint8_t f = p[8];
        snprintf(out, size, "mode %s%s, flags 0x%02x",
                 f & 0x02 ? "UHQ v2 (96 kHz/24-bit)" : f & 0x04 ? "HiFi (48 kHz/24-bit)"
                 : f & 0x80 ? "legacy UHQ (96 kHz)" : "SSC (16-bit)",
                 f & 0x08 ? ", unlimited bitrate" : "", f);
        break;
    }
    default:
        break;
    }
}
static void log_codec(const char *prefix, const dbt_codec_config_t *c) {
    char hex[3u * DBT_CODEC_CIE_BYTES + 1u], detail[112];
    unsigned n = c->cie_size > DBT_CODEC_CIE_BYTES ? DBT_CODEC_CIE_BYTES : c->cie_size;
    size_t at = 0;
    hex[0] = 0;
    for (unsigned i = 0; i < n && at < sizeof(hex); ++i)
        at += (size_t)snprintf(hex + at, sizeof(hex) - at, i ? " %02x" : "%02x", c->cie[i]);
    describe(c, detail, sizeof(detail));
    ESP_LOGI(TAG, "%s: %s %" PRIu32 " Hz %s %u ch, %s | CIE[%u] %s", prefix, codec_label(c), c->rate,
             c->sample_bytes == 2 ? "S16" : c->sample_bytes == 3 ? "S24" : "S32", c->channels, detail, n, hex);
}
static unsigned ssc_kbps(unsigned index) {
    static const uint16_t f_family[] = {88, 96, 128, 192, 229, 328, 256};
    static const uint16_t e_family[] = {291, 308, 442, 584, 886, 250, 152};
    if (index >= 0x10 && index <= 0x16) return f_family[index - 0x10];
    if (index <= 0x06) return e_family[index];
    return 0;
}
static void log_stream_info(int32_t value) {
    switch (family) {
    case FAM_SBC:
        ESP_LOGI(TAG, "SBC stream: bitpool %d", (int)value);
        break;
    case FAM_LDAC: {
        uint32_t samples = host_config.samples ? host_config.samples : 128u;
        uint32_t kbps = (uint32_t)value * 8u * host_config.rate / samples / 1000u;
        ESP_LOGI(TAG, "LDAC stream: %d-byte frames = %" PRIu32 " kbps (%s) at %" PRIu32 " Hz", (int)value, kbps,
                 kbps >= 800 ? "HQ" : kbps >= 500 ? "SQ" : kbps >= 300 ? "MQ" : "ABR low", host_config.rate);
        break;
    }
    case FAM_SSC: {
        unsigned kbps = ssc_kbps((unsigned)value);
        if (kbps) ESP_LOGI(TAG, "SSC stream: bitrate %u kbps (index 0x%02x)", kbps, (unsigned)value);
        else ESP_LOGI(TAG, "SSC stream: unknown bitrate index 0x%02x", (unsigned)value);
        break;
    }
    default:
        break;
    }
}

/* ------------------------------------------------------- worker hooks */
static uint8_t *allocate(void *context, uint16_t bytes) {
    (void)context;
    return heap_caps_malloc(bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
}
static uint8_t *codec_allocate(void *context, uint32_t bytes) {
    (void)context;
    return heap_caps_aligned_alloc(8, bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
}
static void release(void *context, uint8_t *bytes) {
    (void)context;
    heap_caps_free(bytes);
}
static void wake_worker(void *context) {
    (void)context;
    if (decoder_task) xTaskNotifyGive(decoder_task);
}
static void wake_owner(void *context) {
    (void)context;
    xTaskNotifyGive(owner);
}
/* Decoder task: copy one <=256-frame chunk into the ring or report BUSY. */
static dbt_result_t pcm(void *context, const void *samples, uint16_t frames, const dbt_codec_config_t *config,
                        uint32_t timestamp) {
    (void)context;
    (void)timestamp;
    if (!config) return DBT_INVALID;
    return audio_out_pcm(samples, frames, config->rate, config->channels, config->sample_bytes);
}
/* Decoder task: START/SUSPEND/CLOSE/fault. Nonblocking hand-off to the writer. */
static dbt_result_t stream(void *context, bool start, const dbt_codec_config_t *config) {
    (void)context;
    static bool started; /* decoder-task only */
    if (!config) return DBT_INVALID;
    if (start) log_codec("stream start", config);
    else if (started) ESP_LOGI(TAG, "stream stop");
    started = start;
    return audio_out_stream(start, config->rate, config->channels, config->sample_bytes);
}
static dbt_codec_queue_t queue_policy(void *context, const dbt_codec_config_t *config) {
    (void)context;
    /* Burst capacity for AAC, LDAC and SSC (96 kHz packets may carry two frames). */
    uint8_t depth = 4;
    if (config->ops == &dbt_aac_codec || config->ops == &dbt_ldac_codec || config->ops == &dbt_ssc_codec) depth = 8;
    uint8_t low = (uint8_t)(depth / 2u);
    if (low > depth - 3u) low = (uint8_t)(depth - 3u);
    return (dbt_codec_queue_t){depth, low};
}

/* --------------------------------------------------------- sink hooks */
static dbt_result_t sink_work(void *context, const dbt_sink_job_t *job) {
    dbt_result_t result = dbt_codec_worker_submit(context, job);
    if (result != DBT_OK || !job) return result;
    if (job->operation == DBT_SINK_CONFIGURE || job->operation == DBT_SINK_RECONFIGURE) {
        dbt_avdtp_caps_t caps;
        dbt_avdtp_cap_error_t error;
        dbt_codec_config_t config = {0};
        if (dbt_avdtp_caps_validate(job->configuration, job->size, DBT_AVDTP_CONFIGURATION, &caps, &error) == DBT_OK) {
            for (unsigned i = 0; i < sizeof(codecs) / sizeof(codecs[0]); ++i)
                if (codecs[i]->configure(caps.codec.data, caps.codec.size, &config) == DBT_OK) {
                    config.ops = codecs[i];
                    break;
                }
        }
        if (config.ops) {
            bool changed = host_config_valid && strcmp(previous_label, codec_label(&config)) != 0;
            pending_prefix = job->operation == DBT_SINK_RECONFIGURE ? "reconfigured" : changed ? "codec changed" : "configured";
            pending_config = config;
            host_config = config;
            host_config_valid = true;
            family = codec_family(&config);
        }
    } else if (job->operation == DBT_SINK_START) {
        info_value = info_logged = -1; /* log the first in-band parameters again */
    }
    return result;
}
static void sink_event(void *context, dbt_sink_event_t event, dbt_stream_state_t state) {
    (void)context;
    dbt_sink_operation_t operation = profile.sink.pending.operation;
    /* A (re)configuration may change the rate: report the delay again. */
    if (event == DBT_SINK_STATE && profile.sink.pending.used &&
        (operation == DBT_SINK_CONFIGURE || operation == DBT_SINK_RECONFIGURE))
        delay_binding = 0;
    if (event == DBT_SINK_FAULT) ESP_LOGW(TAG, "AVDTP sink fault (state %d)", (int)state);
}
/* Host owner: peek at in-band codec parameters, then queue for the decoder. */
static dbt_result_t media(void *context, const dbt_avdtp_media_t *packet) {
    if (packet && packet->payload && packet->size >= 4) {
        const uint8_t *p = packet->payload;
        int32_t value = -1;
        if (family == FAM_SBC && p[1] == 0x9c) value = p[3];
        else if (family == FAM_LDAC && p[1] == 0xaa) value = (int32_t)(((((unsigned)p[2] & 7u) << 6) | (p[3] >> 2)) + 1u + 3u);
        else if (family == FAM_SSC) value = p[3] & 0x1f;
        if (value >= 0) info_value = value;
    }
    return dbt_codec_worker_media(context, packet);
}

/* ------------------------------------------------------- decoder task */
static void decode_loop(void *unused) {
    (void)unused;
    uint32_t seen_faults = 0;
    for (;;) {
        (void)dbt_codec_worker_step(&worker, now_ms(), 2);
        uint32_t faults = atomic_load_explicit(&worker.faults, memory_order_acquire);
        if (faults != seen_faults) {
            seen_faults = faults;
            xTaskNotifyGive(owner);
        }
        if (dbt_codec_worker_runnable(&worker)) {
            taskYIELD();
            continue;
        }
        TickType_t wait = portMAX_DELAY;
        uint32_t when;
        if (dbt_codec_worker_deadline(&worker, &when)) {
            int32_t remaining = (int32_t)(when - now_ms());
            wait = remaining <= 0 ? 0 : pdMS_TO_TICKS((uint32_t)remaining);
            if (remaining > 0 && !wait) wait = 1;
        }
        /* Notifications sent before this call stay pending: no lost wakeup. */
        (void)ulTaskNotifyTake(pdTRUE, wait);
    }
}

/* -------------------------------------------------------- public API */
dbt_result_t a2dp_sink_init(dbt_runtime_t *runtime, TaskHandle_t host_owner) {
    if (initialized) return DBT_INVALID;
    owner = host_owner;
    dbt_codec_worker_hooks_t hooks = {0};
    hooks.decoder = (dbt_codec_hooks_t){0, codec_allocate, release, pcm};
    hooks.codecs = codecs;
    hooks.codec_count = (uint8_t)(sizeof(codecs) / sizeof(codecs[0]));
    hooks.wake_worker = wake_worker;
    hooks.wake_owner = wake_owner;
    hooks.stream = stream;
    hooks.deferred_pcm = true; /* copy <=256-frame chunks; BUSY = back-pressure */
    hooks.queue_policy = queue_policy;
    dbt_result_t result = dbt_codec_worker_init(&worker, hooks, A2DP_RX_MTU, A2DP_MEDIA_SLOTS);
    if (result != DBT_OK) return result;
    dbt_avdtp_sink_hooks_t client = {&worker, allocate, release, sink_work, sink_event, media};
    result = dbt_a2dp_acceptor_init(&profile, runtime, client, endpoints, (uint8_t)(sizeof(endpoints) / sizeof(endpoints[0])),
                                    A2DP_RX_MTU, 64, 5000, 2000, 5000);
    if (result != DBT_OK) return result;
    if (xTaskCreatePinnedToCore(decode_loop, "decoder", PURE_SINK_DECODER_STACK, NULL, PURE_SINK_DECODER_PRIORITY,
                                &decoder_task, PURE_SINK_DECODER_CORE) != pdPASS)
        return DBT_NO_SPACE;
    audio_out_set_producer(decoder_task);
    initialized = true;
    ESP_LOGI(TAG, "A2DP sink ready: SBC, AAC, aptX, aptX HD, aptX LL, LDAC, Opus, Opus05, SSC (worker %u B)",
             (unsigned)sizeof(worker));
    return DBT_OK;
}
bool a2dp_sink_accept(const dbt_link_t *link, uint16_t psm) {
    return psm != A2DP_PSM || (initialized && dbt_a2dp_acceptor_accept(&profile, link));
}
void a2dp_sink_event(dbt_channel_event_t event, const dbt_channel_t *channel, dbt_channel_reason_t reason,
                     uint16_t wire_result) {
    if (!initialized || !channel || channel->psm != A2DP_PSM) return;
    dbt_result_t result = dbt_a2dp_acceptor_event_ex(&profile, event, channel, reason, wire_result);
    if (result != DBT_OK && result != DBT_EMPTY) {
        ESP_LOGW(TAG, "AVDTP channel event %d rejected (%d), retiring session", (int)event, (int)result);
        (void)dbt_a2dp_acceptor_retire(&profile, now_ms());
    }
}
void a2dp_sink_receive(const dbt_channel_t *channel, const uint8_t *data, uint16_t size) {
    if (initialized) (void)dbt_a2dp_acceptor_receive(&profile, channel, data, size);
}

static uint32_t delay_report_wait_ms(uint32_t now) {
    const dbt_avdtp_sink_t *sink = &profile.sink;
    if (!profile.enabled || !profile.active || profile.draining || sink->failed || sink->retiring ||
        sink->signal.failed || !sink->delay_enabled || sink->state < DBT_STREAM_CONFIGURED ||
        sink->state > DBT_STREAM_STREAMING || sink->pending.used || sink->signal.tx[0].used)
        return UINT32_MAX;
    if (delay_binding != sink->binding) return 0;
    uint32_t elapsed = now - delay_at;
    return elapsed >= DELAY_REPORT_PERIOD_MS ? 0 : DELAY_REPORT_PERIOD_MS - elapsed;
}

static const char *state_name(dbt_stream_state_t s) {
    static const char *const names[] = {"idle", "configured", "open", "streaming", "closing", "aborting"};
    return (unsigned)s < sizeof(names) / sizeof(names[0]) ? names[s] : "?";
}

void a2dp_sink_poll(uint32_t now) {
    if (!initialized) return;
    dbt_codec_receipt_t receipt;
    for (unsigned i = 0; i < DBT_CODEC_WORKER_JOBS && dbt_codec_worker_receipt(&worker, &receipt) == DBT_OK; ++i) {
        if (receipt.result != DBT_SINK_WORK_READY)
            ESP_LOGW(TAG, "decoder job %" PRIu32 " %s", receipt.token,
                     receipt.result == DBT_SINK_WORK_REJECTED ? "rejected" : "failed");
        (void)dbt_a2dp_acceptor_complete(&profile, receipt.binding, receipt.token, receipt.result, now);
    }
    uint32_t faults = atomic_load_explicit(&worker.faults, memory_order_acquire);
    if (faults != worker_faults_seen) {
        worker_faults_seen = faults;
        ESP_LOGE(TAG, "decoder/output fault (pcm timeouts %" PRIu32 ", pcm failures %" PRIu32 ", decode rejects %" PRIu32
                      "): retiring the stream",
                 atomic_load(&worker.pcm_timeouts), atomic_load(&worker.pcm_failures), atomic_load(&worker.decode_rejects));
        (void)dbt_a2dp_acceptor_retire(&profile, now);
    }
    (void)dbt_a2dp_acceptor_poll(&profile, now);

    if (delay_report_wait_ms(now) == 0) {
        delay_binding = profile.sink.binding;
        delay_at = now;
        uint16_t delay = audio_out_delay_100us(host_config_valid ? host_config.rate : 0);
        if (dbt_avdtp_sink_delay(&profile.sink, delay, now) == DBT_OK && delay_binding)
            ESP_LOGD(TAG, "delay report %u.%u ms", delay / 10u, delay % 10u);
        if (profile.sink.failed || profile.sink.signal.failed) (void)dbt_a2dp_acceptor_retire(&profile, now);
    }

    /* --- logging (host owner, outside protocol callbacks) --- */
    if (profile.active != active_seen) {
        active_seen = profile.active;
        if (active_seen) ESP_LOGI(TAG, "A2DP signaling connected (handle 0x%03x)", profile.signaling.handle);
        else {
            ESP_LOGI(TAG, "A2DP disconnected");
            host_config_valid = false;
            previous_label[0] = 0;
            family = FAM_NONE;
        }
    }
    if (pending_prefix) {
        log_codec(pending_prefix, &pending_config);
        strncpy(previous_label, codec_label(&pending_config), sizeof(previous_label) - 1u);
        pending_prefix = NULL;
    }
    if (profile.sink.state != state_seen) {
        ESP_LOGI(TAG, "AVDTP stream %s -> %s", state_name(state_seen), state_name(profile.sink.state));
        state_seen = profile.sink.state;
    }
    if (info_value >= 0 && info_value != info_logged &&
        (info_logged < 0 || now - info_logged_at >= INFO_LOG_SPACING_MS)) {
        info_logged = info_value;
        info_logged_at = now;
        log_stream_info(info_value);
    }
    uint32_t resident = atomic_load_explicit(&worker.resident_bytes, memory_order_acquire);
    if (resident != resident_seen) {
        if (resident && !resident_seen) {
            ESP_LOGI(TAG, "codec opened: %s, decoder working set %" PRIu32 " bytes", codec_label(&host_config), resident);
            pure_sink_heap_log("codec open");
        } else if (!resident) {
            ESP_LOGI(TAG, "codec closed (released %" PRIu32 " bytes)", resident_seen);
            pure_sink_heap_log("codec close");
        } else {
            ESP_LOGI(TAG, "codec working set now %" PRIu32 " bytes", resident);
        }
        resident_seen = resident;
    }
}

uint32_t a2dp_sink_wait_ms(uint32_t now) {
    if (!initialized) return UINT32_MAX;
    uint32_t wait = dbt_a2dp_acceptor_wait_ms(&profile, now);
    uint32_t report = delay_report_wait_ms(now);
    if (report < wait) wait = report;
    if (info_value >= 0 && info_value != info_logged) {
        uint32_t elapsed = now - info_logged_at;
        uint32_t next = info_logged < 0 || elapsed >= INFO_LOG_SPACING_MS ? 0 : INFO_LOG_SPACING_MS - elapsed;
        if (next < wait) wait = next;
    }
    if (atomic_load_explicit(&worker.resident_bytes, memory_order_relaxed) != resident_seen) wait = 0;
    return wait;
}

/* Credit policy from probe_audio.c: while streaming, never let more ACL
 * packets in than free encoded slots (minus one for a partial PDU). */
uint16_t a2dp_sink_rx_credit_window(void) { return 3u; }
uint16_t a2dp_sink_rx_credit_limit(void *context, const dbt_link_t *link, uint16_t pending, uint16_t window) {
    (void)context;
    if (!pending) return 0;
    if (!initialized || !link || link->kind != DBT_LINK_CLASSIC || !profile.active || profile.draining ||
        profile.sink.failed || profile.sink.retiring || profile.sink.state != DBT_STREAM_STREAMING ||
        !profile.signaling.serial || profile.signaling.handle != link->handle ||
        profile.signaling.generation != link->generation)
        return pending;
    uint32_t free_slots = dbt_codec_worker_free_slots(&worker);
    if (!free_slots) return 0;
    uint32_t outside = window > pending ? window - pending : 0;
    uint32_t free = free_slots - 1u;
    if (free <= outside) return 0;
    uint32_t allowed = free - outside;
    return allowed < pending ? (uint16_t)allowed : pending;
}

bool a2dp_sink_connected(void) { return initialized && profile.active; }
bool a2dp_sink_streaming(void) { return initialized && profile.active && profile.sink.state == DBT_STREAM_STREAMING; }
uint32_t a2dp_sink_codec_bytes(void) { return atomic_load_explicit(&worker.resident_bytes, memory_order_relaxed); }
const char *a2dp_sink_codec_label(void) { return host_config_valid ? codec_label(&host_config) : "none"; }
TaskHandle_t a2dp_sink_decoder_task(void) { return decoder_task; }
