/*
 * Host-side external backend for connecting to Zephyr IIO devices
 * This implements the "zephyr:" URI scheme and connects to Zephyr devices over serial
 */

#include <iio/iio-backend.h>
#include <iio/iio-debug.h>
#include <iio/iio-lock.h>
#include <iio/iiod-client.h>
#include "iiod-responder.h"

#include <ctype.h>
#include <errno.h>
#include <libserialport.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <Windows.h>
#else
#include <time.h>
#endif

/* ==================================================================== */
/*                           Data structures                            */
/* ==================================================================== */

struct iio_context_pdata {
    struct sp_port *port;
    struct iiod_client *iiod_client;
    struct iio_context_params params;
    bool shutdown;
};

struct iio_buffer_pdata {
    struct iiod_client_buffer_pdata *pdata;
};

struct p_options {
	char flag;
	enum sp_parity parity;
};

struct f_options {
	char flag;
	enum sp_flowcontrol flowcontrol;
};

static const struct p_options parity_options[] = {
	{'n', SP_PARITY_NONE},
	{'o', SP_PARITY_ODD},
	{'e', SP_PARITY_EVEN},
	{'m', SP_PARITY_MARK},
	{'s', SP_PARITY_SPACE},
	{'\0', SP_PARITY_INVALID},
};

static const struct f_options flow_options[] = {
	{'n', SP_FLOWCONTROL_NONE},
	{'x', SP_FLOWCONTROL_XONXOFF},
	{'r', SP_FLOWCONTROL_RTSCTS},
	{'d', SP_FLOWCONTROL_DTRDSR},
	{'\0', SP_FLOWCONTROL_NONE},
};

/* ==================================================================== */
/*                      Static functions declaration                    */
/* ==================================================================== */

static int zephyr_parse_options(const struct iio_context_params *params,
                    const char *options, unsigned int *baud_rate,
                    unsigned int *bits, unsigned int *stop,
                    enum sp_parity *parity, enum sp_flowcontrol *flow);

static struct iio_context* zephyr_create_context(
        const struct iio_context_params *params, const char *port_name,
        unsigned int baud_rate, unsigned int bits, unsigned int stop,
        enum sp_parity parity, enum sp_flowcontrol flow);        
        
        
static inline int libserialport_to_errno(enum sp_return ret);

static int apply_settings(struct sp_port *port, unsigned int baud_rate,
		unsigned int bits, unsigned int stop_bits,
		enum sp_parity parity, enum sp_flowcontrol flow);

static ssize_t zephyr_write_data(struct iiod_client_pdata *io_data,
				 const char *data, size_t len,
				 unsigned int timeout_ms);

static ssize_t zephyr_read_data(struct iiod_client_pdata *io_data,
				char *buf, size_t len, unsigned int timeout_ms);

static char parity_char(enum sp_parity pc);

static char flow_char(enum sp_flowcontrol fc);

static const struct iiod_client_ops zephyr_iiod_client_ops = {
	.write = zephyr_write_data,
	.read = zephyr_read_data,
};

/* ==================================================================== */
/*                      Static functions definition                     */
/* ==================================================================== */

extern struct iio_context *
zephyr_create_context_from_args(const struct iio_context_params *params, const char *args)
{
    char *comma, *uri_dup;
    unsigned int baud_rate, bits, stop;
    enum sp_parity parity;
    enum sp_flowcontrol flow;
    int ret;
    struct iio_context *ctx = NULL;

    uri_dup = iio_strdup(args);
    if (!uri_dup)
        return iio_ptr(-ENOMEM);

    comma = strchr(uri_dup, ',');
    if (comma) {
        ret = zephyr_parse_options(params, (char*)((uintptr_t) comma + 1),
                        &baud_rate, &bits, &stop, &parity, &flow);
    } else {
        ret = zephyr_parse_options(params, NULL, &baud_rate, &bits, 
                        &stop, &parity, &flow);
    }

    if (ret)
        goto err_free_dup;

    /* TODO different in serial.c, but for now i believe port_name should be equal to /dev/ttyACM0
        (string before comma), not the entire uri_dup, which would be all the args
        to be determined after testing the integration with the iio device */
    uri_dup[strlen(uri_dup) - strlen(comma)] = '\0';
    ctx = zephyr_create_context(params, uri_dup, baud_rate,
                    bits, stop, parity, flow);

    free(uri_dup);
    return ctx;

err_free_dup:
    printf("ERR\n");
    free(uri_dup);
    printf("Bad URI: \'zephyr:%s\'\n", args);
    prm_err(params, "Bad URI: \'zephyr:%s\'\n", args);
    return iio_ptr(-EINVAL);
}

