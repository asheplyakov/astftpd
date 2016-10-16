#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <pthread.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/epoll.h>
#include <sys/timerfd.h>
#include <sys/signalfd.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <time.h>
#include <string.h>
#include <strings.h>
#include <stdlib.h>
#include <stdio.h>
#include "util.h"

enum tftp_commands {
	OP_RRQ  = 1,
	OP_WRQ  = 2,
	OP_DATA = 3,
	OP_ACK  = 4,
	OP_ERROR = 5,
	OP_OACK = 6,
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

struct tftp_ack {
	uint16_t opcode;
	uint16_t block;
} __attribute__((packed));

struct tftp_oack {
	uint16_t opcode;
	char opts[0];
} __attribute__((packed));

struct tftp_error {
	uint16_t opcode;
	uint16_t errcode;
	char data[0];
} __attribute__((packed));

enum {
	TFTP_PORT = 69,
};

enum {
	MAX_EVENTS = 128,
	CLIENTS_AT_ONCE = 32,
};

enum {
	TFTP_BUFSZ = 1024,
	TFTP_CLIENT_BUFSZ = 128,
};

enum {
	BLKSIZE_DFLT = 512,
};

enum tftp_client_state {
	S_RRQ_RECIEVED   = 1,
	S_SEND_DATA      = 2,
	S_WAIT_ACK       = 3,
	S_WAIT_FINAL_ACK = 4,
	S_TIMEOUT        = 5,
};

/*
 * stores the whole file cached in the RAM
 * The data gets read in before starting server threads, after that both
 * the list and its entries are immutable (hence no locks).
 * TODO: use a lock free list instead
 */
struct tftpd_file {
	char *filename;
	char *cache;
	size_t size;
	int fd;
	int refcnt;
	struct tftpd_file *next;
};

ssize_t cache_tftpd_file(struct tftpd_file *tf);
void close_tftpd_file(struct tftpd_file *tf);
int populate_file_cache(struct tftpd_file *head);
struct tftpd_file *find_tftpd_file(struct tftpd_file *head, const char *filename);
struct tftpd_file *add_tftpd_file(struct tftpd_file **headp, const char *filename);

/* No locks - each thread maintains its own clients list */
struct tftpd_client {
	int sock;
	int fd;
	struct tftpd_file *file; // owned by the main thread
	char *data; // points into file->cache and is owned by file
	size_t data_len;
	uint16_t block_num;
	uint16_t block_size;
	uint16_t tftp_tid; // local port number, in network byte order
	int pending;
	int state;
	int has_options;
	struct sockaddr_storage client_addr;
	char buf[TFTP_CLIENT_BUFSZ];
	size_t buf_len;
	size_t reply_len;
	char str_addr[INET_ADDRSTRLEN + sizeof("12345")];
	time_t sent_ts; /* measured in milliseconds since the server start */
	time_t acked_ts;
	struct tftpd_client *next;
};

struct tftpd_ctx;

void tftpd_send_error(struct tftpd_ctx *ctx, struct tftpd_client *client,
		      uint16_t code, const char *msg);
void tftpd_close_connection(struct tftpd_ctx *ctx, struct tftpd_client *client);
void tftpd_client_reset_timestamps(struct tftpd_ctx *ctx, struct tftpd_client *client);

struct tftpd_conf {
	unsigned int client_timeout;
	uint16_t port;
	int check_tid;
};

struct tftpd_ctx {
	int sock;
	int epoll_fd;
	int timer_fd;
	int signal_fd;
	struct tftpd_client *clients;
	struct tftpd_file *files;
	char buf[TFTP_BUFSZ];
	char cbuf[TFTP_BUFSZ];
	size_t buf_len;
	size_t cbuf_len;
	struct sockaddr_storage curr_client;
	struct sockaddr_storage curr_dest;
	time_t last_ts;
	struct tftpd_client *dead_clients;
	struct tftpd_conf conf;
};

/*
 * @param optsp pointer to TFTP options start, set by this function
 * @param optlenp length of TFTP options, set by this function
 * @return requested file name, can be NULL if the message is invalid
 */
char *tftp_parse_rrq(struct tftp_rrq *rrq, size_t len, char **optsp, size_t *optlenp);

/*
 * @param optstart pointer to TFTP options start
 * @param optslen total length of TFTP options
 * @param optname the name of the option to look for
 * @param optlenp the length of the option in question, set by this function
 * @return pointer to the option value, located in [optstart, optstart + optlen)
 */
char *tftp_find_option(char *optstart, size_t optslen, const char *optname, size_t *optlenp);

/*
 * @param optstart pointer to TFTP options start
 * @param blksize TFTP block size, set by this function
 * @return 0 if block size is not specified or is invalid, 1 otherwize
 */
int tftp_get_blksize(char *optstart, size_t optslen, int *blksize);

static int sprintf_addr(char *buf, size_t buf_len, struct sockaddr_storage const *addr);

/* @return 1 if the daemon should exit, otherwise 0 */
int tftpd_handle_signals(struct tftpd_ctx *ctx);

static ssize_t tftpd_recv_pkt(struct tftpd_ctx *ctx, int sock)
{
	struct msghdr msg;
	struct iovec iov;
	ssize_t bytes_read = 0;
	int have_orig_dest = 0;
	iov.iov_base = ctx->buf;
	iov.iov_len = ctx->buf_len;
	msg.msg_name = &ctx->curr_client;
	msg.msg_namelen = sizeof(ctx->curr_client);
	msg.msg_iov = &iov;
	msg.msg_iovlen = 1;
	msg.msg_control = ctx->cbuf;
	msg.msg_controllen = ctx->cbuf_len;

	if ((bytes_read = recvmsg(sock, &msg, 0)) < 0) {
		bzero(&ctx->curr_dest, sizeof(ctx->curr_dest));
		bzero(&ctx->curr_client, sizeof(ctx->curr_client));
		if (EWOULDBLOCK == errno || EAGAIN == errno) {
			return 0;
		} else {
			perror("recvmsg");
			return -1;
		}
	}

	for (struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msg);
	     cmsg && cmsg->cmsg_len >= sizeof(*cmsg);
	     cmsg = CMSG_NXTHDR(&msg, cmsg)) {
		if (SOL_IP != cmsg->cmsg_level || IP_ORIGDSTADDR != cmsg->cmsg_type) {
			continue;
		}
		memcpy(&ctx->curr_dest, CMSG_DATA(cmsg), sizeof(struct sockaddr_in));
		have_orig_dest = 1;
		break;
	}
	if (unlikely(!have_orig_dest)) {
		fprintf(stderr, "%s: WARN: unknown orig dest\n", __func__);
		bzero(&ctx->curr_dest, sizeof(ctx->curr_dest));
		return -1;
	}
	if (unlikely(bytes_read <= sizeof(struct tftp_hdr))) {
		fprintf(stderr, "%s: datagram is too short\n", __func__);
		return -1;
	}
	return bytes_read;
}

