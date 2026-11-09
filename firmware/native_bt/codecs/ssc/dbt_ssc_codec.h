#ifndef DBT_SSC_CODEC_H
#define DBT_SSC_CODEC_H
#include "dbt_codec.h"

/* Samsung Scalable Codec: A2DP vendor codec 0x00000075 / 0x0103. One RTP packet carries exactly one
 * frame (4-byte 0xFF 0xEx header + range-coded payload) that decodes to 864 stereo samples. */
#define DBT_SSC_FRAME_SAMPLES 864u
#define DBT_SSC_FRAME_MAX 2304u /* largest frame (886 kbps) is 2172 bytes */

dbt_result_t dbt_ssc_configure(const uint8_t *cie, uint16_t size, dbt_codec_config_t *config);
uint8_t dbt_a2dp_ssc_validate(const dbt_avdtp_endpoint_t *endpoint, const dbt_avdtp_cap_t *codec);
/* PCM samples per channel in this packet, zero when the packet is not a valid SSC frame. */
uint32_t dbt_ssc_packet_frames(const dbt_codec_config_t *config, const dbt_avdtp_media_t *packet);
extern const dbt_codec_ops_t dbt_ssc_codec;
#endif