static int zephyr_parse_options(const struct iio_context_params *params,
                    const char *options, unsigned int *baud_rate,
                    unsigned int *bits, unsigned int *stop,
                    enum sp_parity *parity, enum sp_flowcontrol *flow)
{
    char *end, ch;
    unsigned int i;

    /* Default settings */
    *baud_rate = 115200;
    *parity = 0; /* TODO */
    *bits = 8;
    *stop = 1;
    *flow = 0;

    if (!options || !options[0])
        return 0;

    /* Get baud rate */
    errno = 0;
    *baud_rate = strtoul(options, &end, 10);

    /* baud_rate in [110, 1 000 000] TODO baud_rate checked against another interval, despite comment in serial.c */
    if (options == end || errno == ERANGE || 
        *baud_rate < 110 || *baud_rate > 1000000) {
        prm_err(params, "Invalid baud rate\n");
        return -EINVAL;
    }

    /* Get number of bits */
    /* TODO in what case is the else branch reachable here */
    if (*end == ',')
        end++;

    if (!*end)
        return 0;

    options = (const char *)(end);

    errno = 0;
    *bits = strtoul(options, &end, 10);
    /* bits in [5, 9] */
    if (options == end || errno == ERANGE || *bits < 5 || *bits > 9) {
        prm_err(params, "Invalid number of bits\n");
        return -EINVAL;
    }

    /* Get parity */
    if (*end == ',')
        end++;
    
    if (*end == '\0')
        return 0;
    
    ch = (char)(tolower(*end) & 0xFF);
    for (i = 0; parity_options[i].flag != '\0'; i++) {
        if (ch == parity_options[i].flag) {
            *parity = parity_options[i].parity;
            break;
        }
    }

    if (parity_options[i].flag == '\0') {
        prm_err(params, "Invalid parity character\n");
        return -EINVAL;
    }

    /* Get stop bits */
    end++;

    if (*end == ',')
        end++;

    options = (const char *)(end);
    if (!*options)
        return 0;
    
    errno = 0;
    *stop = strtoul(options, &end, 10);
    /* stop_bits in [1, 2] */
    if (options == end || errno == ERANGE || !*stop || *stop > 2) {
        prm_err(params, "Invalid number of stop bits\n");
        return -EINVAL;
    }

    /* Get flow */
    if (*end  == ',')
        end++;

    if (*end == '\0')
        return 0;

    ch = (char)(tolower(*end) & 0xFF);
    for (i = 0; flow_options[i].flag != '\0'; i++) {
        if (ch == flow_options[i].flag) {
            *flow = flow_options[i].flowcontrol;
            break;
        }
    }

    if (flow_options[i].flag == '\0') {
        prm_err(params, "Invalid flow control character\n");
        return -EINVAL;
    }

    if (end[1]) {
        prm_err(params, "Invalid characters after flow control flag\n");
        return -EINVAL;
    }

    return 0;
}

static struct iio_context* zephyr_create_context(
        const struct iio_context_params *params, const char *port_name,
        unsigned int baud_rate, unsigned int bits, unsigned int stop,
        enum sp_parity parity, enum sp_flowcontrol flow)
{
    struct sp_port *port;
    struct iio_context_pdata *pdata;
    struct iio_context *ctx;
    const char *ctx_params[] = {
		"uri", "zephyr,port", "zephyr,description",
	};
    const char *ctx_params_values[ARRAY_SIZE(ctx_params)];
    char *uri, buf[16];
    size_t uri_len;
    int ret;

