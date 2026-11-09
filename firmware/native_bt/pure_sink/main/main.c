/* BDK SPEAKER - pure A2DP playback sink on the in-house native_bt host.
 *
 * Bluetooth Classic only (the ESP32 controller runs in dual mode because the
 * host's controller bring-up requires it, but nothing LE is enabled: no
 * advertising, GATT or LE SMP). Services: SDP, A2DP sink (all codecs incl.
 * SSC), AVRCP controller/target. Output: ring buffer + I2S to a PCM5102A.
 *
 * Task layout
 *   core 1: BT controller (prio 23), I2S writer (21), this host owner (20),
 *           bond-store NVS worker (1)
 *   core 0: codec decoder (19)
 * The host owner below is a single-threaded event loop modelled on
 * esp32_probe/main/probe_main.c: poll the runtime and every profile, then
 * sleep until the earliest deadline or a notification. */
#include <inttypes.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

#include "a2dp_sink.h"
#include "audio_out.h"
#include "bt_sdp.h"
#include "bt_security.h"
#include "button.h"
#include "dbt_gap.h"
#include "esp_bt.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_task.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "pure_sink.h"
#include "sdkconfig.h"
#include "sink_avrcp.h"

#if !CONFIG_IDF_TARGET_ESP32 || !CONFIG_BT_CONTROLLER_ONLY || !CONFIG_BTDM_CTRL_MODE_BTDM
#error "pure_sink requires the original ESP32, controller-only Bluetooth and dual-mode controller"
#endif
_Static_assert(PURE_SINK_HOST_PRIORITY < ESP_TASK_BT_CONTROLLER_PRIO, "The host must stay below the controller");
_Static_assert(CONFIG_BTDM_CTRL_PINNED_TO_CORE == 1 && CONFIG_ESP_MAIN_TASK_AFFINITY == 1,
               "Controller and host owner share core 1; core 0 is reserved for decoding");

#define DEVICE_NAME "BDK SPEAKER"
#define L2CAP_RX_MAX 4096u
#define PAIRING_WINDOW_MS 120000u
#define INITIAL_VOLUME 100u /* AVRCP 0..127 (about -11 dB); the phone sets its own level on connect */
#define HEAP_LOG_STREAMING_MS 10000u
#define HEAP_LOG_IDLE_MS 60000u
#define KEY_RELEASE_MS 100u
#define RESTART_DELAY_MS 3000u

static const char *TAG = "pure_sink";

static dbt_runtime_t runtime;
static dbt_gap_t gap;
static TaskHandle_t owner;
static _Atomic bool vhci_tx_inflight;
static struct {
    bool used;
    uint16_t handle;
    uint8_t peer[6];
} links[DBT_MAX_LINKS];

static uint32_t now_ms(void) { return (uint32_t)(esp_timer_get_time() / 1000); }
static bool due(uint32_t now, uint32_t at) { return (int32_t)(now - at) >= 0; }
static uint32_t until(uint32_t now, uint32_t at) { return due(now, at) ? 0 : at - now; }

static _Noreturn void park(const char *stage) {
    ESP_LOGE(TAG, "startup failed: %s", stage);
    for (;;) vTaskSuspend(NULL);
}

void pure_sink_heap_log(const char *why) {
    const uint32_t caps = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
    ESP_LOGI(TAG, "heap[%s]: free %u, min %u, largest %u", why, (unsigned)heap_caps_get_free_size(caps),
             (unsigned)heap_caps_get_minimum_free_size(caps), (unsigned)heap_caps_get_largest_free_block(caps));
}

