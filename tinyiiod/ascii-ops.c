// SPDX-License-Identifier: TODO
/*
 * libiio - Library for interfacing industrial I/O (IIO) devices
 *
 * ASCII (libiio v0.x) protocol command handlers for the Zephyr iiod server.
 *
 * Copyright (C) 2026 Analog Devices, Inc.
 */

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "iio/iio.h"

#include "../iiod/ops.h"
#include "../iiod/parser.h"

int yyparse(yyscan_t scanner);

/* Free all per-connection ASCII streaming state (see struct ascii_pdata). */
static void ascii_pdata_free(struct parser_pdata *pdata);

/* Identical to print_value() in ops.c. */
static void print_value(struct parser_pdata *pdata, long value)
{
	char buf[128];
	snprintf(buf, sizeof(buf), "%li\n", value);
	output(pdata, buf);
}

/* Identical to iiod_htobe32() in ops.c. */
static inline uint32_t iiod_htobe32(uint32_t word)
{
#if __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
	return word;
#elif defined(__GNUC__)
	return __builtin_bswap32(word);
#else
	return ((word & 0xff) << 24) | ((word & 0xff00) << 8) | ((word >> 8) & 0xff00) |
	       ((word >> 24) & 0xff);
#endif
}

/* Identical to iiod_be32toh() in ops.c. */
static inline uint32_t iiod_be32toh(uint32_t word)
{
	return iiod_htobe32(word);
}

#define READ_ATTR_BUF_SIZE 1024

typedef struct iio_attr *(*rw_attr_cb_t)(const void *, unsigned int);

/* Identical to get_mask() in ops.c. Parses the v0 channel mask, a big-endian
 * sequence of 8-hex-digit words, into the little-endian words[] array. Defined
 * here because ops.c (where the original is static) is not part of the Zephyr
 * build. */
static void get_mask(const char *mask, size_t len, uint32_t *words)
{
	size_t nb = (len + 7) / 8;
	uint32_t *ptr = words + nb;
	char buf[9];

	while (*mask) {
		snprintf(buf, sizeof(buf), "%.*s", 8, mask);
		sscanf(buf, "%08x", --ptr);
		mask += 8;
	}
}

/* Identical to buffer_analyze() in ops.c. */
static int buffer_analyze(unsigned int nb, const char *src, size_t len)
{
	while (nb--) {
		int32_t val;

		if (len < 4)
			return -EINVAL;

		val = (int32_t)iiod_be32toh(*(uint32_t *)src);
		src += 4;
		len -= 4;

		if (val > 0) {
			if ((uint32_t)val > len)
				return -EINVAL;

			/* Align the length to 4 bytes */
			if (val & 3)
				val = ((val >> 2) + 1) << 2;
			len -= val;
			src += val;
		}
	}

	/* We should have analyzed the whole buffer by now */
	return !len ? 0 : -EINVAL;
}

/* Identical to read_each_attr() in ops.c. */
static ssize_t read_each_attr(
		const void *iio, char *buf, size_t len, unsigned int nb, rw_attr_cb_t cb)
{
	const struct iio_attr *attr;
	unsigned int i;
	char *ptr = buf;
	ssize_t ret;

	for (i = 0; len >= 4 && i < nb; i++) {
		attr = (*cb)(iio, i);
		if (!attr)
			ret = -ENOENT;
		else
			ret = iio_attr_read_raw(attr, ptr + 4, len - 4);
		*(uint32_t *)ptr = iiod_htobe32(ret);

		/* Align the length to 4 bytes */
		ret = ret < 0 ? 0 : (ret + 3) & ~0x3;
		ptr += 4 + ret;
		len -= 4 + ret;
	}

	return ptr - buf;
}

/* Identical to write_each_attr() in ops.c. */
static ssize_t write_each_attr(
		const void *iio, const char *buf, size_t len, unsigned int nb, rw_attr_cb_t cb)
{
	const struct iio_attr *attr;
	const char *ptr = buf;
	unsigned int i;
	ssize_t ret;
	int32_t val;

	ret = buffer_analyze(nb, buf, len);
	if (ret < 0)
		return ret;

	for (i = 0; i < nb; i++) {
		val = (int32_t)iiod_be32toh(*(uint32_t *)ptr);
		ptr += 4;

		if (val > 0) {
			attr = (*cb)(iio, i);
			if (!attr)
				continue;

			iio_attr_write_raw(attr, ptr, val);

			/* Align the length to 4 bytes */
			ptr += (val + 3) & ~0x3;
		}
	}

	return ptr - buf;
}

