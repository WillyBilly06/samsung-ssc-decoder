/* Pure playback output.
 *
 * decoder task (core 0)                         I2S writer task (core 1)
 *   audio_out_stream(start/stop) --gen/ack-->     apply: flush ring, (re)create I2S on rate change
 *   audio_out_pcm(frames) --> PCM ring (source format) --> convert to 32-bit stereo slots
 *                                                          * volume (one multiply), drift slip
 *                                                          --> i2s_channel_write (blocks on DMA)
 *
 * The ring stores PCM exactly as the codec emitted it (S16/S24/S32, mono or
 * stereo) so a 64 KB ring holds 341 ms of 48 kHz S16 stereo or 85 ms of
 * 96 kHz S32 stereo. Conversion to the 32-bit I2S slot happens per chunk on
 * the writer into one small static buffer. No EQ, limiter, dither or DSP.
 *
 * Clock drift: the phone's media clock and our APLL differ by tens of ppm.
 * Instead of a resampler, the writer tracks a slow moving average of the ring
 * level (time constant ~2.5 s, so Bluetooth bursts do not trigger it) and,
 * only while that average is more than 12 ms away from its set-point, drops or
 * repeats ONE stereo frame per 4 I2S chunks (max ~50 frames/s, i.e. ~1000 ppm
 * of correction capacity versus typical <100 ppm drift). A single-sample slip a
 * few times per minute is inaudible in practice and cannot oscillate. */
#include "audio_out.h"

#include <math.h>
#include <stdatomic.h>
#include <string.h>

#include "driver/i2s_std.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "soc/soc_caps.h"

#define RING_MAX_BYTES (64u * 1024u)
#define RING_MIN_BYTES (24u * 1024u)
#define DMA_DESCRIPTORS 4u /* 4 x 240 frames: 20 ms at 48 kHz, 10 ms at 96 kHz */
#define DMA_FRAMES 240u
#define CHUNK_FRAMES 240u
#define DMA_FRAMES_TOTAL (DMA_DESCRIPTORS * DMA_FRAMES)
#define WRITER_STACK 3072u
#define WRITER_PRIORITY 21
#define WRITER_CORE 1
#define DRIFT_BAND_MS 12u
#define DRIFT_SHIFT 9         /* moving average over 512 chunks (~2.5 s at 48 kHz) */
#define DRIFT_SPACING 4u      /* at most one slip per 4 chunks */
#define VOLUME_RANGE_DB 50.0f /* AVRCP 1 -> -50 dB, 127 -> 0 dB, 0 -> mute */

static const char *TAG = "audio_out";

typedef struct {
    bool start;
    uint32_t rate;
    uint8_t channels, sample_bytes;
} control_t;
typedef enum { OUT_IDLE, OUT_PREFILL, OUT_PLAY, OUT_DRAIN } out_state_t;

/* Shared ring. wpos is written only by the producer, rpos only by the writer
 * (including flushes, which happen only while the producer is gated). */
static uint8_t *ring;
static uint32_t ring_bytes;
static _Atomic uint32_t wpos, rpos;

/* Producer -> writer control. gen is bumped by the producer after it stores a
 * new control; the writer applies it and publishes ack=gen. Until then the
 * producer gets BUSY, so it can never write into a ring being flushed. */
static portMUX_TYPE control_lock = portMUX_INITIALIZER_UNLOCKED;
static control_t control;
static _Atomic uint32_t gen, ack;
/* Applied format, written by the writer before the ack (release) and read by
 * the producer after observing the ack (acquire). */
static bool applied_start;
static uint32_t applied_rate, applied_frame_bytes, applied_capacity;
static uint8_t applied_channels, applied_sample_bytes;

static _Atomic bool producer_blocked, writer_waiting;
static TaskHandle_t writer_task_handle, producer_task;
static _Atomic int32_t gain_q16 = 65536;
static _Atomic uint32_t volume_now = 127;

/* Writer-owned state. */
static i2s_chan_handle_t channel;
static bool channel_enabled;
static uint32_t channel_rate;
static out_state_t state;
/* start = ring level that ends a (re)prefill; target = drift set-point for the
 * ring level once the DMA queue has been filled from it (start - DMA frames). */
static uint32_t frame_bytes, capacity, start, target, band;
static uint8_t in_channels, in_sample_bytes;
static int32_t level_average_q8;
static uint32_t chunks_since_slip;
static int64_t prefill_began_us, last_underrun_log_us;
static int32_t out_buffer[(CHUNK_FRAMES + 1u) * 2u];