    uri_len = sizeof("zephyr:,1000000,8n1n") + strnlen(port_name, PATH_MAX);
    uri = malloc(uri_len);
    if(!uri)
        return iio_ptr(-ENOMEM);

    printf("port_name = %s\n", port_name);
    
    ret = libserialport_to_errno(sp_get_port_by_name(port_name, &port));
    if (ret)
        goto err_free_uri;

    ret = libserialport_to_errno(sp_open(port, SP_MODE_READ_WRITE));
    if (ret)
        goto err_free_port;

    ret = apply_settings(port, baud_rate, bits, stop, parity, flow);
    if (ret)
        goto err_close_port;

    /* Empty the output buffer */
    ret = libserialport_to_errno(sp_flush(port, SP_BUF_OUTPUT));
    if (ret)
        prm_warn(params, "Unable to flush the output buffer\n");

    /* Drain the input buffer */
    do {
        ret = libserialport_to_errno(sp_blocking_read(port, buf,
                                sizeof(buf), 1));
        if (ret < 0) {
            prm_warn(params, "Unable to drain input buffer\n");
            break;
        }
    } while (ret);

    pdata = zalloc(sizeof(*pdata));
    if (!pdata) {
        ret = -ENOMEM;
        goto err_close_port;
    }

    pdata->port = port;
    pdata->params = *params;
    pdata->iiod_client = iiod_client_new(params, 
                            (struct iiod_client_pdata*) pdata,
                            &zephyr_iiod_client_ops);
    ret = iio_err(pdata->iiod_client);
    if (ret)
		goto err_free_pdata;

    iio_snprintf(uri, uri_len, "zephyr:%s,%u,%u%c%u%c",
            port_name, baud_rate, bits,
            parity_char(parity), stop, flow_char(flow));

    ctx_params_values[0] = uri;
	ctx_params_values[1] = sp_get_port_name(port);
	ctx_params_values[2] = sp_get_port_description(port);

	ctx = iiod_client_create_context(pdata->iiod_client,
					 &iio_external_backend, NULL,
					 ctx_params, ctx_params_values,
					 ARRAY_SIZE(ctx_params));

	ret = iio_err(ctx);
	if (ret)
		goto err_destroy_iiod_client;

	iio_context_set_pdata(ctx, pdata);
	free(uri);

	return ctx;

err_destroy_iiod_client:
	iiod_client_destroy(pdata->iiod_client);
err_free_pdata:
	free(pdata);
err_close_port:
	sp_close(port);
err_free_port:
	sp_free_port(port);
err_free_uri:
	free(uri);
	return iio_ptr(ret);
}


static inline int libserialport_to_errno(enum sp_return ret)
{
	switch (ret) {
	case SP_ERR_ARG:
		return -EINVAL;
	case SP_ERR_FAIL:
		return -sp_last_error_code();
	case SP_ERR_MEM:
		return -ENOMEM;
	case SP_ERR_SUPP:
		return -ENOSYS;
	default:
		return (int) ret;
	}
}

static int apply_settings(struct sp_port *port, unsigned int baud_rate,
		unsigned int bits, unsigned int stop_bits,
		enum sp_parity parity, enum sp_flowcontrol flow)
{
	int ret;

#ifdef _WIN32
	ret = libserialport_to_errno(sp_set_dtr(port, SP_DTR_ON));
	if (ret)
		return ret;
#endif
	ret = libserialport_to_errno(sp_set_baudrate(port, (int) baud_rate));
	if (ret)
		return ret;

	ret = libserialport_to_errno(sp_set_bits(port, (int) bits));
	if (ret)
		return ret;

	ret = libserialport_to_errno(sp_set_stopbits(port, (int) stop_bits));
	if (ret)
		return ret;

	ret = libserialport_to_errno(sp_set_parity(port, parity));
	if (ret)
		return ret;

	return libserialport_to_errno(sp_set_flowcontrol(port, flow));
}

