/*
 * Copyright (c) 2019 Vestas Wind Systems A/S
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/kernel.h>
#include <zephyr/drivers/can.h>
#include <zephyr/init.h>
#include <zephyr/sys/util.h>

#include <CANopen.h>

#define LOG_LEVEL CONFIG_CANOPENNODE_LOG_LEVEL
#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(canopen_driver);

K_KERNEL_STACK_DEFINE(canopen_tx_workq_stack,
		      CONFIG_CANOPENNODE_TX_WORKQUEUE_STACK_SIZE);

struct k_work_q canopen_tx_workq;

struct canopen_tx_work_container {
	struct k_work work;
	CO_CANmodule_t *CANmodule;
};

struct canopen_tx_work_container canopen_tx_queue;

K_MUTEX_DEFINE(canopen_send_mutex);
K_MUTEX_DEFINE(canopen_emcy_mutex);
K_MUTEX_DEFINE(canopen_co_mutex);

static canopen_rxmsg_callback_t rxmsg_callback;

inline void canopen_send_lock(void)
{
	k_mutex_lock(&canopen_send_mutex, K_FOREVER);
}

inline void canopen_send_unlock(void)
{
	k_mutex_unlock(&canopen_send_mutex);
}

inline void canopen_emcy_lock(void)
{
	k_mutex_lock(&canopen_emcy_mutex, K_FOREVER);
}

inline void canopen_emcy_unlock(void)
{
	k_mutex_unlock(&canopen_emcy_mutex);
}

inline void canopen_od_lock(void)
{
	k_mutex_lock(&canopen_co_mutex, K_FOREVER);
}

inline void canopen_od_unlock(void)
{
	k_mutex_unlock(&canopen_co_mutex);
}

void canopen_set_rxmsg_callback(canopen_rxmsg_callback_t callback)
{
	rxmsg_callback = callback;
}

static void canopen_detach_all_rx_filters(CO_CANmodule_t *CANmodule)
{
	uint16_t i;

	if (!CANmodule || !CANmodule->rxArray || !CANmodule->configured) {
		return;
	}

	for (i = 0U; i < CANmodule->rxSize; i++) {
		if (CANmodule->rxArray[i].filter_id != -ENOSPC) {
			/* In SW filtering mode, filter_id 0 == shared catch-all
			 * filter; only remove real hardware filters.
			 */
			if (CANmodule->useCANrxFilters ||
			    i == 0) {
				can_remove_rx_filter(CANmodule->dev,
					CANmodule->rxArray[i].filter_id);
			}
			CANmodule->rxArray[i].filter_id = -ENOSPC;
		}
	}
}

static void canopen_rx_callback(const struct device *dev, struct can_frame *frame, void *user_data)
{
	CO_CANmodule_t *CANmodule = (CO_CANmodule_t *)user_data;
	CO_CANrxMsg_t rxMsg;
	CO_CANrx_t *buffer;
	canopen_rxmsg_callback_t callback = rxmsg_callback;
	int i;

	ARG_UNUSED(dev);

	/* Fill our message structure (convert DLC code to byte count) */
	rxMsg.ident = frame->id;
	rxMsg.DLC = can_dlc_to_bytes(frame->dlc);
	memcpy(rxMsg.data, frame->data, rxMsg.DLC);

	/* Loop through registered rx buffers in priority order */
	for (i = 0; i < CANmodule->rxSize; i++) {
		buffer = &CANmodule->rxArray[i];

		if (buffer->filter_id == -ENOSPC || buffer->pCANrx_callback == NULL) {
			continue;
		}

		if (((frame->id ^ buffer->ident) & buffer->mask) == 0U) {
#ifdef CONFIG_CAN_ACCEPT_RTR
			if (buffer->rtr && ((frame->flags & CAN_FRAME_RTR) == 0U)) {
				continue;
			}
#endif /* CONFIG_CAN_ACCEPT_RTR */
			buffer->pCANrx_callback(buffer->object, &rxMsg);
			if (callback != NULL) {
				callback();
			}
			break;
		}
	}
}

