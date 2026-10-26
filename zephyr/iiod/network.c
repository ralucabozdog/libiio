/*
 * Copyright (c) 2025 Analog Devices, Inc.
 *
 * SPDX-License-Identifier: MIT
 */

#include <zephyr/kernel.h>
#include <zephyr/posix/sys/socket.h>
#include <zephyr/posix/netinet/in.h>
#include <zephyr/posix/netinet/tcp.h>
#include <zephyr/posix/arpa/inet.h>
#include <zephyr/posix/unistd.h>
#include <zephyr/logging/log.h>
#include <tinyiiod/tinyiiod.h>
#include <zephyr/net/dns_sd.h>
#if defined(CONFIG_NET_CONNECTION_MANAGER)
#include <zephyr/net/net_mgmt.h>
#include <zephyr/net/net_event.h>
#include <zephyr/net/conn_mgr_monitor.h>
#endif

LOG_MODULE_REGISTER(iiod_network, CONFIG_LIBIIO_LOG_LEVEL);

/* Each client's responder takes one pool thread for its writer task. */
BUILD_ASSERT(!IS_ENABLED(CONFIG_LIBIIO_MULTITHREADING) ||
	     CONFIG_LIBIIO_THREAD_POOL_SIZE >= CONFIG_LIBIIO_IIOD_NETWORK_CLIENT_MAX,
	     "CONFIG_LIBIIO_THREAD_POOL_SIZE must be at least "
	     "CONFIG_LIBIIO_IIOD_NETWORK_CLIENT_MAX");

/* One net context for the listening socket and one per client, on top of
 * whatever else (e.g. the mDNS responder) the application uses.
 */
#if defined(CONFIG_NET_MAX_CONTEXTS) && !defined(CONFIG_NET_SOCKETS_OFFLOAD)
BUILD_ASSERT(CONFIG_NET_MAX_CONTEXTS >= CONFIG_LIBIIO_IIOD_NETWORK_CLIENT_MAX + 1,
	     "CONFIG_NET_MAX_CONTEXTS is too small for "
	     "CONFIG_LIBIIO_IIOD_NETWORK_CLIENT_MAX clients");
#endif

#if defined(CONFIG_NET_CONNECTION_MANAGER)
static K_SEM_DEFINE(network_ready, 0, 1);
static struct net_mgmt_event_callback net_mgmt_cb;

static void network_event_handler(struct net_mgmt_event_callback *cb,
				   uint64_t mgmt_event, struct net_if *iface)
{
	if (mgmt_event == NET_EVENT_L4_CONNECTED) {
		k_sem_give(&network_ready);
	}
}
#endif

/* Structure to hold client data */
struct client_data {
	int fd;
	struct sockaddr_in addr;
	int client_num;
	bool in_use;
	struct k_thread thread;
	k_tid_t thread_id;
};

/* Pool of client data structures */
static struct client_data client_pool[CONFIG_LIBIIO_IIOD_NETWORK_CLIENT_MAX];
static K_MUTEX_DEFINE(client_pool_mutex);

/* Shared IIO context and XML - used by all clients. Created by the first
 * client thread rather than the server thread, so the server thread only
 * needs a stack big enough for its accept loop.
 */
static struct iio_context *shared_ctx = NULL;
static const void *shared_xml = NULL;
static size_t shared_xml_len = 0;
static K_MUTEX_DEFINE(shared_ctx_mutex);

/* Thread stacks for client threads */
K_THREAD_STACK_ARRAY_DEFINE(client_thread_stacks,
			    CONFIG_LIBIIO_IIOD_NETWORK_CLIENT_MAX,
			    CONFIG_LIBIIO_IIOD_NETWORK_CLIENT_THREAD_STACK_SIZE);

