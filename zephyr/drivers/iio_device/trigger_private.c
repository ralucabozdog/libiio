/*
 * Copyright (c) 2026 Analog Devices, Inc.
 *
 * SPDX-License-Identifier: MIT
 */

#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>
#include <iio/iio-backend.h>
#include <iio_trigger.h>
#include <iio_trigger_private.h>
#include <zephyr/rtio/rtio.h>
#include <zephyr/drivers/adc.h>

LOG_MODULE_REGISTER(iio_trigger_private, CONFIG_LIBIIO_LOG_LEVEL);

/**
 * @brief Create and attach a trigger device to an IIO device.
 *
 * Shared helper used by iio,sensor and iio,io-channels drivers.
 * Reads trigger type and id from the trigger Zephyr device config,
 * creates the IIO trigger device, associates it with @p iio_device,
 * and calls iio_trigger_init().
 *
 * @param ctx         IIO context.
 * @param iio_device  IIO device to attach the trigger to.
 * @param trigger_dev Zephyr device that implements the iio_trigger API.
 *                    May be NULL, in which case -ENODEV is returned.
 * @return 0 on success, negative errno on failure.
 */
int iio_trigger_attach(struct iio_context *ctx,
				     struct iio_device *iio_device,
				     const struct device *trigger_dev)
{
	struct iio_device *trig;
	int ret;

	if (!trigger_dev) {
		return -ENODEV;
	}

	trig = iio_trigger_create(ctx, trigger_dev);
	if (!trig) {
		return -ENOMEM;
	}

	ret = iio_device_set_trigger(iio_device, trig);
	if (ret < 0) {
		return ret;
	}

	iio_trigger_init(trigger_dev);
	return 0;
}

/**
 * @brief Initialise the common stream trigger data.
 *
 * Must be called once from the driver's init callback before any
 * subscribe/unsubscribe calls.  Idempotent – subsequent calls while
 * @p common->inited is true are silently ignored.
 *
 * Initialises the mutex, the work item, and starts the dedicated work
 * queue thread that will process incoming RTIO completion events.
 *
 * @param common          Pointer to the common stream data embedded in the
 *                        driver-specific data struct.
 * @param stack_size      Stack size in bytes for the work queue thread.
 * @param priority        Thread priority for the work queue thread.
 * @param work_handler_cb Work handler callback invoked by the work queue
 *                        when a stream RTIO CQE is ready to be processed.
 */
void iio_trigger_stream_init(struct iio_device_trigger_common_stream_data *common, unsigned stack_size,
	unsigned priority, void (*work_handler_cb)(struct k_work *w))
{
	if (common->inited) {
		return;
	}

	common->subscriber = NULL;
	common->inited = true;

	k_mutex_init(&common->lock);
	k_work_init(&common->work, work_handler_cb);
	
	k_work_queue_init(&common->trig_wq);
	k_work_queue_start(&common->trig_wq, common->trig_wq_stack, stack_size, priority, NULL);
}

/**
 * @brief Create and register the IIO trigger device inside an IIO context.
 *
 * Shared helper for stream trigger drivers.  Adds a new IIO device with
 * the given @p id and the name stored in @p common->name, then stores
 * the Zephyr device pointer as the IIO device's private data so that
 * driver callbacks can recover it via iio_device_get_pdata().
 *
 * @param ctx    IIO context to add the trigger device to.
 * @param dev    Zephyr device struct for IIO trigger.
 * @param common Common stream data whose @p name field is used as the
 *               IIO device name.
 * @return Pointer to the newly created IIO device, or NULL on failure.
 */
 struct iio_device * iio_trigger_stream_create(struct iio_context *ctx, const struct device *dev,
										struct iio_device_trigger_common_stream_data *common)
{
	struct iio_device *trig;
	struct iio_device_trigger_config *trig_config = (struct iio_device_trigger_config *)dev->config;