/* Functionally identical to read_dev_attr() in ops.c, except the value buffer
 * is heap-allocated (READ_ATTR_BUF_SIZE) instead of a large on-stack array */
ssize_t read_dev_attr(struct parser_pdata *pdata, struct iio_device *dev, const char *name,
		enum iio_attr_type type)
{
	const struct iio_attr *attr;
	struct iio_buffer *buffer;
	char *buf;
	ssize_t ret = -EINVAL;
	unsigned int nb;

	if (!dev) {
		print_value(pdata, -ENODEV);
		return -ENODEV;
	}

	buf = malloc(READ_ATTR_BUF_SIZE);
	if (!buf) {
		print_value(pdata, -ENOMEM);
		return -ENOMEM;
	}

	if (!name) {
		switch (type) {
		case IIO_ATTR_TYPE_DEVICE:
			nb = iio_device_get_attrs_count(dev);
			ret = read_each_attr(dev, buf, READ_ATTR_BUF_SIZE - 1, nb,
					(rw_attr_cb_t)iio_device_get_attr);
			break;
		case IIO_ATTR_TYPE_DEBUG:
			nb = iio_device_get_debug_attrs_count(dev);
			ret = read_each_attr(dev, buf, READ_ATTR_BUF_SIZE - 1, nb,
					(rw_attr_cb_t)iio_device_get_debug_attr);
			break;
		default:
			ret = -EINVAL;
			goto out_free_buffer;
		}

		goto out_print_value;
	}

	switch (type) {
	case IIO_ATTR_TYPE_DEVICE:
		attr = iio_device_find_attr(dev, name);
		if (attr)
			ret = iio_attr_read_raw(attr, buf, READ_ATTR_BUF_SIZE - 1);
		else
			ret = -ENOENT;
		break;
	case IIO_ATTR_TYPE_DEBUG:
		attr = iio_device_find_debug_attr(dev, name);
		if (attr)
			ret = iio_attr_read_raw(attr, buf, READ_ATTR_BUF_SIZE - 1);
		else
			ret = -ENOENT;
		break;
	case IIO_ATTR_TYPE_BUFFER:
		buffer = iio_device_get_buffer(dev, 0);
		if (buffer) {
			attr = iio_buffer_find_attr(buffer, name);
			if (attr)
				ret = iio_attr_read_raw(attr, buf, READ_ATTR_BUF_SIZE - 1);
			else
				ret = -ENOENT;
		} else {
			ret = -EBADF;
		}
		break;
	default:
		ret = -EINVAL;
		break;
	}

out_print_value:
	print_value(pdata, ret);
	if (ret < 0)
		goto out_free_buffer;

	buf[ret] = '\n';
	ret = write_all(pdata, buf, ret + 1);

out_free_buffer:
	free(buf);
	return ret;
}

/* Functionally identical to write_dev_attr() in ops.c. Differs only on the
 * unsupported-attr-type path, where the allocated buffer is freed (via goto)
 * instead of returned past, avoiding a leak. TODO - check supposed leak */
