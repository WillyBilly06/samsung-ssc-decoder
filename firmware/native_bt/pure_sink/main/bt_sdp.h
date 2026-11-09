#ifndef PURE_SINK_BT_SDP_H
#define PURE_SINK_BT_SDP_H
/* SDP server (PSM 1): A2DP Sink + AVRCP controller/target records only. */
#include "dbt_sdp_bearer.h"

dbt_result_t bt_sdp_init(dbt_runtime_t *runtime);
dbt_result_t bt_sdp_receive(const dbt_channel_t *channel, const uint8_t *data, uint16_t size);
void bt_sdp_event(dbt_channel_event_t event, const dbt_channel_t *channel);
dbt_result_t bt_sdp_poll(void);
uint32_t bt_sdp_wait_ms(void);
#endif
