/* SSC playback example for ESP32 + PCM5102A.
 *
 * Decodes SSC streams embedded in flash and plays them through I2S, one after another, forever:
 *   1. SSC 44.1 kHz, 192 kbps          (ssc_decoder_create(44100))
 *   2. Seamless HiFi 48 kHz / 24-bit   (ssc_decoder_create(48000), 32-bit output)
 *   3. UHQ v2 96 kHz, 584 kbps         (ssc_decoder_create(96000))
 *
 * The embedded files use the test-vector format: [u16 little-endian length][SSC frame] ...
 * In a real product the frames come from A2DP media packets instead (see docs/integration.md).
 *
 * Wiring (PCM5102A): BCK -> GPIO 27, LRCK -> GPIO 25, DIN -> GPIO 26, SCK -> GND, VIN -> 3V3. */
#include <inttypes.h>
#include <stdint.h>
#include <string.h>

#include "driver/i2s_std.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "ssc_decoder.h"

#define PIN_BCLK 27
#define PIN_WS 25
#define PIN_DOUT 26
#define VOLUME_SHIFT 2 /* play 12 dB below full scale; set to 0 for full level */

static const char *TAG = "ssc_playback";

extern const uint8_t mix_192000_ssc_start[] asm("_binary_mix_192000_ssc_start");
extern const uint8_t mix_192000_ssc_end[] asm("_binary_mix_192000_ssc_end");
extern const uint8_t hifi48_192000_ssc_start[] asm("_binary_hifi48_192000_ssc_start");
extern const uint8_t hifi48_192000_ssc_end[] asm("_binary_hifi48_192000_ssc_end");
extern const uint8_t uhq96_584000_ssc_start[] asm("_binary_uhq96_584000_ssc_start");
extern const uint8_t uhq96_584000_ssc_end[] asm("_binary_uhq96_584000_ssc_end");

typedef struct {
    const char *name;
    const uint8_t *start, *end;
    int rate; /* playback rate; also selects the decoder mode (44100/48000 share one, 96000 has its own) */
} clip_t;

static i2s_chan_handle_t tx;
/* One decoded frame, interleaved stereo, 32-bit: 864 * 2 * 4 = 6912 bytes. */
static int32_t pcm[2 * SSC_FRAME_SAMPLES];

static void i2s_start(int rate) {
    if (!tx) {
        i2s_chan_config_t chan = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
        chan.dma_desc_num = 6;
        chan.dma_frame_num = 288; /* 6 x 288 = 2 SSC frames of buffering */
        chan.auto_clear = true;
        ESP_ERROR_CHECK(i2s_new_channel(&chan, &tx, NULL));
        i2s_std_config_t std = {
            .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(rate),
            .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_STEREO),
            .gpio_cfg = {.mclk = I2S_GPIO_UNUSED, .bclk = PIN_BCLK, .ws = PIN_WS, .dout = PIN_DOUT, .din = I2S_GPIO_UNUSED},
        };
#if SOC_I2S_SUPPORTS_APLL
        std.clk_cfg.clk_src = I2S_CLK_SRC_APLL; /* accurate 44.1/48/96 kHz clocks */
#endif
        ESP_ERROR_CHECK(i2s_channel_init_std_mode(tx, &std));
    } else {
        ESP_ERROR_CHECK(i2s_channel_disable(tx));
        i2s_std_clk_config_t clk = I2S_STD_CLK_DEFAULT_CONFIG(rate);
#if SOC_I2S_SUPPORTS_APLL
        clk.clk_src = I2S_CLK_SRC_APLL;
#endif
        ESP_ERROR_CHECK(i2s_channel_reconfig_std_clock(tx, &clk));
    }
    ESP_ERROR_CHECK(i2s_channel_enable(tx));
}

static void play(const clip_t *c) {
    ssc_decoder *dec = ssc_decoder_create(c->rate);
    if (!dec) {
        ESP_LOGE(TAG, "%s: decoder allocation failed", c->name);
        return;
    }
    ESP_LOGI(TAG, "%s: %d Hz, decoder uses %d bytes, free heap %u", c->name, c->rate, ssc_decoder_size(),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    i2s_start(c->rate);

    const uint8_t *p = c->start;
    unsigned frames = 0, errors = 0;
    int64_t busy_us = 0, worst_us = 0;
    while (p + 2 <= c->end) {
        int len = p[0] | p[1] << 8;
        p += 2;
        if (len <= 0 || p + len > c->end) break;

        if (frames == 0) {
            ssc_frame_info info;
            if (ssc_parse_frame_info(p, len, &info) == 0)
                ESP_LOGI(TAG, "%s: %d kbps, %d coded bands, %d-byte frames", c->name, info.bitrate / 1000,
                         info.end_band, ssc_frame_bytes(p, len, c->rate));
        }
        int64_t t0 = esp_timer_get_time();
        int n = ssc_decode_s32(dec, p, len, pcm); /* 16-bit full scale = 1 << 31; keeps 24-bit detail */
        int64_t dt = esp_timer_get_time() - t0;
        busy_us += dt;
        if (dt > worst_us) worst_us = dt;
        if (n != SSC_FRAME_SAMPLES) ++errors; /* pcm is silence on error */
        p += len;
        ++frames;

        for (int i = 0; i < 2 * SSC_FRAME_SAMPLES; ++i) pcm[i] >>= VOLUME_SHIFT;
        size_t written;
        i2s_channel_write(tx, pcm, sizeof(pcm), &written, portMAX_DELAY); /* blocks: paces decoding to real time */
    }

    int64_t frame_us = (int64_t)SSC_FRAME_SAMPLES * 1000000 / c->rate;
    ESP_LOGI(TAG, "%s: %u frames, %u errors, decode avg %" PRId64 " us / worst %" PRId64 " us per %" PRId64
                  " us frame (%" PRId64 "%% of one core)",
             c->name, frames, errors, frames ? busy_us / frames : 0, worst_us, frame_us,
             frames ? busy_us * 100 / ((int64_t)frames * frame_us) : 0);
    ssc_decoder_destroy(dec);
}

void app_main(void) {
    const clip_t clips[] = {
        {"SSC 44.1 kHz", mix_192000_ssc_start, mix_192000_ssc_end, 44100},
        {"HiFi 48 kHz/24-bit", hifi48_192000_ssc_start, hifi48_192000_ssc_end, 48000},
        {"UHQ 96 kHz", uhq96_584000_ssc_start, uhq96_584000_ssc_end, 96000},
    };
    for (;;) {
        for (unsigned i = 0; i < sizeof(clips) / sizeof(clips[0]); ++i) play(&clips[i]);
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
