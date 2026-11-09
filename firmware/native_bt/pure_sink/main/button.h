#ifndef PURE_SINK_BUTTON_H
#define PURE_SINK_BUTTON_H
/* GPIO0 (BOOT) button, active low. Edge interrupt only wakes the host owner;
 * debouncing and timing run in button_poll on the owner. */
#include <stdbool.h>
#include <stdint.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

typedef enum { BUTTON_NONE, BUTTON_SHORT, BUTTON_LONG } button_event_t;
bool button_init(TaskHandle_t owner);
button_event_t button_poll(uint32_t now_ms);
uint32_t button_wait_ms(uint32_t now_ms);
#endif