struct tftpd_client *tftpd_new_client(struct tftpd_ctx *ctx)
{
	struct sockaddr_storage local_addr;
	socklen_t local_addr_size = sizeof(local_addr);
	int one = 1;
	struct tftpd_client *client;
	struct epoll_event ev;
	struct itimerspec its;
	client = calloc(1, sizeof(struct tftpd_client));
	if (!client) {
		fprintf(stderr, "%s: ERR: buy more RAM\n", __func__);
		return NULL;
	}
	client->block_size = BLKSIZE_DFLT;
	client->block_num = 1;
	client->buf_len = TFTP_CLIENT_BUFSZ;
	memcpy(&client->client_addr, &ctx->curr_client, sizeof(client->client_addr));
	if (sprintf_addr(client->str_addr, sizeof(client->str_addr), &ctx->curr_client) < 0) {
		bzero(client->str_addr, sizeof(client->str_addr));
		fprintf(stderr, "%s: WARN: failed to print the client address\n", __func__);
	}
	if ((client->sock = socket(ctx->curr_client.ss_family, SOCK_DGRAM | SOCK_NONBLOCK, 0)) < 0) {
		perror("client socket");
		goto err_socket;
	}
	if (ctx->curr_dest.ss_family != AF_INET) {
		fprintf(stderr, "%s: only IPv4 is supported\n", __func__);
		goto err_socket;
	}
	/* bind the client socket to local IP */
	memcpy(&local_addr, &ctx->curr_dest, sizeof(local_addr));
	((struct sockaddr_in *)&local_addr)->sin_port = 0;
	if (bind(client->sock, (struct sockaddr *)&local_addr, sizeof(local_addr)) < 0) {
		perror("client bind");
		goto err_socket;
	}
	/* read back the assigned port, will be used in the TID check */
	if (getsockname(client->sock, (struct sockaddr *)&local_addr, &local_addr_size) < 0) {
		perror("getsockname");
		goto err_socket;
	}
	client->tftp_tid = ((struct sockaddr_in *)&local_addr)->sin_port;
	if (connect(client->sock, (struct sockaddr *)&(client->client_addr), sizeof(client->client_addr)) < 0) {
		perror("client connect");
		goto err_socket;
	}

	/* recieve the original dst address to check the TID */
	if (setsockopt(client->sock, SOL_IP, IP_RECVORIGDSTADDR, &one, sizeof(one)) != 0) {
		perror("setsockopt IP_RECVORIGDSTADDR");
		goto err_socket;
	}

	bzero(&ev, sizeof(ev));
	ev.data.ptr = client;
	ev.events = EPOLLIN;
	if (epoll_ctl(ctx->epoll_fd, EPOLL_CTL_ADD, client->sock, &ev) < 0) {
		perror("client epoll_ctl");
		goto err_socket;
	}
	tftpd_client_reset_timestamps(ctx, client);
	if (!ctx->clients) {
		/* rearm the timer */
		bzero(&its, sizeof(its));
		its.it_value.tv_sec = ctx->conf.client_timeout;
		its.it_interval.tv_sec = ctx->conf.client_timeout;
		if (timerfd_settime(ctx->timer_fd, 0, &its, NULL) < 0) {
			fprintf(stderr, "%s: failed to rearm the timer\n", __func__);
		}
	}
	list_append(&ctx->clients, client);
	return client;
err_socket:
	if (client->sock >= 0) {
		close(client->sock);
		client->sock = -1;
	}
	if (client) {
		free(client);
	}
	return NULL;
}

ssize_t tftpd_read_data_block(struct tftpd_ctx *ctx, struct tftpd_client *client)
{
	struct tftp_data *reply = (struct tftp_data *)client->buf;
	size_t offset = 0, chunk_len = 0;
	if (client->pending) {
		return 0;
	}
	reply->opcode = htons(OP_DATA);
	reply->block = htons(client->block_num);
	offset = (client->block_num - 1)*client->block_size;
	if (offset + client->block_size > client->file->size) {
		client->state = S_WAIT_FINAL_ACK;
		chunk_len = client->file->size - offset;
	} else {
		chunk_len = client->block_size;
	}
	client->data = client->file->cache + offset;
	client->data_len = chunk_len;
	client->reply_len = sizeof(*reply);
	return client->reply_len + chunk_len;
}

