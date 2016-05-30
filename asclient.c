#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/epoll.h>
#include <sys/timerfd.h>
#include <sys/signalfd.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <time.h>
#include <string.h>
#include <strings.h>
#include <stdlib.h>
#include <stdio.h>
#include <assert.h>
#include "util.h"

enum tftp_commands {
	OP_RRQ  = 1,
	OP_WRQ  = 2,
	OP_DATA = 3,
	OP_ACK  = 4,
	OP_ERROR = 5,
	OP_OACK = 6,
};

enum tftp_client_state {
	START,
	SEND_RRQ,
	WANT_OACK_ACK,
	WANT_DATA,
	SEND_ACK,
	LAST_ACK,
};

struct tftp_hdr {
	uint16_t opcode;
} __attribute__((packed));

struct tftp_rrq {
	uint16_t opcode;
	char filename[0];
} __attribute__((packed));

struct tftp_data {
	uint16_t opcode;
	uint16_t block;
	char data[0];
} __attribute__((packed));

enum {
	TFTP_PORT = 69,
};

enum {
	BLKSIZE_MIN = 8,
	BLKSIZE_DFLT = 512,
	BLKSIZE_MAX = 65464,
};

enum {
	TFTP_HDR_SZ = 4,
	DATA_BUF_LEN = BLKSIZE_DFLT,
	MAX_EVENTS = 128,
};

enum {
	CBUF_LEN = 256,
};

struct tftp_event_loop;

struct tftp_client {
	int sock;
	char *buf;
	size_t buf_len;
	size_t reply_len;
	int state;
	uint16_t block_num;
	uint16_t block_size;
	uint16_t server_tid;
	uint16_t my_tid;
	time_t ts; /* measured in milliseconds since event loop start */
	struct sockaddr_storage server_addr;
	char *filename;
	struct tftp_event_loop *evloop;
	struct tftp_client *next;
};

struct tftp_event_loop {
	int epoll_fd;
	int timer_fd;
	int signal_fd;
	char cbuf[CBUF_LEN];
	size_t cbuf_len;
	struct sockaddr_storage curr_server_addr;
	struct sockaddr_storage curr_orig_dst;
	struct tftp_client *clients;
	struct tftp_client *dead_clients;
	int valid_orig_dst;
	int exit_on_last_client;
	time_t ts; /* measured in milliseconds since the event loop start */
};

int add_tftp_client(struct tftp_event_loop *ctx, struct tftp_client *client);
int step_tftp_client(struct tftp_event_loop *ctx, struct tftp_client *client);

struct tftp_client *alloc_client(size_t data_buf_len);
void free_client(struct tftp_client **ptrp);
void destroy_client(struct tftp_client **ptrp);

ssize_t tftp_recv_pkt(struct tftp_event_loop *ctx, struct tftp_client *client);
ssize_t send_rrq(struct tftp_event_loop *ctx, struct tftp_client *client);
ssize_t send_ack(struct tftp_event_loop *ctx, struct tftp_client *client);
int step_tftp_client(struct tftp_event_loop *ctx, struct tftp_client *client);

