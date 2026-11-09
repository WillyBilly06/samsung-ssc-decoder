#ifndef PURE_SINK_A2DP_SINK_H
#define PURE_SINK_A2DP_SINK_H
/* A2DP sink: AVDTP acceptor on PSM 0x19, all codecs, codec worker and the
 * decoder task. Everything except the decoder task runs on the host owner. */
#include "dbt_a2dp_acceptor.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

/* Owner only, after runtime.ready. Starts the decoder task (core 0). */
dbt_result_t a2dp_sink_init(dbt_runtime_t *runtime, TaskHandle_t owner);
bool a2dp_sink_accept(const dbt_link_t *link, uint16_t psm);
void a2dp_sink_event(dbt_channel_event_t event, const dbt_channel_t *channel, dbt_channel_reason_t reason,
                     uint16_t result);
void a2dp_sink_receive(const dbt_channel_t *channel, const uint8_t *data, uint16_t size);
void a2dp_sink_poll(uint32_t now_ms);
uint32_t a2dp_sink_wait_ms(uint32_t now_ms);
/* HCI receive-credit policy: never grant more ACL credits than the encoded
 * media queue can absorb while streaming. */
uint16_t a2dp_sink_rx_credit_window(void);
uint16_t a2dp_sink_rx_credit_limit(void *context, const dbt_link_t *link, uint16_t pending, uint16_t window);
bool a2dp_sink_connected(void);
bool a2dp_sink_streaming(void);
uint32_t a2dp_sink_codec_bytes(void);
const char *a2dp_sink_codec_label(void);
TaskHandle_t a2dp_sink_decoder_task(void);
#endif