int subscribe_sock_is_writable(struct tftpd_ctx *ctx, struct tftpd_client *client, int val)
{
	struct epoll_event ev;
	bzero(&ev, sizeof(ev));
	ev.data.ptr = client;
	ev.events = EPOLLIN;
	if (val) {
		ev.events |= EPOLLOUT;
	}
	if (epoll_ctl(ctx->epoll_fd, EPOLL_CTL_MOD, client->sock, &ev) < 0) {
		perror("subscribe_sock_is_writable");
		return -1;
	}
	return 0;
}

ssize_t tftpd_send_reply_pkt(struct tftpd_ctx *ctx, struct tftpd_client *client)
{
	size_t bytes_sent = 0;
	struct msghdr msg;
	struct iovec iov[2];
	bzero(&msg, sizeof(msg));
	iov[0].iov_base = client->buf;
	iov[0].iov_len = client->reply_len;
	msg.msg_iov = iov;
	if (client->data_len) {
		iov[1].iov_base = client->data;
		iov[1].iov_len = client->data_len;
		msg.msg_iovlen = 2;
	} else {
		msg.msg_iovlen = 1;
	}
	bytes_sent = sendmsg(client->sock, &msg, 0);
	if (likely(bytes_sent > 0)) {
		if (unlikely(client->pending)) {
			/* write readiness notification is not necessary any more,
			 * unsubscribe to prevent epoll_wait from awaking every cycle */
			if (subscribe_sock_is_writable(ctx, client, 0) < 0) {
				fprintf(stderr, "%s: client %s: failed to unsubscribe from "
						"socket write readiness notifications\n",
						__func__, client->str_addr);
				return -1;
			}
			client->pending = 0;
		}
		/* Not quite accurate but saves a number of clock_gettime syscalls */
		client->sent_ts = ctx->last_ts;
		return bytes_sent;
	}
	if (EAGAIN == errno || EWOULDBLOCK == errno) {
		if (client->pending) {
			return 0;
		}
		/* ask the kernel to notify us when it's possible to send data */
		if (subscribe_sock_is_writable(ctx, client, 1) < 0) {
			fprintf(stderr, "%s: client %s: failed to subscribe to "
					"socket write readiness notifications\n",
					__func__, client->str_addr);
			return -1;
		}
		client->pending = 1;
		return 0;
	} else {
		fprintf(stderr, "%s: client %s: send failed: %s\n",
				__func__, client->str_addr,
				strerror(errno));
		return -1;
	}
}

ssize_t tftpd_send_data(struct tftpd_ctx *ctx, struct tftpd_client *client)
{
	ssize_t bytes_read = 0;
	if (likely(!client->pending)) {
		bytes_read = tftpd_read_data_block(ctx, client);
	}
	if (unlikely(bytes_read < 0)) {
		fprintf(stderr, "%s: client %s: failed to read block %d\n",
				__func__, client->str_addr, client->block_num);
		return -1;
	}
	return tftpd_send_reply_pkt(ctx, client);
}

ssize_t tftpd_send_oack(struct tftpd_ctx *ctx, struct tftpd_client *client)
{
	struct tftp_oack *reply = (struct tftp_oack *)client->buf;
	char *optp = reply->opts;
	char *optname = "blksize";
	size_t buf_len = client->buf_len;
	size_t reply_len = sizeof(*reply);
	int bytes_required = 0;
	if (unlikely(buf_len < sizeof(*reply))) {
		fprintf(stderr, "%s: client send buffer too short\n", __func__);
		goto err_close;
	}
	buf_len -= sizeof(*reply);
	bzero(reply, sizeof(*reply));
	reply->opcode = htons(OP_OACK);
	if (unlikely(buf_len <= strlen(optname) + 1)) {
		fprintf(stderr, "%s: ERR: send buffer too short: need %ld, got %ld\n",
				__func__, strlen(optname) + 1, buf_len);
		goto err_close;
	}
	strcpy(optp, optname);
	reply_len += strlen(optname) + 1;
	buf_len -= strlen(optname) + 1;
	optp += strlen(optname) + 1;
	bytes_required = snprintf(optp, buf_len, "%d", (int)client->block_size);
	if (unlikely(bytes_required >= buf_len)) {
		fprintf(stderr, "%s: ERR: send buffer too short: need: %d, got %ld\n",
				__func__, bytes_required, buf_len);
		goto err_close;
	}
	reply_len += bytes_required + 1;
	buf_len -= bytes_required + 1;
	client->reply_len = reply_len;
	return tftpd_send_reply_pkt(ctx, client);
err_close:
	tftpd_send_error(ctx, client, ENOMEM, "internal error");
	tftpd_close_connection(ctx, client);
	return -1;
}

void tftpd_send_error(struct tftpd_ctx *ctx, struct tftpd_client *client,
		      uint16_t code, const char *msg)
{
	struct tftp_error *err = (struct tftp_error *)client->buf;
	client->reply_len = sizeof(*err) + strlen(msg) + 1;
	if (client->reply_len > client->buf_len) {
		client->reply_len = 0;
		fprintf(stderr, "%s: error message too long\n", __func__);
		return;
	}
	err->opcode = htons(OP_ERROR);
	err->errcode = htons(code);
	strcpy(err->data, msg);
	if (send(client->sock, client->buf, client->reply_len, 0) < 0) {
		fprintf(stderr, "%s: WARN: failed to send error pkt to client %s\n",
				__func__, client->str_addr);
		/* Don't bother to retransmit the ERROR packet */
	}
}

