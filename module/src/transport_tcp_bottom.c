/* Copyright (c) 2025 Brilliant Labs
 * SPDX-License-Identifier: Apache-2.0
 *
 * Host-context half of the TCP Lua transport (ticket 0005): BSD sockets +
 * one pthread doing accept/read, compiled into the native_simulator runner
 * (no Zephyr headers, no Kconfig — see transport_tcp_bottom.h).
 *
 * Threading model: the pthread owns the sockets' read side and parses the
 * [channel][len16][payload] framing; completed frames are handed to the
 * Zephyr side one at a time through a mutex/condvar mailbox that the top's
 * pump thread polls (peek/pop). Holding at most one frame is deliberate —
 * the kernel's TCP receive buffer is the queue, so a slow consumer
 * backpressures the client instead of ballooning host memory. Writes come
 * straight from Zephyr threads under tx_lock on the non-blocking socket
 * (bounded POLLOUT waits), mirroring how hardware writers block on
 * notification credit.
 */

#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "nsi_tracing.h"
#include "transport_tcp_bottom.h"

#define TX_POLL_SLICE_MS  100
#define TX_TIMEOUT_MS     5000

static uint16_t cfg_port;
static uint32_t cfg_max_payload;

static int listen_fd = -1;
static volatile int client_fd = -1;
static volatile bool connected;
static volatile bool close_req;
static int wake_pipe[2] = {-1, -1};
static pthread_t rx_thread;

/* One-frame RX mailbox (pthread producer -> Zephyr pump consumer) */
static pthread_mutex_t rx_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t rx_consumed = PTHREAD_COND_INITIALIZER;
static uint8_t *rx_buf;
static uint32_t rx_len;
static uint8_t rx_ch;
static bool rx_pending;

/* Frame assembly state (pthread only) */
static uint8_t *stage_buf;
static uint8_t hdr[EMU_TCP_FRAME_HDR_LEN];
static uint32_t hdr_got;
static uint32_t pay_need, pay_got;
static uint8_t pay_ch;

/* TX serialization (called from Zephyr threads) */
static pthread_mutex_t tx_lock = PTHREAD_MUTEX_INITIALIZER;

static void set_nonblock(int fd)
{
	int flags = fcntl(fd, F_GETFL, 0);

	if (flags >= 0) {
		(void)fcntl(fd, F_SETFL, flags | O_NONBLOCK);
	}
}

static void drop_client(const char *why)
{
	int fd = client_fd;

	if (fd < 0) {
		return;
	}

	connected = false;
	client_fd = -1;
	close(fd);

	hdr_got = 0;
	pay_need = pay_got = 0;

	/* A frame parked in the mailbox belongs to the dead connection; the
	 * Zephyr side resets its rings on the disconnect edge anyway. */
	pthread_mutex_lock(&rx_lock);
	rx_pending = false;
	pthread_cond_signal(&rx_consumed);
	pthread_mutex_unlock(&rx_lock);

	nsi_print_trace("emu tcp: client disconnected (%s)\n", why);
}

/* Park the completed stage_buf frame in the mailbox; waits (with periodic
 * close_req checks) until the consumer freed it. */
static void handoff_frame(void)
{
	struct timespec ts;

	/* Control codes get the ISR-like out-of-band nudge (see the header)
	 * BEFORE the frame becomes consumable, so the pump's matching
	 * decrement can never precede this increment. Same first-byte match
	 * as the GATT write path. */
	if (pay_ch == EMU_TCP_CH_LUA && pay_need >= 1 && stage_buf[0] >= 0x02 &&
	    stage_buf[0] <= 0x07) {
		emu_tcp_ctrl_notify();
	}

	pthread_mutex_lock(&rx_lock);
	while (rx_pending && !close_req && client_fd >= 0) {
		clock_gettime(CLOCK_REALTIME, &ts);
		ts.tv_nsec += 20 * 1000 * 1000;
		if (ts.tv_nsec >= 1000000000L) {
			ts.tv_sec++;
			ts.tv_nsec -= 1000000000L;
		}
		(void)pthread_cond_timedwait(&rx_consumed, &rx_lock, &ts);
	}
	if (!rx_pending) {
		memcpy(rx_buf, stage_buf, pay_need);
		rx_len = pay_need;
		rx_ch = pay_ch;
		rx_pending = true;
	}
	pthread_mutex_unlock(&rx_lock);

	hdr_got = 0;
	pay_need = pay_got = 0;
}