ssize_t write_dev_attr(struct parser_pdata *pdata, struct iio_device *dev, const char *name,
		size_t len, enum iio_attr_type type)
{
	const struct iio_attr *attr;
	struct iio_buffer *buffer;
	unsigned int nb;
	ssize_t ret = -ENOMEM;
	char *buf;

	if (!dev) {
		ret = -ENODEV;
		goto out_print_value;
	}

	buf = malloc(len);
	if (!buf)
		goto out_print_value;

	ret = read_all(pdata, buf, len);
	if (ret < 0)
		goto out_free_buffer;

	if (!name) {
		switch (type) {
		case IIO_ATTR_TYPE_DEVICE:
			nb = iio_device_get_attrs_count(dev);
			ret = write_each_attr(
					dev, buf, len - 1, nb, (rw_attr_cb_t)iio_device_get_attr);
			break;
		case IIO_ATTR_TYPE_DEBUG:
			nb = iio_device_get_debug_attrs_count(dev);
			ret = write_each_attr(dev, buf, len - 1, nb,
					(rw_attr_cb_t)iio_device_get_debug_attr);
			break;
		default:
			ret = -EINVAL;
			goto out_free_buffer;
		}

		goto out_free_buffer;
	}

	switch (type) {
	case IIO_ATTR_TYPE_DEVICE:
		attr = iio_device_find_attr(dev, name);
		if (attr)
			ret = iio_attr_write_raw(attr, buf, len);
		else
			ret = -ENOENT;
		break;
	case IIO_ATTR_TYPE_DEBUG:
		attr = iio_device_find_debug_attr(dev, name);
		if (attr)
			ret = iio_attr_write_raw(attr, buf, len);
		else
			ret = -ENOENT;
		break;
	case IIO_ATTR_TYPE_BUFFER:
		buffer = iio_device_get_buffer(dev, 0);
		if (buffer) {
			attr = iio_buffer_find_attr(buffer, name);
			if (attr)
				ret = iio_attr_write_raw(attr, buf, len);
			else
				ret = -ENOENT;
		} else {
			ret = -EBADF;
		}
		break;
	default:
		ret = -EINVAL;
		break;
	}

out_free_buffer:
	free(buf);
out_print_value:
	print_value(pdata, ret);
	return ret;
}

/* Functionally identical to read_chn_attr() in ops.c, except the value buffer
 * is heap-allocated (READ_ATTR_BUF_SIZE) instead of a large on-stack array */
ssize_t read_chn_attr(struct parser_pdata *pdata, struct iio_channel *chn, const char *name)
{
	char *buf;
	ssize_t ret = -ENODEV;
	const struct iio_attr *attr;
	unsigned int nb;

	if (!chn) {
		ret = pdata->dev ? -ENXIO : -ENODEV;
		print_value(pdata, ret);
		return ret;
	}

	buf = malloc(READ_ATTR_BUF_SIZE);
	if (!buf) {
		print_value(pdata, -ENOMEM);
		return -ENOMEM;
	}

	if (!name) {
		nb = iio_channel_get_attrs_count(chn);
		ret = read_each_attr(chn, buf, READ_ATTR_BUF_SIZE - 1, nb,
				(rw_attr_cb_t)iio_channel_get_attr);
	} else {
		attr = iio_channel_find_attr(chn, name);
		if (attr)
			ret = iio_attr_read_raw(attr, buf, READ_ATTR_BUF_SIZE - 1);
		else
			ret = -ENOENT;
	}

	print_value(pdata, ret);
	if (ret < 0)
		goto out_free_buffer;

	buf[ret] = '\n';
	ret = write_all(pdata, buf, ret + 1);

out_free_buffer:
	free(buf);
	return ret;
}

/* Identical to write_chn_attr() in ops.c. */
ssize_t write_chn_attr(
		struct parser_pdata *pdata, struct iio_channel *chn, const char *name, size_t len)
{
	const struct iio_attr *attr;
	ssize_t ret = -ENOMEM;
	unsigned int nb;
	char *buf;

	buf = malloc(len);
	if (!buf)
		goto err_print_value;

	ret = read_all(pdata, buf, len);
	if (ret < 0)
		goto err_free_buffer;

	if (!chn) {
		ret = pdata->dev ? -ENXIO : -ENODEV;
		goto err_free_buffer;
	}

	if (!name) {
		nb = iio_channel_get_attrs_count(chn);
		ret = write_each_attr(
				chn, buf, sizeof(buf) - 1, nb, (rw_attr_cb_t)iio_channel_get_attr);
	} else {
		attr = iio_channel_find_attr(chn, name);
		if (attr)
			ret = iio_attr_write_raw(attr, buf, len);
		else
			ret = -ENOENT;
	}

err_free_buffer:
	free(buf);
err_print_value:
	print_value(pdata, ret);
	return ret;
}

/* Identical to set_trigger() in ops.c. */
ssize_t set_trigger(struct parser_pdata *pdata, struct iio_device *dev, const char *trigger)
{
	struct iio_device *trig = NULL;
	ssize_t ret = -ENOENT;

	if (!dev) {
		ret = -ENODEV;
		goto err_print_value;
	}

	if (trigger) {
		trig = iio_context_find_device(pdata->ctx, trigger);
		if (!trig)
			goto err_print_value;
	}

	ret = iio_device_set_trigger(dev, trig);
err_print_value:
	print_value(pdata, ret);
	return ret;
}

