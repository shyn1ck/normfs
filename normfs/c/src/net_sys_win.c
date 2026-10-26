/*
 * The Windows bodies behind normfs/net_sys.h; never given to Frama-C. Only
 * the stream calls the connection state uses: on Windows tokio owns the
 * listening socket and accepts, so listen, accept and local_addr have no
 * body here. os_error is the WSA code, which io::Error::from_raw_os_error
 * reads on Windows.
 */
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>

#include "normfs/net_sys.h"

/* recv and send take an int length. */
#define NORMFS_NET_SYS_IO_MAX 0x7ffff000u

static int
normfs_net_sys_fail(int *os_error)
{
	int e = WSAGetLastError();

	*os_error = e > 0 ? e : WSAEINVAL;
	return -1;
}

long
normfs_net_sys_read(normfs_net_fd fd, uint8_t *buf, size_t off, size_t len, int *os_error)
{
	int n;

	*os_error = 0;
	if (len > NORMFS_NET_SYS_IO_MAX)
		len = NORMFS_NET_SYS_IO_MAX;
	n = recv((SOCKET)fd, (char *)(buf + off), (int)len, 0);
	if (n >= 0)
		return (long)n;
	if (WSAGetLastError() == WSAEWOULDBLOCK)
		return NORMFS_NET_SYS_WOULD_BLOCK;
	return (long)normfs_net_sys_fail(os_error);
}

long
normfs_net_sys_write(normfs_net_fd fd, const uint8_t *buf, size_t off, size_t len,
    int *os_error)
{
	int n;

	*os_error = 0;
	if (len > NORMFS_NET_SYS_IO_MAX)
		len = NORMFS_NET_SYS_IO_MAX;
	n = send((SOCKET)fd, (const char *)(buf + off), (int)len, 0);
	if (n > 0)
		return (long)n;
	if (n < 0 && WSAGetLastError() == WSAEWOULDBLOCK)
		return NORMFS_NET_SYS_WOULD_BLOCK;
	if (n == 0) {
		/* No progress would spin the caller. */
		*os_error = WSAECONNABORTED;
		return -1L;
	}
	return (long)normfs_net_sys_fail(os_error);
}

void
normfs_net_sys_shutdown_write(normfs_net_fd fd)
{
	(void)shutdown((SOCKET)fd, SD_SEND);
}

void
normfs_net_sys_close(normfs_net_fd fd)
{
	(void)closesocket((SOCKET)fd);
}
