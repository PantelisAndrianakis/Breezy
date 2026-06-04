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
  #include <arpa/inet.h>
  #include <netdb.h>
  #include <fcntl.h>
  #include <unistd.h>
  #include <errno.h>
  void *bzy_sock_wrap(int fd);                                             /* Managed handle (object_size 40). */
#endif

int bzy_resolve4(const char *host, int port, struct sockaddr_in *out);     /* 0 on success. */
#endif