static void canopen_tx_callback(const struct device *dev, int error, void *arg)
{
	CO_CANmodule_t *CANmodule = arg;

	ARG_UNUSED(dev);

	if (!CANmodule) {
		LOG_ERR("failed to process CAN tx callback");
		return;
	}

	if (error == 0) {
		CANmodule->firstCANtxMessage = false;
	}

	k_work_submit_to_queue(&canopen_tx_workq, &canopen_tx_queue.work);
}

static void canopen_tx_retry(struct k_work *item)
{
	struct canopen_tx_work_container *container =
		CONTAINER_OF(item, struct canopen_tx_work_container, work);
	CO_CANmodule_t *CANmodule = container->CANmodule;
	struct can_frame frame;
	CO_CANtx_t *buffer;
	int err;
	uint16_t i;

	memset(&frame, 0, sizeof(frame));

	CO_LOCK_CAN_SEND(CANmodule);

	for (i = 0; i < CANmodule->txSize; i++) {
		buffer = &CANmodule->txArray[i];
		if (buffer->bufferFull) {
			frame.id = buffer->ident;
			frame.dlc = buffer->DLC;
			frame.flags |= (buffer->rtr ? CAN_FRAME_RTR : 0);
			memcpy(frame.data, buffer->data, buffer->DLC);

			err = can_send(CANmodule->dev, &frame, K_NO_WAIT,
				       canopen_tx_callback, CANmodule);
			if (err == -EAGAIN) {
				break;
			} else if (err != 0) {
				LOG_ERR("failed to send CAN frame (err %d)",
					err);
				CO_errorReport(CANmodule->em,
					       CO_EM_CAN_TX_OVERFLOW,
					       CO_EMC_CAN_OVERRUN,
					       buffer->ident);
				/* Keep bufferFull set so the frame is
				 * retried on the next TX completion. */
				break;
			}

			buffer->bufferFull = false;
		}
	}

	CO_UNLOCK_CAN_SEND(CANmodule);
}

void CO_CANsetConfigurationMode(void *CANptr)
{
	const struct device *dev = (const struct device *)CANptr;
	int err;

	err = can_stop(dev);
	if (err != 0 && err != -EALREADY) {
		LOG_ERR("failed to stop CAN interface (err %d)", err);
	}
}

void CO_CANsetNormalMode(CO_CANmodule_t *CANmodule)
{
	int err;

	err = can_start(CANmodule->dev);
	if (err != 0 && err != -EALREADY) {
		LOG_ERR("failed to start CAN interface (err %d)", err);
		return;
	}

	CANmodule->CANnormal = true;
}

CO_ReturnError_t CO_CANmodule_init(CO_CANmodule_t *CANmodule,
				   void *CANptr,
				   CO_CANrx_t rxArray[], uint16_t rxSize,
				   CO_CANtx_t txArray[], uint16_t txSize,
				   uint16_t CANbitRate)
{
	const struct device *dev = (const struct device *)CANptr;
	uint16_t i;
	int err;
	int max_filters;

	LOG_DBG("rxSize = %d, txSize = %d", rxSize, txSize);

	if (!CANmodule || !rxArray || !txArray || !CANptr) {
		LOG_ERR("failed to initialize CAN module");
		return CO_ERROR_ILLEGAL_ARGUMENT;
	}

	max_filters = can_get_max_filters(dev, false);
	if (max_filters != -ENOSYS && (int16_t)max_filters > 0) {
		if (rxSize > max_filters) {
			LOG_WRN("SW filtering: %d RX buffers, only %d HW filters "
				"available. All CAN frames will be processed.",
				rxSize, max_filters);
		} else {
			CANmodule->useCANrxFilters = true;
		}
	}

	canopen_detach_all_rx_filters(CANmodule);
	canopen_tx_queue.CANmodule = CANmodule;

	CANmodule->dev = dev;
	CANmodule->CANptr = CANptr;
	CANmodule->rxArray = rxArray;
	CANmodule->rxSize = rxSize;
	CANmodule->txArray = txArray;
	CANmodule->txSize = txSize;
	CANmodule->CANnormal = false;
	CANmodule->useCANrxFilters = false;
	CANmodule->bufferInhibitFlag = false;
	CANmodule->firstCANtxMessage = true;
	CANmodule->CANtxCount = 0;
	CANmodule->CANerrorStatus = 0;
	CANmodule->errOld = 0;
	CANmodule->em = NULL;

	for (i = 0U; i < rxSize; i++) {
		rxArray[i].ident = 0U;
		rxArray[i].mask = 0U;
		rxArray[i].object = NULL;
		rxArray[i].pCANrx_callback = NULL;
		rxArray[i].filter_id = -ENOSPC;
	}

	for (i = 0U; i < txSize; i++) {
		txArray[i].bufferFull = false;
	}

	err = can_set_bitrate(dev, KHZ(CANbitRate));
	if (err) {
		LOG_ERR("failed to configure CAN bitrate (err %d)", err);
		return CO_ERROR_ILLEGAL_BAUDRATE;
	}

	err = can_set_mode(CANmodule->dev, CAN_MODE_NORMAL);
	if (err) {
		LOG_ERR("failed to configure CAN interface (err %d)", err);
		return CO_ERROR_ILLEGAL_ARGUMENT;
	}

	/*
	 * If hardware filters are insufficient, register a single catch-all
	 * filter and do all matching in software (canopen_rx_callback).
	 */
	if (!CANmodule->useCANrxFilters) {
		struct can_filter catch_all = { .flags = 0U, .id = 0U, .mask = 0U };

		CANmodule->rxArray[0].filter_id = can_add_rx_filter(
			CANmodule->dev, canopen_rx_callback, CANmodule, &catch_all);
		if (CANmodule->rxArray[0].filter_id == -ENOSPC) {
			LOG_ERR("failed to add catch-all CAN RX filter");
			return CO_ERROR_OUT_OF_MEMORY;
		}
	} else {
		/* Mark filter slots as available for per-buffer registration */
		;
	}

	CANmodule->configured = true;

	return CO_ERROR_NO;
}