static void periodic_log(void) {
    const uint32_t caps = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
    audio_out_stats_t s;
    audio_out_stats(&s);
    TaskHandle_t decoder = a2dp_sink_decoder_task(), writer = audio_out_task();
    uint32_t rate = s.rate ? s.rate : 48000u;
    ESP_LOGI(TAG,
             "heap free %u min %u largest %u | stack free: decoder %u, i2s %u, host %u | codec %s %" PRIu32
             " B | ring %" PRIu32 "/%" PRIu32 " ms (target %" PRIu32 ") %s | vol %u | underruns %" PRIu32
             ", slips -%" PRIu32 "/+%" PRIu32,
             (unsigned)heap_caps_get_free_size(caps), (unsigned)heap_caps_get_minimum_free_size(caps),
             (unsigned)heap_caps_get_largest_free_block(caps), decoder ? (unsigned)uxTaskGetStackHighWaterMark(decoder) : 0u,
             writer ? (unsigned)uxTaskGetStackHighWaterMark(writer) : 0u, (unsigned)uxTaskGetStackHighWaterMark(NULL),
             a2dp_sink_codec_label(), a2dp_sink_codec_bytes(), s.level_frames * 1000u / rate,
             s.capacity_frames * 1000u / rate, s.target_frames * 1000u / rate, s.playing ? "playing" : "idle",
             s.volume, s.underruns, s.dropped_frames, s.repeated_frames);
}

/* ------------------------------------------------- VHCI transport (as probe_main.c) */
static void wake_owner(void) {
    if (xPortInIsrContext()) {
        BaseType_t higher = pdFALSE;
        vTaskNotifyGiveFromISR(owner, &higher);
        if (higher) portYIELD_FROM_ISR();
    } else {
        xTaskNotifyGive(owner);
    }
}
static void send_available(void) {
    atomic_store_explicit(&vhci_tx_inflight, false, memory_order_release);
    wake_owner();
}
static int receive_packet(uint8_t *data, uint16_t size) {
    /* No allocation, logging or waiting here: copy into the host ingress queue. */
    (void)dbt_runtime_receive(&runtime, data, size);
    wake_owner();
    return 0;
}
static bool transport_ready(void *context) {
    (void)context;
    if (atomic_load_explicit(&vhci_tx_inflight, memory_order_acquire)) return false;
    return esp_vhci_host_check_send_available();
}
static bool transport_send(void *context, uint8_t *data, uint16_t size) {
    if (!transport_ready(context)) return false;
    atomic_store_explicit(&vhci_tx_inflight, true, memory_order_release); /* completion may race the call */
    esp_vhci_host_send_packet(data, size);
    return true;
}
static uint8_t *allocate_rx(void *context, uint16_t size) {
    (void)context;
    uint8_t *raw = heap_caps_aligned_alloc(8, (size_t)size + 8u, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!raw) return NULL;
    uint32_t *header = (uint32_t *)(void *)raw;
    header[0] = size;
    header[1] = ~(uint32_t)size;
    return raw + 8;
}
static void release_rx(void *context, uint8_t *data) {
    (void)context;
    if (!data) return;
    uint32_t *header = (uint32_t *)(void *)(data - 8);
    configASSERT(header[0] <= UINT16_MAX && header[1] == ~header[0]);
    volatile uint8_t *wipe = data - 8;
    for (uint32_t i = 0; i < header[0] + 8u; ++i) wipe[i] = 0;
    heap_caps_free(data - 8);
}