	trig = iio_context_add_device(ctx, trig_config->trigger_id, trig_config->name, NULL);
	if (!trig) {
		LOG_ERR("Could not add trigger device.");
		return NULL;
	}

	iio_device_set_pdata(trig, (struct iio_device_pdata *) dev);

	return trig;
}

/**
 * @brief Drain all pending RTIO completion queue entries.
 *
 * Consumes and releases every CQE currently queued in the RTIO context
 * associated with @p common.  Any mempool buffer attached to a CQE is
 * also released.  Used to flush stale data after a stream is cancelled
 * or before a new stream is started.
 *
 * @param common Common stream data whose @p rtio_ctx is drained.
 */
static void iio_trigger_stream_drain_cqe(struct iio_device_trigger_common_stream_data *common)
{
	struct rtio_cqe *stale_cqe;
	uint8_t *stale_buf;
	uint32_t stale_len;

	while ((stale_cqe = rtio_cqe_consume(common->rtio_ctx)) != NULL) {
		/* Only release the mempool buffer when the executor has not
		 * already released it on error/cancel (rtio_executor.c).
		 * Checking result == 0 avoids the double-free on cancelled CQEs.
		 */
		if (stale_cqe->result == 0) {
			if (rtio_cqe_get_mempool_buffer(common->rtio_ctx, stale_cqe,
						&stale_buf, &stale_len) == 0) {
				rtio_release_buffer(common->rtio_ctx, stale_buf, stale_len);
			}
		}
		rtio_cqe_release(common->rtio_ctx, stale_cqe);
	}
}

/**
 * @brief Subscribe a buffer node to a stream trigger.
 *
 * Records @p node as the sole active subscriber, drains any stale CQEs
 * left over from a previous session, then starts streaming by calling
 * the driver-supplied @p stream callback (e.g. adc_stream() or
 * sensor_stream()).
 *
 * Returns -EBUSY if a subscriber is already registered.
 *
 * @param node   IIO trigger node embedded in the buffer pdata that is
 *               requesting the stream.
 * @param common Common stream data for this trigger instance.
 * @param stream Driver-specific function used to start the RTIO stream
 *               (signature matches adc_stream / sensor_stream).
 * @return 0 on success, -EBUSY if already subscribed.
 */
int iio_trigger_stream_subscribe(struct iio_trigger_node *node,
	struct iio_device_trigger_common_stream_data * common,
	int (*stream)(const struct rtio_iodev *iodev, struct rtio *ctx, void *userdata,
		struct rtio_sqe **handle))
{
	int ret = 0;

	k_mutex_lock(&common->lock, K_FOREVER);

	if (common->subscriber != NULL) {
		k_mutex_unlock(&common->lock);
		return -EBUSY;
	}

	common->subscriber = node;

	iio_trigger_stream_drain_cqe(common);

	ret = stream(common->iodev, common->rtio_ctx, NULL, &common->handle);

	k_mutex_unlock(&common->lock);

	return ret;
}

/**
 * @brief Unsubscribe a buffer node from a stream trigger.
 *
 * Cancels the outstanding RTIO SQE, clears the subscriber pointer, and
 * drains any CQEs that the hardware may still produce after cancellation.
 * Asserts that @p node matches the currently registered subscriber.
 *
 * @param node   IIO trigger node that is unsubscribing.
 * @param common Common stream data for this trigger instance.
 */
void iio_trigger_stream_unsubscribe(struct iio_trigger_node *node,
	struct iio_device_trigger_common_stream_data *common)
{
	k_mutex_lock(&common->lock, K_FOREVER);

	__ASSERT(common->subscriber == node, "Subscriber mismatch");

	if (common->subscriber == node) {
		rtio_sqe_cancel(common->handle);
		common->handle = NULL;
		common->subscriber = NULL;

		iio_trigger_stream_drain_cqe(common);
	}

	k_mutex_unlock(&common->lock);
}

