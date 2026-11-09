/* Classic security owner + NVS bond store, reduced from
 * esp32_probe/main/probe_security.c (+ the Classic part of probe_admission.c)
 * with all diagnostic logging removed. NVS writes run on a small worker task
 * so flash erase/write never stalls the host owner. */
#include "bt_security.h"

#include <string.h>

#include "esp_log.h"
#include "freertos/queue.h"
#include "nvs.h"
#include "pure_sink.h"

#define BOND_LIMIT 8u
#define STORE_STACK 3072u

static const char *TAG = "security";

typedef struct {
    uint8_t address[6];
    dbt_classic_key_t key;
    bool used;
} bond_t;
typedef struct {
    uint32_t token;
    esp_err_t error;
    uint8_t address[6], evict[6];
    dbt_classic_key_t key;
    bool deleting, evicting, committed;
} store_job_t;

static dbt_classic_security_t security;
static dbt_runtime_t *runtime;
static dbt_gap_t *gap;
static TaskHandle_t owner;
static nvs_handle_t storage;
static QueueHandle_t work_queue, done_queue;
static bond_t bonds[BOND_LIMIT];
static unsigned store_inflight, evict_next;
static bool window_open;
static struct {
    uint16_t handle;
    uint32_t generation;
    bool used;
} close_requests[DBT_MAX_LINKS];

static void wipe(void *data, size_t size) {
    volatile uint8_t *p = data;
    while (size--) *p++ = 0;
}
static bool same(const uint8_t *a, const uint8_t *b) { return memcmp(a, b, 6) == 0; }
static void key_name(const uint8_t address[6], char key[15]) {
    static const char digits[] = "0123456789abcdef";
    key[0] = 'b';
    key[1] = 'r';
    for (unsigned i = 0; i < 6; ++i) {
        key[2 + i * 2] = digits[address[i] >> 4];
        key[3 + i * 2] = digits[address[i] & 15];
    }
    key[14] = 0;
}
static int find(const uint8_t address[6]) {
    for (unsigned i = 0; i < BOND_LIMIT; ++i)
        if (bonds[i].used && same(bonds[i].address, address)) return (int)i;
    return -1;
}
static int free_slot(void) {
    for (unsigned i = 0; i < BOND_LIMIT; ++i)
        if (!bonds[i].used) return (int)i;
    return -1;
}
/* 28-byte v1 record: [0]=1 [1]=key type [2]=PIN length [3]=0 [4..9]=BD_ADDR (LE) [10..25]=key [26..27]=0 */
static bool decode(const uint8_t b[28], const char *key, bond_t *bond) {
    if (b[0] != 1 || b[3] || b[26] || b[27]) return false;
    memcpy(bond->address, b + 4, 6);
    bond->key.type = b[1];
    bond->key.pin_length = b[2];
    memcpy(bond->key.bytes, b + 10, 16);
    char expected[15];
    key_name(bond->address, expected);
    if (strcmp(expected, key)) return false;
    bond->used = dbt_classic_key_valid(&bond->key);
    return bond->used;
}
static void encode(uint8_t b[28], const store_job_t *job) {
    memset(b, 0, 28);
    b[0] = 1;
    b[1] = job->key.type;
    b[2] = job->key.pin_length;
    memcpy(b + 4, job->address, 6);
    memcpy(b + 10, job->key.bytes, 16);
}
static esp_err_t preload(void) {
    esp_err_t err = nvs_open("native_bonds", NVS_READWRITE, &storage);
    if (err != ESP_OK) return err;
    nvs_iterator_t it = NULL;
    err = nvs_entry_find_in_handle(storage, NVS_TYPE_BLOB, &it);
    while (err == ESP_OK) {
        nvs_entry_info_t entry;
        if (nvs_entry_info(it, &entry) != ESP_OK) break;
        int index = free_slot();
        uint8_t b[28];
        size_t size = sizeof(b);
        if (index >= 0 && nvs_get_blob(storage, entry.key, b, &size) == ESP_OK && size == sizeof(b) &&
            decode(b, entry.key, &bonds[index])) {
            /* loaded */
        } else {
            if (index >= 0) wipe(&bonds[index], sizeof(bonds[index]));
            ESP_LOGW(TAG, "ignoring NVS entry %s", entry.key);
        }
        wipe(b, sizeof(b));
        err = nvs_entry_next(&it);
    }
    nvs_release_iterator(it);
    return err == ESP_ERR_NVS_NOT_FOUND ? ESP_OK : err;
}