/* Identical to get_trigger() in ops.c. */
ssize_t get_trigger(struct parser_pdata *pdata, struct iio_device *dev)
{
	const struct iio_device *trigger;
	ssize_t ret;

	if (!dev) {
		print_value(pdata, -ENODEV);
		return -ENODEV;
	}

	trigger = iio_device_get_trigger(dev);
	ret = iio_err(trigger);
	if (!ret) {
		const char *name = iio_device_get_name(trigger);
		char buf[256];

		ret = strlen(name);
		print_value(pdata, ret);

		snprintf(buf, sizeof(buf), "%s\n", name);
		ret = write_all(pdata, buf, ret + 1);
	} else {
		print_value(pdata, ret);
	}
	return ret;
}

/* Identical to set_timeout() in ops.c. */
int set_timeout(struct parser_pdata *pdata, int timeout)
{
	int translated_timeout = timeout;
	int ret;

	/* Translate v0 client timeout semantics to v1 semantics:
	 * - v0 clients (non-binary) use: 0 = infinite, positive = timeout
	 * - v1 clients (binary) use: -1 = infinite, 0 = backend default, positive = timeout
	 */
	if (!pdata->binary && timeout == 0) {
		/* v0 client sending 0 (infinite) -> translate to v1 infinite (-1) */
		translated_timeout = -1;
	}

	ret = iio_context_set_timeout(pdata->ctx, translated_timeout);
	print_value(pdata, ret);
	return ret;
}

/*
 * Per-connection streaming state for the ASCII (v0.x) protocol.
 *
 * The upstream POSIX server (ops.c) keeps its equivalent state on the device
 * via iio_device_get_data(), spins up a dedicated rw_thd() per device, and lets
 * several clients share one buffer. None of that fits Zephyr: the backend
 * already uses the device userdata slot to store the trigger, and there is one
 * ASCII connection at a time driving a synchronous backend. So the state is
 * anchored on the connection (pdata->ascii_pdata) and the transfer is driven
 * inline on the connection thread using the iio_stream helper (stream.c), which
 * is exactly the single-threaded block enqueue/dequeue loop the Zephyr backend
 * expects.
 */
struct ascii_dev_stream {
	struct iio_device *dev;
	struct iio_stream *stream;
	struct iio_channels_mask *mask;
	size_t sample_size;
	bool cyclic;
	/* Re-send the channel mask on the next READBUF, mirroring the
	 * "new_client" flag in ops.c's send_data(): the v0 client expects the
	 * mask line once per READBUF/WRITEBUF command. */
	bool send_mask;
};

/* Default number of blocks, matching the POSIX server (iiod.c). Overridable
 * per device by the "SET <dev> BUFFERS_COUNT <n>" command before OPEN. */
#define ASCII_DEFAULT_NB_BLOCKS 4

struct ascii_pdata {
	struct ascii_dev_stream dev_stream;
	unsigned int nb_blocks;
};

static struct ascii_pdata *ascii_pdata_get(struct parser_pdata *pdata)
{
	struct ascii_pdata *ap = pdata->ascii_pdata;

	if (!ap) {
		ap = zalloc(sizeof(*ap));
		if (!ap)
			return NULL;

		ap->nb_blocks = ASCII_DEFAULT_NB_BLOCKS;
		pdata->ascii_pdata = ap;
	}

	return ap;
}

/* Tear down a device stream, if any is open. Safe to call when idle. */
static void ascii_dev_stream_close(struct ascii_dev_stream *ds)
{
	if (ds->stream) {
		iio_stream_destroy(ds->stream);
		ds->stream = NULL;
	}
	if (ds->mask) {
		iio_channels_mask_destroy(ds->mask);
		ds->mask = NULL;
	}
	ds->dev = NULL;
	ds->sample_size = 0;
	ds->cyclic = false;
	ds->send_mask = false;
}

/* Free all ASCII streaming state on the connection. Called at teardown. */
static void ascii_pdata_free(struct parser_pdata *pdata)
{
	struct ascii_pdata *ap = pdata->ascii_pdata;

	if (!ap)
		return;

		
	ascii_dev_stream_close(&ap->dev_stream);
	free(ap);
	pdata->ascii_pdata = NULL;
}