static ssize_t iiod_network_read(struct iiod_pdata *pdata, void *buf, size_t size)
{
	struct client_data *client = (struct client_data *)pdata;
	uint8_t *buffer = (uint8_t *)buf;
	size_t bytes_read = 0;
	int read = 0;
	int this_fd;

	if (!client) {
		return -EPERM;
	}

	this_fd = client->fd;
	if (this_fd < 0) {
		return -ENOENT;
	}

	while (bytes_read < size) {
		read = recv(this_fd, buffer, size - bytes_read, 0);
		LOG_DBG("[Client %d] Read %d/%d bytes", client->client_num, read, size);
		LOG_HEXDUMP_DBG(buffer, read, "");

		if (read > 0) {
			buffer += read;
			bytes_read += read;
		}
		else if (read < 0) {
			if (errno == ECONNRESET || errno == ENOTCONN) {
				LOG_DBG("[Client %d] Connection closed by peer",
					client->client_num);
			} else {
				LOG_ERR("[Client %d] error: recv: %d",
					client->client_num, errno);
			}
			return -ESRCH;
		}
		else {
			return 0;
		}
	}
	return bytes_read;
}


static ssize_t iiod_network_write(struct iiod_pdata *pdata, const void *buf, size_t size)
{
	struct client_data *client = (struct client_data *)pdata;
	const uint8_t *buffer = (const uint8_t *)buf;
	int bytes_sent = 0;
	int sent = 0;
	int this_fd;

	if (!client) {
		return -EPERM;
	}

	this_fd = client->fd;
	if (this_fd < 0) {
		return -ENOENT;
	}

	while (bytes_sent < size) {
		sent = send(this_fd, buffer, size - bytes_sent, 0);
		LOG_DBG("[Client %d] Wrote %d/%d bytes", client->client_num, sent, size);
		LOG_HEXDUMP_DBG(buffer, sent, "");

		if (sent < 0) {
			LOG_ERR("[Client %d] error: send: %d",
				client->client_num, errno);
			return -ESRCH;
		}

		buffer += sent;
		bytes_sent += sent;
	}

	return (ssize_t)bytes_sent;
}

static int iiod_network_create_server(void)
{
	int server_fd;
	struct sockaddr_in bind_addr = {
		.sin_family = AF_INET,
		.sin_addr = { .s_addr = INADDR_ANY },
		.sin_port = htons(CONFIG_LIBIIO_IIOD_NETWORK_PORT),
	};
	int reuse = 1;
	int ret;

	LOG_DBG("Creating test TCP server on port %d", CONFIG_LIBIIO_IIOD_NETWORK_PORT);

	server_fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	if (server_fd < 0) {
		LOG_ERR("Failed to create socket: %d (errno: %d)", server_fd, errno);
		return -EPERM;
	}
	LOG_DBG("Socket created successfully: fd=%d", server_fd);

	ret = setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
	if (ret < 0) {
		LOG_ERR("Failed to set socket options: %d (errno: %d)", ret, errno);
		close(server_fd);
		return -EPERM;
	}
	LOG_DBG("Socket options set successfully");

	ret = bind(server_fd, (struct sockaddr *)&bind_addr, sizeof(bind_addr));
	if (ret < 0) {
		LOG_ERR("Failed to bind socket: %d (errno: %d)", ret, errno);
		close(server_fd);
		return -EPERM;
	}
	LOG_DBG("Socket bound successfully to port %d", CONFIG_LIBIIO_IIOD_NETWORK_PORT);

	ret = listen(server_fd, CONFIG_LIBIIO_IIOD_NETWORK_CLIENT_MAX);
	if (ret < 0) {
		LOG_ERR("Failed to listen on socket: %d (errno: %d)", ret, errno);
		close(server_fd);
		return -EPERM;
	}
	LOG_DBG("Socket listening successfully: backlog = %d",
		CONFIG_LIBIIO_IIOD_NETWORK_CLIENT_MAX);

	return server_fd;
}