/* ACK or ignore a data packet */
int handle_data_pkt(struct tftp_event_loop *ctx, struct tftp_client *client) {
	ssize_t bytes_read = 0;
	struct tftp_hdr *hdr = NULL;
	size_t expected_data_len = 0;
	uint16_t opcode, block_num = 0;
	struct sockaddr_in *server_addr = NULL;
	if ((bytes_read = tftp_recv_pkt(ctx, client)) < 0) {
		fprintf(stderr, "%s: failed to recv datagram\n", __func__);
		return 0;
	}
	if (unlikely(bytes_read == 0)) {
		/* FIXME: implement resending RRQ/ACK */
		fprintf(stderr, "%s: DBG: nothing to read from server\n", __func__);
		return 0;
	}
	if (unlikely(bytes_read < sizeof(struct tftp_hdr))) {
		fprintf(stderr, "%s: INF: tftp packet too short: %d < %d bytes\n",
				__func__,
				(int)bytes_read,
				(int)sizeof(struct tftp_hdr));
		return 0;
	}
	server_addr = (struct sockaddr_in *)(&ctx->curr_server_addr);
	hdr = (struct tftp_hdr *)client->buf;
	opcode = ntohs(hdr->opcode);
	if (unlikely(OP_ERROR == opcode)) {
		fprintf(stderr, "%s: got an error from server, closing\n", __func__);
		free_client(&client);
		return -1;
	}

	if (WANT_DATA == client->state) {
		if (OP_DATA != opcode) {
			fprintf(stderr, "%s: DBG: expected DATA packet, got %d => ignore",
					__func__, (int)opcode);
			return 0;
		}
	}

	if (WANT_OACK_ACK == client->state) {
		if ((OP_OACK != opcode) && (OP_DATA != opcode)) {
			fprintf(stderr, "%s: expected OACK or DATA, got %d\n",
					__func__, (int)opcode);
			return 0;
		}
		/* This is the 1st reply we've received from the server.
		 * Connect the socket so it won't receive other clients'
		 * replies, and record the port number for TID checks
		 */
		client->server_tid = server_addr->sin_port;
		if (connect(client->sock, (struct sockaddr *)server_addr, sizeof(*server_addr)) < 0) {
			perror("connect");
			return -1;
		}
		if (OP_OACK != opcode) {
			client->block_size = BLKSIZE_DFLT;
		}
		if (OP_OACK == opcode) {
			/* TODO: check if the server acknowledged blksize */
		}
	} else {
		if (client->server_tid != server_addr->sin_port) {
			fprintf(stderr, "%s: DBG: got a datagram with wrong TID, ignoring\n", __func__);
			return 0;
		}
	}

	if (OP_OACK == opcode) {
		block_num = 0;
		client->state = SEND_ACK;
	}

	if (likely(OP_DATA == opcode)) {
		struct tftp_data *data = NULL;
		if (unlikely(bytes_read < sizeof(struct tftp_data))) {
			fprintf(stderr, "%s: INF: packet too short: %d < %d bytes\n",
					__func__,
					(int)client->reply_len,
					(int)sizeof(struct tftp_data));
			return 0;
		}
		data = (struct tftp_data *)client->buf;
		block_num = ntohs(data->block);

		if (block_num != client->block_num + 1) {
			fprintf(stderr, "%s: ACK retransmitted packet\n", __func__);
		} else {
			client->block_num++;
		}
		expected_data_len = sizeof(*data) + client->block_size;
		if (likely(bytes_read == expected_data_len)) {
			client->state = SEND_ACK;
		} else {
			if (client->reply_len < expected_data_len) {
				client->state = LAST_ACK;
			} else {
				/* reply too large, ignore */
			}
		}
	}
	if (likely(SEND_ACK == client->state)) {
		return send_ack(ctx, client);
	} else {
		return step_tftp_client(ctx, client);
	}
}

int step_tftp_client(struct tftp_event_loop *ctx, struct tftp_client *client) {
	if (likely(WANT_DATA == client->state)) {
		return handle_data_pkt(ctx, client);
	}
	switch (client->state) {
		case WANT_DATA:
		case WANT_OACK_ACK:
			return handle_data_pkt(ctx, client);
			break;
		case SEND_ACK:
			return send_ack(ctx, client);
			break;
		case LAST_ACK:
			send_ack(ctx, client);
			free_client(&client);
			return 0;
			break;
		default:
			fprintf(stderr, "%s: unexpected state: %d\n",
					__func__, client->state);
			return -1;
	}
}

