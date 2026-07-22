/*
 * Copyright (c) 2026 Analog Devices, Inc.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef ZEPHYR_INCLUDE_IIO_TRIGGER_NODE_H_
#define ZEPHYR_INCLUDE_IIO_TRIGGER_NODE_H_

#include <zephyr/kernel.h>

#define IIO_TRIGGER_NODE_LIST_STATIC_INIT(_name) { .list = SYS_SLIST_STATIC_INIT(&_name.list) }

struct iio_trigger_node {
	sys_snode_t node;
};

struct iio_trigger_list {
	sys_slist_t list;
};

static inline void iio_trigger_node_add(struct iio_trigger_list *list, struct iio_trigger_node *node)
{
	sys_slist_append(&list->list, &node->node);
}

static inline bool iio_trigger_node_remove(struct iio_trigger_list *list, struct iio_trigger_node *node)
{
	return sys_slist_find_and_remove(&list->list, &node->node);
}

static inline void iio_trigger_list_init(struct iio_trigger_list *list)
{
	sys_slist_init(&list->list);
}

static inline void iio_trigger_node_foreach(struct iio_trigger_list *list,
					    int (*cb)(struct iio_trigger_node *node, void *user_data),
					    void *user_data)
{
	sys_snode_t *n;

	SYS_SLIST_FOR_EACH_NODE(&list->list, n) {
		struct iio_trigger_node *child_node = CONTAINER_OF(n, struct iio_trigger_node, node);
		if (cb(child_node, user_data)) {
			break;
		}
	}
}

static inline int iio_trigger_node_compare(struct iio_trigger_node *node,
	struct iio_trigger_node *compare_node)
{
	if (&node->node == &compare_node->node) {
		return 1;
	}

	return 0;
}

static inline int iio_trigger_node_find(struct iio_trigger_list *list, struct iio_trigger_node *compare_node)
{
	sys_snode_t *n;

	SYS_SLIST_FOR_EACH_NODE(&list->list, n) {
		struct iio_trigger_node *child_node = CONTAINER_OF(n, struct iio_trigger_node, node);
		if (iio_trigger_node_compare(child_node, compare_node)) {
			return 1;
		}
	}

	return 0;
}

static inline struct iio_trigger_node *iio_trigger_node_get(struct iio_trigger_list *list)
{
	sys_snode_t *node = sys_slist_get(&list->list);
	return node ? CONTAINER_OF(node, struct iio_trigger_node, node) : NULL;
}

static inline bool iio_trigger_list_is_empty(struct iio_trigger_list *list)
{
	return sys_slist_is_empty(&list->list);
}

#endif /* ZEPHYR_INCLUDE_IIO_TRIGGER_NODE_H_ */