void CO_CANmodule_disable(CO_CANmodule_t *CANmodule)
{
	int err;

	if (!CANmodule || !CANmodule->dev) {
		return;
	}

	canopen_detach_all_rx_filters(CANmodule);

	err = can_stop(CANmodule->dev);
	if (err != 0 && err != -EALREADY) {
		LOG_ERR("failed to disable CAN interface (err %d)", err);
	}
}

CO_ReturnError_t CO_CANrxBufferInit(CO_CANmodule_t *CANmodule, uint16_t index,
				uint16_t ident, uint16_t mask, bool_t rtr,
				void *object,
				void (*CANrx_callback)(void *object, void *message))
{
	struct can_filter filter;
	CO_CANrx_t *buffer;

	if (CANmodule == NULL) {
		return CO_ERROR_ILLEGAL_ARGUMENT;
	}

	if (!CANrx_callback || (index >= CANmodule->rxSize)) {
		LOG_ERR("failed to initialize CAN rx buffer, illegal argument");
		CO_errorReport(CANmodule->em, CO_EM_GENERIC_SOFTWARE_ERROR,
			       CO_EMC_SOFTWARE_INTERNAL, 0);
		return CO_ERROR_ILLEGAL_ARGUMENT;
	}

	buffer = &CANmodule->rxArray[index];
	buffer->object = object;
	buffer->pCANrx_callback = CANrx_callback;
	buffer->ident = ident;
	buffer->mask = mask;

#ifndef CONFIG_CAN_ACCEPT_RTR
	if (rtr) {
		LOG_ERR("request for RTR frames, but RTR frames are rejected");
		CO_errorReport(CANmodule->em, CO_EM_GENERIC_SOFTWARE_ERROR,
			       CO_EMC_SOFTWARE_INTERNAL, 0);
		return CO_ERROR_ILLEGAL_ARGUMENT;
	}
#else /* !CONFIG_CAN_ACCEPT_RTR */
	buffer->rtr = rtr;
#endif /* CONFIG_CAN_ACCEPT_RTR */

	filter.flags = 0U;
	filter.id = ident;
	filter.mask = mask;

	if (CANmodule->useCANrxFilters) {
		/* Hardware filtering: register individual filter */
		if (buffer->filter_id != -ENOSPC) {
			can_remove_rx_filter(CANmodule->dev, buffer->filter_id);
		}

		buffer->filter_id = can_add_rx_filter(CANmodule->dev,
						      canopen_rx_callback,
						      CANmodule, &filter);
		if (buffer->filter_id == -ENOSPC) {
			LOG_ERR("failed to add CAN rx callback, no free filter");
			CO_errorReport(CANmodule->em,
				       CO_EM_MEMORY_ALLOCATION_ERROR,
				       CO_EMC_SOFTWARE_INTERNAL, 0);
			return CO_ERROR_OUT_OF_MEMORY;
		}
	} else {
		/* Software filtering: use shared catch-all filter at index 0 */
		buffer->filter_id = 0;
	}

	return CO_ERROR_NO;
}

