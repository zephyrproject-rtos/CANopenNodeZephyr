/*
 * Copyright (c) 2019 Vestas Wind Systems A/S
 * Copyright (c) 2024 Ported to CANopenNode v4.x
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <CANopen.h>

extern CO_t *CO;

struct canopen_leds_state {
	CO_NMT_t *nmt;
	canopen_led_callback_t green_cb;
	void *green_arg;
	canopen_led_callback_t red_cb;
	void *red_arg;
	bool green : 1;
	bool red : 1;
	bool program_download : 1;
};

static struct canopen_leds_state canopen_leds;

static void canopen_leds_update(struct k_timer *timer_id)
{
	bool green = false;
	bool red = false;

	ARG_UNUSED(timer_id);

	if (CO && CO->LEDs && CO->NMT) {
		/* Advance LED state by 50ms */
		CO_LEDs_process(CO->LEDs, 50000, CO->NMT->operatingState,
				false,				/* LSSconfig */
				false,				/* ErrCANbusOff */
				false,				/* ErrCANbusWarn */
				false,				/* ErrRpdo */
				false,				/* ErrSync */
				false,				/* ErrHbCons */
				false,				/* ErrOther */
				canopen_leds.program_download,	/* firmwareDownload */
				NULL);				/* timerNext_us */

		if (canopen_leds.program_download) {
			green = CO_LED_GREEN(CO->LEDs, CO_LED_flash_3);
		} else {
			green = CO_LED_GREEN(CO->LEDs, CO_LED_CANopen);
		}

		red = CO_LED_RED(CO->LEDs, CO_LED_CANopen);
	}

#ifdef CONFIG_CANOPENNODE_LEDS_BICOLOR
	if (red && canopen_leds.red_cb) {
		green = false;
	}
#endif

	if (canopen_leds.green_cb) {
		if (green != canopen_leds.green) {
			canopen_leds.green_cb(green, canopen_leds.green_arg);
			canopen_leds.green = green;
		}
	}

	if (canopen_leds.red_cb) {
		if (red != canopen_leds.red) {
			canopen_leds.red_cb(red, canopen_leds.red_arg);
			canopen_leds.red = red;
		}
	}
}

K_TIMER_DEFINE(canopen_leds_timer, canopen_leds_update, NULL);

void canopen_leds_init(CO_NMT_t *nmt,
		       canopen_led_callback_t green_cb, void *green_arg,
		       canopen_led_callback_t red_cb, void *red_arg)
{
	k_timer_stop(&canopen_leds_timer);

	canopen_leds.nmt = nmt;

	/* Call existing callbacks to turn off LEDs */
	if (canopen_leds.green_cb) {
		canopen_leds.green_cb(false, canopen_leds.green_arg);
	}
	if (canopen_leds.red_cb) {
		canopen_leds.red_cb(false, canopen_leds.red_arg);
	}

	canopen_leds.green_cb = green_cb;
	canopen_leds.green_arg = green_arg;
	canopen_leds.green = false;
	canopen_leds.red_cb = red_cb;
	canopen_leds.red_arg = red_arg;
	canopen_leds.red = false;

	/* Call new callbacks to turn off LEDs */
	if (canopen_leds.green_cb) {
		canopen_leds.green_cb(false, canopen_leds.green_arg);
	}
	if (canopen_leds.red_cb) {
		canopen_leds.red_cb(false, canopen_leds.red_arg);
	}

	if (nmt && (green_cb || red_cb)) {
		k_timer_start(&canopen_leds_timer, K_MSEC(50), K_MSEC(50));
	}
}

void canopen_leds_program_download(bool in_progress)
{
	canopen_leds.program_download = in_progress;
}