/**
 * @brief Submit a work item to the trigger's work queue.
 *
 * Called from the trigger's submit_work driver callback when the IIO
 * core requests that the next block of stream data be produced.  The
 * work item executes the driver's work handler on the dedicated trigger
 * work queue thread.
 *
 * Asserts that @p node matches the currently registered subscriber and
 * is a no-op if it does not.
 *
 * @param node   IIO trigger node requesting work submission.
 * @param common Common stream data for this trigger instance.
 */
void iio_trigger_stream_submit_work(struct iio_trigger_node *node,
	struct iio_device_trigger_common_stream_data *common)
{
	k_mutex_lock(&common->lock, K_FOREVER);

	__ASSERT(common->subscriber == node, "Subscriber mismatch");

	if (common->subscriber == node) {
		k_work_submit_to_queue(&common->trig_wq, &common->work);
	}

	k_mutex_unlock(&common->lock);
}

/**
 * @brief Fetch the next pending IIO block from the buffer's work queue.
 *
 * When @p get_block is true, dequeues the next @ref iio_block_pdata from
 * @p iio_buf's pending-blocks list and computes the layout parameters
 * (bytes per channel, number of samples, output pointer) that the caller's
 * work handler needs to fill the block with decoded samples.  When
 * @p get_block is false the function is a no-op and returns 0, allowing the
 * caller to reuse the current block across multiple RTIO CQEs.
 *
 * @param get_block              True to dequeue and set up a new block; false to skip.
 * @param iio_buf                IIO buffer pdata whose pending-blocks list is queried.
 * @param[out] num_samples       Number of samples that fit in the block.
 * @param[out] out               Pointer to the start of the block's data buffer.
 * @param[out] blk               Pointer to the dequeued @ref iio_block_pdata.
 * @param channel_cnt            Number of enabled channels; forwarded to
 *                               @p get_bytes_per_sample for drivers that need it.
 * @param[out] bytes_per_channel Byte width of one sample for a single channel,
 *                               as written by @p get_bytes_per_sample.
 * @param[out] ptr_pos           Write offset within @p out; reset to 0 on a new block.
 * @param trig_data              Driver-private trigger data forwarded to
 *                               @p get_bytes_per_sample.
 * @param fmt                    IIO data format of the active scan element;
 *                               forwarded to @p get_bytes_per_sample.
 * @param get_bytes_per_sample   Driver callback that computes the total bytes per
 *                               sample and sets @p bytes_per_channel.
 * @return 0 on success, -1 if no pending block is available.
 */
int iio_trigger_stream_get_block(bool get_block, struct iio_buffer_pdata *iio_buf,
	size_t *num_samples, uint8_t **out, struct iio_block_pdata **blk,
	unsigned channel_cnt, size_t *bytes_per_channel, unsigned *ptr_pos, void *trig_data,
	const struct iio_data_format *fmt, size_t (*get_bytes_per_sample)(void *trig_data,
		const struct iio_data_format *fmt, size_t *bytes_per_channel))
{
	struct iio_trigger_node *block_n = NULL;
	size_t bytes_per_sample;

	if (get_block)
	{
		k_mutex_lock(&iio_buf->lock, K_FOREVER);

		block_n = iio_trigger_node_get(&iio_buf->pending_blocks);

		if (!block_n) {
			LOG_ERR("no pending blocks");
			k_mutex_unlock(&iio_buf->lock);
			return -1;
		}

		*blk = CONTAINER_OF(block_n, struct iio_block_pdata, node);

		bytes_per_sample = get_bytes_per_sample(trig_data, fmt, bytes_per_channel);

		size_t block_len = MIN((*blk)->bytes_used, (*blk)->size);
		*num_samples = block_len / bytes_per_sample;
		*out = (uint8_t *)(*blk)->data;
		*ptr_pos = 0;

		k_mutex_unlock(&iio_buf->lock);
	}

	return 0;
}

