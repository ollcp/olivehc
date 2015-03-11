/*
 * Utils for TCP socket.
 *
 * Author: Wu Bingzheng
 *
 */

#ifndef _OHC_SOCKTCP_H_
#define _OHC_SOCKTCP_H_

int tcp_bind(unsigned short port);
int tcp_listen(int fd);
int tcp_accept(int fd, struct sockaddr_in *client);

int set_sndbuf(int fd, int value);
int set_rcvbuf(int fd, int value);
int set_defer_accept(int fd, int value);
int set_cork(int fd, int cork);

int set_nonblock(int fd);

#endif