/* One poll()-signalled read step of the frame parser. */
static void client_rx_step(void)
{
	ssize_t n;

	if (hdr_got < EMU_TCP_FRAME_HDR_LEN) {
		n = recv(client_fd, hdr + hdr_got, EMU_TCP_FRAME_HDR_LEN - hdr_got, 0);
		if (n == 0) {
			drop_client("EOF");
			return;
		}
		if (n < 0) {
			if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
				drop_client(strerror(errno));
			}
			return;
		}
		hdr_got += (uint32_t)n;
		if (hdr_got < EMU_TCP_FRAME_HDR_LEN) {
			return;
		}

		pay_ch = hdr[0];
		pay_need = (uint32_t)hdr[1] | ((uint32_t)hdr[2] << 8);
		pay_got = 0;

		/* Hardware honesty: a GATT write can neither exceed the MTU
		 * nor target an unknown characteristic. */
		if (pay_ch > EMU_TCP_CH_MAX || pay_need > cfg_max_payload) {
			nsi_print_warning(
				"emu tcp: protocol violation (channel %u, len %u > MTU %u) — "
				"dropping client\n",
				pay_ch, pay_need, cfg_max_payload);
			drop_client("protocol violation");
			return;
		}
		if (pay_need == 0) {
			handoff_frame();
		}
		return;
	}

	n = recv(client_fd, stage_buf + pay_got, pay_need - pay_got, 0);
	if (n == 0) {
		drop_client("EOF");
		return;
	}
	if (n < 0) {
		if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
			drop_client(strerror(errno));
		}
		return;
	}
	pay_got += (uint32_t)n;
	if (pay_got == pay_need) {
		handoff_frame();
	}
}

static void accept_step(void)
{
	int fd = accept(listen_fd, NULL, NULL);

	if (fd < 0) {
		return;
	}

	if (client_fd >= 0) {
		/* Single-connection policy, like the one BLE link */
		nsi_print_warning("emu tcp: second client refused\n");
		close(fd);
		return;
	}

	int one = 1;

	(void)setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
	set_nonblock(fd);

	hdr_got = 0;
	pay_need = pay_got = 0;
	client_fd = fd;
	connected = true;
	nsi_print_trace("emu tcp: client connected\n");
}

static void *tcp_thread_fn(void *arg)
{
	(void)arg;

	for (;;) {
		struct pollfd fds[3];
		int nfds = 0;
		int client_idx = -1;

		fds[nfds].fd = listen_fd;
		fds[nfds].events = POLLIN;
		nfds++;
		fds[nfds].fd = wake_pipe[0];
		fds[nfds].events = POLLIN;
		nfds++;
		if (client_fd >= 0) {
			client_idx = nfds;
			fds[nfds].fd = client_fd;
			fds[nfds].events = POLLIN;
			nfds++;
		}

		if (poll(fds, nfds, -1) < 0) {
			if (errno == EINTR) {
				continue;
			}
			nsi_print_warning("emu tcp: poll: %s\n", strerror(errno));
			return NULL;
		}

		if (fds[1].revents & POLLIN) {
			uint8_t scratch[16];

			while (read(wake_pipe[0], scratch, sizeof(scratch)) > 0) {
			}
		}
		if (close_req) {
			close_req = false;
			drop_client("requested");
		}

		if (fds[0].revents & POLLIN) {
			accept_step();
		}

		if (client_idx >= 0 && client_fd >= 0 &&
		    (fds[client_idx].revents & (POLLIN | POLLHUP | POLLERR))) {
			client_rx_step();
		}
	}

	return NULL;
}

