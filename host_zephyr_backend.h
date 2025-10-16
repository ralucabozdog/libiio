#ifndef __LIBIIO_COMPAT_H__
#define __LIBIIO_COMPAT_H__

#include <iio/iio-backend.h>

extern struct iio_context *
zephyr_create_context_from_args(const struct iio_context_params *params, const char *args);

#endif /* __LIBIIO_COMPAT_H__ */