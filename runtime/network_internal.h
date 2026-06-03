#ifndef BZY_NETWORK_INTERNAL_H
#define BZY_NETWORK_INTERNAL_H
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

/* Shared by socket.c (TCP) and udp.c. The real winsock types live here so they
   stay out of the public, windows-free breezy.h. */
void *bzy_sock_wrap(SOCKET fd);                                  /* Managed handle (object_size 40). */
int   bzy_resolve4(const char *host, int port, struct sockaddr_in *out);   /* 0 on success. */
#endif
