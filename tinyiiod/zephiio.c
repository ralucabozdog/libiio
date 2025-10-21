#include <iio/iio-backend.h>
#include <iio-private.h>
#include <errno.h>
#include <string.h>

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

static char arr[100] = "alaportocala";

static ssize_t zephyr_read_attr(const struct iio_attr *attr, char *dst, size_t len)
{
	strncpy(dst, arr, len);
	dst[len] = 0;
	return len;
}

static ssize_t zephyr_write_attr(const struct iio_attr *attr, const char *src, size_t len)
{
	strncpy(arr, src, len);
	arr[len] = 0;
	return len;
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

static struct iio_buffer_pdata *
zephyr_create_buffer(const struct iio_device *dev,
			struct iio_buffer_params *params,
			struct iio_channels_mask *mask)
{
	struct iio_buffer_pdata *pdata = malloc(10 /* arbitrary, backend specific */);
	if (!pdata)
		return iio_ptr(-ENOMEM);
	
	return pdata;
}

void zephyr_free_buffer(struct iio_buffer_pdata *pdata)
{
	free(pdata);
}

const struct iio_device * zephyr_get_trigger(const struct iio_device *dev)
{
	return iio_ptr(-ENODEV);
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
	.get_trigger = zephyr_get_trigger,
};

extern const struct iio_backend iio_external_backend = {
	.name = "zephyr",
	.api_version = IIO_BACKEND_API_V1,
	.default_timeout_ms = 1000,
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

	return ctx;
end:
	return iio_ptr(ret);
}