#ifndef BZY_NETWORK_INTERNAL_H
#define BZY_NETWORK_INTERNAL_H

/* Shared by socket.c (TCP) and udp.c. The real socket types live here so they
   stay out of the public, OS-free breezy.h. */
#ifdef _WIN32
  #define WIN32_LEAN_AND_MEAN
  #include <winsock2.h>
  #include <ws2tcpip.h>
  #include <windows.h>
  void *bzy_sock_wrap(SOCKET fd);                                          /* Managed handle (object_size 40). */
#else
  #include <sys/socket.h>
  #include <netinet/in.h>
  #include <netinet/tcp.h>   /* TCP_NODELAY. */
  #include <arpa/inet.h>
  #include <netdb.h>
  #include <fcntl.h>
  #include <unistd.h>
  #include <errno.h>
  void *bzy_sock_wrap(int fd);                                             /* Managed handle (object_size 40). */
#endif

int bzy_resolve_any(const char *host, int port, struct sockaddr_storage *out,
                    socklen_t *outlen, int *fam);                          /* 0 on success; AF_UNSPEC (v4/v6). */

int     bzy_sched_local_runnable(void);                                    /* 1 if this worker has other ready breezes (spin-gate hint). */

/* Reactor-parked raw byte I/O on a plain Socket handle (shared with tls.c). */
int     bzy_sock_recv(void *s, char *buf, int max, int64_t timeout_ms);    /* bytes, 0 EOF, -1 err, -2 timeout. */
int64_t bzy_sock_send_all(void *s, const char *buf, int64_t len);          /* bytes sent, <0 on error. */
#endif