static _Atomic uint32_t stat_underruns, stat_dropped, stat_repeated, stat_write_errors, stat_target;
static _Atomic bool stat_playing;

static void notify_producer(void) {
    if (atomic_exchange_explicit(&producer_blocked, false, memory_order_acq_rel) && producer_task)
        xTaskNotifyGive(producer_task);
}

/* ---------------------------------------------------------------- producer */
static bool acked(void) {
    return atomic_load_explicit(&ack, memory_order_acquire) == atomic_load_explicit(&gen, memory_order_relaxed);
}
static bool room_for(uint32_t frames) {
    if (!acked() || !applied_start) return false;
    uint32_t level = atomic_load_explicit(&wpos, memory_order_relaxed) - atomic_load_explicit(&rpos, memory_order_acquire);
    return level <= applied_capacity && applied_capacity - level >= frames;
}

dbt_result_t audio_out_stream(bool start, uint32_t rate, uint8_t channels, uint8_t sample_bytes) {
    if (!ring || !writer_task_handle) return start ? DBT_INVALID : DBT_OK;
    if (start && (!rate || channels < 1 || channels > 2 || sample_bytes < 2 || sample_bytes > 4)) return DBT_INVALID;
    portENTER_CRITICAL(&control_lock);
    control = (control_t){start, rate, channels, sample_bytes};
    atomic_fetch_add_explicit(&gen, 1, memory_order_release);
    portEXIT_CRITICAL(&control_lock);
    xTaskNotifyGive(writer_task_handle);
    return DBT_OK;
}

dbt_result_t audio_out_pcm(const void *samples, uint16_t frames, uint32_t rate, uint8_t channels,
                           uint8_t sample_bytes) {
    if (!ring || !samples || !frames) return DBT_INVALID;
    if (acked() && (!applied_start || rate != applied_rate || channels != applied_channels ||
                    sample_bytes != applied_sample_bytes))
        return DBT_INVALID; /* PCM outside a started stream or with another format */
    if (!room_for(frames)) {
        atomic_store_explicit(&producer_blocked, true, memory_order_seq_cst);
        if (!room_for(frames)) return DBT_BUSY; /* writer wakes us when it frees space */
        atomic_store_explicit(&producer_blocked, false, memory_order_relaxed);
    }
    uint32_t w = atomic_load_explicit(&wpos, memory_order_relaxed);
    uint32_t at = (w % applied_capacity) * applied_frame_bytes;
    uint32_t bytes = (uint32_t)frames * applied_frame_bytes;
    uint32_t first = applied_capacity * applied_frame_bytes - at;
    if (first > bytes) first = bytes;
    memcpy(ring + at, samples, first);
    if (bytes > first) memcpy(ring, (const uint8_t *)samples + first, bytes - first);
    atomic_store_explicit(&wpos, w + frames, memory_order_release);
    if (atomic_load_explicit(&writer_waiting, memory_order_acquire)) xTaskNotifyGive(writer_task_handle);
    return DBT_OK;
}

/* ------------------------------------------------------------------ volume */
void audio_out_set_volume(uint8_t volume) {
    if (volume > 127) volume = 127;
    int32_t gain;
    if (volume == 127) gain = 65536;
    else if (volume == 0) gain = 0;
    else {
        float db = -VOLUME_RANGE_DB * (float)(127u - volume) / 126.0f;
        gain = (int32_t)lroundf(65536.0f * powf(10.0f, db / 20.0f));
    }
    atomic_store_explicit(&gain_q16, gain, memory_order_relaxed);
    atomic_store_explicit(&volume_now, volume, memory_order_relaxed);
}

/* --------------------------------------------------------------------- I2S */
static void channel_stop(void) {
    if (channel && channel_enabled) (void)i2s_channel_disable(channel);
    channel_enabled = false;
}
static void channel_delete(void) {
    channel_stop();
    if (channel) (void)i2s_del_channel(channel);
    channel = NULL;
    channel_rate = 0;
}
static esp_err_t channel_create(uint32_t rate, bool apll) {
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_1, I2S_ROLE_MASTER);
    chan_cfg.dma_desc_num = DMA_DESCRIPTORS;
    chan_cfg.dma_frame_num = DMA_FRAMES;
    chan_cfg.auto_clear_after_cb = true; /* underrun plays zeros, never stale audio */
    esp_err_t err = i2s_new_channel(&chan_cfg, &channel, NULL);
    if (err != ESP_OK) {
        channel = NULL;
        return err;
    }
    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(rate),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {.mclk = I2S_GPIO_UNUSED,
                     .bclk = AUDIO_OUT_GPIO_BCLK,
                     .ws = AUDIO_OUT_GPIO_WS,
                     .dout = AUDIO_OUT_GPIO_DATA,
                     .din = I2S_GPIO_UNUSED,
                     .invert_flags = {.mclk_inv = false, .bclk_inv = false, .ws_inv = false}},
    };
