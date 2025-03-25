#ifndef LED_BUTTON_H
#define LED_BUTTON_H

#include <stdint.h>
#include "driver/gpio.h"
#include "driver/rmt_tx.h"

// Configuration constants
#define RMT_LED_STRIP_RESOLUTION_HZ  (10 * 1000 * 1000)
#define RMT_LED_STRIP_GPIO_NUM       15
#define EXAMPLE_LED_NUMBERS          1
#define BUTTON_GPIO                  4
#define BLINK_DELAY_MS               300

// LED control functions
void blink_all_leds(uint8_t r, uint8_t g, uint8_t b, uint8_t w, int delay_ms);
void set_leds_color(uint8_t r, uint8_t g, uint8_t b, uint8_t w);
void init_led_strip(void);
void turn_off_leds(void);

// Button functions
void init_button(void);
bool check_button_press(void);

#endif // LED_BUTTON_H
