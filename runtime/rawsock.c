/* Network.rawSocket(protocol): open a raw IP socket (SOCK_RAW) for low-level
   packet work. Privilege-gated by the OS -- needs CAP_NET_RAW/root on POSIX or
   Administrator on Windows -- so a denied open sets the io-error (the codegen
   post-call bzy_io_check turns it into a catchable IOException) rather than
   crashing. The returned handle is an ordinary socket handle (bzy_sock_wrap), so
   send/recv/close reuse the existing reactor-integrated socket machinery.
   Own translation unit: a program that never calls rawSocket links none of it. */
#include "breezy.h"
#include "network_internal.h"

void *bzy_raw_socket(int64_t protocol)
{
#ifdef _WIN32
	bzy_iocp_ensure();
	SOCKET fd = WSASocketW(AF_INET, SOCK_RAW, (int)protocol, NULL, 0, WSA_FLAG_OVERLAPPED);
	if (fd == INVALID_SOCKET)
	{
		bzy_io_fail("Network.rawSocket: open failed (needs Administrator).");
		return NULL;
	}

	u_long nb = 1;
	ioctlsocket(fd, FIONBIO, &nb);
	bzy_iocp_associate((void*)fd);
	return bzy_sock_wrap(fd);
#else
	bzy_reactor_ensure();
	int fd = socket(AF_INET, SOCK_RAW | SOCK_NONBLOCK, (int)protocol);
	if (fd < 0)
	{
		bzy_io_fail("Network.rawSocket: open failed (needs CAP_NET_RAW/root).");
		return NULL;
	}

	return bzy_sock_wrap(fd);
#endif
}