ssize_t tftpd_new_connection(struct tftpd_ctx *ctx) {
	ssize_t msg_len;
	int blksize = BLKSIZE_DFLT;
	char *optstart = NULL;
	size_t optslen = 0;
	int has_options = 0;
	if ((msg_len = tftpd_recv_pkt(ctx, ctx->sock)) < 0) {
		fprintf(stderr, "%s: failed to read datagram\n", __func__);
		return -1;
	}
	if (msg_len == 0) {
		return 0;
	}

	struct tftp_rrq *rrq = (struct tftp_rrq *)ctx->buf;
	if (ntohs(rrq->opcode) != OP_RRQ) {
		fprintf(stderr, "%s: ERR: expected RRQ, got %d\n", __func__, ntohs(rrq->opcode));
		return 0;
	}
	char *filename = tftp_parse_rrq(rrq, msg_len, &optstart, &optslen);
	if (!filename) {
		fprintf(stderr, "%s: ERR: RRQ without a filename\n", __func__);
		return 0;
	}
	if (tftp_get_blksize(optstart, optslen, &blksize)) {
		has_options = 1;
		// fprintf(stderr, "%s: DBG: block size %d\n", __func__, blksize);
	}
	struct tftpd_file *file = find_tftpd_file(ctx->files, filename);
	struct tftpd_client *client;
	for (client = ctx->clients; client; client = client->next) {
		if (!memcmp(&client->client_addr, &ctx->curr_client, sizeof(ctx->curr_client))) {
			break;
		}
	}
	if (!client) {
		if (!(client = tftpd_new_client(ctx))) {
			fprintf(stderr, "%s: failed to initialize client socket\n", __func__);
			return -1;
		}
	}
	if (!file) {
		fprintf(stderr, "%s: no such file: %s (requested by %s)\n",
				__func__, filename, client->str_addr);
		tftpd_send_error(ctx, client, ENOENT, filename);
		tftpd_close_connection(ctx, client);
		return 0;
	}
	__sync_add_and_fetch(&file->refcnt, 1);
	if (client->file != file) {
		if (client->file) {
			client->file->refcnt--;
		}
		client->file = file;
	}

	client->block_size = blksize;
	if (has_options) {
		client->block_num = 0;
		client->has_options = 1;
	}
	if (client->has_options) {
		msg_len = tftpd_send_oack(ctx, client);
	} else {
		msg_len = tftpd_send_data(ctx, client);
	}
	return msg_len;
}

void tftpd_close_connection(struct tftpd_ctx *ctx, struct tftpd_client *client)
{
	if (epoll_ctl(ctx->epoll_fd, EPOLL_CTL_DEL, client->sock, NULL) < 0) {
		perror("close_connection: epoll_ctl DEL");
	}
	if (close(client->sock) < 0) {
		perror("close_connection: close sock");
	}
	list_remove(&ctx->clients, client);
	list_append(&ctx->dead_clients, client);
	if (client->file && __sync_sub_and_fetch(&client->file->refcnt, 1) == 0) {
		/*
		 * TODO: make the file list non-blocking
		 * TODO: implement reading in files in a dedicated thread
		 * TODO: implement the control socket and commands controlling the cache
		 */
	}
	client->file = NULL;
	if (!ctx->clients) {
		struct itimerspec its;
		bzero(&its, sizeof(its));
		/* don't tick if there are no clients. Makes greenpeace happy */
		timerfd_settime(ctx->timer_fd, 0, &its, NULL);
	}
}

int tftpd_handle_ack(struct tftpd_ctx *ctx, struct tftpd_client *client)
{
	struct tftp_ack *reply = (struct tftp_ack *)ctx->buf;
	uint16_t block_num = ntohs(reply->block);
	if (block_num != client->block_num) {
		fprintf(stderr, "%s: WARN: bogus ACK, expected block %d, got %d\n",
				__func__,
				(int)client->block_num,
				(int)block_num);
				return 0;
	}
	client->acked_ts = ctx->last_ts;
	if (client->state != S_WAIT_FINAL_ACK) {
		client->block_num = block_num + 1;
		tftpd_send_data(ctx, client);
	} else {
		tftpd_close_connection(ctx, client);
	}
	return 0;
}

int tftpd_handle_client(struct tftpd_ctx *ctx, struct tftpd_client *client)
{
	ssize_t bytes_read;
	if ((bytes_read = tftpd_recv_pkt(ctx, client->sock)) < 0) {
		fprintf(stderr, "%s: failed to receive datagram\n", __func__);
		return -1;
	}
	if (unlikely(bytes_read == 0)) {
		if (client->pending) {
			if (tftpd_send_data(ctx, client) < 0) {
				/* TODO: send an error message */
				return -1;
			}
		}
		return 0;
	}
	if (unlikely(AF_INET != ctx->curr_dest.ss_family)) {
		fprintf(stderr, "%s: ERR: only IPv4 is supported\n", __func__);
		return -1;
	}
	uint16_t tid = ((struct sockaddr_in *)&ctx->curr_dest)->sin_port;
	if (ctx->conf.check_tid && tid != client->tftp_tid) {
		fprintf(stderr, "%s: DBG: wrong tid: %u (expected: %u) => drop packet\n",
				__func__,
				(unsigned)ntohs(tid),
				(unsigned)ntohs(client->tftp_tid));
		return 0;
	}
	struct tftp_hdr *hdr = (struct tftp_hdr *)ctx->buf;
	uint16_t opcode = ntohs(hdr->opcode);
	switch (opcode) {
		case OP_ACK:
			if (unlikely(bytes_read < sizeof(struct tftp_ack))) {
				fprintf(stderr, "%s: ACK should be at least %d bytes long\n",
						__func__,
						(int)sizeof(struct tftp_ack));
				return -1;
			}
			return tftpd_handle_ack(ctx, client);
			break;
		case OP_ERROR:
			fprintf(stderr, "%s: client %s: ERR, closing\n", __func__, client->str_addr);
			tftpd_close_connection(ctx, client);
			return -1;
			break;
		default:
			fprintf(stderr, "%s: unknown opcode: %d\n", __func__, (int)opcode);
			return -1;
			break;
	}
	return 0;
}

