/*
 * Utils for TCP socket.
 *
 * Author: Wu Bingzheng
 *
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdint.h>
#include <sys/types.h>
#include <unistd.h>
#include <fcntl.h>
#include <arpa/inet.h>
//TCP_CORK   
#include <netinet/tcp.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <strings.h>
#include <errno.h>
#include <sys/socket.h>

#include "socktcp.h"

static int idle_fd = -1;

static int get_tcp_rmem(int *s)
{
    FILE *fp;
    int n;

    fp = fopen("/proc/sys/net/ipv4/tcp_rmem", "r");
    if(fp == NULL) {
        return 1;
    }

    n = fscanf(fp, "%d%d%d", &s[0], &s[1], &s[2]);
    fclose(fp);
    return n == 3 ? 0 : 1;
}

static int get_tcp_wmem(int *s)
{
    FILE *fp;
    int n;

    fp = fopen("/proc/sys/net/ipv4/tcp_wmem", "r");
    if(fp == NULL) {
        return 1;
    }

    n = fscanf(fp, "%d%d%d", &s[0], &s[1], &s[2]);
    fclose(fp);
    return n == 3 ? 0 : 1;
}

int set_sndbuf(int fd, int value)
{
    int mems[3];

    if(value == 0) {
        if(get_tcp_wmem(mems) != 0) {
            return -1;
        }
        value = mems[1];
    }

    if(setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &value, sizeof(int)) < 0) {
        return -1;
    }

    return 0;
}

int set_rcvbuf(int fd, int value)
{
    int mems[3];

    if(value == 0) {
        if(get_tcp_rmem(mems) != 0) {
            return -1;
        }
        value = mems[1];
    }

    if(setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &value, sizeof(int)) < 0) {
        return -1;
    }

    return 0;
}

int set_cork(int fd, int cork)
{
    return setsockopt(fd, IPPROTO_TCP, TCP_CORK, &cork, sizeof(int));
}

int set_defer_accept(int fd, int value)
{
    return setsockopt(fd, IPPROTO_TCP, TCP_DEFER_ACCEPT, &value, sizeof(int));
}

int set_nonblock(int fd)
{
    return fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
}

int tcp_bind(unsigned short port)
{
    int fd, val;
    struct sockaddr_in servaddr;  
    //address family internet 
    fd = socket(AF_INET, SOCK_STREAM, 0);  
    if(fd < 0) {
        return -1;
    }
    //set non block
    if(set_nonblock(fd) != 0) {
        return -1;
    }

    val = 1;
    if(setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &val, sizeof(int)) < 0) {
        return -1;
    }

    if(set_defer_accept(fd, 60) < 0) {
        return -1;
    }

    bzero(&servaddr, sizeof(servaddr));
    servaddr.sin_family = AF_INET;
    //host to net long
    servaddr.sin_addr.s_addr = htonl(INADDR_ANY);
    //host to net short
    servaddr.sin_port = htons(port);
    if(bind(fd, (struct sockaddr *)&servaddr, sizeof(servaddr)) < 0) {
        close(fd);
        return -1;
    }

    return fd;
}

int tcp_listen(int fd)
{
    if(idle_fd == -1) {
        idle_fd = open("/dev/null", O_RDONLY);
    }
    return listen(fd, 1000);
}

static void clear_backlog(int s)
{
    while(idle_fd >= 0) {
        close(idle_fd);
        idle_fd = accept(s, NULL, 0);
    }

    idle_fd = open("/dev/null", O_RDONLY);
}
// test the vesion of glib
#if __GLIBC_PREREQ(2, 10)
#define OHC_HAVE_ACCEPT4
#endif

int tcp_accept(int s, struct sockaddr_in *client)
{
    socklen_t addrlen = sizeof(struct sockaddr_in);
    int fd;

again:
#ifdef OHC_HAVE_ACCEPT4
    fd = accept4(s, (struct sockaddr *)client, &addrlen, SOCK_NONBLOCK);
#else
    fd = accept(s, (struct sockaddr *)client, &addrlen);
#endif
    if(fd < 0) {
        if(errno == EINTR)
            goto again;
        if(errno == EMFILE || errno == ENFILE)
            clear_backlog(s);
        return fd;
    }

#ifndef OHC_HAVE_ACCEPT4
    /* we don't use set_nonblock(), to cut down one syscall. */
    static int flag_init = -1;
    if(flag_init == -1) {
        flag_init = fcntl(fd, F_GETFL);
    }
    fcntl(fd, F_SETFL, flag_init | O_NONBLOCK);
#endif
    return fd;
}