/* ------------------------------------------------------------ worker task */
static void store_worker(void *unused) {
    (void)unused;
    for (;;) {
        store_job_t job;
        if (xQueueReceive(work_queue, &job, portMAX_DELAY) != pdTRUE) continue;
        char key[15];
        esp_err_t err = ESP_OK;
        if (job.evicting) {
            key_name(job.evict, key);
            err = nvs_erase_key(storage, key);
            if (err == ESP_ERR_NVS_NOT_FOUND) err = ESP_OK;
        }
        key_name(job.address, key);
        if (err == ESP_OK) {
            if (job.deleting) {
                err = nvs_erase_key(storage, key);
                if (err == ESP_ERR_NVS_NOT_FOUND) err = ESP_OK;
            } else {
                uint8_t b[28];
                encode(b, &job);
                err = nvs_set_blob(storage, key, b, sizeof(b));
                wipe(b, sizeof(b));
            }
        }
        if (err == ESP_OK) err = nvs_commit(storage);
        if (err == ESP_OK && !job.deleting) { /* read back */
            uint8_t actual[28], expected[28];
            size_t size = sizeof(actual);
            err = nvs_get_blob(storage, key, actual, &size);
            encode(expected, &job);
            if (err == ESP_OK && (size != sizeof(actual) || memcmp(actual, expected, sizeof(actual))))
                err = ESP_ERR_INVALID_STATE;
            wipe(actual, sizeof(actual));
            wipe(expected, sizeof(expected));
        }
        job.error = err;
        job.committed = err == ESP_OK;
        (void)xQueueSend(done_queue, &job, portMAX_DELAY);
        wipe(&job, sizeof(job));
        xTaskNotifyGive(owner);
    }
}

/* -------------------------------------------------------- security hooks */
static const dbt_link_t *classic_link(const uint8_t address[6]) {
    for (unsigned i = 0; i < DBT_MAX_LINKS; ++i) {
        const dbt_link_t *l = &runtime->acl.links[i];
        if (l->used && l->kind == DBT_LINK_CLASSIC && same(l->peer, address)) return l;
    }
    return NULL;
}
static bool other_encrypted(const uint8_t address[6]) {
    for (unsigned i = 0; i < DBT_MAX_LINKS; ++i) {
        const dbt_link_t *l = &runtime->acl.links[i];
        if (l->used && l->kind == DBT_LINK_CLASSIC && l->encrypted && !same(l->peer, address)) return true;
    }
    return false;
}
static bool pairing_allowed(void *context, const uint8_t address[6]) {
    (void)context;
    if (!classic_link(address)) return false;
    bool allowed = window_open || !other_encrypted(address);
    ESP_LOGI(TAG, "pairing request from %02X:%02X:%02X:%02X:%02X:%02X: %s", address[5], address[4], address[3],
             address[2], address[1], address[0], allowed ? "accepted (just works)" : "refused (another phone is connected)");
    return allowed;
}
static dbt_result_t load(void *context, const uint8_t address[6], dbt_classic_key_t *key) {
    (void)context;
    int index = find(address);
    if (index < 0) return DBT_EMPTY;
    memcpy(key, &bonds[index].key, sizeof(*key));
    return DBT_OK;
}
static bool store(void *context, const uint8_t address[6], const dbt_classic_key_t *key, uint32_t token) {
    (void)context;
    store_job_t job;
    wipe(&job, sizeof(job));
    memcpy(job.address, address, 6);
    job.token = token;
    job.deleting = key == NULL;
    if (key) {
        memcpy(&job.key, key, sizeof(*key));
        if (find(address) < 0 && free_slot() < 0) { /* table full: replace the oldest-slot bond */
            unsigned victim = evict_next++ % BOND_LIMIT;
            memcpy(job.evict, bonds[victim].address, 6);
            job.evicting = true;
        }
    }
    bool accepted = xQueueSend(work_queue, &job, 0) == pdTRUE;
    if (accepted) ++store_inflight;
    wipe(&job, sizeof(job));
    return accepted;
}
static void event(void *context, dbt_classic_security_event_t kind, const dbt_classic_security_info_t *info,
                  dbt_classic_security_reason_t reason, uint8_t status) {
    (void)context;
    const uint8_t *a = info->address;
    switch (kind) {
    case DBT_CLASSIC_SECURITY_READY:
        ESP_LOGI(TAG, "link %02X:%02X:%02X:%02X:%02X:%02X secured: encrypted=%d bonded=%d key type %u size %u",
                 a[5], a[4], a[3], a[2], a[1], a[0], info->encrypted, info->bonded, info->key_type, info->key_size);
        break;
    case DBT_CLASSIC_BOND_STORED:
        ESP_LOGI(TAG, "bond stored for %02X:%02X:%02X:%02X:%02X:%02X", a[5], a[4], a[3], a[2], a[1], a[0]);
        break;
    case DBT_CLASSIC_BOND_REMOVED:
        ESP_LOGI(TAG, "bond removed for %02X:%02X:%02X:%02X:%02X:%02X", a[5], a[4], a[3], a[2], a[1], a[0]);
        break;
    case DBT_CLASSIC_SECURITY_FAILED:
        ESP_LOGW(TAG, "security failed for %02X:%02X:%02X:%02X:%02X:%02X: reason %u, controller status 0x%02x",
                 a[5], a[4], a[3], a[2], a[1], a[0], (unsigned)reason, status);
        if (info->connected)
            for (unsigned i = 0; i < DBT_MAX_LINKS; ++i)
                if (!close_requests[i].used) {
                    close_requests[i].handle = info->handle;
                    close_requests[i].generation = info->generation;
                    close_requests[i].used = true;
                    break;
                }
        break;
    default:
        break;
    }
    xTaskNotifyGive(owner);
}

