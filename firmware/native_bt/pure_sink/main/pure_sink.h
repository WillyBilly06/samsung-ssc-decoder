#ifndef PURE_SINK_H
#define PURE_SINK_H
/* Task layout. Controller and host share core 1 (as in esp32_probe); codec
 * decoding owns core 0; the light I2S writer sits on core 1 above the host. */
#define PURE_SINK_HOST_PRIORITY 20
#define PURE_SINK_DECODER_PRIORITY 19
#define PURE_SINK_DECODER_CORE 0
#define PURE_SINK_DECODER_STACK 8192u
#define PURE_SINK_BACKGROUND_CORE 1

/* "heap[why]: free/min/largest" for MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT. */
void pure_sink_heap_log(const char *why);
#endif
