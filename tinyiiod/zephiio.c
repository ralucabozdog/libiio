#include <iio/iio-backend.h>
#include <iio-private.h>
#include <errno.h>
#include <string.h>
#include <network.h>

const char* xml_ad7768 = 
"<?xml version=\"1.0\"?>\n"
"<!DOCTYPE context [\n"
"  <!ELEMENT context (device | context-attribute)*>\n"
"  <!ELEMENT context-attribute EMPTY>\n"
"  <!ELEMENT device (channel | attribute | debug-attribute | buffer-attribute)*>\n"
"  <!ELEMENT channel (scan-element?, attribute*)>\n"
"  <!ELEMENT attribute EMPTY>\n"
"  <!ELEMENT scan-element EMPTY>\n"
"  <!ELEMENT debug-attribute EMPTY>\n"
"  <!ELEMENT buffer-attribute EMPTY>\n"
"  <!ATTLIST context name CDATA #REQUIRED\n"
"            version-major CDATA #REQUIRED\n"
"            version-minor CDATA #REQUIRED\n"
"            version-git CDATA #REQUIRED>\n"
"  <!ATTLIST context-attribute name CDATA #REQUIRED\n"
"                                 value CDATA #REQUIRED>\n"
"  <!ATTLIST device id CDATA #REQUIRED\n"
"                  name CDATA #IMPLIED\n"
"                  label CDATA #IMPLIED>\n"
"  <!ATTLIST channel id CDATA #REQUIRED\n"
"                    type (input|output) #REQUIRED\n"
"                    name CDATA #IMPLIED>\n"
"  <!ATTLIST scan-element index CDATA #REQUIRED\n"
"                         format CDATA #REQUIRED\n"
"                         scale CDATA #IMPLIED>\n"
"  <!ATTLIST attribute name CDATA #REQUIRED\n"
"                      filename CDATA #IMPLIED>\n"
"  <!ATTLIST debug-attribute name CDATA #REQUIRED>\n"
"  <!ATTLIST buffer-attribute name CDATA #REQUIRED>\n"
"]>\n"
"<context name=\"xml\" version-major=\"1\" version-minor=\"0\" version-git=\"abcdefg\">\n"
"  <device id=\"iio:device0\" name=\"ad7768\">\n"
"    <channel id=\"voltage0\" name=\"voltage0\" type=\"input\">\n"
"      <scan-element index=\"0\" format=\"le:s32/32>>0\" />\n"
"    </channel>\n"
"    <channel id=\"voltage1\" name=\"voltage1\" type=\"input\">\n"
"      <scan-element index=\"1\" format=\"le:s32/32>>0\" />\n"
"    </channel>\n"
"    <channel id=\"voltage2\" name=\"voltage2\" type=\"input\">\n"
"      <scan-element index=\"2\" format=\"le:s32/32>>0\" />\n"
"    </channel>\n"
"    <channel id=\"voltage3\" name=\"voltage3\" type=\"input\">\n"
"      <scan-element index=\"3\" format=\"le:s32/32>>0\" />\n"
"    </channel>\n"
"    <attribute name=\"sampling_frequency\" />\n"
"    <attribute name=\"hardwaregain\" />\n"
"    <attribute name=\"scale_available\" />\n"
"    <attribute name=\"raw\" />\n"
"  </device>\n"
"</context>\n";

static struct iio_context*
zephiio_create_context(const struct iio_context_params *params, const char *args);

static char arr[100] = {"alaportocala"};

static ssize_t zephyr_read_attr(const struct iio_attr *attr, char *dst, size_t len)
{
	strncpy(dst, arr, len);
	dst[len] = 0;
	return strlen(arr) + 1;
}

static ssize_t zephyr_write_attr(const struct iio_attr *attr, const char *src, size_t len)
{
	strncpy(arr, src, len);
	arr[len] = 0;
	return strlen(arr) + 1;
}

static void zephyr_shutdown(struct iio_context *ctx)
{
	/* TODO */
}

static int zephyr_get_version(const struct iio_context *ctx, unsigned int *major,
			      unsigned int *minor, char git_tag[8])
{
	/* TODO */
	// if (major) *major = 11;
	// if (minor) *minor = 2;
	// if (git_tag) strcpy(git_tag, "pampam");
	return 0;
}

static int zephyr_set_timeout(struct iio_context *ctx, unsigned int timeout)
{
	/* TODO */
	ctx->params.timeout_ms = timeout;
	return 0;
}

struct iio_buffer_pdata {
	struct iiod_client_pdata io_ctx;
	struct iiod_client_buffer_pdata *pdata;

	const struct iio_device *dev;
	struct iiod_client *iiod_client;
};

static struct iio_buffer_pdata *
zephyr_create_buffer(const struct iio_device *dev,
			struct iio_buffer_params *params,
			struct iio_channels_mask *mask)
{
	struct iio_buffer_pdata *pdata = malloc(sizeof(struct iio_buffer_pdata));
	if (!pdata)
		return iio_ptr(pdata);
	