static ssize_t zephyr_write_data(struct iiod_client_pdata *io_data,
				 const char *data, size_t len,
				 unsigned int timeout_ms)
{
	struct iio_context_pdata *pdata = (struct iio_context_pdata *) io_data;
	enum sp_return sp_ret;
	ssize_t ret;

	sp_ret = sp_blocking_write(pdata->port, data, len, timeout_ms);
	ret = (ssize_t) libserialport_to_errno(sp_ret);

	prm_dbg(&pdata->params, "Write returned %li: %s\n", (long) ret, data);
    printf("Write returned %li: %s\n", (long) ret, data);

    for (size_t i = 0; i < len; i++) {
        printf("%02x ", data[i]);
    }
    printf("\n");

	if (ret < 0) {
		prm_err(&pdata->params, "sp_blocking_write returned %i\n",
			(int) ret);
        printf("sp_blocking_write returned %i\n", (int) ret);
		return ret;
	} else if ((size_t) ret < len) {
		prm_err(&pdata->params, "sp_blocking_write has timed out\n");
        printf("sp_blocking_write has timed out\n");
		return -ETIMEDOUT;
	}

	return ret;
}

void sleep_one_ms1(void)
{
#ifdef _WIN32
	Sleep(1);
#else
	const struct timespec ts = {
		/* One millisecond */
		.tv_nsec = 10 * 1000 * 1000,
	};

	nanosleep(&ts, NULL);
#endif
}

static ssize_t zephyr_read_data(struct iiod_client_pdata *io_data,
				char *buf, size_t len, unsigned int timeout_ms)
{
	struct iio_context_pdata *pdata = (struct iio_context_pdata *) io_data;
	long long time_left_ms = (long long)timeout_ms;
	enum sp_return sp_ret;
	ssize_t ret = 0;

	while (true) {
		if (timeout_ms && time_left_ms <= 0)
			break;
		sp_ret = sp_nonblocking_read(pdata->port, buf, len);
		ret = (ssize_t) libserialport_to_errno(sp_ret);

        // if (ret)
        //     printf("Read returned %li: %hhu -> %c\n", (long) ret, buf[0], buf[0]);

		if (ret || pdata->shutdown)
			break;

		sleep_one_ms1();
		time_left_ms--;
	}

	if (ret == 0) {
		if (!pdata->shutdown){
            prm_err(&pdata->params, "serial_read_data has timed out\n");
            printf("sp_blocking_write has timed out\n");
        }
		return -ETIMEDOUT;
	}

	return ret;
}

static char flow_char(enum sp_flowcontrol fc)
{
	unsigned int i;

	for (i = 0; flow_options[i].flag != '\0'; i++) {
		if (fc == flow_options[i].flowcontrol)
			return flow_options[i].flag;
	}
	return '\0';
}

static char parity_char(enum sp_parity pc)
{
	unsigned int i;

	for (i = 0; parity_options[i].flag != '\0'; i++) {
		if (pc == parity_options[i].parity)
			return parity_options[i].flag;
	}
	return '\0';
}

static ssize_t
zephyr_read_attr_val(const struct iio_attr *attr, char *dst, size_t len)
{
	const struct iio_device *dev = iio_attr_get_device(attr);
	const struct iio_context *ctx = iio_device_get_context(dev);
	struct iio_context_pdata *pdata = iio_context_get_pdata(ctx);

	return iiod_client_attr_read(pdata->iiod_client, attr, dst, len);
}

static ssize_t
zephyr_write_attr_val(const struct iio_attr *attr, const char *src, size_t len)
{
	const struct iio_device *dev = iio_attr_get_device(attr);
	const struct iio_context *ctx = iio_device_get_context(dev);
	struct iio_context_pdata *pdata = iio_context_get_pdata(ctx);

	return iiod_client_attr_write(pdata->iiod_client, attr, src, len);
}

/* ==================================================================== */
/*                               Variables                              */
/* ==================================================================== */

static const struct iio_backend_ops zephyr_ops = {
    .create = zephyr_create_context_from_args,
    .read_attr = zephyr_read_attr_val,
    .write_attr = zephyr_write_attr_val,
};

// This is the external backend symbol that libiio will look for
const struct iio_backend iio_external_backend = {
    .name = "zephyr",
    .api_version = IIO_BACKEND_API_V1,
    .default_timeout_ms = 1000,
    .uri_prefix = "zephyr:",
    .ops = &zephyr_ops,
};