#if SOC_I2S_SUPPORTS_APLL
    if (apll) std_cfg.clk_cfg.clk_src = I2S_CLK_SRC_APLL;
#else
    (void)apll;
#endif
    err = i2s_channel_init_std_mode(channel, &std_cfg);
    if (err != ESP_OK) {
        (void)i2s_del_channel(channel);
        channel = NULL;
        return err;
    }
    channel_rate = rate;
    channel_enabled = false;
    return ESP_OK;
}
static esp_err_t channel_prepare(uint32_t rate) {
    if (channel && channel_rate == rate) return ESP_OK;
    channel_delete(); /* sample rate changed: re-create the channel */
    esp_err_t err = channel_create(rate, true);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "I2S with APLL at %u Hz failed (%s), using the default clock", (unsigned)rate, esp_err_to_name(err));
        err = channel_create(rate, false);
    }
    return err;
}

/* ------------------------------------------------------------------ writer */
static void apply_control(void) {
    control_t c;
    uint32_t g;
    portENTER_CRITICAL(&control_lock);
    c = control;
    g = atomic_load_explicit(&gen, memory_order_acquire);
    portEXIT_CRITICAL(&control_lock);
    /* The producer is gated (ack != gen), so the ring can be flushed here. */
    if (c.start) {
        atomic_store_explicit(&rpos, atomic_load_explicit(&wpos, memory_order_acquire), memory_order_release);
        in_channels = c.channels;
        in_sample_bytes = c.sample_bytes;
        frame_bytes = (uint32_t)c.channels * c.sample_bytes;
        capacity = ring_bytes / frame_bytes;
        /* AUDIO_OUT_PREFILL_MS is the total buffered audio (ring + DMA) when
         * playback starts; afterwards the DMA queue holds DMA_FRAMES_TOTAL of it. */
        start = (uint32_t)((uint64_t)c.rate * AUDIO_OUT_PREFILL_MS / 1000u);
        if (start > capacity * 85u / 100u) start = capacity * 85u / 100u;
        target = start > DMA_FRAMES_TOTAL + start / 3u ? start - DMA_FRAMES_TOTAL : start / 3u;
        band = c.rate * DRIFT_BAND_MS / 1000u;
        if (band > target / 2u) band = target / 2u;
        level_average_q8 = (int32_t)(target << 8);
        chunks_since_slip = 0;
        esp_err_t err = channel_prepare(c.rate);
        applied_start = err == ESP_OK;
        applied_rate = c.rate;
        applied_channels = c.channels;
        applied_sample_bytes = c.sample_bytes;
        applied_frame_bytes = frame_bytes;
        applied_capacity = capacity;
        atomic_store_explicit(&stat_target, target, memory_order_relaxed);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "I2S setup for %u Hz failed: %s", (unsigned)c.rate, esp_err_to_name(err));
            state = OUT_IDLE;
        } else {
            state = OUT_PREFILL;
            prefill_began_us = esp_timer_get_time();
            ESP_LOGI(TAG, "stream start: %u Hz %s %s -> I2S 32-bit stereo, prefill %u ms, ring %u ms",
                     (unsigned)c.rate, c.sample_bytes == 2 ? "S16" : c.sample_bytes == 3 ? "S24" : "S32",
                     c.channels == 1 ? "mono" : "stereo", (unsigned)(start * 1000u / c.rate),
                     (unsigned)((uint64_t)capacity * 1000u / c.rate));
        }
    } else {
        applied_start = false;
        if (state == OUT_PLAY) {
            state = OUT_DRAIN; /* play out what is buffered, then stop the clocks */
        } else if (state != OUT_IDLE) {
            atomic_store_explicit(&rpos, atomic_load_explicit(&wpos, memory_order_acquire), memory_order_release);
            channel_stop();
            state = OUT_IDLE;
            ESP_LOGI(TAG, "stream stop");
        }
    }
    atomic_store_explicit(&stat_playing, state == OUT_PLAY, memory_order_relaxed);
    atomic_store_explicit(&ack, g, memory_order_release);
    notify_producer();
}

