/* SDP records as published by esp32_probe/main/probe_sdp.c for the audio
 * build, minus the private diagnostic "echo" record:
 *   0x00000000 SDP server, 0x00010001 A2DP Audio Sink (AVDTP 1.3, speaker),
 *   0x00010002 AVRCP controller (1.4, category 1), 0x00010003 AVRCP target
 *   (1.4, category 2: absolute volume). */
#include "bt_sdp.h"

static dbt_sdp_t server;
static dbt_sdp_bearer_t bearer;
static bool initialized;

static const uint8_t server_class[] = {0x35, 3, 0x19, 0x10, 0};
static const uint8_t versions[] = {0x35, 3, 9, 1, 0};
static const uint8_t state[] = {10, 0, 0, 0, 4};
static const dbt_sdp_attribute_t server_attributes[] = {
    {1, sizeof(server_class), server_class}, {0x200, sizeof(versions), versions}, {0x201, sizeof(state), state}};

static const uint8_t browse[] = {0x35, 3, 0x19, 0x10, 2};
static const uint8_t language[] = {0x35, 9, 9, 0x65, 0x6e, 9, 0, 0x6a, 9, 1, 0};

static const uint8_t sink_class[] = {0x35, 3, 0x19, 0x11, 0x0b};
static const uint8_t sink_protocol[] = {0x35, 16, 0x35, 6, 0x19, 1, 0, 9, 0, 0x19, 0x35, 6, 0x19, 0, 0x19, 9, 1, 3};
static const uint8_t sink_profile[] = {0x35, 8, 0x35, 6, 0x19, 0x11, 0x0d, 9, 1, 3};
static const uint8_t sink_features[] = {9, 0, 2}; /* speaker */
static const uint8_t sink_name[] = "\x25\x0b"
                                   "BDK SPEAKER";
static const dbt_sdp_attribute_t sink_attributes[] = {
    {1, sizeof(sink_class), sink_class},       {4, sizeof(sink_protocol), sink_protocol},
    {5, sizeof(browse), browse},               {6, sizeof(language), language},
    {9, sizeof(sink_profile), sink_profile},   {0x100, sizeof(sink_name) - 1, sink_name},
    {0x311, sizeof(sink_features), sink_features}};

static const uint8_t ct_class[] = {0x35, 6, 0x19, 0x11, 0x0e, 0x19, 0x11, 0x0f};
static const uint8_t tg_class[] = {0x35, 3, 0x19, 0x11, 0x0c};
static const uint8_t avrcp_protocol[] = {0x35, 16, 0x35, 6, 0x19, 1, 0, 9, 0, 0x17, 0x35, 6, 0x19, 0, 0x17, 9, 1, 4};
static const uint8_t avrcp_profile[] = {0x35, 8, 0x35, 6, 0x19, 0x11, 0x0e, 9, 1, 4};
static const uint8_t ct_features[] = {9, 0, 1};
static const uint8_t tg_features[] = {9, 0, 2};
static const dbt_sdp_attribute_t ct_attributes[] = {
    {1, sizeof(ct_class), ct_class}, {4, sizeof(avrcp_protocol), avrcp_protocol}, {5, sizeof(browse), browse},
    {9, sizeof(avrcp_profile), avrcp_profile}, {0x311, sizeof(ct_features), ct_features}};
static const dbt_sdp_attribute_t tg_attributes[] = {
    {1, sizeof(tg_class), tg_class}, {4, sizeof(avrcp_protocol), avrcp_protocol}, {5, sizeof(browse), browse},
    {9, sizeof(avrcp_profile), avrcp_profile}, {0x311, sizeof(tg_features), tg_features}};

#define COUNT(a) ((uint16_t)(sizeof(a) / sizeof((a)[0])))
static const dbt_sdp_record_t records[] = {
    {0, server_attributes, COUNT(server_attributes)},
    {0x10001, sink_attributes, COUNT(sink_attributes)},
    {0x10002, ct_attributes, COUNT(ct_attributes)},
    {0x10003, tg_attributes, COUNT(tg_attributes)},
};

dbt_result_t bt_sdp_init(dbt_runtime_t *runtime) {
    if (initialized) return DBT_INVALID;
    dbt_result_t result = dbt_sdp_init(&server, records, COUNT(records), 4);
    if (result == DBT_OK) result = dbt_sdp_bearer_init(&bearer, runtime, &server, 672);
    initialized = result == DBT_OK;
    return result;
}
dbt_result_t bt_sdp_receive(const dbt_channel_t *channel, const uint8_t *data, uint16_t size) {
    if (!initialized) return DBT_INVALID;
    dbt_result_t result = dbt_sdp_bearer_receive(&bearer, channel, data, size);
    if (result != DBT_OK && channel && channel->psm == 1) {
        /* Close the offending channel from the owner poll, after this callback. */
        dbt_result_t close = dbt_channels_request_close(&bearer.runtime->channels, channel);
        if (close == DBT_OK || close == DBT_EMPTY) return DBT_OK;
    }
    return result;
}
void bt_sdp_event(dbt_channel_event_t event, const dbt_channel_t *channel) {
    if (initialized) dbt_sdp_bearer_event(&bearer, event, channel);
}
dbt_result_t bt_sdp_poll(void) { return initialized ? dbt_sdp_bearer_poll(&bearer) : DBT_OK; }
uint32_t bt_sdp_wait_ms(void) { return initialized ? dbt_sdp_bearer_wait_ms(&bearer) : UINT32_MAX; }
