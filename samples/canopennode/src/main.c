/*
 * CANopenNode v4.x — minimal Zephyr sample.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/kernel.h>
#include <zephyr/drivers/can.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/sys/reboot.h>
#include <CANopen.h>
#include "OD.h"

#define LOG_LEVEL LOG_LEVEL_INF
#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(app);

#define CAN_DEV   DEVICE_DT_GET(DT_CHOSEN(zephyr_canbus))
#define BITRATE   ((DT_PROP_OR(DT_CHOSEN(zephyr_canbus), bitrate, \
                    DT_PROP_OR(DT_CHOSEN(zephyr_canbus), bus_speed, \
                    CONFIG_CAN_DEFAULT_BITRATE))) / 1000)

#define NMT_CONTROL (CO_NMT_STARTUP_TO_OPERATIONAL       \
                     | CO_NMT_ERR_ON_ERR_REG              \
                     | CO_ERR_REG_GENERIC_ERR             \
                     | CO_ERR_REG_COMMUNICATION)
#define FIRST_HB_TIME        500
#define SDO_SRV_TIMEOUT_TIME 1000
#define SDO_CLI_TIMEOUT_TIME 500
#define SDO_CLI_BLOCK        false

CO_t *CO = NULL;

#ifdef CONFIG_CANOPENNODE_LEDS
void canopen_leds_init(CO_NMT_t *nmt,
		       void (*g)(bool, void *), void *ga,
		       void (*r)(bool, void *), void *ra);

static struct gpio_dt_spec led_g = GPIO_DT_SPEC_GET_OR(DT_ALIAS(led1), gpios, {0});
static struct gpio_dt_spec led_r = GPIO_DT_SPEC_GET_OR(DT_ALIAS(led0), gpios, {0});

static void led_set(bool on, void *arg)
{
	struct gpio_dt_spec *l = arg;
	if (l && l->port) gpio_pin_set_dt(l, on);
}

static void led_init(CO_NMT_t *nmt)
{
	if (led_g.port && gpio_is_ready_dt(&led_g))
		gpio_pin_configure_dt(&led_g, GPIO_OUTPUT_INACTIVE);
	if (led_r.port && gpio_is_ready_dt(&led_r))
		gpio_pin_configure_dt(&led_r, GPIO_OUTPUT_INACTIVE);
	/* LEDs are driven by an internal k_timer in canopen_leds.c */
	canopen_leds_init(nmt, led_set, &led_g, led_set, &led_r);
}
#else
#define led_init(nmt)   do { (void)(nmt); } while (0)
#endif

int main(void)
{
	CO_ReturnError_t err;
	CO_NMT_reset_cmd_t reset = CO_RESET_NOT;
	uint32_t heapMemoryUsed = 0;
	uint32_t errInfo;
	const struct device *can_dev = CAN_DEV;
	void *CANptr;
	uint32_t timePrevious;

	if (!device_is_ready(can_dev)) {
		LOG_ERR("CAN not ready");
		return 0;
	}
	CANptr = (void *)can_dev;

	CO = CO_new(NULL, &heapMemoryUsed);
	if (!CO) {
		LOG_ERR("CO_new failed");
		return 0;
	}
	LOG_INF("Allocated %u bytes", heapMemoryUsed);

	/* NMT "reset communication" re-initializes in-place (no MCU reset) */
	while (reset != CO_RESET_APP) {
		LOG_INF("Reset communication...");

		CO_CANsetConfigurationMode(CANptr);
		CO_CANmodule_disable(CO->CANmodule);

		err = CO_CANinit(CO, CANptr, BITRATE);
		if (err) { LOG_ERR("CO_CANinit: %d", err); break; }

		err = CO_CANopenInit(CO, NULL, NULL, OD, NULL, NMT_CONTROL,
				     FIRST_HB_TIME, SDO_SRV_TIMEOUT_TIME,
				     SDO_CLI_TIMEOUT_TIME, SDO_CLI_BLOCK,
				     CONFIG_CANOPEN_NODE_ID, &errInfo);
		if (err != CO_ERROR_NO &&
		    err != CO_ERROR_NODE_ID_UNCONFIGURED_LSS) {
			LOG_ERR("CO_CANopenInit: %d (0x%X)", err, errInfo);
			break;
		}

		err = CO_CANopenInitPDO(CO, CO->em, OD,
					CONFIG_CANOPEN_NODE_ID, &errInfo);
		if (err != CO_ERROR_NO &&
		    err != CO_ERROR_NODE_ID_UNCONFIGURED_LSS) {
			LOG_ERR("CO_CANopenInitPDO: %d (0x%X)", err, errInfo);
			break;
		}

		CO_CANsetNormalMode(CO->CANmodule);
		led_init(CO->NMT);
		LOG_INF("Running...");

		timePrevious = k_uptime_get();
		reset = CO_RESET_NOT;

		while (reset == CO_RESET_NOT) {
			uint32_t timeNow = k_uptime_get();
			uint32_t timeDiff_us = (timeNow - timePrevious) * 1000U;
			timePrevious = timeNow;

			/* Mainline processing */
			reset = CO_process(CO, false, timeDiff_us, NULL);
			CO_CANmodule_process(CO->CANmodule);

			/* Monitor for CAN bus-off */
			if (CO_isError(CO->em, CO_EM_CAN_TX_BUS_OFF)) {
				LOG_WRN("CAN bus-off");
			}

			k_sleep(K_MSEC(1));
		}

		LOG_INF("NMT reset request: %d (%s)", reset,
			reset == CO_RESET_APP ? "reset node" : "reset communication");
	}

	/* NMT "reset node" (or init failure): full MCU reset */
	CO_CANsetConfigurationMode(CANptr);
	CO_delete(CO);
	CO = NULL;
	if (reset == CO_RESET_APP) {
		LOG_INF("Node reset requested, rebooting...");
		sys_reboot(SYS_REBOOT_COLD);
	}
	return 0;
}