/**
 * @brief Serialise a single decoded sample value into the output buffer.
 *
 * Writes @p int_val as a @p bytes_per_channel -wide integer at @p out,
 * respecting the byte order specified by @p fmt->is_be.  The caller is
 * responsible for advancing the output pointer by @p bytes_per_channel
 * after this call.
 *
 * @param fmt               IIO data format describing bit-width and byte order.
 * @param out               Destination byte pointer (must have room for
 *                          @p bytes_per_channel bytes).
 * @param int_val           Decoded integer sample value to write.
 * @param bytes_per_channel Number of bytes to write.
 */
void iio_trigger_stream_write_ch_data(const struct iio_data_format *fmt, uint8_t *out,
	int32_t int_val, size_t bytes_per_channel)
{
	/* Apply format-declared shift */
	int_val >>= fmt->shift;

	/* Saturate to the declared bit width to avoid wrap-around */
	if (fmt->bits > 0 && fmt->bits < 32) {
		if (fmt->is_signed) {
			int32_t max_val = (int32_t)((1u << (fmt->bits - 1)) - 1u);
			int32_t min_val = -(int32_t)(1u << (fmt->bits - 1));
			int_val = CLAMP(int_val, min_val, max_val);
		} else {
			uint32_t max_val = (1u << fmt->bits) - 1u;
			int_val = (int32_t)MIN((uint32_t)int_val, max_val);
		}
	}

	if (fmt->is_be) {
		for (int i = bytes_per_channel - 1; i >= 0; i--) {
			*out++ = (uint8_t)((int_val >> (i * 8)) & 0xFF);
		}
	} else {
		for (size_t i = 0; i < bytes_per_channel; i++) {
			*out++ = (uint8_t)((int_val >> (i * 8)) & 0xFF);
		}
	}
}

/**
 * @brief Signal a popped block as complete.
 *
 * Records @p bytes_used and gives the block's ready semaphore.  Every
 * exit path in the work handler that holds a dequeued block must call
 * this (with bytes_used == 0 on error) so that dequeue_block never
 * waits forever on a block that will never be filled.
 *
 * @param block      Block to signal.
 * @param bytes_used Number of valid bytes written; 0 signals an error
 *                   to the waiter.
 */
static void stream_block_complete(struct iio_block_pdata *block, size_t bytes_used)
{
	block->bytes_used = bytes_used;
	k_sem_give(&block->ready_sem);
}

/**
 * @brief Shared RTIO stream work handler.
 *
 * See iio_trigger_private.h for full documentation.
 */
