#ifndef PURE_SINK_BT_SECURITY_H
#define PURE_SINK_BT_SECURITY_H
/* Classic SSP "just works" + durable link keys in NVS.
 * NVS format is identical to esp32_probe/main/probe_security.c (namespace
 * "native_bonds", key "br<12 hex BD_ADDR>", 28-byte v1 blob), so bonds made
 * by the existing firmware keep working. All calls run on the host owner. */
#include "dbt_classic_security.h"
#include "dbt_gap.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

/* After nvs_flash_init, before the controller is enabled. */
dbt_result_t bt_security_init(dbt_runtime_t *runtime, dbt_gap_t *gap, TaskHandle_t owner);
/* GAP accept_classic hook: every ACL is admitted (SDP is public); pairing and
 * encrypted A2DP/AVRCP access are enforced separately. */
bool bt_security_accept(void *context, const uint8_t address[6], const uint8_t device_class[3]);
bool bt_security_authorize(void *context, const dbt_link_t *link, uint16_t psm, dbt_security_t level);
dbt_result_t bt_security_request(void *context, const dbt_link_t *link, uint16_t psm, dbt_security_t level, bool start);
/* Dispatch HCI events before GAP. UNSUPPORTED means "not mine". */
dbt_result_t bt_security_receive(const dbt_hci_packet_t *packet, uint32_t now_ms);
void bt_security_link(const dbt_link_change_t *change);
void bt_security_poll(uint32_t now_ms);
uint32_t bt_security_wait_ms(uint32_t now_ms);
unsigned bt_security_bond_count(void);
/* Pairing policy: new pairing is accepted while no other phone holds an
 * encrypted link, or anytime while the (button) pairing window is open. */
void bt_security_pairing_window(bool open);
#endif
