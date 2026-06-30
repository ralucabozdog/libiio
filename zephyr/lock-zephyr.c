/*
 * Copyright (c) 2025 Analog Devices, Inc.
 *
 * SPDX-License-Identifier: MIT
 */

#include <iio/iio.h>
#include <iio/iio-lock.h>

#include <zephyr/kernel.h>
#include <errno.h>
#include <stdlib.h>

/* --- Mutex --- */

struct iio_mutex {
	struct k_mutex lock;
};

struct iio_mutex *iio_mutex_create(void)
{
	struct iio_mutex *lock = malloc(sizeof(*lock));

	if (!lock)
		return iio_ptr(-ENOMEM);

	k_mutex_init(&lock->lock);
	return lock;
}

void iio_mutex_destroy(struct iio_mutex *lock)
{
	free(lock);
}

void iio_mutex_lock(struct iio_mutex *lock)
{
	k_mutex_lock(&lock->lock, K_FOREVER);
}

void iio_mutex_unlock(struct iio_mutex *lock)
{
	k_mutex_unlock(&lock->lock);
}

/* --- Condition variable --- */

struct iio_cond {
	struct k_condvar cond;
};

struct iio_cond *iio_cond_create(void)
{
	struct iio_cond *cond = malloc(sizeof(*cond));

	if (!cond)
		return iio_ptr(-ENOMEM);

	k_condvar_init(&cond->cond);
	return cond;
}

void iio_cond_destroy(struct iio_cond *cond)
{
	free(cond);
}

int iio_cond_wait(struct iio_cond *cond, struct iio_mutex *lock,
		  int timeout_ms)
{
	int ret;

	if (timeout_ms == 0)
		return -ETIMEDOUT;

	if (timeout_ms < 0)
		ret = k_condvar_wait(&cond->cond, &lock->lock, K_FOREVER);
	else
		ret = k_condvar_wait(&cond->cond, &lock->lock,
				     K_MSEC(timeout_ms));

	if (ret == -EAGAIN)
		return -ETIMEDOUT;

	return ret;
}

void iio_cond_signal(struct iio_cond *cond)
{
	k_condvar_signal(&cond->cond);
}

/* --- Thread --- */

struct iio_thrd {
	struct k_thread thread;
	int pool_idx;
	int ret;
	int (*fn)(void *);
	void *arg;
};

K_KERNEL_STACK_ARRAY_DEFINE(iio_thread_stacks,
			    CONFIG_LIBIIO_THREAD_POOL_SIZE,
			    CONFIG_LIBIIO_THREAD_STACK_SIZE);

static bool iio_thread_used[CONFIG_LIBIIO_THREAD_POOL_SIZE];
static K_MUTEX_DEFINE(iio_thread_pool_lock);

static void iio_thrd_entry(void *p1, void *p2, void *p3)
{
	struct iio_thrd *t = p1;

	t->ret = t->fn(t->arg);
}

struct iio_thrd *iio_thrd_create(int (*thrd)(void *),
				 void *d, const char *name)
{
	struct iio_thrd *t;
	int idx = -1;

	if (!thrd)
		return iio_ptr(-EINVAL);

	k_mutex_lock(&iio_thread_pool_lock, K_FOREVER);
	for (int i = 0; i < CONFIG_LIBIIO_THREAD_POOL_SIZE; i++) {
		if (!iio_thread_used[i]) {
			iio_thread_used[i] = true;
			idx = i;
			break;
		}
	}
	k_mutex_unlock(&iio_thread_pool_lock);

	if (idx < 0)
		return iio_ptr(-ENOMEM);

	t = malloc(sizeof(*t));
	if (!t) {
		k_mutex_lock(&iio_thread_pool_lock, K_FOREVER);
		iio_thread_used[idx] = false;
		k_mutex_unlock(&iio_thread_pool_lock);
		return iio_ptr(-ENOMEM);
	}

	t->pool_idx = idx;
	t->fn = thrd;
	t->arg = d;
	t->ret = 0;

	k_thread_create(&t->thread, iio_thread_stacks[idx],
			K_KERNEL_STACK_SIZEOF(iio_thread_stacks[idx]),
			iio_thrd_entry, t, NULL, NULL,
			CONFIG_LIBIIO_THREAD_PRIORITY, 0, K_NO_WAIT);

	if (name)
		k_thread_name_set(&t->thread, name);

	return t;
}

int iio_thrd_join_and_destroy(struct iio_thrd *thrd)
{
	int ret;

	k_thread_join(&thrd->thread, K_FOREVER);
	ret = thrd->ret;

	k_mutex_lock(&iio_thread_pool_lock, K_FOREVER);
	iio_thread_used[thrd->pool_idx] = false;
	k_mutex_unlock(&iio_thread_pool_lock);

	free(thrd);
	return ret;
}