/* ----------------------------------------------------- runtime hooks */
static void profile_link(void *context, const dbt_link_change_t *change) {
    (void)context;
    if (!change) return;
    if (change->kind == DBT_LINK_ADDED) {
        const dbt_link_t *l = dbt_acl_find(&runtime.acl, change->handle);
        if (l && l->kind == DBT_LINK_CLASSIC)
            for (unsigned i = 0; i < DBT_MAX_LINKS; ++i)
                if (!links[i].used) {
                    links[i].used = true;
                    links[i].handle = l->handle;
                    memcpy(links[i].peer, l->peer, 6);
                    ESP_LOGI(TAG, "connected: %02X:%02X:%02X:%02X:%02X:%02X (handle 0x%03x)", l->peer[5], l->peer[4],
                             l->peer[3], l->peer[2], l->peer[1], l->peer[0], l->handle);
                    break;
                }
    } else if (change->kind == DBT_LINK_REMOVED) {
        for (unsigned i = 0; i < DBT_MAX_LINKS; ++i)
            if (links[i].used && links[i].handle == change->handle) {
                const uint8_t *p = links[i].peer;
                ESP_LOGI(TAG, "disconnected: %02X:%02X:%02X:%02X:%02X:%02X", p[5], p[4], p[3], p[2], p[1], p[0]);
                links[i].used = false;
            }
    }
    bt_security_link(change);
}
static void observe(void *context, const dbt_hci_packet_t *packet) {
    (void)context;
    if (packet->type != DBT_H4_EVENT || gap.state == DBT_GAP_IDLE) return;
    dbt_result_t owned = bt_security_receive(packet, now_ms());
    if (owned != DBT_UNSUPPORTED) {
        if (owned != DBT_OK) dbt_host_abort(&runtime.host, owned == DBT_NO_SPACE ? DBT_FAULT_RX_OVERRUN : DBT_FAULT_BAD_PACKET);
        return;
    }
    dbt_result_t result = dbt_gap_event_at(&gap, packet, now_ms());
    if (result == DBT_INVALID || result == DBT_NO_SPACE)
        dbt_host_abort(&runtime.host, result == DBT_NO_SPACE ? DBT_FAULT_RX_OVERRUN : DBT_FAULT_BAD_PACKET);
}
static void profile_channel(void *context, dbt_channel_event_t event, const dbt_channel_t *channel,
                            dbt_channel_reason_t reason, uint16_t result) {
    (void)context;
    bt_sdp_event(event, channel);
    sink_avrcp_event(event, channel);
    a2dp_sink_event(event, channel, reason, result);
}
static bool profile_accept(void *context, const dbt_link_t *link, uint16_t psm) {
    return a2dp_sink_accept(link, psm) && sink_avrcp_accept(context, link, psm);
}
static void profile_data(void *context, const dbt_channel_t *channel, const uint8_t *data, uint16_t size) {
    (void)context;
    switch (channel->psm) {
    case 0x19:
        a2dp_sink_receive(channel, data, size);
        break;
    case 0x17:
        sink_avrcp_receive(channel, data, size);
        break;
    case 0x01:
        if (bt_sdp_receive(channel, data, size) != DBT_OK) (void)dbt_channels_request_close(&runtime.channels, channel);
        break;
    default:
        (void)dbt_channels_request_close(&runtime.channels, channel);
        break;
    }
}
static dbt_result_t avrcp_volume(void *context, uint8_t volume) {
    (void)context;
    audio_out_set_volume(volume);
    ESP_LOGI(TAG, "AVRCP absolute volume %u/127 (%u%%)", volume, (unsigned)(volume * 100u + 63u) / 127u);
    return DBT_OK;
}

/* ------------------------------------------------------------ helpers */
static bool classic_encrypted(void) {
    for (unsigned i = 0; i < DBT_MAX_LINKS; ++i) {
        const dbt_link_t *l = &runtime.acl.links[i];
        if (l->used && l->kind == DBT_LINK_CLASSIC && l->encrypted) return true;
    }
    return false;
}
static const char *play_status_name(uint8_t status) {
    switch (status) {
    case 0: return "stopped";
    case 1: return "playing";
    case 2: return "paused";
    case 3: return "fwd seek";
    case 4: return "rev seek";
    default: return "unknown";
    }
}
static void log_avrcp(void) {
    static sink_avrcp_snapshot_t view;
    static uint8_t connected_seen, status_seen = 0xff;
    static uint32_t metadata_seen;
    if (sink_avrcp_snapshot(&view) != DBT_OK) return;
    if (view.connected != connected_seen) {
        connected_seen = view.connected;
        ESP_LOGI(TAG, "AVRCP %s", view.connected ? "connected" : "disconnected");
        status_seen = 0xff;
    }
    if (!view.connected) return;
    if (view.playback_status != status_seen && view.playback_status != 0xff) {
        status_seen = view.playback_status;
        ESP_LOGI(TAG, "AVRCP play status: %s (%" PRIu32 ".%01" PRIu32 " / %" PRIu32 ".%01" PRIu32 " s)",
                 play_status_name(view.playback_status), view.track_position_ms / 1000u,
                 (view.track_position_ms % 1000u) / 100u, view.track_length_ms / 1000u, (view.track_length_ms % 1000u) / 100u);
    }
    if (view.metadata_updates != metadata_seen) {
        metadata_seen = view.metadata_updates;
        if (view.title[0] || view.artist[0])
            ESP_LOGI(TAG, "Now playing: \"%s\" - %s (%s)%s", view.title, view.artist[0] ? view.artist : "?",
                     view.album[0] ? view.album : "?", view.metadata_truncated ? " [truncated]" : "");
    }
}