static int iiod_network_get_shared_ctx(void)
{
	struct iio_context_params ctx_params = {0};
	struct iio_context *ctx;
	char *xml;
	int ret = 0;

	k_mutex_lock(&shared_ctx_mutex, K_FOREVER);
	if (shared_ctx) {
		goto out_unlock;
	}

	LOG_DBG("Creating shared IIO context...");
	ctx = iio_create_context(&ctx_params, "zephyr:");
	ret = iio_err(ctx);
	if (ret) {
		LOG_ERR("Context creation failed: %d", ret);
		goto out_unlock;
	}

	LOG_DBG("Getting xml data");
	xml = iio_context_get_xml(ctx);
	if (!xml) {
		LOG_ERR("Error getting context XML");
		iio_context_destroy(ctx);
		ret = -ENOMEM;
		goto out_unlock;
	}

	shared_xml = xml;
	shared_xml_len = strlen(xml) + 1;
	shared_ctx = ctx;
	LOG_DBG("XML ready, length: %zu bytes", shared_xml_len);

out_unlock:
	k_mutex_unlock(&shared_ctx_mutex);
	return ret;
}

static void iiod_network_client_thread(void *p1, void *p2, void *p3)
{
	struct client_data *client = (struct client_data *)p1;
	int this_client_num, this_fd;

	if (!client) {
		return;
	}

	this_client_num = client->client_num;
	this_fd = client->fd;

	LOG_DBG("[Client %d] Thread started (fd=%d)", this_client_num, this_fd);

	/* Use iiod_interpreter - locks are already created at startup */
	if (!iiod_network_get_shared_ctx()) {
		iiod_interpreter(shared_ctx, (struct iiod_pdata *)client,
				iiod_network_read, iiod_network_write,
				shared_xml, shared_xml_len);
	}

	if (client->fd >= 0) {
		client->fd = -EPERM;
		close(this_fd);
	}

	k_mutex_lock(&client_pool_mutex, K_FOREVER);
	client->in_use = false;
	k_mutex_unlock(&client_pool_mutex);

	LOG_DBG("[Client %d] Thread exiting", this_client_num);
}

static struct client_data* iiod_network_alloc_client_data(void)
{
	struct client_data *client = NULL;
	int i;

	k_mutex_lock(&client_pool_mutex, K_FOREVER);
	for (i = 0; i < CONFIG_LIBIIO_IIOD_NETWORK_CLIENT_MAX; i++) {
		if (!client_pool[i].in_use) {
			client = &client_pool[i];
			client->in_use = true;
			client->client_num = i;
			break;
		}
	}
	k_mutex_unlock(&client_pool_mutex);

	if (!client) {
		LOG_ERR("Reached maximum number of clients! (max = %d)",
			CONFIG_LIBIIO_IIOD_NETWORK_CLIENT_MAX);
	}
	return client;
}

static void iiod_network_free_client_data(struct client_data *client)
{
	if (!client) {
		LOG_ERR("No such client");
		return;
	}

	LOG_DBG("[Client %d] free_client_data called", client->client_num);

	k_mutex_lock(&client_pool_mutex, K_FOREVER);
	if (client->fd >= 0) {
		LOG_DBG("[Client %d] Closing socket in free_client_data",
			client->client_num);
		close(client->fd);
		client->fd = -EPERM;
	}

	client->in_use = false;
	k_mutex_unlock(&client_pool_mutex);

	LOG_DBG("[Client %d] free_client_data completed", client->client_num);
}

#if IS_ENABLED(CONFIG_DNS_SD)

DNS_SD_REGISTER_TCP_SERVICE(iio, CONFIG_NET_HOSTNAME, "_iio", "local",
			    dns_sd_empty_txt, CONFIG_LIBIIO_IIOD_NETWORK_PORT);

#endif

