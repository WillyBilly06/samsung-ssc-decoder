#ifndef PURE_SINK_AUDIO_OUT_H
#define PURE_SINK_AUDIO_OUT_H
/* Pure playback output: one PCM ring buffer and one I2S writer task.
 * Producer = decoder task (codec worker hooks), consumer = I2S writer task. */
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "dbt_queue.h" /* dbt_result_t */

/* PCM5102A wiring (no MCLK). */
#define AUDIO_OUT_GPIO_BCLK 27
#define AUDIO_OUT_GPIO_WS 25
#define AUDIO_OUT_GPIO_DATA 26

/* Audio buffered (PCM ring + I2S DMA) when playback starts or resumes after
 * an underrun; it is also the output latency reported to the phone. */
#define AUDIO_OUT_PREFILL_MS 60u

typedef struct {
    uint32_t rate, level_frames, capacity_frames, target_frames;
    uint32_t underruns, dropped_frames, repeated_frames, write_errors;
    uint8_t channels, sample_bytes, volume;
    bool playing;
} audio_out_stats_t;

/* Startup, before the Bluetooth controller: allocates the PCM ring (largest
 * block that fits from 64 KB down) and starts the I2S writer pinned to core 1. */
esp_err_t audio_out_init(void);
/* The task woken when ring space frees up after audio_out_pcm returned BUSY. */
void audio_out_set_producer(TaskHandle_t task);
/* Decoder task only (codec worker stream hook). Nonblocking. */
dbt_result_t audio_out_stream(bool start, uint32_t rate, uint8_t channels, uint8_t sample_bytes);
/* Decoder task only (codec worker pcm hook): OK = all frames copied,
 * BUSY = nothing copied, retry after the writer wakes the producer. */
dbt_result_t audio_out_pcm(const void *samples, uint16_t frames, uint32_t rate, uint8_t channels, uint8_t sample_bytes);
/* AVRCP absolute volume 0..127 (any task). */
void audio_out_set_volume(uint8_t volume);
/* Approximate output latency (ring target + DMA) for AVDTP delay reports. */
uint16_t audio_out_delay_100us(uint32_t rate);
void audio_out_stats(audio_out_stats_t *stats);
TaskHandle_t audio_out_task(void);
#endif
