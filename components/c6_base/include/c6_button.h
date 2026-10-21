#pragma once
/*
 * c6_button: the NanoC6's button on CONFIG_C6_BUTTON_GPIO (input, pull-up, active low), polled every 20 ms by
 * one task, no ISR; two consecutive samples change its state.
 *
 *   held < 1 s          C6_BUTTON_PRESS        given on the release
 *   held 1 s to < 3 s   nothing
 *   held 3 s to < 10 s  C6_BUTTON_HOLD_SHORT   given on the release
 *   held 10 s           C6_BUTTON_HOLD_LONG    given at 10 s with the button still down; nothing on its release
 *
 * A button already down at power-up gives nothing until it is released. The callback runs on the button's
 * task, never inside a critical section.
 */
enum { C6_BUTTON_NONE, C6_BUTTON_PRESS, C6_BUTTON_HOLD_SHORT, C6_BUTTON_HOLD_LONG };

typedef void (*c6_button_cb_t)(int gesture);
void c6_button_start(c6_button_cb_t cb);