void tftpd_kick_stuck_clients(struct tftpd_ctx *ctx)
{
	time_t client_ts = 0;
	uint64_t tick_count;
	/* read in the tick count so epoll won't report timer_fd as ready */
	if (read(ctx->timer_fd, &tick_count, sizeof(tick_count)) < 0) {
		/* ignore the error */
	}
	for (struct tftpd_client *c = ctx->clients, *next = c->next; c; c = next) {
		next = c->next;
		client_ts = c->sent_ts;
		if (c->acked_ts > c->sent_ts) {
			client_ts = c->acked_ts;
		}
		if (client_ts > ctx->last_ts) {
			fprintf(stderr, "%s: client %s: timestamp in future\n",
					__func__, c->str_addr);
			continue;
		}
		if (ctx->last_ts - client_ts >= ctx->conf.client_timeout*1000) {
			next = c->next;
			tftpd_send_error(ctx, c, ETIMEDOUT, "timed out");
			tftpd_close_connection(ctx, c);
		}
	}
}

int is_dead_client(struct tftpd_ctx *ctx, struct tftpd_client *client)
{
	for (struct tftpd_client *c = ctx->dead_clients; c; c = c->next) {
		if (c == client) {
			return 1;
		}
	}
	return 0;
}

void wipe_dead_clients(struct tftpd_ctx *ctx)
{
	for (struct tftpd_client *c = ctx->dead_clients, *next = NULL; c; c = next) {
		next = c->next;
		/* tftpd_clse_connection has released all resources except the object itself */
		free(c);
	}
	ctx->dead_clients = NULL;
}

void sort_clients_by_deadline(struct tftpd_client **clients, size_t count)
{
	/* shaker sort is often faster than qsort on small datasets */
	int swapped;
	if (count <= 1) {
		return;
	}
	do {
		swapped = 0;
		for (size_t i = 0; i < count - 1; ++i) {
			if (clients[i+1]->sent_ts < clients[i+1]->sent_ts) {
				array_elt_swap(clients, i, i + 1);
				swapped = 1;
			}
		}
		if (!swapped) {
			return;
		}
		swapped = 0;
		for (size_t i = count - 1; i--; ) {
			if (clients[i+1]->sent_ts < clients[i]->sent_ts) {
				array_elt_swap(clients, i, i + 1);
				swapped = 1;
			}
		}
	} while (swapped);
}

/* can be used for sorting with qsort
int compare_clients_by_deadline(const void *cv1, const void *cv2)
{
	struct tftpd_client const * const *pp1 = cv1;
	struct tftpd_client const * const *pp2 = cv1;
	struct tftpd_client const *c1 = *pp1;
	struct tftpd_client const *c2 = *pp2;
	if (c1->sent_ts < c2->sent_ts) {
		return -1;
	}
	if (c1->sent_ts > c2->sent_ts) {
		return 1;
	}
	return 0;
} */

int tftpd_run(struct tftpd_ctx *ctx) {
	int nfds = 0;
	struct tftpd_client *client = NULL;
	struct epoll_event events[MAX_EVENTS];
	struct tftpd_client *pending_clients[MAX_EVENTS];
	struct timespec timestamp;
	time_t start_time;
	if (clock_gettime(CLOCK_MONOTONIC, &timestamp) < 0) {
		perror("clock_gettime");
		return -1;
	}
	start_time = timestamp.tv_sec;
	bzero(events, sizeof(events));
	bzero(pending_clients, sizeof(pending_clients));
	for (;;) {
		/* pick many events to find out which clients are ready and
		 * serve the clients with nearest deadlines
		 */
		if ((nfds = epoll_wait(ctx->epoll_fd, events, MAX_EVENTS, -1)) < 0) {
			perror("epoll_wait");
			return -1;
		}
		if (clock_gettime(CLOCK_MONOTONIC, &timestamp) < 0) {
			perror("clock_gettime");
		} else {
			/* measure time from the server start so time_t has enough bits
			 * to represent the time stamp
			 */
			ctx->last_ts = (timestamp.tv_sec - start_time)*1000 + (timestamp.tv_nsec >> 20);
		}
		int pending_clients_count = 0, served_client_count = 0;
		for (int n = 0; n < nfds; ++n) {
			uint64_t pval = events[n].data.u64;
			if (unlikely(is_ptr_fd(pval))) {
				int fd = unpack_fd_from_ptr(pval);
				/* this is ugly, but a lookup table for 2 fds is even more so */
				if (ctx->timer_fd == fd) {
					tftpd_kick_stuck_clients(ctx);
				} else if (ctx->signal_fd == fd) {
					if (tftpd_handle_signals(ctx)) {
						goto out;
					}
				} else {
					fprintf(stderr, "%s: bogus fd: %d\n", __func__, fd);
				}
				continue;
			}

			client = events[n].data.ptr;

			/* XXX: in theory the kernel should squash events which occured
			 * between epoll_wait calls. Perhaps this check can be skipped.
			 */
			if (unlikely(is_dead_client(ctx, client))) {
				fprintf(stderr, "%s: INFO: client %s is dead\n",
						__func__, client->str_addr);
				continue;
			}
			if (client) {
				pending_clients[pending_clients_count] = client;
				pending_clients_count++;
			} else {
				/* serve new clients immediately */
				tftpd_new_connection(ctx);
			}
		} 
		sort_clients_by_deadline(pending_clients, pending_clients_count);
		/* qsort can be slower on small data sets.
		 * qsort can't inline the comparison function
		qsort(pending_clients, pending_clients_count, sizeof(struct tftpd_client *),
		      compare_clients_by_deadline);
		*/
		for (int n = 0; served_client_count < CLIENTS_AT_ONCE && n < pending_clients_count; ++n) {
			client = pending_clients[n];
			if (unlikely(is_dead_client(ctx, client))) {
				fprintf(stderr, "%s: DBG: client %s is dead\n",
						__func__, client->str_addr);
				continue;
			}
			tftpd_handle_client(ctx, client);
			served_client_count++;
		}
		wipe_dead_clients(ctx);
	}
out:
	return 0;
}