void iio_trigger_stream_work_handler_common(struct k_work *w,
	const struct iio_trigger_stream_ops *ops, void *trig_data)
{
	struct iio_device_trigger_common_stream_data *common =
		CONTAINER_OF(w, struct iio_device_trigger_common_stream_data, work);
	int rc;
	uint8_t scratch[IIO_TRIGGER_STREAM_SCRATCH_SIZE];
	uint32_t fit[IIO_TRIGGER_STREAM_MAX_CHANNELS];
	const void *decoder = NULL;
	const struct iio_data_format *fmt = NULL;
	struct rtio_cqe *cqe = NULL;
	struct iio_block_pdata *blk = NULL;
	uint32_t buf_len = 0;
	uint8_t *buf = NULL;
	uint8_t *out = NULL;
	bool done = false;
	bool get_block = true;
	size_t samples_written = 0;
	size_t bytes_per_channel = 0;
	size_t num_samples = 0;
	unsigned ptr_pos = 0;
	uint16_t min_frames;
	size_t channel_count = ops->get_channel_count(trig_data);

	k_mutex_lock(&common->lock, K_FOREVER);
	struct iio_trigger_node *sub = common->subscriber;
	k_mutex_unlock(&common->lock);

	if (sub == NULL) {
		return;
	}

	struct iio_buffer_pdata *iio_buf = CONTAINER_OF(sub, struct iio_buffer_pdata, node);

	while (!done) {
		cqe = rtio_cqe_consume_block(common->rtio_ctx);

		if (cqe->result != 0) {
			LOG_ERR("async read failed %d\n", cqe->result);
			goto rel_cqe;
		}

		rc = rtio_cqe_get_mempool_buffer(common->rtio_ctx, cqe, &buf, &buf_len);

		if (rc != 0) {
			LOG_ERR("get mempool buffer failed %d\n", rc);
			goto rel_cqe;
		}

		rtio_cqe_release(common->rtio_ctx, cqe);
		cqe = NULL;

		k_mutex_lock(&common->lock, K_FOREVER);
		if (common->subscriber != sub) {
			k_mutex_unlock(&common->lock);
			goto rel_buf;
		}
		k_mutex_unlock(&common->lock);

		if (!iio_buf->enabled) {
			goto rel_buf;
		}

		rc = ops->get_decoder(trig_data, &decoder);

		if (rc != 0) {
			LOG_ERR("get_decoder failed %d\n", rc);
			goto rel_buf;
		}

		fmt = ops->get_fmt(common, trig_data, 0);

		if (fmt == NULL) {
			LOG_ERR("get_fmt failed\n");
			goto rel_buf;
		}

		rc = iio_trigger_stream_get_block(get_block, iio_buf, &num_samples, &out, &blk,
			channel_count, &bytes_per_channel, &ptr_pos, trig_data, fmt,
			ops->get_bytes_per_sample);

		if (rc != 0) {
			LOG_ERR("get_block failed %d\n", rc);
			goto rel_buf;
		}

		memset(fit, 0, channel_count * sizeof(fit[0]));

		rc = ops->get_min_frame_count(trig_data, decoder, buf, buf_len, &min_frames);

		if (rc != 0) {
			LOG_ERR("get_min_frame_count failed %d\n", rc);
			goto rel_buf;
		}

		if (min_frames == 0) {
			get_block = false;
			goto rel_buf;
		}

		for (int i = 0; i < min_frames; i++) {
			bool frame_valid = true;

			for (size_t j = 0; j < channel_count; j++) {
				const struct iio_data_format *ch_fmt;
				size_t ch_bpc;

				ch_fmt = ops->get_fmt(common, trig_data, j);
				if (ch_fmt == NULL) {
					ch_fmt = fmt;
				}
				ch_bpc = DIV_ROUND_UP(ch_fmt->length, BITS_PER_BYTE);

				memset(scratch, 0, sizeof(scratch));
				rc = ops->decode_channel(trig_data, decoder, buf, j,
					&fit[j], scratch);

				if (ops->check_decode_return(rc) < 0) {
					LOG_ERR("decode_channel failed %d\n", rc);
					frame_valid = false;
					break;
				}

				ops->pack_channel(trig_data, j, scratch, ch_fmt, out,
					ch_bpc, &ptr_pos);
			}

			if (!frame_valid) {
				stream_block_complete(blk, 0);
				blk = NULL;
				done = true;
				break;
			}

			samples_written++;

			if (samples_written >= num_samples) {
				done = true;

				k_mutex_lock(&iio_buf->lock, K_FOREVER);
				bool has_more =
					!iio_trigger_list_is_empty(&iio_buf->pending_blocks);
				k_mutex_unlock(&iio_buf->lock);

				if (has_more) {
					done = false;
					samples_written = 0;
					get_block = true;
				}

				stream_block_complete(blk, ptr_pos);
				blk = NULL;
				break;
			} else {
				get_block = false;
			}
		}

		rtio_release_buffer(common->rtio_ctx, buf, buf_len);
		buf = NULL;
	}

	return;

rel_cqe:
	if (cqe) {
		rtio_cqe_release(common->rtio_ctx, cqe);
	}
	if (blk) {
		stream_block_complete(blk, 0);
	}
	return;
rel_buf:
	if (buf) {
		rtio_release_buffer(common->rtio_ctx, buf, buf_len);
	}
	if (blk) {
		stream_block_complete(blk, 0);
	}
}