ssize_t tftp_recv_pkt(struct tftp_event_loop *ctx, struct tftp_client *client) { 
	ssize_t bytes_read = 0;
	struct msghdr msg;
	struct iovec iov[1];
	assert(ctx);
	assert(client);
	ctx->valid_orig_dst = 0;

	bzero(&msg, sizeof(msg));
	iov[0].iov_base = client->buf;
	iov[0].iov_len = client->buf_len;
	msg.msg_name = &ctx->curr_server_addr;
	msg.msg_namelen = sizeof(ctx->curr_server_addr);
	msg.msg_iov = iov;
	msg.msg_iovlen = 1;
	msg.msg_control = ctx->cbuf;
	msg.msg_controllen = ctx->cbuf_len;

	if ((bytes_read = recvmsg(client->sock, &msg, 0)) < 0) {
		if (EAGAIN == errno || EWOULDBLOCK == errno) {
			fprintf(stderr, "%s: FIXME: handle EAGAIN\n", __func__);
		} else {
			perror("recvmsg");
		}
		return -1;
	}
	client->reply_len = bytes_read;

	for (struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msg);
	     cmsg && cmsg->cmsg_len >= sizeof(*cmsg);
	     cmsg = CMSG_NXTHDR(&msg, cmsg)) {
		if (SOL_IP != cmsg->cmsg_level || IP_ORIGDSTADDR != cmsg->cmsg_type) {
			continue;
		} else {
			memcpy(&ctx->curr_orig_dst, CMSG_DATA(cmsg), sizeof(struct sockaddr_in));
			ctx->valid_orig_dst = 1;
			break;
		}
	}

	return bytes_read;
}

ssize_t send_ack(struct tftp_event_loop *ctx, struct tftp_client *client) {
	struct msghdr msg;
	struct iovec iov[1];
	size_t bytes_sent;
	struct {
		uint16_t opcode;
		uint16_t block_num;
	} __attribute__((packed)) ack_pkt;
	ack_pkt.opcode = htons(OP_ACK);
	ack_pkt.block_num = htons(client->block_num);
	iov[0].iov_base = &ack_pkt;
	iov[0].iov_len = sizeof(ack_pkt);
	bzero(&msg, sizeof(msg));
	msg.msg_iov = iov;
	msg.msg_iovlen = 1;
	if (likely((bytes_sent = sendmsg(client->sock, &msg, 0)) > 0)) {
		client->state = WANT_DATA;
	} else {
		if (EAGAIN == errno || EWOULDBLOCK == errno) {
			fprintf(stderr, "%s: FIXME: handle EAGAIN\n", __func__);
		} else {
			perror("sendmsg");
		}
		return -1;
	}
	return bytes_sent;
}

ssize_t send_rrq(struct tftp_event_loop *ctx, struct tftp_client *client)
{
	struct msghdr msg;
	ssize_t bytes_sent;
	struct tftp_hdr hdr;
	struct sockaddr_storage server_addr;
	char blksize_str[sizeof("XXXXX")];
	bzero(&msg, sizeof(msg));
	sprintf(blksize_str, "%d", (int)client->block_size);

	hdr.opcode = htons(OP_RRQ);
	struct iovec iov[] = {
		{&hdr, sizeof(hdr)},
		{client->filename, strlen(client->filename) + 1},
		{"octet", strlen("octet") + 1},
		{"blksize", strlen("blksize") + 1},
		{blksize_str, strlen(blksize_str) + 1}
	};

	msg.msg_iov = iov;
	msg.msg_iovlen = ARRAY_SIZE(iov);

	memcpy(&server_addr, &client->server_addr, sizeof(server_addr));
	((struct sockaddr_in *)&server_addr)->sin_port = htons(TFTP_PORT);
	msg.msg_name = &server_addr;
	msg.msg_namelen = sizeof(server_addr);

	bytes_sent = sendmsg(client->sock, &msg, 0);
	if (bytes_sent > 0) {
		client->state = WANT_OACK_ACK;
	} else {
		if (EAGAIN == errno || EWOULDBLOCK == errno) {
			fprintf(stderr, "%s: FIXME: handle EAGAIN\n", __func__);
		} else {
			perror("sendmsg");
		}
		return -1;
	}
	return bytes_sent;
}