int tftpd_start(struct tftpd_ctx *ctx) {
	int one = 1;
	struct epoll_event ev;
	struct sockaddr_in server_addr;
	sigset_t sigmask;

	ctx->timer_fd = -1;
	ctx->epoll_fd = -1;
	ctx->sock = -1;
	ctx->signal_fd = -1;

	bzero(&server_addr, sizeof(server_addr));
	server_addr.sin_family = AF_INET;
	server_addr.sin_port = htons(ctx->conf.port);
	server_addr.sin_addr.s_addr = htonl(INADDR_ANY);

	if ((ctx->epoll_fd = epoll_create(MAX_EVENTS)) < 0) {
		perror("epoll_create");
		goto out;
	}
	if ((ctx->timer_fd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK)) < 0) {
		perror("timerfd_create");
		goto out;
	}

	sigemptyset(&sigmask);
	sigaddset(&sigmask, SIGTERM);
	sigaddset(&sigmask, SIGINT);
	sigaddset(&sigmask, SIGQUIT);
	sigaddset(&sigmask, SIGHUP);
	if (sigprocmask(SIG_BLOCK, &sigmask, NULL)) {
		perror("sigprocmask");
		goto out;
	}
	if ((ctx->signal_fd = signalfd(-1, &sigmask, SFD_NONBLOCK)) < 0) {
		perror("signalfd");
		goto out;
	}

	if ((ctx->sock = socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK, 0)) < 0) {
		perror("socket");
		goto out;
	}

	if (setsockopt(ctx->sock, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one)) != 0) {
		perror("setsockopt SO_REUSEADDR");
		goto out;
	}
	/* Allow more than one process/thread to bind to the address:port.
	 * The kernel will distribute incoming datagrams between the server threads.
	 * Therefore all threads are equal (no special thread accepting the connections)
	 * and clients list and all that are per thread and don't need any locks
	 */
	if (setsockopt(ctx->sock, SOL_SOCKET, SO_REUSEPORT, &one, sizeof(one)) != 0) {
		perror("setsockopt SO_REUSEPORT");
		goto out;
	}

	if (bind(ctx->sock, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
		perror("bind");
		goto out;
	}

	if (setsockopt(ctx->sock, SOL_IP, IP_RECVORIGDSTADDR, &one, sizeof(one)) != 0) {
		perror("setsockopt IP_RECVORIGDSTADDR");
		goto out;
	}

	bzero(&ev, sizeof(ev));
	ev.events = EPOLLIN;
	/* pointers to heap allocated memory are aligned at the word boundary.
	 * Therefore lower 2 or 3 bits can be used to encode some data. Here
	 * we use the lowest bit to distinguish between the file descriptors
	 * and pointers.
	 */
	ev.data.u64 = pack_fd_as_ptr(ctx->signal_fd);
	if (epoll_ctl(ctx->epoll_fd, EPOLL_CTL_ADD, ctx->signal_fd, &ev) < 0) {
		perror("epoll_ctl: signalfd");
		goto out;
	}
	bzero(&ev, sizeof(ev));
	ev.events = EPOLLIN;
	ev.data.u64 = pack_fd_as_ptr(ctx->timer_fd);
	if (epoll_ctl(ctx->epoll_fd, EPOLL_CTL_ADD, ctx->timer_fd, &ev) < 0) {
		perror("epoll_ctl: timerfd");
		goto out;
	}
	bzero(&ev, sizeof(ev));
	ev.events = EPOLLIN;
	ev.data.ptr = NULL;
	if (epoll_ctl(ctx->epoll_fd, EPOLL_CTL_ADD, ctx->sock, &ev) < 0) {
		perror("epoll_ctl: sock");
		goto out;
	}
	return 0;
out:
	if (ctx->epoll_fd >= 0) {
		close(ctx->epoll_fd);
		ctx->epoll_fd = -1;
	}
	if (ctx->sock >= 0) {
		close(ctx->sock);
		ctx->sock = -1;
	}
	if (ctx->timer_fd >= 0) {
		close(ctx->timer_fd);
		ctx->timer_fd = -1;
	}
	if (ctx->signal_fd >= 0) {
		close(ctx->signal_fd);
		ctx->signal_fd = -1;
	}
	return -1;
}