void app_main(void) {
    owner = xTaskGetCurrentTaskHandle();
    vTaskPrioritySet(owner, PURE_SINK_HOST_PRIORITY);
    ESP_LOGI(TAG, "%s: pure A2DP sink on the native_bt host", DEVICE_NAME);
    pure_sink_heap_log("boot");

    /* NVS holds the Classic bonds (namespace native_bonds). */
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGE(TAG, "NVS unusable (%s): erasing, existing bonds are lost", esp_err_to_name(err));
        if (nvs_flash_erase() == ESP_OK) err = nvs_flash_init();
    }
    if (err != ESP_OK) park("NVS");

    /* Allocate the PCM ring first, while the heap is unfragmented. */
    if (audio_out_init() != ESP_OK) park("audio output");
    audio_out_set_volume(INITIAL_VOLUME);

    dbt_transport_t transport = {0, transport_ready, transport_send};
    dbt_runtime_hooks_t hooks = {0};
    hooks.allocate = allocate_rx;
    hooks.release = release_rx;
    hooks.packet = observe;
    hooks.link = profile_link;
    hooks.channels.authorize = bt_security_authorize;
    hooks.channels.security_request = bt_security_request;
    hooks.channels.event = profile_channel;
    hooks.channels.data = profile_data;
    hooks.channels.accept = profile_accept;
    if (dbt_runtime_init(&runtime, transport, hooks, L2CAP_RX_MAX, 2000) != DBT_OK) park("runtime");

    dbt_gap_config_t gap_config = {
        .name = DEVICE_NAME,
        /* CoD 0x240414: Audio + Rendering service, Audio/Video major, Loudspeaker minor (LSB first). */
        .class_of_device = {0x14, 0x04, 0x24},
        /* The host's GAP bring-up always writes LE advertising data/parameters;
         * keep the probe's values, but advertising is never enabled. */
        .advertising = {2, 1, 2},
        .advertising_size = 3,
        .advertising_minimum = 0x80,
        .advertising_maximum = 0x100,
    };
    gap_config.classic_services[gap_config.classic_service_count++] = 0x110b; /* Audio Sink */
    gap_config.classic_services[gap_config.classic_service_count++] = 0x110e; /* A/V Remote Control */
    gap_config.classic_services[gap_config.classic_service_count++] = 0x110f; /* A/V Remote Control Controller */
    gap_config.classic_services[gap_config.classic_service_count++] = 0x110c; /* A/V Remote Control Target */
    dbt_gap_hooks_t gap_hooks = {0};
    gap_hooks.accept_classic = bt_security_accept;
    if (dbt_gap_init(&gap, &runtime.host, gap_config, gap_hooks) != DBT_OK) park("GAP");
    if (bt_security_init(&runtime, &gap, owner) != DBT_OK) park("Classic security / bond store");
    if (!button_init(owner)) ESP_LOGW(TAG, "GPIO0 button unavailable");

    esp_bt_controller_config_t controller = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    err = esp_bt_controller_init(&controller);
    if (err != ESP_OK) park("controller init");
    static const esp_vhci_host_callback_t callbacks = {send_available, receive_packet};
    if (esp_vhci_host_register_callback(&callbacks) != ESP_OK) park("VHCI callbacks");
    err = esp_bt_controller_enable(ESP_BT_MODE_BTDM);
    if (err != ESP_OK) park("controller enable");
    if (dbt_runtime_start(&runtime, now_ms()) != DBT_OK) park("runtime start");
    pure_sink_heap_log("controller enabled");

    dbt_host_t *host = &runtime.host;
    dbt_host_state_t host_seen = DBT_HOST_IDLE;
    dbt_gap_state_t gap_seen = DBT_GAP_IDLE;
    bool rx_flow_started = false, services = false, ready_logged = false, window = false;
    uint8_t scan_seen = 0xff, key_pending = 0;
    uint32_t window_until = 0, key_release_at = 0, restart_at = 0, next_heap_log = now_ms() + HEAP_LOG_IDLE_MS;

    for (;;) {
        uint32_t now = now_ms();
        dbt_runtime_poll(&runtime, now, 8, 8);

        if (runtime.ready && !rx_flow_started) {
            rx_flow_started = true;
            /* Host-to-controller flow control: the encoded media queue, not
             * the controller, decides how many ACL packets may be in flight. */
            uint16_t bytes = DBT_MAX_H4_BYTES - 5u;
            uint16_t packets = (DBT_QUEUE_BYTES - DBT_CONTROL_RESERVE_BYTES) / (DBT_MAX_H4_BYTES + 2u);
            if (packets > a2dp_sink_rx_credit_window()) packets = a2dp_sink_rx_credit_window();
            dbt_result_t r = dbt_rx_flow_set_limit(&runtime.rx_flow, a2dp_sink_rx_credit_limit, NULL);
            if (r == DBT_OK) r = dbt_runtime_enable_rx_flow(&runtime, bytes, packets);
            if (r != DBT_OK) {
                ESP_LOGE(TAG, "HCI flow control setup failed (%d)", (int)r);
                dbt_runtime_abort(&runtime, DBT_FAULT_COMMAND_REJECTED);
            }
        }
        bool rx_ready = runtime.rx_flow.state == DBT_RX_FLOW_ACTIVE;

        if (runtime.ready && !services) {
            dbt_result_t r = sink_avrcp_init(&runtime, avrcp_volume, NULL, INITIAL_VOLUME);
            if (r == DBT_OK) r = bt_sdp_init(&runtime);
            if (r == DBT_OK) r = a2dp_sink_init(&runtime, owner);
            if (r != DBT_OK) {
                ESP_LOGE(TAG, "profile initialization failed (%d)", (int)r);
                dbt_runtime_abort(&runtime, DBT_FAULT_BAD_RESPONSE);
            }
            services = true;
        }
        bt_security_poll(now);
        if (services) {
            sink_avrcp_poll(now);
            a2dp_sink_poll(now);
            if (bt_sdp_poll() != DBT_OK) dbt_runtime_abort(&runtime, DBT_FAULT_BAD_RESPONSE);
        }

        /* BOOT button */
        button_event_t button = button_poll(now);
        if (button == BUTTON_SHORT && services && !key_pending) {
            static sink_avrcp_snapshot_t view;
            uint8_t op = sink_avrcp_snapshot(&view) == DBT_OK && view.playback_status == 1 ? 0x46 /* PAUSE */ : 0x44 /* PLAY */;
            if (sink_avrcp_key(op, false, now) == DBT_OK) {
                key_pending = op;
                key_release_at = now + KEY_RELEASE_MS;
                ESP_LOGI(TAG, "button: %s", op == 0x46 ? "pause" : "play");
            } else {
                ESP_LOGI(TAG, "button: no AVRCP connection");
            }
        } else if (button == BUTTON_LONG) {
            window = true;
            window_until = now + PAIRING_WINDOW_MS;
            bt_security_pairing_window(true);
            ESP_LOGI(TAG, "button: pairing mode for %u s (disconnecting current phone)", PAIRING_WINDOW_MS / 1000u);
            for (unsigned i = 0; i < DBT_MAX_LINKS; ++i) {
                const dbt_link_t *l = &runtime.acl.links[i];
                if (l->used && l->kind == DBT_LINK_CLASSIC) (void)dbt_gap_disconnect(&gap, l, 0x13);
            }
        }
        if (key_pending && due(now, key_release_at)) {
            (void)sink_avrcp_key(key_pending, true, now);
            key_pending = 0;
        }
        if (window && due(now, window_until)) {
            window = false;
            bt_security_pairing_window(false);
            ESP_LOGI(TAG, "pairing mode ended");
        }

        /* GAP: discoverable while no phone holds an encrypted link (or in
         * pairing mode), always connectable so bonded phones can reconnect. */
        if (runtime.ready && rx_ready && gap.state == DBT_GAP_IDLE) (void)dbt_gap_start(&gap);
        dbt_gap_poll(&gap, now);
        bool discoverable = window || !classic_encrypted();
        uint8_t wanted_scan = (uint8_t)((discoverable ? 1u : 0u) | 2u);
        bool scan_mismatch = gap.state == DBT_GAP_READY && !gap.classic_pending && gap.classic_scan != wanted_scan;
        if (scan_mismatch && services) (void)dbt_gap_set_classic_scan(&gap, discoverable, true);
        if (gap.state == DBT_GAP_READY && !gap.classic_pending && gap.classic_scan != scan_seen) {
            scan_seen = gap.classic_scan;
            ESP_LOGI(TAG, "%s, %s", scan_seen & 1 ? "discoverable" : "not discoverable",
                     scan_seen & 2 ? "connectable" : "not connectable");
            if (!ready_logged && (scan_seen & 2)) {
                ready_logged = true;
                ESP_LOGI(TAG, "ready as \"%s\" (CoD 0x240414, SSP just works, %u bonded device(s))", DEVICE_NAME,
                         bt_security_bond_count());
                pure_sink_heap_log("bt ready");
            }
        }

        if (host->state != host_seen) {
            host_seen = host->state;
            if (host->state == DBT_HOST_IDENTIFIED) {
                const uint8_t *a = host->controller.address_le;
                ESP_LOGI(TAG, "controller up: %02X:%02X:%02X:%02X:%02X:%02X, HCI %u, manufacturer %u", a[5], a[4],
                         a[3], a[2], a[1], a[0], host->controller.hci_version, host->controller.manufacturer);
            } else if (host->state == DBT_HOST_FAILED) {
                ESP_LOGE(TAG, "host failed: fault %u, controller status 0x%02x, opcode 0x%04x; restarting",
                         (unsigned)host->fault, host->controller_status, host->pending_opcode);
                restart_at = now + RESTART_DELAY_MS;
            }
        }
        if (gap.state != gap_seen) {
            gap_seen = gap.state;
            if (gap.state == DBT_GAP_FAILED) {
                ESP_LOGE(TAG, "GAP configuration failed (status 0x%02x); restarting", gap.last_status);
                restart_at = now + RESTART_DELAY_MS;
            }
        }
        if (restart_at && due(now, restart_at)) esp_restart();
        if (services) log_avrcp();
        bool streaming = a2dp_sink_streaming();
        if (streaming && until(now, next_heap_log) > HEAP_LOG_STREAMING_MS) next_heap_log = now + HEAP_LOG_STREAMING_MS;
        if (due(now, next_heap_log)) {
            periodic_log();
            next_heap_log = now + (streaming ? HEAP_LOG_STREAMING_MS : HEAP_LOG_IDLE_MS);
        }

        /* Sleep until the earliest deadline or a notification. */
        uint32_t t = now_ms();
        uint32_t wait = dbt_runtime_wait_ms(&runtime, t);
        if (wait) {
            uint32_t next;
#define MIN_WAIT(expr) do { next = (expr); if (next < wait) wait = next; } while (0)
            MIN_WAIT(dbt_gap_wait_ms(&gap));
            MIN_WAIT(bt_security_wait_ms(t));
            MIN_WAIT(button_wait_ms(t));
            MIN_WAIT(until(t, next_heap_log));
            if (services) {
                MIN_WAIT(sink_avrcp_wait_ms(t));
                MIN_WAIT(a2dp_sink_wait_ms(t));
                MIN_WAIT(bt_sdp_wait_ms());
            }
            if (key_pending) MIN_WAIT(until(t, key_release_at));
            if (window) MIN_WAIT(until(t, window_until));
            if (restart_at) MIN_WAIT(until(t, restart_at));
            if (scan_mismatch) MIN_WAIT(10u); /* retry a refused scan-mode command */
#undef MIN_WAIT
            uint32_t elapsed = now_ms() - t;
            if (wait != UINT32_MAX) wait = elapsed >= wait ? 0 : wait - elapsed;
        }
        if (wait == 0) continue;
        TickType_t ticks = portMAX_DELAY;
        if (wait != UINT32_MAX) {
            ticks = pdMS_TO_TICKS(wait);
            if (!ticks) ticks = 1;
        }
        (void)ulTaskNotifyTake(pdTRUE, ticks); /* earlier notifications stay pending */
    }
}