/* Send the current channel mask as an ASCII hex line, e.g. "00000003\n".
 * Identical framing to the "new_client" branch of send_data() in ops.c. */
static ssize_t send_mask(struct parser_pdata *pdata, struct iio_device *dev,
		const struct iio_channels_mask *mask)
{
	unsigned int i, nb_channels = iio_device_get_channels_count(dev);
	unsigned int nb_words = (nb_channels + 31) / 32;
	const struct iio_channel *chn;
	uint32_t *words;
	char buf[129], *ptr = buf;
	ssize_t length;

	words = calloc(nb_words, 4);
	if (!words)
		return -ENOMEM;

	for (i = 0; i < nb_channels; i++) {
		chn = iio_device_get_channel(dev, i);

		if (iio_channel_is_enabled(chn, mask))
			words[IIO_BIT_WORD(i)] |= IIO_BIT_MASK(i);
	}

	length = sizeof(buf);
	for (i = nb_words; i > 0 && ptr < buf + sizeof(buf); i--, ptr += 8) {
		snprintf(ptr, length, "%08x", words[i - 1]);
		length -= 8;
	}

	*ptr = '\n';
	length--;

	free(words);

	if (length < 0) {
		IIO_ERROR("send_mask: string length error\n");
		return -ENOSPC;
	}

	return write_all(pdata, buf, ptr + 1 - buf);
}

/* Functionally equivalent to set_buffers_count() in ops.c, but records the
 * count on the connection instead of the device userdata. Takes effect on the
 * next OPEN. */
int set_buffers_count(struct parser_pdata *pdata, struct iio_device *dev, long value)
{
	unsigned int nb = (unsigned int)value;
	struct ascii_pdata *ap;
	int ret = 0;

	if (value < 1) {
		ret = -EINVAL;
		goto err_print_value;
	}

	if (!dev) {
		ret = -ENODEV;
		goto err_print_value;
	}

	ap = ascii_pdata_get(pdata);
	if (!ap) {
		ret = -ENOMEM;
		goto err_print_value;
	}

	ap->nb_blocks = nb;

err_print_value:
	print_value(pdata, ret);
	return ret;
}

/*
 * Functionally equivalent to open_dev()/open_dev_helper() in ops.c, but driven
 * synchronously via the iio_stream helper rather than a dedicated rw thread.
 * The channel mask is parsed from the v0 hex string, a stream of nb_blocks
 * blocks is created, and the state is stashed on the connection for the
 * following READBUF/WRITEBUF/CLOSE commands.
 */
int open_dev(struct parser_pdata *pdata, struct iio_device *dev, size_t samples_count,
		const char *mask_str, bool cyclic)
{
	size_t nb_channels, nb_words, len;
	struct ascii_dev_stream *ds;
	struct iio_channels_mask *mask;
	struct iio_stream *stream;
	struct iio_buffer *buffer;
	struct ascii_pdata *ap;
	const struct iio_channel *chn;
	uint32_t *words = NULL;
	ssize_t sample_size;
	unsigned int i;
	int ret;

	if (!dev) {
		ret = -ENODEV;
		goto err_print_value;
	}

	ap = ascii_pdata_get(pdata);
	if (!ap) {
		ret = -ENOMEM;
		goto err_print_value;
	}

	ds = &ap->dev_stream;

	/* Only one open device per connection is supported. */
	if (ds->stream) {
		ret = -EBUSY;
		goto err_print_value;
	}

	nb_channels = iio_device_get_channels_count(dev);
	nb_words = (nb_channels + 31) / 32;
	len = strlen(mask_str);
	if (len != nb_words * 8) {
		ret = -EINVAL;
		goto err_print_value;
	}

	if (!samples_count) {
		ret = -EINVAL;
		goto err_print_value;
	}

	words = malloc(sizeof(*words) * nb_words);
	if (!words) {
		ret = -ENOMEM;
		goto err_print_value;
	}

	get_mask(mask_str, len, words);

	mask = iio_create_channels_mask(nb_channels);
	if (!mask) {
		ret = -ENOMEM;
		goto err_free_words;
	}

	for (i = 0; i < nb_channels; i++) {
		chn = iio_device_get_channel(dev, i);

		if (IIO_TEST_BIT(words, i))
			iio_channel_enable(chn, mask);
		else
			iio_channel_disable(chn, mask);
	}

	buffer = iio_device_get_buffer(dev, 0);
	if (!buffer) {
		ret = -ENODEV;
		goto err_free_mask;
	}

	sample_size = iio_device_get_sample_size(dev, mask);
	if (sample_size <= 0) {
		ret = sample_size < 0 ? (int)sample_size : -EINVAL;
		goto err_free_mask;
	}

	/* iio_buffer_create_stream() (via iio_buffer_open) copies the mask, so
	 * our copy is retained only to answer the READBUF mask query. */
	stream = iio_buffer_create_stream(buffer, ap->nb_blocks, samples_count, mask);
	ret = iio_err(stream);
	if (ret)
		goto err_free_mask;

	ds->dev = dev;
	ds->stream = stream;
	ds->mask = mask;
	ds->sample_size = (size_t)sample_size;
	ds->cyclic = cyclic;
	ds->send_mask = true;

	free(words);

	print_value(pdata, 0);
	return 0;

err_free_mask:
	iio_channels_mask_destroy(mask);
err_free_words:
	free(words);
err_print_value:
	print_value(pdata, ret);
	return ret;
}

