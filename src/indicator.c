/* Fidelio
 *
 * (c) 2023 Daniele Lacamera <root@danielinux.net>
 *
 *
 * Fidelio is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * Fidelio is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1335, USA
 *
 */

#include <stdint.h>
#include "indicator.h"
#include "pins.h"
#include "colors.h"
#include "pico/stdlib.h"
#include "hardware/gpio.h"

#ifdef RGB_LED
#include "hardware/clocks.h"
#include "hardware/pio.h"

#define WS2812_T1 2
#define WS2812_T2 5
#define WS2812_T3 3
#define WS2812_WRAP_TARGET 0
#define WS2812_WRAP 3

static uint16_t ws2812_program_instructions[4];

static struct pio_program ws2812_program = {
    .instructions = ws2812_program_instructions,
    .length = 4,
    .origin = -1,
};

static inline pio_sm_config ws2812_program_get_default_config(uint offset)
{
    pio_sm_config c = pio_get_default_sm_config();
    sm_config_set_sideset(&c, 1, false, false);
    sm_config_set_wrap(&c, offset + WS2812_WRAP_TARGET, offset + WS2812_WRAP);
    sm_config_set_out_shift(&c, false, true, 24);
    sm_config_set_fifo_join(&c, PIO_FIFO_JOIN_TX);
    return c;
}

static void ws2812_program_init(PIO pio, uint sm, uint offset, uint pin, float freq)
{
    pio_gpio_init(pio, pin);
    pio_sm_set_consecutive_pindirs(pio, sm, pin, 1, true);
    pio_sm_config c = ws2812_program_get_default_config(offset);
    sm_config_set_sideset_pins(&c, pin);
    sm_config_set_clkdiv(&c, clock_get_hz(clk_sys) / (freq * (WS2812_T1 + WS2812_T2 + WS2812_T3)));
    pio_sm_init(pio, sm, offset, &c);
    pio_sm_set_enabled(pio, sm, true);
}

static PIO rgb_pio = pio0;
static uint rgb_sm = 0;
static uint rgb_offset = 0;

static inline uint32_t rgb_pack(uint8_t r, uint8_t g, uint8_t b)
{
    return ((uint32_t)g << 16) | ((uint32_t)r << 8) | b;
}

static inline void rgb_write(uint8_t r, uint8_t g, uint8_t b)
{
    pio_sm_put_blocking(rgb_pio, rgb_sm, rgb_pack(r, g, b) << 8u);
}

#endif

typedef struct {
    uint16_t r;
    uint16_t g;
    uint16_t b;
    uint16_t freq_hz;
    uint16_t duration_ms;
    absolute_time_t start_time;
    int active;
} blink_state_t;

static blink_state_t blink_state = {0};

void indicator_init(void)
{
#ifdef RGB_LED
    ws2812_program_instructions[0] = (uint16_t)(pio_encode_out(pio_x, 1) |
                                                pio_encode_sideset(1, 0) |
                                                pio_encode_delay(WS2812_T3 - 1));
    ws2812_program_instructions[1] = (uint16_t)(pio_encode_jmp_not_x(WS2812_WRAP) |
                                                pio_encode_sideset(1, 1) |
                                                pio_encode_delay(WS2812_T1 - 1));
    ws2812_program_instructions[2] = (uint16_t)(pio_encode_jmp(WS2812_WRAP_TARGET) |
                                                pio_encode_sideset(1, 1) |
                                                pio_encode_delay(WS2812_T2 - 1));
    ws2812_program_instructions[3] = (uint16_t)(pio_encode_nop() |
                                                pio_encode_sideset(1, 0) |
                                                pio_encode_delay(WS2812_T2 - 1));

    rgb_offset = pio_add_program(rgb_pio, &ws2812_program);
    ws2812_program_init(rgb_pio, rgb_sm, rgb_offset, RGB_LED, 800000.0f);
    rgb_write(0, 0, 0);
#else
    gpio_init(PRESENCE_LED);
    gpio_set_dir(PRESENCE_LED, GPIO_OUT);
    gpio_put(PRESENCE_LED, 0);
#endif
}

void indicator_set_idle(void)
{
    indicator_set(0, 0, 0);
}

void indicator_set(uint16_t r, uint16_t g, uint16_t b)
{
#ifdef RGB_LED
    rgb_write((uint8_t)r, (uint8_t)g, (uint8_t)b);
#else
    gpio_put(PRESENCE_LED, (r || g || b) ? 1 : 0);
#endif
}