ssize_t cache_tftpd_file(struct tftpd_file *tf) {
	struct stat stbuf;
	bzero(&stbuf, sizeof(stbuf));
	if (tf->fd <= 0) {
		tf->fd = open(tf->filename, O_RDONLY);
	}
	if (tf->fd < 0) {
		fprintf(stderr, "%s: failed to open %s: %s\n",
				__func__,
				tf->filename,
				strerror(errno));
		goto out;
	}
	if (fstat(tf->fd, &stbuf) < 0) {
		fprintf(stderr, "%s: failed to stat %s: %s\n",
				__func__,
				tf->filename,
				strerror(errno));
		goto out;
	}
	if (!(tf->cache = mmap(NULL, stbuf.st_size, PROT_READ, MAP_SHARED, tf->fd, 0))) {
		fprintf(stderr, "%s: failed to mmap %s: %s (%d)\n",
				__func__,
				tf->filename,
				strerror(errno),
				errno);
		goto out;
	}
	if (mlock(tf->cache, stbuf.st_size) < 0) {
		fprintf(stderr, "%s: failed to lock %ld bytes\n",
				__func__,
				tf->size);
	}
	tf->size = stbuf.st_size;
	if (readahead(tf->fd, 0, tf->size) < 0) {
		fprintf(stderr, "%s: failed to readahead file %s: %s (%d)\n",
				__func__,
				tf->filename,
				strerror(errno),
				errno);
	}
	fprintf(stderr, "%s: cached file %s (%ld bytes)\n",
			__func__, tf->filename, tf->size);
	if (close(tf->fd) < 0) {
		fprintf(stderr, "%s: failed to close file %s (fd %d): %s (%d)\n",
				__func__,
				tf->filename,
				tf->fd,
				strerror(errno),
				errno);
	}
	tf->fd = -1;
	return tf->size;
out:
	close_tftpd_file(tf);
	return -1;
}

void close_tftpd_file(struct tftpd_file *tf) {
	if (tf->cache && tf->size > 0) {
		munlock(tf->cache, tf->size);
		if (munmap(tf->cache, tf->size) < 0) {
			fprintf(stderr, "%s: failed to unmap %s: %s (%d), continue anyway\n",
					__func__,
					tf->filename,
					strerror(errno),
					errno);
		}
		tf->cache = NULL;
		tf->size = 0;
	}
	if (tf->fd >= 0) {
		close(tf->fd);
		tf->fd = -1;
	}
}

int populate_file_cache(struct tftpd_file *head)
{
	int nfiles = 0;
	for (struct tftpd_file *tf = head; tf; tf = tf->next) {
		if (!tf->filename) {
			fprintf(stderr, "%s: ERR: nameless file\n", __func__);
			goto out;
		}
		if (cache_tftpd_file(tf) < 0) {
			fprintf(stderr, "%s: ERR: failed to cache file %s\n",
					__func__, tf->filename);
			goto out;
		}
		++nfiles;
	}
	return nfiles;
out:
	for (struct tftpd_file *tf = head; tf; tf = tf->next) {
		close_tftpd_file(tf);
	}
	return -1;
}

struct tftpd_file *find_tftpd_file(struct tftpd_file *head, const char *filename)
{
	for (struct tftpd_file *f = head; f; f = f->next) {
		if (!strcmp(filename, f->filename)) {
			return f;
		}
	}
	return NULL;
}

struct tftpd_file *add_tftpd_file(struct tftpd_file **headp, const char *filename) {
	if (!headp) {
		return NULL;
	}
	struct tftpd_file *f = find_tftpd_file(*headp, filename);
	if (f) {
		return f;
	}
	f = calloc(1, sizeof(struct tftpd_file));
	if (!f) {
		fprintf(stderr, "%s: buy more RAM\n", __func__);
		return NULL;
	}
	f->filename = strdup(filename);
	if (!f->filename) {
		fprintf(stderr, "%s: buy more RAM\n", __func__);
		goto out;
	}
	list_append(headp, f);
	return f;
out:
	if (f && f->filename) {
		free(f->filename);
		f->filename = NULL;
	}
	if (f) {
		free(f);
		f = NULL;
	}
	return NULL;
}

void *tftpd_thread(void *arg)
{
	struct tftpd_ctx ctx;
	bzero(&ctx, sizeof(ctx));
	ctx.conf.port = TFTP_PORT;
	ctx.conf.check_tid = 1;
	ctx.conf.client_timeout = 5;
	ctx.buf_len = TFTP_BUFSZ;
	ctx.cbuf_len = TFTP_BUFSZ;
	ctx.files = (struct tftpd_file *)arg;
	if (tftpd_start(&ctx) < 0) {
		return NULL;
	}
	if (tftpd_run(&ctx) < 0) {
		return NULL;
	}
	return NULL;
}

int main(int argc, char **argv) {
	struct tftpd_file *cached_files = NULL;
	int cpu_count = 1, max_threads = 1;
	for (int i = 1; i < argc; ++i) {
		if (!add_tftpd_file(&cached_files, argv[i])) {
			fprintf(stderr, "%s: failed to add file: %s\n", __func__, argv[i]);
			exit(EXIT_FAILURE);
		}
	}
	if (populate_file_cache(cached_files) < 0) {
		fprintf(stderr, "%s: failed to cache files\n", __func__);
		exit(EXIT_FAILURE);
	}
	if ((cpu_count = sysconf(_SC_NPROCESSORS_ONLN)) <= 0) {
		cpu_count = 1;
	}
	max_threads = cpu_count;
	pthread_t tids[max_threads];

	for (int i = 0; i < max_threads; ++i) {
		if (pthread_create(&tids[i], NULL, tftpd_thread, cached_files) != 0) {
			exit(EXIT_FAILURE);
		}
	}
	for (int i = 0; i < max_threads; ++i) {
		pthread_join(tids[i], NULL);
	}
	exit(EXIT_SUCCESS);
}

void tftpd_client_reset_timestamps(struct tftpd_ctx *ctx, struct tftpd_client *client)
{
	client->sent_ts = ctx->last_ts;
	client->acked_ts = ctx->last_ts;
}