/* Functionally equivalent to close_dev()/close_dev_helper() in ops.c. */
int close_dev(struct parser_pdata *pdata, struct iio_device *dev)
{
	struct ascii_pdata *ap = pdata->ascii_pdata;
	struct ascii_dev_stream *ds;
	int ret;

	if (!dev) {
		ret = -ENODEV;
		goto err_print_value;
	}

	if (!ap || !ap->dev_stream.stream || ap->dev_stream.dev != dev) {
		ret = -EBADF;
		goto err_print_value;
	}

	ds = &ap->dev_stream;
	ascii_dev_stream_close(ds);
	ret = 0;

err_print_value:
	print_value(pdata, ret);
	return ret;
}

/*
 * Functionally equivalent to rw_dev()/rw_buffer() in ops.c for the Zephyr
 * synchronous backend. For a read (READBUF): grab the next filled block, tell
 * the client how many bytes follow, (re)send the channel mask, then stream the
 * raw sample data. For a write (WRITEBUF): TX buffers are not yet wired up in
 * the Zephyr backend, so report -ENOSYS - matching the commented-out .writebuf
 * op in zephyr/backend.c.
 *
 * On the wire, READBUF replies with:
 *     <byte_count>\n <mask>\n <raw_sample_bytes>
 * exactly as the POSIX server's send_data() does.
 */
ssize_t rw_dev(struct parser_pdata *pdata, struct iio_device *dev, unsigned int nb, bool is_write)
{
	struct ascii_pdata *ap = pdata->ascii_pdata;
	struct ascii_dev_stream *ds;
	const struct iio_block *block;
	size_t remaining, block_len, chunk;
	ssize_t total = 0;
	void *start;
	ssize_t ret;

	if (!dev) {
		ret = -ENODEV;
		goto err_print_value;
	}

	if (!ap || !ap->dev_stream.stream || ap->dev_stream.dev != dev) {
		ret = -EBADF;
		goto err_print_value;
	}

	ds = &ap->dev_stream;

	/* Writing samples is not supported by the Zephyr backend yet, matching
	 * the commented-out .writebuf op in zephyr/backend.c. */
	if (is_write) {
		ret = -ENOSYS;
		goto err_print_value;
	}

	/* Only whole samples are transferred, like send_data() in ops.c. */
	remaining = nb - (nb % ds->sample_size);
	if (!remaining) {
		/* Too small to hold a single sample: nothing to transfer. */
		print_value(pdata, 0);
		return 0;
	}

	/* Re-send the channel mask on the first chunk of this READBUF. The
	 * POSIX server does the same by setting new_client=true on every
	 * rw_buffer() (ops.c); the v0 client reads the mask once per READBUF. */
	ds->send_mask = true;

	/*
	 * A single READBUF may span several blocks: the client sent one
	 * "READBUF <dev> <nb>" and then reads length-prefixed chunks until it
	 * has collected nb bytes (see iiod_client_read_unlocked()). Feed it one
	 * block per iteration until the request is satisfied.
	 */
	while (remaining >= ds->sample_size) {
		block = iio_stream_get_next_block(ds->stream);
		ret = iio_err(block);
		if (ret) {
			/* If nothing has been sent yet, the error code can still
			 * be delivered in-band as the chunk length. Otherwise the
			 * client is mid-transfer, so stop with a 0 terminator
			 * below. */
			if (total == 0)
				goto err_print_value;
			break;
		}

		start = iio_block_start(block);
		block_len = (char *)iio_block_end(block) - (char *)start;

		chunk = remaining < block_len ? remaining : block_len;
		chunk -= chunk % ds->sample_size;

		/* Byte count that follows. */
		print_value(pdata, (long)chunk);

		if (ds->send_mask) {
			ret = send_mask(pdata, dev, ds->mask);
			if (ret < 0)
				return ret;

			ds->send_mask = false;
		}

		ret = write_all(pdata, start, chunk);
		if (ret < 0)
			return ret;

		total += (ssize_t)chunk;
		remaining -= chunk;
	}

	/* If fewer than the requested nb bytes were delivered, tell the client
	 * to stop reading chunks (ops.c does the same with print_value(0)). */
	if (total > 0 && (size_t)total < (size_t)nb)
		print_value(pdata, 0);

	return total;

err_print_value:
	print_value(pdata, ret);
	return ret;
}