void indicator_blink(uint16_t r, uint16_t g, uint16_t b, uint16_t freq_hz, uint16_t duration_ms)
{
    blink_state.r = r;
    blink_state.g = g;
    blink_state.b = b;
    blink_state.freq_hz = freq_hz;
    blink_state.duration_ms = duration_ms;
    blink_state.start_time = get_absolute_time();
    blink_state.active = 1;
}

int indicator_process_blink(void)
{
    if (!blink_state.active)
        return 0;

    uint32_t elapsed = absolute_time_diff_us(blink_state.start_time, get_absolute_time()) / 1000;
    
    if (blink_state.duration_ms > 0 && elapsed >= blink_state.duration_ms) {
        blink_state.active = 0;
        indicator_set_idle();
        return 0;
    }

    uint16_t half_period_ms = 500 / blink_state.freq_hz;
    uint16_t phase = (elapsed / half_period_ms) % 2;
    
    if (phase == 0) {
        indicator_set(blink_state.r, blink_state.g, blink_state.b);
    } else {
        indicator_set_idle();
    }
    
    return 1;
}

void indicator_stop_blinking(void)
{
    blink_state.active = 0;
    indicator_set_idle();
}

void indicator_wait_for_button(uint16_t r, uint16_t g, uint16_t b)
{
#ifdef RGB_LED
    absolute_time_t next_step = make_timeout_time_ms(80);
    gpio_init(PRESENCE_BUTTON);
    gpio_set_dir(PRESENCE_BUTTON, GPIO_IN);
    gpio_pull_up(PRESENCE_BUTTON);
    asm volatile("dmb");

    indicator_set(r, g, b);
    while (gpio_get(PRESENCE_BUTTON) == 0) {
        sleep_ms(2);
    }

    next_step = make_timeout_time_ms(80);
    while (gpio_get(PRESENCE_BUTTON) != 0) {
        sleep_ms(2);
    }
    sleep_ms(30);
    indicator_set_idle();
#else
    indicator_set(r, g, b);
    while (gpio_get(PRESENCE_BUTTON) == 0) {
        sleep_ms(2);
    }
    while (gpio_get(PRESENCE_BUTTON) != 0) {
        sleep_ms(2);
    }
    sleep_ms(30);
    indicator_set_idle();
#endif
}

void indicator_wait_for_button_blinking(void)
{
    gpio_init(PRESENCE_BUTTON);
    gpio_set_dir(PRESENCE_BUTTON, GPIO_IN);
    gpio_pull_up(PRESENCE_BUTTON);
    asm volatile("dmb");

    /* Blink while *waiting* for the user to press the button (logic inverted
       compared to the original implementation, which only blinked while the
       button was already held down). */
    indicator_blink(COLOR_BLUE_R, COLOR_BLUE_G, COLOR_BLUE_B, 4, 0);

    /* wait until the button is pressed */
    while (gpio_get(PRESENCE_BUTTON) != 0) {
        indicator_process_blink();
        sleep_ms(10);
    }

    /* stop blinking once the press is detected, then debounce the release */
    blink_state.active = 0;
    while (gpio_get(PRESENCE_BUTTON) == 0) {
        sleep_ms(2);
    }
    sleep_ms(30);
    indicator_set_idle();
}

void indicator_wait_for_action(void)
{
    indicator_stop_blinking();
    indicator_set(COLOR_BLUE_R, COLOR_BLUE_G, COLOR_BLUE_B);
}

void indicator_action_start(void)
{
    indicator_blink(COLOR_BLUE_R, COLOR_BLUE_G, COLOR_BLUE_B, 4, 0);
}

void indicator_action_end(void)
{
    /* show a steady green for a short period instead of blinking; the caller
       typically invokes this when an operation completes successfully. */
    indicator_set(COLOR_GREEN_R, COLOR_GREEN_G, COLOR_GREEN_B);
    /* keep green for two seconds so the user can easily see it, then return
       to the normal "waiting for action" state */
    sleep_ms(2000);
    indicator_wait_for_action();
}

void indicator_pin_not_set(void)
{
    indicator_set(COLOR_RED_R, COLOR_RED_G, COLOR_RED_B);
}

void indicator_locked(void)
{
    indicator_blink(COLOR_RED_R, COLOR_RED_G, COLOR_RED_B, 2, 0);
}

void indicator_test_delay(void)
{
    indicator_blink(COLOR_PURPLE_R, COLOR_PURPLE_G, COLOR_PURPLE_B, 4, 2000);
    while (blink_state.active) {
        indicator_process_blink();
        sleep_ms(10);
    }
}