CO_CANtx_t *CO_CANtxBufferInit(CO_CANmodule_t *CANmodule, uint16_t index,
			       uint16_t ident, bool_t rtr, uint8_t noOfBytes,
			       bool_t syncFlag)
{
	CO_CANtx_t *buffer;

	if (CANmodule == NULL) {
		return NULL;
	}

	if (index >= CANmodule->txSize) {
		LOG_ERR("failed to initialize CAN tx buffer, illegal argument");
		CO_errorReport(CANmodule->em, CO_EM_GENERIC_SOFTWARE_ERROR,
			       CO_EMC_SOFTWARE_INTERNAL, 0);
		return NULL;
	}

	buffer = &CANmodule->txArray[index];
	buffer->ident = ident;
	buffer->rtr = rtr;
	buffer->DLC = noOfBytes;
	buffer->bufferFull = false;
	buffer->syncFlag = syncFlag;

	return buffer;
}

CO_ReturnError_t CO_CANsend(CO_CANmodule_t *CANmodule, CO_CANtx_t *buffer)
{
	CO_ReturnError_t ret = CO_ERROR_NO;
	struct can_frame frame;
	int err;

	if (!CANmodule || !CANmodule->dev || !buffer) {
		return CO_ERROR_ILLEGAL_ARGUMENT;
	}

	memset(&frame, 0, sizeof(frame));

	CO_LOCK_CAN_SEND(CANmodule);

	if (buffer->bufferFull) {
		if (!CANmodule->firstCANtxMessage) {
			CO_errorReport(CANmodule->em, CO_EM_CAN_TX_OVERFLOW,
				       CO_EMC_CAN_OVERRUN, buffer->ident);
		}
		buffer->bufferFull = false;
		ret = CO_ERROR_TX_OVERFLOW;
	}

	frame.id = buffer->ident;
	frame.dlc = buffer->DLC;
	frame.flags = (buffer->rtr ? CAN_FRAME_RTR : 0);
	memcpy(frame.data, buffer->data, buffer->DLC);

	err = can_send(CANmodule->dev, &frame, K_NO_WAIT, canopen_tx_callback,
		       CANmodule);
	if (err == -EAGAIN) {
		buffer->bufferFull = true;
	} else if (err != 0) {
		LOG_ERR("failed to send CAN frame (err %d)", err);
		CO_errorReport(CANmodule->em, CO_EM_GENERIC_SOFTWARE_ERROR,
			       CO_EMC_COMMUNICATION, 0);
		ret = CO_ERROR_TX_UNCONFIGURED;
	}

	CO_UNLOCK_CAN_SEND(CANmodule);

	return ret;
}

void CO_CANclearPendingSyncPDOs(CO_CANmodule_t *CANmodule)
{
	bool_t tpdoDeleted = false;
	CO_CANtx_t *buffer;
	uint16_t i;

	if (!CANmodule) {
		return;
	}

	CO_LOCK_CAN_SEND(CANmodule);

	for (i = 0; i < CANmodule->txSize; i++) {
		buffer = &CANmodule->txArray[i];
		if (buffer->bufferFull && buffer->syncFlag) {
			buffer->bufferFull = false;
			tpdoDeleted = true;
		}
	}

	CO_UNLOCK_CAN_SEND(CANmodule);

	if (tpdoDeleted) {
		CO_errorReport(CANmodule->em, CO_EM_TPDO_OUTSIDE_WINDOW,
			       CO_EMC_COMMUNICATION, 0);
	}
}