static void wait_for_work(TickType_t ticks) {
    atomic_store_explicit(&writer_waiting, true, memory_order_seq_cst);
    (void)ulTaskNotifyTake(pdTRUE, ticks);
    atomic_store_explicit(&writer_waiting, false, memory_order_relaxed);
}

/* Convert n frames starting at ring frame index 'from' into 32-bit stereo. */
static void convert(uint32_t from, uint32_t n, int32_t *out, int32_t gain) {
    const bool unity = gain >= 65536;
    while (n) {
        uint32_t index = from % capacity;
        uint32_t run = capacity - index;
        if (run > n) run = n;
        const uint8_t *src = ring + index * frame_bytes;
        for (uint32_t i = 0; i < run; ++i) {
            int32_t left, right;
            if (in_sample_bytes == 2) {
                const int16_t *s = (const int16_t *)(const void *)src;
                int32_t a = s[0], b = in_channels == 2 ? s[1] : s[0];
                left = unity ? a * 65536 : a * gain; /* |a*gain| <= 2^31 */
                right = unity ? b * 65536 : b * gain;
            } else {
                int32_t a, b;
                if (in_sample_bytes == 4) {
                    const int32_t *s = (const int32_t *)(const void *)src;
                    a = s[0];
                    b = in_channels == 2 ? s[1] : s[0];
                } else { /* packed S24 little endian */
                    a = (int32_t)((uint32_t)src[0] << 8 | (uint32_t)src[1] << 16 | (uint32_t)src[2] << 24);
                    b = in_channels == 2 ? (int32_t)((uint32_t)src[3] << 8 | (uint32_t)src[4] << 16 | (uint32_t)src[5] << 24) : a;
                }
                left = unity ? a : (int32_t)(((int64_t)a * gain) >> 16);
                right = unity ? b : (int32_t)(((int64_t)b * gain) >> 16);
            }
            *out++ = left;
            *out++ = right;
            src += frame_bytes;
        }
        from += run;
        n -= run;
    }
}

static void writer(void *unused) {
    (void)unused;
    for (;;) {
        if (atomic_load_explicit(&gen, memory_order_acquire) != atomic_load_explicit(&ack, memory_order_relaxed)) {
            apply_control();
            continue;
        }
        if (state == OUT_IDLE) {
            wait_for_work(portMAX_DELAY);
            continue;
        }
        uint32_t r = atomic_load_explicit(&rpos, memory_order_relaxed);
        uint32_t level = atomic_load_explicit(&wpos, memory_order_acquire) - r;
        if (state == OUT_PREFILL) {
            if (level < start) {
                wait_for_work(pdMS_TO_TICKS(10));
                continue;
            }
            if (!channel_enabled) {
                esp_err_t err = i2s_channel_enable(channel);
                if (err != ESP_OK) {
                    /* Producer will see BUSY, time out and the stream is retired. */
                    ESP_LOGE(TAG, "i2s_channel_enable: %s", esp_err_to_name(err));
                    state = OUT_IDLE;
                    continue;
                }
                channel_enabled = true;
            }
            state = OUT_PLAY;
            atomic_store_explicit(&stat_playing, true, memory_order_relaxed);
            ESP_LOGI(TAG, "playing (buffered %u ms in %u ms)", (unsigned)(level * 1000u / applied_rate),
                     (unsigned)((esp_timer_get_time() - prefill_began_us) / 1000));
        }
        if (level == 0) {
            if (state == OUT_DRAIN) {
                channel_stop();
                state = OUT_IDLE;
                atomic_store_explicit(&stat_playing, false, memory_order_relaxed);
                ESP_LOGI(TAG, "stream stop (drained)");
                continue;
            }
            /* Underrun: the DMA now plays zeros (auto clear); refill to target. */
            uint32_t count = atomic_fetch_add_explicit(&stat_underruns, 1, memory_order_relaxed) + 1u;
            int64_t now = esp_timer_get_time();
            if (now - last_underrun_log_us > 2000000) {
                last_underrun_log_us = now;
                ESP_LOGW(TAG, "underrun #%u, re-buffering %u ms", (unsigned)count, (unsigned)(start * 1000u / applied_rate));
            }
            state = OUT_PREFILL;
            prefill_began_us = now;
            level_average_q8 = (int32_t)(target << 8);
            atomic_store_explicit(&stat_playing, false, memory_order_relaxed);
            continue;
        }
        uint32_t n = level < CHUNK_FRAMES ? level : CHUNK_FRAMES;
        int slip = 0;
        if (state == OUT_PLAY) {
            level_average_q8 += ((int32_t)(level << 8) - level_average_q8) >> DRIFT_SHIFT;
            if (++chunks_since_slip >= DRIFT_SPACING) {
                if (level_average_q8 > (int32_t)((target + band) << 8) && level > n) slip = -1;
                else if (level_average_q8 < (int32_t)((target - band) << 8)) slip = 1;
                if (slip) chunks_since_slip = 0;
            }
        }
        convert(r, n, out_buffer, atomic_load_explicit(&gain_q16, memory_order_relaxed));
        uint32_t out_frames = n;
        if (slip > 0) { /* repeat the last frame: consume less than we play */
            out_buffer[2u * n] = out_buffer[2u * n - 2u];
            out_buffer[2u * n + 1u] = out_buffer[2u * n - 1u];
            ++out_frames;
            atomic_fetch_add_explicit(&stat_repeated, 1, memory_order_relaxed);
        } else if (slip < 0) {
            atomic_fetch_add_explicit(&stat_dropped, 1, memory_order_relaxed);
        }
        atomic_store_explicit(&rpos, r + n + (slip < 0 ? 1u : 0u), memory_order_release);
        notify_producer();
        size_t written = 0;
        esp_err_t err = i2s_channel_write(channel, out_buffer, out_frames * 8u, &written, pdMS_TO_TICKS(200));
        if (err != ESP_OK) {
            if (atomic_fetch_add_explicit(&stat_write_errors, 1, memory_order_relaxed) == 0)
                ESP_LOGE(TAG, "i2s_channel_write: %s", esp_err_to_name(err));
            vTaskDelay(1); /* never spin above the host if the driver keeps failing */
        }
    }
}