/*
 * Differs from invalidate_sample_size_cache() in ops.c: reimplemented as a
 * no-op. Called from responder.c (binary path) when a channel's format changes.
 * The ops.c version signals the per-device RW thread to recompute the sample
 * size; there is no such thread here, so nothing needs to be done. Still
 * provided because responder.c references it whenever WITH_IIOD_V0_COMPAT is
 * set, so the symbol must exist at link time.
 */
void invalidate_sample_size_cache(const struct iio_device *dev)
{
	(void)dev;
}

/* Differs from read_line() in ops.c: reimplemented with only the byte-at-a-time
 * path. ops.c also has a socket branch (recv() with MSG_PEEK/MSG_TRUNC) and a
 * USB bulk branch, both of which can read past the newline; that would corrupt
 * the ASCII->binary handoff on the Zephyr transports. Here we read one byte at
 * a time until the newline, so nothing past the line is ever consumed.
 *
 * On a transport error (e.g. -ESHUTDOWN when the client closes the pipe) the
 * connection is dead, so mark it stopped. That terminates the ascii_interpreter
 * loop and lets the per-connection interpreter thread exit instead of spinning
 * on a closed transport - which, on the USB backend, would otherwise leave a
 * stale thread racing the next connection's thread for the same pipe. The
 * upstream read_line() sets pdata->stop for the same reason on disconnect. */
ssize_t read_line(struct parser_pdata *pdata, char *buf, size_t len)
{
	size_t bytes_read = 0;
	bool found;

	while (len) {
		ssize_t ret = pdata->readfd(pdata, buf, 1);
		if (ret < 0) {
			pdata->stop = true;
			return ret;
		}

		bytes_read++;

		if (*buf == '\n')
			break;

		len--;
		buf++;
	}

	found = !!len;

	return found ? (ssize_t)bytes_read : -EIO;
}

/* Identical to enable_binary() in ops.c. */
void enable_binary(struct parser_pdata *pdata)
{
	pdata->binary = true;

	print_value(pdata, 0);
}

/* Differs from ascii_interpreter() in ops.c: the yylex/yyparse loop is the
 * same, but the trailing per-device cleanup loop (close_dev_helper() over every
 * device) is replaced by a single ascii_pdata_free(). The POSIX server keeps
 * per-device RW threads that must each be torn down; here all streaming state
 * lives on the connection (pdata->ascii_pdata), so one free tears down whatever
 * stream is still open when the client disconnects without a CLOSE. */
void ascii_interpreter(struct parser_pdata *pdata)
{
	yyscan_t scanner;
	int ret;

	yylex_init_extra(pdata, &scanner);

	do {
		ret = yyparse(scanner);
	} while (!pdata->stop && !pdata->binary && ret >= 0);

	yylex_destroy(scanner);

	ascii_pdata_free(pdata);
}