	return pdata;
}

void zephyr_free_buffer(struct iio_buffer_pdata *pdata)
{
	if (pdata)
		free(pdata);
}

int zephyr_enable_buffer(struct iio_buffer_pdata *pdata,
			     size_t nb_samples, bool enable, bool cyclic)
{
	return 0;
}

const struct iio_device * zephyr_get_trigger(const struct iio_device *dev)
{
	struct iio_device *trig = malloc(sizeof(struct iio_device));
	if (!trig)
		return iio_ptr(trig);
	return trig;
}

struct iio_event_stream_pdata {
	struct iiod_client *client;
	const struct iio_device *dev;
	struct iio_event event;
	struct iiod_io *io;
	uint16_t idx;
};

struct iio_event_stream_pdata * zephyr_open_ev(const struct iio_device *dev)
{
	struct iio_event_stream_pdata * pdata = malloc(sizeof(struct iio_event_stream_pdata));

	if (!pdata)
		return iio_err(pdata);

	pdata->dev = dev;

	return pdata;
}

void zephyr_close_ev(struct iio_event_stream_pdata *pdata)
{
	free(pdata);
}

struct iio_block_pdata {
	struct iio_buffer_pdata *buf;
	struct iio_block_impl_pdata *pdata;
	size_t size;
	void *data;
	bool dequeued;
	bool cpu_access_disabled;
};

struct iio_block_pdata * zephyr_create_block(struct iio_buffer_pdata *pdata,
						size_t size, void **data)
{
	struct iio_block_pdata * block_pdata = malloc(sizeof(struct iio_block_pdata));

	if (!block_pdata)
		return iio_ptr(block_pdata);

	block_pdata->size = size;

	block_pdata->data = malloc(size);
	if (!block_pdata->data)
		return -ENOMEM;

	*data = block_pdata->data;
	
	return block_pdata;
}

void zephyr_free_block(struct iio_block_pdata *pdata)
{
	free(pdata);
}

int zephyr_enqueue_block(struct iio_block_pdata *pdata,
			     size_t bytes_used, bool cyclic)
{
	printf("Buffer message sent to device\n");
	for (int i = 0; i < bytes_used; i++)
		printf("%02x ", ((char*)pdata->data)[i]);
	printf("\n");
	return 0;
}

int zephyr_dequeue_block(struct iio_block_pdata *pdata, bool nonblock)
{
	char *samples = (char *)pdata->data;
	size_t num_samples = pdata->size;

	for (size_t i = 0; i < num_samples; i++) {
		samples[i] = '0' + i % 10;
	}

	return 0;
}

/* Only for legacy - not called in rwdev, only in readdev */
ssize_t zephyr_readbuf(struct iio_buffer_pdata *pdata,
			   void *dst, size_t len)
{
	strncpy((char*)dst, arr, len);
	((char*)dst)[len] = 0;
	return strlen(arr) + 1;
}

ssize_t zephyr_writebuf(struct iio_buffer_pdata *pdata,
			    const void *src, size_t len)
{
	for (int i = 0; i < len; i++)
		printf("%c", arr[i]);

	strncpy(arr, src, len);
	arr[len] = 0;
	return strlen(arr) + 1;	
}

void zephyr_cancel_buffer(struct iio_buffer_pdata *pdata)
{
	return;
}

int zephyr_set_trigger(const struct iio_device *dev,
			const struct iio_device *trigger)
{
	if (!dev || !trigger || !iio_device_is_trigger(trigger))
		return -EINVAL;
	
	return 0;
}

int zephyr_read_ev(struct iio_event_stream_pdata *pdata,
		       struct iio_event *out_event,
		       bool nonblock)
{
	out_event->id = ((uint64_t)IIO_EV_TYPE_THRESH << 56) |	// Event type: threshold
					((uint64_t)0 << 55) |					// Not differential
					((uint64_t)IIO_EV_DIR_RISING << 48) |	// Direction: rising
					((uint64_t)IIO_NO_MOD << 40) |			// No modifier
					((uint64_t)IIO_VOLTAGE << 32) |			// Channel type: voltage
					((uint64_t)0);							// Channel ID: 0 (for voltage0)
	out_event->timestamp = k_uptime_get();
}

const struct iio_backend_ops zephyr_ops = {
	.create = zephiio_create_context,
	.read_attr = zephyr_read_attr,
	.write_attr = zephyr_write_attr,
	.shutdown = zephyr_shutdown,
	.get_version = zephyr_get_version,
	.set_timeout = zephyr_set_timeout,
	.create_buffer = zephyr_create_buffer,
	.free_buffer = zephyr_free_buffer,
	.enable_buffer = zephyr_enable_buffer,
	.cancel_buffer = zephyr_cancel_buffer,
	.readbuf = zephyr_readbuf,
	.writebuf = zephyr_writebuf,
	.get_trigger = zephyr_get_trigger,
	.set_trigger = zephyr_set_trigger,
	.open_ev = zephyr_open_ev,
	.close_ev = zephyr_close_ev,
	.read_ev = zephyr_read_ev,
	.create_block = zephyr_create_block,
	.free_block = zephyr_free_block,
	.enqueue_block = zephyr_enqueue_block,
	.dequeue_block = zephyr_dequeue_block,
};