static void iiod_network_server_thread(void *p1, void *p2, void *p3)
{
	int server_fd, new_fd, i;
	struct sockaddr_in client_addr;
	socklen_t client_addr_len;
	char addr_str[32];
	struct client_data *client;
	static int counter;
	int nodelay = 1;
	int ret;

	LOG_DBG("*** Simple TCP Test Server for %s ***", CONFIG_BOARD_TARGET);
	LOG_DBG("Waiting for network to initialize...");
#if defined(CONFIG_NET_CONNECTION_MANAGER)
	net_mgmt_init_event_callback(&net_mgmt_cb, network_event_handler,
				     NET_EVENT_L4_CONNECTED);
	net_mgmt_add_event_callback(&net_mgmt_cb);
	conn_mgr_mon_resend_status();
	k_sem_take(&network_ready, K_FOREVER);
#endif
	LOG_DBG("Starting simplified TCP test server...");

	server_fd = iiod_network_create_server();
	if (server_fd < 0) {
		LOG_ERR("Failed to create test server");
		return;
	}

	LOG_DBG("Test server ready, waiting for connections on port %d...",
		CONFIG_LIBIIO_IIOD_NETWORK_PORT);

	LOG_DBG("Maximum concurrent clients: %d",
		CONFIG_LIBIIO_IIOD_NETWORK_CLIENT_MAX);

	/* Initialize tinyiiod global resources */
	LOG_DBG("Initializing tinyiiod resources...");
	if (iiod_init() < 0) {
		LOG_ERR("Failed to initialize tinyiiod resources");
		return;
	}

	/* Initialize client pool */
	for (i = 0; i < CONFIG_LIBIIO_IIOD_NETWORK_CLIENT_MAX; i++) {
		client_pool[i].fd = -EPERM;
		client_pool[i].in_use = false;
		client_pool[i].client_num = i;
	}

	while (1) {
		client_addr_len = sizeof(client_addr);
		LOG_DBG("Calling accept() (connections served: %d)...", counter);

		new_fd = accept(server_fd, (struct sockaddr *)&client_addr, &client_addr_len);
		if (new_fd < 0) {
			LOG_ERR("Accept failed: %d (errno: %d)", new_fd, errno);
			k_sleep(K_SECONDS(1));
			continue;
		}
		LOG_DBG("Accept successful! Client fd: %d", new_fd);

		ret = setsockopt(new_fd, IPPROTO_TCP, TCP_NODELAY, &nodelay,
				 sizeof(nodelay));
		if (ret < 0) {
			LOG_WRN("Failed to set TCP_NODELAY: %d", errno);
		}

		inet_ntop(client_addr.sin_family, &client_addr.sin_addr,
				addr_str, sizeof(addr_str));
		LOG_DBG("Connection #%d from %s", counter++, addr_str);

		client = iiod_network_alloc_client_data();
		if (!client) {
			LOG_ERR("Reached maximum number of clients!");
			close(new_fd);
			continue;
		}
		client->fd = new_fd;
		memcpy(&client->addr, &client_addr, sizeof(client_addr));

		client->thread_id = k_thread_create(&client->thread,
				client_thread_stacks[client->client_num],
				CONFIG_LIBIIO_IIOD_NETWORK_CLIENT_THREAD_STACK_SIZE,
				iiod_network_client_thread,
				client, NULL, NULL,
				CONFIG_LIBIIO_IIOD_NETWORK_CLIENT_THREAD_PRIORITY,
				0, K_NO_WAIT);
		if (!client->thread_id) {
			LOG_ERR("[Client %d] Failed to create thread", client->client_num);
			iiod_network_free_client_data(client);
			continue;
		}
	}

	/* Cleanup resources if server loop exits */
	if (shared_ctx) {
		iio_context_destroy(shared_ctx);
	}
	close(server_fd);
	iiod_cleanup();

	LOG_DBG("Network server thread exiting");
}

K_THREAD_DEFINE(iiod_network, CONFIG_LIBIIO_IIOD_NETWORK_SERVER_THREAD_STACK_SIZE,
		iiod_network_server_thread, NULL, NULL, NULL,
		CONFIG_LIBIIO_IIOD_NETWORK_SERVER_THREAD_PRIORITY, 0, 1);
