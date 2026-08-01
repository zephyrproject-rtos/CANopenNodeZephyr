/*
 * Copyright (c) 2019 Vestas Wind Systems A/S
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ZEPHYR_MODULES_CANOPENNODE_CO_DRIVER_TARGET_H
#define ZEPHYR_MODULES_CANOPENNODE_CO_DRIVER_TARGET_H

/*
 * Zephyr RTOS CAN driver interface and configuration for CANopenNode
 * CANopen protocol stack v4.x.
 *
 * See CANopenNode/301/CO_driver.h for API description.
 */

#ifdef __cplusplus
extern "C" {
#endif

#include <zephyr/kernel.h>
#include <zephyr/types.h>
#include <zephyr/device.h>
#include <zephyr/toolchain.h>
#include <zephyr/dsp/types.h> /* float32_t, float64_t */

/* Use static variables instead of calloc() */
#define CO_USE_GLOBALS

/* Use Zephyr's CRC16 CCITT implementation instead of CANopenNode's own */
#define CO_USE_OWN_CRC16

/* Stack configuration overrides from Kconfig. */

#ifdef CONFIG_CANOPENNODE_SDO_BUFFER_SIZE
#define CO_SDO_BUFFER_SIZE CONFIG_CANOPENNODE_SDO_BUFFER_SIZE
#endif

#ifdef CONFIG_CANOPENNODE_TRACE_BUFFER_SIZE
#define CO_TRACE_BUFFER_SIZE_FIXED CONFIG_CANOPENNODE_TRACE_BUFFER_SIZE
#endif

#ifdef CONFIG_CANOPENNODE_LEDS
#define CO_USE_LEDS 1
#endif

// Custom CANOpenNode Driver Configuration goes here, for example:
// #define CO_CONFIG_SDO_SRV (CO_CONFIG_SDO_SRV_SEGMENTED | CO_CONFIG_SDO_SRV_BLOCK )


#ifdef CONFIG_LITTLE_ENDIAN
#define CO_LITTLE_ENDIAN
#define CO_SWAP_16(x) (x)
#define CO_SWAP_32(x) (x)
#define CO_SWAP_64(x) (x)
#else
#define CO_BIG_ENDIAN
#define CO_SWAP_16(x) __builtin_bswap16(x)
#define CO_SWAP_32(x) __builtin_bswap32(x)
#define CO_SWAP_64(x) __builtin_bswap64(x)
#endif

typedef bool          bool_t;
typedef char          char_t;
typedef unsigned char oChar_t;
typedef unsigned char domain_t;

BUILD_ASSERT(sizeof(float32_t) >= 4);
BUILD_ASSERT(sizeof(float64_t) >= 8);

typedef struct canopen_rx_msg {
	uint8_t data[8];
	uint16_t ident;
	uint8_t DLC;
} CO_CANrxMsg_t;

/*
 * Inline helpers for reading received CAN frame fields.
 * These are called from CANrx_callback() in the fast receive path.
 */
static inline uint16_t CO_CANrxMsg_readIdent(const CO_CANrxMsg_t *rxMsg)
{
	return rxMsg->ident;
}

static inline uint8_t CO_CANrxMsg_readDLC(const CO_CANrxMsg_t *rxMsg)
{
	return rxMsg->DLC;
}

static inline const uint8_t *CO_CANrxMsg_readData(const CO_CANrxMsg_t *rxMsg)
{
	return rxMsg->data;
}

typedef struct {
	uint16_t ident;
	uint16_t mask;
	void *object;
	void (*pCANrx_callback)(void *object, void *message);
	int filter_id;
#ifdef CONFIG_CAN_ACCEPT_RTR
	bool_t rtr;
#endif
} CO_CANrx_t;

typedef struct {
	uint32_t ident;
	uint8_t DLC;
	uint8_t data[8];
	volatile bool_t bufferFull;
	volatile bool_t syncFlag;
	bool_t rtr;
} CO_CANtx_t;

typedef struct {
	const struct device *dev;
	void *CANptr;
	CO_CANrx_t *rxArray;
	uint16_t rxSize;
	CO_CANtx_t *txArray;
	uint16_t txSize;
	volatile uint16_t CANerrorStatus;
	volatile bool_t CANnormal;
	volatile bool_t useCANrxFilters;
	volatile bool_t bufferInhibitFlag;
	volatile bool_t firstCANtxMessage;
	volatile uint16_t CANtxCount;
	uint32_t errOld;
	void *em;
	bool_t configured;
} CO_CANmodule_t;

void canopen_send_lock(void);
void canopen_send_unlock(void);
#define CO_LOCK_CAN_SEND(CAN_MODULE)   canopen_send_lock()
#define CO_UNLOCK_CAN_SEND(CAN_MODULE) canopen_send_unlock()

void canopen_emcy_lock(void);
void canopen_emcy_unlock(void);
#define CO_LOCK_EMCY(CAN_MODULE)   canopen_emcy_lock()
#define CO_UNLOCK_EMCY(CAN_MODULE) canopen_emcy_unlock()

void canopen_od_lock(void);
void canopen_od_unlock(void);
#define CO_LOCK_OD(CAN_MODULE)   canopen_od_lock()
#define CO_UNLOCK_OD(CAN_MODULE) canopen_od_unlock()

/*
 * CAN receive synchronization flags.
 * CANopenNode RX callbacks run in interrupt context on Zephyr, so no
 * memory barrier is needed.
 */
#define CO_FLAG_READ(rxNew)    ((rxNew) != NULL)
#define CO_FLAG_SET(rxNew)     do { rxNew = (void *)1L; } while (0)
#define CO_FLAG_CLEAR(rxNew)   do { rxNew = NULL; } while (0)

/* Incoming CAN message callback (ISR context, for waking main loop) */
typedef void (*canopen_rxmsg_callback_t)(void);
void canopen_set_rxmsg_callback(canopen_rxmsg_callback_t callback);

/** Callback invoked when a CANopen LED state changes. */
typedef void (*canopen_led_callback_t)(bool value, void *arg);

#ifdef __cplusplus
}
#endif

#endif /* ZEPHYR_MODULES_CANOPENNODE_CO_DRIVER_TARGET_H */
