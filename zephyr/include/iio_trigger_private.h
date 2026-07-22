/*
 * Copyright (c) 2026 Analog Devices, Inc.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef ZEPHYR_INCLUDE_IIO_TRIGGER_PRIVATE_H_
#define ZEPHYR_INCLUDE_IIO_TRIGGER_PRIVATE_H_

#include <zephyr/device.h>
#include <iio_device.h>

#ifdef __cplusplus
extern "C" {
#endif

struct iio_device_trigger_common_stream_data {
	struct k_work work;
	struct k_mutex lock;
	bool inited;
	struct iio_trigger_node *subscriber;
	struct rtio_iodev *iodev;
	struct rtio *rtio_ctx;
	struct rtio_sqe *handle;
	k_thread_stack_t *trig_wq_stack;
	struct k_work_q trig_wq;
};

int iio_trigger_attach(struct iio_context *ctx,
				     struct iio_device *iio_device,
				     const struct device *trigger_dev);

void iio_trigger_stream_init(struct iio_device_trigger_common_stream_data *common, unsigned stack_size,
	unsigned priority, void (*work_handler_cb)(struct k_work *w));

struct iio_device * iio_trigger_stream_create(struct iio_context *ctx, const struct device *dev,
										struct iio_device_trigger_common_stream_data *common);

int iio_trigger_stream_subscribe(struct iio_trigger_node *node,
	struct iio_device_trigger_common_stream_data * common,
	int (*stream)(const struct rtio_iodev *iodev, struct rtio *ctx, void *userdata,
		struct rtio_sqe **handle));

void iio_trigger_stream_unsubscribe(struct iio_trigger_node *node,
	struct iio_device_trigger_common_stream_data *common);

void iio_trigger_stream_submit_work(struct iio_trigger_node *node,
	struct iio_device_trigger_common_stream_data *common);

void iio_trigger_stream_write_ch_data(const struct iio_data_format *fmt, uint8_t *out,
	int32_t int_val, size_t bytes_per_channel);

int iio_trigger_stream_get_block(bool get_block, struct iio_buffer_pdata *iio_buf,
	size_t *num_samples, uint8_t **out, struct iio_block_pdata **blk,
	unsigned channel_cnt, size_t *bytes_per_channel, unsigned *ptr_pos, void *trig_data,
	const struct iio_data_format *fmt, size_t (*get_bytes_per_sample)(void *trig_data,
		const struct iio_data_format *fmt, size_t *bytes_per_channel));

/** Maximum number of DT channels a stream trigger instance can handle. */
#define IIO_TRIGGER_STREAM_MAX_CHANNELS	8

/** Scratch buffer size for one decoded frame (large enough for sensor XYZ union). */
#define IIO_TRIGGER_STREAM_SCRATCH_SIZE	64

/**
 * @brief Per-driver vtable for the shared RTIO stream work handler.
 *
 * Implement one of these per stream trigger driver and pass it to
 * iio_trigger_stream_work_handler_common().  All callbacks receive the
 * driver-specific trig_data pointer as their first argument.
 */
struct iio_trigger_stream_ops {
	/** Return the number of DT channels configured for this trigger. */
	size_t (*get_channel_count)(void *trig_data);

	/**
	 * Obtain the driver-specific decoder object.
	 * @return 0 on success, negative errno on failure.
	 */
	int (*get_decoder)(void *trig_data, const void **decoder);

	/**
	 * Return the iio_data_format for channel @p ch_idx.
	 * @return Pointer to format, or NULL if channel has no format.
	 */
	const struct iio_data_format *(*get_fmt)(
		struct iio_device_trigger_common_stream_data *common,
		void *trig_data, size_t ch_idx);

	/**
	 * Compute total bytes per sample; also writes *bytes_per_channel.
	 * Signature matches the callback accepted by iio_trigger_stream_get_block().
	 */
	size_t (*get_bytes_per_sample)(void *trig_data,
		const struct iio_data_format *fmt, size_t *bytes_per_channel);

	/**
	 * Get the minimum available frame count across all channels.
	 * @return 0 on success, negative errno on failure.
	 */
	int (*get_min_frame_count)(void *trig_data, const void *decoder,
		uint8_t *buf, uint32_t buf_len, uint16_t *min_frames);

	/**
	 * Decode one frame for channel @p ch_idx into @p scratch
	 * (IIO_TRIGGER_STREAM_SCRATCH_SIZE bytes, already zeroed).
	 * @return > 0 (frames decoded) on success, <= 0 on failure.
	 */
	int (*decode_channel)(void *trig_data, const void *decoder,
		uint8_t *buf, size_t ch_idx, uint32_t *fit, void *scratch);

	/**
	 * Pack the decoded value from @p scratch into the IIO output block.
	 */
	void (*pack_channel)(void *trig_data, size_t ch_idx,
		const void *scratch, const struct iio_data_format *fmt,
		uint8_t *out, size_t bytes_per_channel, unsigned *ptr_pos);

	int (*check_decode_return)(int rc);
};

/**
 * @brief Shared RTIO stream work handler.
 *
 * Implements the full CQE-consume → get-decoder → get-block →
 * decode-frames → pack-channels → signal-block loop via the driver
 * vtable.  Fixes the decode return-value check (rc <= 0 is an error)
 * and ensures fmt == NULL is always guarded.
 *
 * Each stream trigger driver's k_work handler should be a thin wrapper:
 * @code
 *   static void my_work_handler(struct k_work *w) {
 *       struct ... *common = CONTAINER_OF(w, ..., work);
 *       struct ... *trig_data = CONTAINER_OF(common, ..., common);
 *       iio_trigger_stream_work_handler_common(w, &my_ops, trig_data);
 *   }
 * @endcode
 *
 * @param w         Work item embedded in iio_device_trigger_common_stream_data.
 * @param ops       Driver-specific vtable.
 * @param trig_data Driver-specific data struct (contains the common struct).
 */
void iio_trigger_stream_work_handler_common(struct k_work *w,
	const struct iio_trigger_stream_ops *ops, void *trig_data);

#ifdef __cplusplus
}
#endif

#endif  /* ZEPHYR_INCLUDE_IIO_TRIGGER_PRIVATE_H_ */