int add_tftp_client(struct tftp_event_loop *ctx, struct tftp_client *client) {
	struct sockaddr_storage local_addr;
	socklen_t local_addr_len = 0;
	struct epoll_event ev;
	int one = 1;
	bzero(&ev, sizeof(ev));
	if ((client->sock = socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK, 0)) < 0) {
		perror("socket");
		goto err;
	}
	if (setsockopt(client->sock, SOL_IP, IP_RECVORIGDSTADDR, &one, sizeof(one)) != 0) {
		perror("setsockopt IP_RECVORIGDSTADDR");
		goto err;
	}
	if (connect(client->sock, (struct sockaddr *)&client->server_addr, sizeof(client->server_addr)) < 0) {
		perror("connect");
		goto err;
	}
	/* read back the assigned port, will be used in the TID check */
	if (getsockname(client->sock, (struct sockaddr *)&local_addr,
			&local_addr_len) < 0) {
		perror("getsockname");
		goto err;
	}
	client->my_tid = ((struct sockaddr_in *)&local_addr)->sin_port;
	ev.events = EPOLLIN;
	ev.data.ptr = client;
	if (epoll_ctl(ctx->epoll_fd, EPOLL_CTL_ADD, client->sock, &ev) < 0) {
		perror("epoll_ctl");
		goto err;
	}
	client->state = SEND_RRQ;
	list_append(&ctx->clients, client);
	client->evloop = ctx;
	return send_rrq(ctx, client);
err:
	list_remove(&ctx->clients, client);
	if (client->sock >= 0) {
		close(client->sock);
		client->sock = -1;
	}
	return -1;
}

int tftp_event_loop_start(struct tftp_event_loop *ctx) {
	bzero(ctx->cbuf, sizeof(ctx->cbuf));
	ctx->cbuf_len = CBUF_LEN;
	if ((ctx->epoll_fd = epoll_create(MAX_EVENTS)) < 0) {
		perror("epoll_create");
		goto err;
	}
	return 0;
err:
	return -1;
}

void cleanup_dead_clients(struct tftp_client **dead_headp) {
	struct tftp_client *deadh = NULL;
	if (!dead_headp || !*dead_headp) {
		return;
	}
	deadh = *dead_headp;
	*dead_headp = NULL;
	for (struct tftp_client *c = deadh, *next = NULL; c; c = next) {
		next = c->next;
		destroy_client(&c);
	}
}

int is_dead_client(struct tftp_event_loop *ctx, struct tftp_client const *client) {
	for (struct tftp_client const *c = ctx->dead_clients; c; c = c->next) {
		if (c == client) {
			return 1;
		}
	}
	return 0;
}

int tftp_event_loop_run(struct tftp_event_loop *ctx) {
	struct epoll_event events[MAX_EVENTS];
	struct timespec start_ts;
	int event_cnt = 0;
	if (clock_gettime(CLOCK_MONOTONIC, &start_ts) < 0) {
		perror("clock_gettime");
		return -1;
	}
	for (;;) {
		struct tftp_client *client = NULL;
		if (ctx->exit_on_last_client && !ctx->clients && !ctx->dead_clients) {
			fprintf(stderr, "%s: no more clients, exiting\n", __func__);
			goto out;
		}
		if ((event_cnt = epoll_wait(ctx->epoll_fd, events, MAX_EVENTS, -1)) < 0) {
			perror("epoll_wait");
			continue;
		}
		for (int i = 0; i < event_cnt; ++i) {
			client = events[i].data.ptr;
			if (!client) {
				fprintf(stderr, "%s: bogus event without a client\n", __func__);
				continue;
			}
			if (unlikely(is_dead_client(ctx, client))) {
				continue;
			}
			step_tftp_client(ctx, client);
			cleanup_dead_clients(&ctx->dead_clients);
		}
	}
out:
	cleanup_dead_clients(&ctx->dead_clients);
	return 0;
}

static void print_help() {
	printf("Usage: asclient -r <remote file> <server> [-b block size] [-c connections number]\n");
}