void CO_CANmodule_process(CO_CANmodule_t *CANmodule)
{
	CO_EM_t *em = (CO_EM_t *)CANmodule->em;
	struct can_bus_err_cnt err_cnt;
	enum can_state state;
	uint8_t rx_overflows;
	uint32_t errors;
	int err;

	/*
	 * TODO: Zephyr lacks an API for reading the rx mailbox
	 * overflow counter.
	 */
	rx_overflows  = 0;

	err = can_get_state(CANmodule->dev, &state, &err_cnt);
	if (err != 0) {
		LOG_ERR("failed to get CAN controller state (err %d)", err);
		return;
	}

	errors = ((uint32_t)err_cnt.tx_err_cnt << 16) |
		 ((uint32_t)err_cnt.rx_err_cnt << 8) |
		 rx_overflows;

	if (errors != CANmodule->errOld) {
		CANmodule->errOld = errors;

		if (state == CAN_STATE_BUS_OFF) {
			/* Bus off */
			CANmodule->CANerrorStatus |= CO_CAN_ERRTX_BUS_OFF;
			CO_errorReport(em, CO_EM_CAN_TX_BUS_OFF,
				       CO_EMC_BUS_OFF_RECOVERED, errors);
		} else {
			/* Bus not off */
			CANmodule->CANerrorStatus &= ~CO_CAN_ERRTX_BUS_OFF;
			CO_errorReset(em, CO_EM_CAN_TX_BUS_OFF, errors);

			if ((err_cnt.rx_err_cnt >= 96U) ||
			    (err_cnt.tx_err_cnt >= 96U)) {
				/* Bus warning */
				CANmodule->CANerrorStatus |= CO_CAN_ERRTX_WARNING;
				CO_errorReport(em, CO_EM_CAN_BUS_WARNING,
					       CO_EMC_NO_ERROR, errors);
			} else {
				/* Bus not warning */
				CANmodule->CANerrorStatus &= ~CO_CAN_ERRTX_WARNING;
				CO_errorReset(em, CO_EM_CAN_BUS_WARNING,
					      errors);
			}

			if (err_cnt.rx_err_cnt >= 128U) {
				/* Bus rx passive */
				CANmodule->CANerrorStatus |= CO_CAN_ERRRX_PASSIVE;
				CO_errorReport(em, CO_EM_CAN_RX_BUS_PASSIVE,
					       CO_EMC_CAN_PASSIVE, errors);
			} else {
				/* Bus not rx passive */
				CANmodule->CANerrorStatus &= ~CO_CAN_ERRRX_PASSIVE;
				CO_errorReset(em, CO_EM_CAN_RX_BUS_PASSIVE,
					      errors);
			}

			if (err_cnt.tx_err_cnt >= 128U &&
			    !CANmodule->firstCANtxMessage) {
				/* Bus tx passive */
				CANmodule->CANerrorStatus |= CO_CAN_ERRTX_PASSIVE;
				CO_errorReport(em, CO_EM_CAN_TX_BUS_PASSIVE,
					       CO_EMC_CAN_PASSIVE, errors);
			} else if (CO_isError(em, CO_EM_CAN_TX_BUS_PASSIVE)) {
				/* Bus not tx passive */
				CANmodule->CANerrorStatus &= ~CO_CAN_ERRTX_PASSIVE;
				CO_errorReset(em, CO_EM_CAN_TX_BUS_PASSIVE,
					      errors);
				CO_errorReset(em, CO_EM_CAN_TX_OVERFLOW,
					      errors);
			}
		}

		/* This code can be activated if we can read the overflows*/
		if (false && rx_overflows != 0U) {
			CO_errorReport(em, CO_EM_CAN_RXB_OVERFLOW,
				       CO_EMC_CAN_OVERRUN, errors);
		}
	}
}

static int canopen_init(void)
{

	k_work_queue_start(&canopen_tx_workq, canopen_tx_workq_stack,
			   K_KERNEL_STACK_SIZEOF(canopen_tx_workq_stack),
			   CONFIG_CANOPENNODE_TX_WORKQUEUE_PRIORITY, NULL);

	k_thread_name_set(k_work_queue_thread_get(&canopen_tx_workq), "canopen_tx_workq");

	k_work_init(&canopen_tx_queue.work, canopen_tx_retry);

	return 0;
}

SYS_INIT(canopen_init, APPLICATION, CONFIG_KERNEL_INIT_PRIORITY_DEFAULT);