/* ----------------------------------------------------------- public API */
dbt_result_t bt_security_init(dbt_runtime_t *host_runtime, dbt_gap_t *host_gap, TaskHandle_t host_owner) {
    runtime = host_runtime;
    gap = host_gap;
    owner = host_owner;
    esp_err_t err = preload();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "bond store unavailable: %s", esp_err_to_name(err));
        return DBT_INVALID;
    }
    work_queue = xQueueCreate(4, sizeof(store_job_t));
    done_queue = xQueueCreate(4, sizeof(store_job_t));
    if (!work_queue || !done_queue) return DBT_NO_SPACE;
    dbt_classic_security_config_t config = {0};
    config.io_capability = 3;               /* NoInputNoOutput */
    config.authentication_requirements = 4; /* general bonding, no MITM */
    config.minimum_key_size = 16;
    config.allow_just_works = true;
    config.allow_legacy = false;
    config.require_durable_bond = true;
    config.procedure_timeout_ms = 30000;
    config.store_timeout_ms = 5000;
    dbt_classic_security_hooks_t hooks = {0};
    hooks.pairing_allowed = pairing_allowed;
    hooks.load = load;
    hooks.store = store;
    hooks.event = event;
    dbt_result_t result = dbt_classic_security_init(&security, &runtime->host, &runtime->acl, config, hooks);
    if (result != DBT_OK) return result;
    dbt_classic_security_pairable(&security, true);
    if (xTaskCreatePinnedToCore(store_worker, "bond_store", STORE_STACK, NULL, 1, NULL, PURE_SINK_BACKGROUND_CORE) != pdPASS)
        return DBT_NO_SPACE;
    for (unsigned i = 0; i < BOND_LIMIT; ++i)
        if (bonds[i].used) {
            const uint8_t *a = bonds[i].address;
            ESP_LOGI(TAG, "bonded: %02X:%02X:%02X:%02X:%02X:%02X", a[5], a[4], a[3], a[2], a[1], a[0]);
        }
    return DBT_OK;
}
bool bt_security_accept(void *context, const uint8_t address[6], const uint8_t device_class[3]) {
    (void)context;
    (void)device_class;
    ESP_LOGI(TAG, "incoming connection from %02X:%02X:%02X:%02X:%02X:%02X (%s)", address[5], address[4], address[3],
             address[2], address[1], address[0], find(address) >= 0 ? "bonded" : "new");
    return true;
}
bool bt_security_authorize(void *context, const dbt_link_t *link, uint16_t psm, dbt_security_t level) {
    (void)context;
    (void)psm;
    return dbt_classic_security_authorize(&security, link, level);
}
dbt_result_t bt_security_request(void *context, const dbt_link_t *link, uint16_t psm, dbt_security_t level, bool start) {
    (void)context;
    (void)psm;
    if (dbt_classic_security_authorize(&security, link, level)) return DBT_OK;
    if (start) return dbt_classic_security_require(&security, link, level, runtime->now_ms);
    for (unsigned i = 0; i < DBT_MAX_LINKS; ++i) {
        const dbt_classic_peer_t *peer = &security.peers[i];
        if (peer->used && peer->connected && peer->handle == link->handle && peer->generation == link->generation)
            return peer->failed ? DBT_INVALID : DBT_BUSY;
    }
    return DBT_UNSUPPORTED;
}
dbt_result_t bt_security_receive(const dbt_hci_packet_t *packet, uint32_t now_ms) {
    return runtime && runtime->ready ? dbt_classic_security_receive(&security, packet, now_ms) : DBT_UNSUPPORTED;
}
void bt_security_link(const dbt_link_change_t *change) {
    dbt_result_t result = dbt_classic_security_link(&security, change);
    if (result != DBT_OK && result != DBT_UNSUPPORTED) {
        const dbt_link_t *link = dbt_acl_find(&runtime->acl, change->handle);
        if (link && link->generation == change->generation) (void)dbt_gap_disconnect(gap, link, 0x13);
    }
}
void bt_security_poll(uint32_t now_ms) {
    store_job_t job;
    while (xQueueReceive(done_queue, &job, 0) == pdTRUE) {
        if (store_inflight) --store_inflight;
        if (job.committed) {
            if (job.evicting) {
                int victim = find(job.evict);
                if (victim >= 0) wipe(&bonds[victim], sizeof(bonds[victim]));
            }
            int index = find(job.address);
            if (job.deleting) {
                if (index >= 0) wipe(&bonds[index], sizeof(bonds[index]));
            } else {
                if (index < 0) index = free_slot();
                if (index >= 0) {
                    memcpy(bonds[index].address, job.address, 6);
                    memcpy(&bonds[index].key, &job.key, sizeof(job.key));
                    bonds[index].used = true;
                } else {
                    job.committed = false;
                }
            }
        } else {
            ESP_LOGE(TAG, "bond NVS write failed: %s", esp_err_to_name(job.error));
        }
        (void)dbt_classic_security_complete_store(&security, job.token, job.committed);
        wipe(&job, sizeof(job));
    }
    dbt_classic_security_poll(&security, now_ms);
    for (unsigned i = 0; i < DBT_MAX_LINKS; ++i)
        if (close_requests[i].used) {
            const dbt_link_t *link = dbt_acl_find(&runtime->acl, close_requests[i].handle);
            if (!link || link->generation != close_requests[i].generation || dbt_gap_disconnect(gap, link, 0x13) == DBT_OK)
                close_requests[i].used = false;
        }
}
uint32_t bt_security_wait_ms(uint32_t now_ms) {
    if (done_queue && uxQueueMessagesWaiting(done_queue)) return 0;
    if (gap->state == DBT_GAP_READY && runtime->host.command_count < DBT_COMMAND_SLOTS)
        for (unsigned i = 0; i < DBT_MAX_LINKS; ++i)
            if (close_requests[i].used) return 0;
    return dbt_classic_security_wait_ms(&security, now_ms);
}
unsigned bt_security_bond_count(void) {
    unsigned count = 0;
    for (unsigned i = 0; i < BOND_LIMIT; ++i) count += bonds[i].used ? 1u : 0u;
    return count;
}
void bt_security_pairing_window(bool open) { window_open = open; }