int main(int argc, char **argv) {
	struct tftp_client *client = NULL;
	struct sockaddr_storage server_addr;
	struct tftp_event_loop loop;
	int opt = -1, client_count = 1, block_size = BLKSIZE_DFLT;
	char *remote_file = NULL, *server_name = NULL;

	bzero(&loop, sizeof(loop));
	bzero(&server_addr, sizeof(server_addr));

	while ((opt = getopt(argc, argv, "hr:b:c:")) != -1) {
		switch (opt) {
			case 'h':
				print_help();
				exit(EXIT_SUCCESS);
				break;
			case 'r':
				if (!(remote_file = strdup(optarg))) {
					fprintf(stderr, "%s: OOM when parsing options\n", __func__);
					exit(EXIT_FAILURE);
				}
				break;
			case 'b':
				block_size = atoi(optarg);
				if (!block_size) {
					block_size = BLKSIZE_DFLT;
				}
				if (block_size < BLKSIZE_MIN || block_size > BLKSIZE_MAX) {
					block_size = BLKSIZE_DFLT;
				}
				break;
			case 'c':
				client_count = atoi(optarg);
				if (!client_count) {
					client_count = 1;
				}
				break;
			case '?':
				print_help();
				exit(EXIT_FAILURE);
				break;
		}
	}
	if (optind >= argc) {
		fprintf(stderr, "%s: expected server name after options\n", argv[0]);
		print_help();
		exit(EXIT_FAILURE);
	}
	server_name = argv[optind];
	server_addr.ss_family = AF_INET;
	if (inet_pton(AF_INET, server_name, &(((struct sockaddr_in *)&server_addr)->sin_addr)) != 1) {
		fprintf(stderr, "%s: invalid IPv4 address: %s\n", argv[0], server_name);
		exit(EXIT_FAILURE);
	}
	if (tftp_event_loop_start(&loop) < 0) {
		fprintf(stderr, "%s: failed to start event loop\n", __func__);
		goto out;
	}
	for (int cnt = 0; cnt < client_count ; ++cnt) {
		if (!(client = alloc_client(1500))) {
			fprintf(stderr, "%s: buy more RAM\n", __func__);
			goto out;
		}
		client->block_size = block_size;
		if (!(client->filename = strdup(remote_file))) {
			fprintf(stderr, "%s: buy more RAM [2]\n", __func__);
			goto out;
		}
		memcpy(&client->server_addr, &server_addr, sizeof(server_addr));
		if (client->server_addr.ss_family != AF_INET) {
			fprintf(stderr, "%s: bogus ss_family: got %d, expected %d",
					__func__,
					client->server_addr.ss_family,
					AF_INET);
			goto out;
		}
		if (add_tftp_client(&loop, client) <= 0) {
			fprintf(stderr, "%s: failed to send RRQ\n", __func__);
			goto out;
		}
		client = NULL;
	}
	loop.exit_on_last_client = 1;
	tftp_event_loop_run(&loop);
	exit(EXIT_SUCCESS);
out:
	for (struct tftp_client *c = loop.clients, *next = NULL; c; c = next) {
		next = c->next;
		destroy_client(&c);
	}
	exit(EXIT_FAILURE);
}

struct tftp_client *alloc_client(size_t buf_len)
{
	struct tftp_client *c = NULL;
	if (buf_len < BLKSIZE_DFLT + 4) {
		goto err;
	}
	
	if (!(c = calloc(1, sizeof(struct tftp_client)))) {
		goto err;
	}
	c->state = START;
	c->sock = -1;
	if (!(c->buf = calloc(1, buf_len))) {
		goto err;
	}
	c->buf_len = buf_len;
	c->block_size = BLKSIZE_DFLT;
	return c;
err:
	destroy_client(&c);
	return NULL;
}

void free_client_impl(struct tftp_client **ptrp, int destroy) {
	struct tftp_client *c = NULL;
	struct tftp_event_loop *loop = NULL;
	if (!ptrp || !*ptrp) {
		return;
	}
	c = *ptrp;
	if (c->evloop) {
		loop = c->evloop;
		if (c->sock >= 0) {
			if (epoll_ctl(loop->epoll_fd, EPOLL_CTL_DEL, c->sock, NULL) < 0) {
				perror("epoll_ctl DEL");
			}
		}
		list_remove(&loop->clients, c);
		if (!destroy) {
			list_append(&loop->dead_clients, c);
		}
	}
	if (c->sock >= 0) {
		close(c->sock);
		c->sock = -1;
	}
	if (c->buf) {
		free(c->buf);
		c->buf = NULL;
		c->buf_len = 0;
	}
	if (destroy) {
		*ptrp = NULL;
		free(c);
	}
}

void destroy_client(struct tftp_client **ptrp) {
	free_client_impl(ptrp, /* destroy = */ 1);
}

void free_client(struct tftp_client **ptrp) {
	free_client_impl(ptrp, /* destroy = */ 0);
}