static int sprintf_addr(char *buf, size_t buf_len, struct sockaddr_storage const *addr)
{
	bzero(buf, buf_len);
	if (AF_INET == addr->ss_family) {
		struct sockaddr_in const *a = (struct sockaddr_in const *)addr;
		if (!inet_ntop(AF_INET, &a->sin_addr, buf, buf_len)) {
			fprintf(stderr, "%s: ERR: inet_ntop\n", __func__);
		}
		char *endp = memchr(buf, 0, buf_len);
		if (!endp) {
			fprintf(stderr, "%s: buffer too short\n", __func__);
			return -1;
		}
		buf_len -= endp - buf;
		if (!buf_len) {
			fprintf(stderr, "%s: buffer too short\n", __func__);
			return -1;
		}
		if (snprintf(endp, buf_len, ":%d", ntohs(a->sin_port)) >= (int)buf_len) {
			fprintf(stderr, "%s: buffer too short [1]\n", __func__);
			return -1;
		}
		return 0;
	}
	fprintf(stderr, "%s: unhandled address family %d\n", __func__, addr->ss_family);
	return -1;
}

char *tftp_parse_rrq(struct tftp_rrq *rrq, size_t len, char **optsp, size_t *optlenp)
{
	char *filename = rrq->filename, *fileend = NULL;
	char *modep = NULL, *modeend = NULL;
	if (len <= sizeof(*rrq)) {
		fprintf(stderr, "%s: message too short\n", __func__);
		goto err_out;
	}
	len -= sizeof(*rrq);
	if (!(fileend = (char *)memchr(filename, 0, len))) {
		fprintf(stderr, "%s: RRQ without a filename\n", __func__);
		goto err_out;
	}
	len -= (fileend - filename) + 1;
	if (len == 0) {
		fprintf(stderr, "%s: WARN: RRQ without a mode\n", __func__);
		goto err_out;
	}
	modep = fileend + 1;
	if (!(modeend = (char *)memchr(modep, 0, len))) {
		fprintf(stderr, "%s: WARN: RRQ without a mode\n", __func__);
		if (optsp) {
			*optsp = NULL;
		}
		if (optlenp) {
			*optlenp = 0;
		}
		return filename;
	}
	len -= (modeend - modep) + 1;
	if (len == 0) {
		fprintf(stderr, "%s: DBG: no TFTP options\n", __func__);
		if (optsp) {
			*optsp = NULL;
		}
		if (optlenp) {
			*optlenp = 0;
		}
	} else {
		if (optsp) {
			*optsp = modeend + 1;
		}
		if (optlenp) {
			*optlenp = len;
		}
	}
	return filename;
err_out:
	if (optsp) {
		*optsp = NULL;
	}
	if (optlenp) {
		*optlenp = 0;
	}
	return NULL;
}

char *tftp_find_option(char *optstart, size_t optslen, const char *optname, size_t *optlenp)
{
	size_t namelen = 0, optlen = 0;
	char *namep = optstart, *nameend = NULL, *optp = NULL, *optend = NULL;
	if (!optslen) {
		return NULL;
	}
	for (; optslen > 0; namep = optend + 1) {
		if (!(nameend = (char *)memchr(namep, 0, optslen))) {
			fprintf(stderr, "%s: no terminating null for opt name\n", __func__);
			return NULL;
		}
		namelen = nameend - namep;
		if (optslen <= namelen + 1) {
			fprintf(stderr, "%s: option %s: premature end\n", __func__, namep);
			return NULL;
		}
		optslen -= namelen + 1;
		optp = nameend  + 1;
		if (!(optend = (char *)memchr(optp, 0, optslen))) {
			fprintf(stderr, "%s: no terminating null for opt %s\n", __func__, namep);
			return NULL;
		}
		optlen = optend - optp;
		if (optslen < optlen + 1) {
			fprintf(stderr, "%s: BUG: premature buffer end\n", __func__);
			return NULL;
		}
		optslen -= optlen + 1;
		if (!strcmp(optname, namep)) {
			// fprintf(stderr, "%s: DBG: opt %s, val %s\n", __func__, (char *)namep, (char *)optp);
			if (optlenp) {
				*optlenp = optlen;
			}
			return optp;
		}
		fprintf(stderr, "%s: current opt: %s, want: %s\n", __func__, (char *)namep, optname);
	}
	fprintf(stderr, "%s: no option %s\n", __func__, optname);
	return NULL;
}

int tftp_get_blksize(char *optstart, size_t optlen, int *blksize)
{
	char *p = NULL, *endp = NULL;
	size_t len = 0;
	int val = 0;
	if (!blksize) {
		return 0;
	}
	if (!(p = tftp_find_option(optstart, optlen, "blksize", &len))) {
		return 0;
	}
	val = strtol(p, &endp, 10);
	if (*endp != '\0') {
		fprintf(stderr, "%s: invalid blksize: %s\n", __func__, p);
		*blksize = BLKSIZE_DFLT;
		return 0;
	}
	if (val < 8 || val > 65464) {
		/* See RFC 2348 for valid block sizes */
		fprintf(stderr, "%s: invalid blksize: %d\n", __func__, val);
		*blksize = BLKSIZE_DFLT;
		return 0;
	}
	*blksize = val;
	return 1;
}

int tftpd_handle_signals(struct tftpd_ctx *ctx)
{
	struct signalfd_siginfo si;
	ssize_t ret;
	if ((ret = read(ctx->signal_fd, &si, sizeof(si))) != sizeof(si)) {
		if (EAGAIN == errno || EWOULDBLOCK == errno) {
				/* ok */
		} else {
			perror("read signal_fd");
		}
		return 0;
	}
	switch (si.ssi_signo) {
		case SIGINT:
		case SIGTERM:
		case SIGQUIT:
			return 1;
			break;
		case SIGHUP:
			/* FIXME: reload the config */
			return 0;
			break;
		default:
			fprintf(stderr, "%s: got unexpected signal %u\n",
					__func__, si.ssi_signo);
			return 0;
			break;
	}
}