int emu_tcp_bottom_start(uint16_t port, uint32_t max_payload)
{
	if (listen_fd >= 0) {
		return 0;
	}

	cfg_port = port;
	cfg_max_payload = max_payload;
	rx_buf = malloc(max_payload);
	stage_buf = malloc(max_payload);
	if (rx_buf == NULL || stage_buf == NULL) {
		nsi_print_warning("emu tcp: out of memory\n");
		return -1;
	}

	if (pipe(wake_pipe) != 0) {
		nsi_print_warning("emu tcp: pipe: %s\n", strerror(errno));
		return -1;
	}
	set_nonblock(wake_pipe[0]);

	int fd = socket(AF_INET, SOCK_STREAM, 0);

	if (fd < 0) {
		nsi_print_warning("emu tcp: socket: %s\n", strerror(errno));
		return -1;
	}

	int one = 1;

	(void)setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

	/* Loopback only: the REPL is arbitrary code execution and BLE at
	 * least demanded pairing; don't expose it to the LAN. */
	struct sockaddr_in addr = {
		.sin_family = AF_INET,
		.sin_port = htons(port),
		.sin_addr.s_addr = htonl(INADDR_LOOPBACK),
	};

	if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0 || listen(fd, 2) != 0) {
		nsi_print_warning("emu tcp: cannot listen on 127.0.0.1:%u: %s\n", port,
				  strerror(errno));
		close(fd);
		return -1;
	}
	set_nonblock(fd);
	listen_fd = fd;

	if (pthread_create(&rx_thread, NULL, tcp_thread_fn, NULL) != 0) {
		nsi_print_warning("emu tcp: pthread_create failed\n");
		close(fd);
		listen_fd = -1;
		return -1;
	}

	nsi_print_trace("emu tcp: Lua transport listening on 127.0.0.1:%u (MTU %u)\n", port,
			max_payload);
	return 0;
}

bool emu_tcp_bottom_is_connected(void)
{
	return connected;
}

int emu_tcp_bottom_rx_peek(uint8_t *channel)
{
	int ret = -1;

	pthread_mutex_lock(&rx_lock);
	if (rx_pending) {
		*channel = rx_ch;
		ret = (int)rx_len;
	}
	pthread_mutex_unlock(&rx_lock);
	return ret;
}

int emu_tcp_bottom_rx_pop(uint8_t *buf, uint32_t maxlen)
{
	int ret = -1;

	pthread_mutex_lock(&rx_lock);
	if (rx_pending && rx_len <= maxlen) {
		memcpy(buf, rx_buf, rx_len);
		ret = (int)rx_len;
		rx_pending = false;
		pthread_cond_signal(&rx_consumed);
	}
	pthread_mutex_unlock(&rx_lock);
	return ret;
}

static int send_all(int fd, const uint8_t *p, uint32_t n)
{
	int waited_ms = 0;

	while (n > 0) {
		ssize_t w = send(fd, p, n, MSG_NOSIGNAL);

		if (w > 0) {
			p += w;
			n -= (uint32_t)w;
			waited_ms = 0;
			continue;
		}
		if (w < 0 && errno == EINTR) {
			continue;
		}
		if (w < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
			if (waited_ms >= TX_TIMEOUT_MS) {
				return -1;
			}

			struct pollfd pfd = {.fd = fd, .events = POLLOUT};

			(void)poll(&pfd, 1, TX_POLL_SLICE_MS);
			waited_ms += TX_POLL_SLICE_MS;
			continue;
		}
		return -1;
	}
	return 0;
}

int emu_tcp_bottom_tx(uint8_t channel, const uint8_t *data, uint32_t len)
{
	if (len > cfg_max_payload) {
		return -2;
	}

	pthread_mutex_lock(&tx_lock);

	int fd = client_fd;

	if (fd < 0) {
		pthread_mutex_unlock(&tx_lock);
		return -1;
	}

	uint8_t frame_hdr[EMU_TCP_FRAME_HDR_LEN] = {channel, (uint8_t)(len & 0xFF),
						    (uint8_t)(len >> 8)};
	int ret = send_all(fd, frame_hdr, sizeof(frame_hdr));

	if (ret == 0) {
		ret = send_all(fd, data, len);
	}

	pthread_mutex_unlock(&tx_lock);
	return (ret == 0) ? (int)len : -1;
}

void emu_tcp_bottom_disconnect(void)
{
	if (client_fd < 0) {
		return;
	}
	close_req = true;
	if (wake_pipe[1] >= 0) {
		uint8_t b = 0;

		(void)!write(wake_pipe[1], &b, 1);
	}
}