extern const struct iio_backend iio_external_backend = {
	.name = "zephyr",
	.api_version = IIO_BACKEND_API_V1,
	.default_timeout_ms = 5000,
	.uri_prefix = "zephyr:",
	.ops = &zephyr_ops,
};

const struct iio_data_format frmt = {
	.length = 16,
	.bits = 16,
	.is_signed = true,
};

static struct iio_context*
zephiio_create_context(const struct iio_context_params *params, const char *args)
{
    // printk("Hello from zephiio_create_context\n");

	int ret;
	struct iio_context *ctx;
	const char *ctx_attrs[] = { "xml" };
	const char *ctx_values[1];
	char uri[255 + 3];
	char *description = "zephyr backend";
	
	// ctx = iio_create_context_from_xml(params, fakexml, &iio_external_backend, description, ctx_attrs, ctx_values, 1);
	// ret = iio_err(ctx);
	// if (ret)
	// 	return iio_err_cast(ctx);

	ctx = iio_context_create_from_backend(params, &iio_external_backend, description, 11, 2, "pampam");
	ret = iio_err(ctx);
	if (ret)
		return iio_err_cast(ctx);
	
	struct iio_device * d0 = iio_context_add_device(ctx, "iio:device0", "adc_demo", NULL);
	ret = iio_err(d0);
	if (ret)
		return iio_err_cast(d0);
	
	const struct iio_data_format fmt = {
		.length = 16,
		.bits = 16,
		.is_signed = true,
	};
	struct iio_channel * d0c0 = iio_device_add_channel(d0, 0, "voltage0", "adc_in_ch0", "label", false,
		       true, &fmt);
	ret = iio_err(d0c0);
	if (ret)
		return iio_err_cast(d0c0);

	ret = iio_channel_add_attr(d0c0, "adc_channel_attr", "in_voltage0_adc_channel_attr");
	if (ret)
		return iio_ptr(ret);
	struct iio_channel * d0c1 = iio_device_add_channel(d0, 1, "voltage1", "adc_in_ch1", "label", false,
		       true, &fmt);
	ret = iio_err(d0c1);
	if (ret)
		return iio_err_cast(d0c1);

	ret = iio_channel_add_attr(d0c1, "adc_channel_attr", "in_voltage1_adc_channel_attr");
	if (ret)
		return iio_ptr(ret);

	ret = iio_device_add_attr(d0, "adc_global_attr", IIO_ATTR_TYPE_DEVICE);
	if (ret)
		return iio_ptr(ret);

	ret = iio_device_add_attr(d0, "direct_reg_access", IIO_ATTR_TYPE_DEBUG);
	if (ret)
		return iio_ptr(ret);

	
	/* DAC added as device1 */

	struct iio_device * d1 = iio_context_add_device(ctx, "iio:device1", "dac_demo", NULL);
	ret = iio_err(d1);
	if (ret)
		return iio_err_cast(d1);
	
	struct iio_channel * d1c0 = iio_device_add_channel(d1, 0, "voltage_out0", "dac_out_ch0", "label", true,
		       true, &fmt);
	ret = iio_err(d1c0);
	if (ret)
		return iio_err_cast(d1c0);

	ret = iio_channel_add_attr(d1c0, "dac_channel_attr", "out_voltage0_dac_channel_attr");
	if (ret)
		return iio_ptr(ret);
	struct iio_channel * d1c1 = iio_device_add_channel(d1, 1, "voltage_out1", "dac_out_ch1", "label", true,
		       true, &fmt);
	ret = iio_err(d1c1);
	if (ret)
		return iio_err_cast(d1c1);

	ret = iio_channel_add_attr(d1c1, "dac_channel_attr", "out_voltage1_dac_channel_attr");
	if (ret)
		return iio_ptr(ret);

	ret = iio_device_add_attr(d1, "dac_global_attr", IIO_ATTR_TYPE_DEVICE);
	if (ret)
		return iio_ptr(ret);

	ret = iio_device_add_attr(d1, "direct_reg_access", IIO_ATTR_TYPE_DEBUG);
	if (ret)
		return iio_ptr(ret);	

	/* Trigger */
	struct iio_device * t0 = iio_context_add_device(ctx, "trigger0", "trigger0", NULL);
	ret = iio_err(t0);
	if (ret)
		return iio_ptr(t0);

	ret = iio_device_add_attr(t0, "sampling_frequency", IIO_ATTR_TYPE_DEVICE);
	if (ret)
		return iio_ptr(ret);

	return ctx;
end:
	return iio_ptr(ret);
}