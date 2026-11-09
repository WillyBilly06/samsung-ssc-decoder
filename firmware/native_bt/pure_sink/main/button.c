/* BOOT button: short press (<1.5 s) = play/pause, hold 3 s = pairing mode.
 * The ISR (installed without ESP_INTR_FLAG_IRAM, so it lives in flash and
 * needs no IRAM_ATTR) only sets a flag and notifies the owner. */
#include "button.h"

#include <stdatomic.h>

#include "driver/gpio.h"

#define BUTTON_GPIO GPIO_NUM_0
#define DEBOUNCE_MS 30u
#define SHORT_MAX_MS 1500u
#define LONG_MS 3000u

enum { WAIT_RELEASE, RELEASED, PRESSED };
static TaskHandle_t owner;
static _Atomic bool changed;
static bool initialized, settling, long_fired;
static unsigned state;
static uint32_t settle_at, pressed_at;

static void edge(void *unused) {
    (void)unused;
    atomic_store_explicit(&changed, true, memory_order_release);
    BaseType_t wake = pdFALSE;
    vTaskNotifyGiveFromISR(owner, &wake);
    if (wake) portYIELD_FROM_ISR();
}
static bool due(uint32_t now, uint32_t at) { return (int32_t)(now - at) >= 0; }

bool button_init(TaskHandle_t task) {
    if (initialized || !task) return false;
    gpio_config_t config = {.pin_bit_mask = 1ULL << BUTTON_GPIO,
                            .mode = GPIO_MODE_INPUT,
                            .pull_up_en = GPIO_PULLUP_ENABLE,
                            .pull_down_en = GPIO_PULLDOWN_DISABLE,
                            .intr_type = GPIO_INTR_ANYEDGE};
    if (gpio_config(&config) != ESP_OK) return false;
    esp_err_t err = gpio_install_isr_service(0);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) return false;
    owner = task;
    if (gpio_isr_handler_add(BUTTON_GPIO, edge, NULL) != ESP_OK) return false;
    state = WAIT_RELEASE; /* ignore a press that is already held at boot */
    initialized = true;
    atomic_store_explicit(&changed, true, memory_order_release);
    return true;
}

button_event_t button_poll(uint32_t now) {
    if (!initialized) return BUTTON_NONE;
    if (atomic_exchange_explicit(&changed, false, memory_order_acq_rel)) {
        settling = true;
        settle_at = now + DEBOUNCE_MS;
    }
    button_event_t result = BUTTON_NONE;
    if (settling && due(now, settle_at)) {
        settling = false;
        bool down = gpio_get_level(BUTTON_GPIO) == 0;
        if (!down) {
            if (state == PRESSED && !long_fired && now - pressed_at < SHORT_MAX_MS) result = BUTTON_SHORT;
            state = RELEASED;
        } else if (state == RELEASED) {
            state = PRESSED;
            pressed_at = now;
            long_fired = false;
        }
    }
    if (state == PRESSED && !settling && !long_fired && now - pressed_at >= LONG_MS) {
        long_fired = true;
        result = BUTTON_LONG;
    }
    return result;
}

uint32_t button_wait_ms(uint32_t now) {
    if (!initialized) return UINT32_MAX;
    if (atomic_load_explicit(&changed, memory_order_acquire)) return 0;
    if (settling) return due(now, settle_at) ? 0 : settle_at - now;
    if (state == PRESSED && !long_fired) {
        uint32_t held = now - pressed_at;
        return held >= LONG_MS ? 0 : LONG_MS - held;
    }
    return UINT32_MAX;
}
