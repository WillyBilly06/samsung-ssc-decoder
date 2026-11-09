/* SSC decoder benchmark on the ESP32: decodes real frames produced by Samsung's encoder and reports CPU time
 * per decoder stage. Frames are embedded in flash in the harness format [u16 LE length][frame]. */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "esp_cpu.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "ssc_decoder.h"

#define NSTAGES 8
static const char *const stage_names[NSTAGES] = {"start",   "coarse", "alloc",  "fine+pvq",
                                                 "final+ac", "denorm", "imdct",  "postfilt"};
static uint64_t stage_cycles[NSTAGES];
static uint32_t last_cycle;
static int last_stage;

void ssc_prof_mark(int stage) {
    uint32_t now = esp_cpu_get_cycle_count();
    if (stage != 0) {
        /* denorm/imdct/postfilter repeat per channel: stage 5 follows 7 for the second channel */
        stage_cycles[stage] += now - last_cycle;
    }
    last_cycle = now;
    last_stage = stage;
}

static void *bench_alloc(void *ctx, unsigned size) {
    (void)ctx;
    return heap_caps_malloc(size, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
}
static void bench_free(void *ctx, void *p) {
    (void)ctx;
    heap_caps_free(p);
}

static int32_t pcm32[2 * SSC_FRAME_SAMPLES];
static int16_t pcm16[2 * SSC_FRAME_SAMPLES];

static void run(const char *name, const uint8_t *blob, const uint8_t *blob_end, int rate, int s32, int max_frames) {
    ssc_decoder *dec = ssc_decoder_create_with_allocator(rate, bench_alloc, bench_free, NULL);
    if (!dec) {
        printf("%s: create failed\n", name);
        return;
    }
    memset(stage_cycles, 0, sizeof stage_cycles);
    const uint8_t *p = blob;
    int frames = 0, errors = 0, bytes = 0;
    uint32_t worst = 0;
    uint64_t total = 0;
    uint32_t hash = 2166136261u;
    while (p + 2 <= blob_end && frames < max_frames) {
        int len = p[0] | p[1] << 8;
        const uint8_t *f = p + 2;
        if (f + len > blob_end) break;
        uint32_t t0 = esp_cpu_get_cycle_count();
        int r = s32 ? ssc_decode_s32(dec, f, len, pcm32) : ssc_decode(dec, f, len, pcm16);
        uint32_t dt = esp_cpu_get_cycle_count() - t0;
        if (r != SSC_FRAME_SAMPLES) errors++;
        total += dt;
        if (dt > worst) worst = dt;
        const uint8_t *o = s32 ? (const uint8_t *)pcm32 : (const uint8_t *)pcm16;
        int ob = s32 ? (int)sizeof pcm32 : (int)sizeof pcm16;
        for (int i = 0; i < ob; i++) hash = (hash ^ o[i]) * 16777619u;
        bytes += len;
        frames++;
        p = f + len;
    }
    const double mhz = 240.0;
    double frame_ms = SSC_FRAME_SAMPLES * 1000.0 / (rate == 96000 ? 96000 : (rate == 48000 ? 48000 : 44100));
    double avg_ms = frames ? total / (double)frames / mhz / 1000.0 : 0;
    printf("\n== %s: %d frames (%d B avg), %d errors, hash %08lx\n", name, frames, frames ? bytes / frames : 0,
           errors, (unsigned long)hash);
    printf("   avg %.2f ms, worst %.2f ms per frame; frame period %.2f ms -> %.0f%% of one core\n", avg_ms,
           worst / mhz / 1000.0, frame_ms, 100.0 * avg_ms / frame_ms);
    for (int s = 1; s < NSTAGES; s++)
        printf("   %-9s %6.3f ms\n", stage_names[s], frames ? stage_cycles[s] / (double)frames / mhz / 1000.0 : 0);
    ssc_decoder_destroy(dec);
}

extern const uint8_t f44_start[] asm("_binary_f44_192k_ssc_start");
extern const uint8_t f44_end[] asm("_binary_f44_192k_ssc_end");
extern const uint8_t hifi_start[] asm("_binary_hifi48_192k_ssc_start");
extern const uint8_t hifi_end[] asm("_binary_hifi48_192k_ssc_end");
extern const uint8_t uhq_start[] asm("_binary_uhq96_584k_ssc_start");
extern const uint8_t uhq_end[] asm("_binary_uhq96_584k_ssc_end");

void app_main(void) {
    vTaskDelay(pdMS_TO_TICKS(500));
    printf("\nSSC_BENCH start, free internal heap %u, decoder state %d bytes\n",
           (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL), ssc_decoder_size());
    run("44.1k 192kbps 16-bit", f44_start, f44_end, 44100, 0, 150);
    run("HiFi 48k 192kbps 24-bit", hifi_start, hifi_end, 48000, 1, 150);
    run("UHQ 96k 584kbps 24-bit", uhq_start, uhq_end, 96000, 1, 150);
    printf("\nSSC_BENCH done\n");
    for (;;) vTaskDelay(pdMS_TO_TICKS(1000));
}