esp_err_t audio_out_init(void) {
    if (ring) return ESP_OK;
    for (uint32_t size = RING_MAX_BYTES; size >= RING_MIN_BYTES; size -= 8u * 1024u) {
        ring = heap_caps_malloc(size, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        if (ring) {
            ring_bytes = size;
            break;
        }
    }
    if (!ring) return ESP_ERR_NO_MEM;
    audio_out_set_volume(127);
    if (xTaskCreatePinnedToCore(writer, "i2s_out", WRITER_STACK, NULL, WRITER_PRIORITY, &writer_task_handle,
                                WRITER_CORE) != pdPASS) {
        heap_caps_free(ring);
        ring = NULL;
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "PCM ring %u KB, I2S BCLK=%d WS=%d DATA=%d, DMA %ux%u frames", (unsigned)(ring_bytes / 1024u),
             AUDIO_OUT_GPIO_BCLK, AUDIO_OUT_GPIO_WS, AUDIO_OUT_GPIO_DATA, (unsigned)DMA_DESCRIPTORS, (unsigned)DMA_FRAMES);
    return ESP_OK;
}

void audio_out_set_producer(TaskHandle_t task) { producer_task = task; }
TaskHandle_t audio_out_task(void) { return writer_task_handle; }

uint16_t audio_out_delay_100us(uint32_t rate) {
    if (!rate) rate = 48000u;
    (void)rate; /* ring set-point + DMA queue = the startup prefill */
    return (uint16_t)(AUDIO_OUT_PREFILL_MS * 10u);
}

void audio_out_stats(audio_out_stats_t *s) {
    if (!s) return;
    memset(s, 0, sizeof(*s));
    uint32_t r = atomic_load_explicit(&rpos, memory_order_acquire);
    s->level_frames = atomic_load_explicit(&wpos, memory_order_acquire) - r;
    s->rate = applied_rate;
    s->channels = applied_channels;
    s->sample_bytes = applied_sample_bytes;
    s->capacity_frames = applied_capacity;
    s->target_frames = atomic_load_explicit(&stat_target, memory_order_relaxed);
    s->underruns = atomic_load_explicit(&stat_underruns, memory_order_relaxed);
    s->dropped_frames = atomic_load_explicit(&stat_dropped, memory_order_relaxed);
    s->repeated_frames = atomic_load_explicit(&stat_repeated, memory_order_relaxed);
    s->write_errors = atomic_load_explicit(&stat_write_errors, memory_order_relaxed);
    s->volume = (uint8_t)atomic_load_explicit(&volume_now, memory_order_relaxed);
    s->playing = atomic_load_explicit(&stat_playing, memory_order_relaxed);
}
