
#ifndef _WIN32

#include <sys/types.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <sys/time.h>
#include <arpa/inet.h>
#include <netinet/tcp.h>
#include <netdb.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <time.h>

#define MRTP_BUILDING_LIB 1
#include "mrtp.h"

#ifdef __APPLE__
#ifdef HAS_POLL
#undef HAS_POLL
#endif
#ifndef HAS_FCNTL
#define HAS_FCNTL 1
#endif
#ifndef HAS_INET_PTON
#define HAS_INET_PTON 1
#endif
#ifndef HAS_INET_NTOP
#define HAS_INET_NTOP 1
#endif
#ifndef HAS_MSGHDR_FLAGS
#define HAS_MSGHDR_FLAGS 1
#endif
#ifndef HAS_SOCKLEN_T
#define HAS_SOCKLEN_T 1
#endif
#endif

#ifdef HAS_FCNTL
#include <fcntl.h>
#endif

#ifdef HAS_POLL
#include <sys/poll.h>
#endif

#ifndef HAS_SOCKLEN_T
typedef int socklen_t;
#endif

#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

static mrtp_uint32 timeBase = 0;

int mrtp_initialize(void) {
	return 0;
}

void mrtp_deinitialize(void) {
}

mrtp_uint32 mrtp_host_random_seed(void) {
	return (mrtp_uint32)time(NULL);
}

mrtp_uint32 mrtp_time_get(void) {
	struct timeval timeVal;

	gettimeofday(&timeVal, NULL);

	return timeVal.tv_sec * 1000 + timeVal.tv_usec / 1000 - timeBase;
}

void mrtp_time_set(mrtp_uint32 newTimeBase) {
	struct timeval timeVal;

	gettimeofday(&timeVal, NULL);

	timeBase = timeVal.tv_sec * 1000 + timeVal.tv_usec / 1000 - newTimeBase;
}

int mrtp_address_set_host(MRtpAddress * address, const char * name) {
	struct hostent * hostEntry = NULL;
#ifdef HAS_GETHOSTBYNAME_R
	struct hostent hostData;
	char buffer[2048];
	int errnum;

#if defined(linux) || defined(__linux) || defined(__linux__) || defined(__FreeBSD__) || defined(__FreeBSD_kernel__) || defined(__DragonFly__)
	gethostbyname_r(name, &hostData, buffer, sizeof(buffer), &hostEntry, &errnum);
#else
	hostEntry = gethostbyname_r(name, &hostData, buffer, sizeof(buffer), &errnum);
#endif
#else
	hostEntry = gethostbyname(name);
#endif

	if (hostEntry == NULL || hostEntry->h_addrtype != AF_INET) {
#ifdef HAS_INET_PTON
		if (!inet_pton(AF_INET, name, &address->host))
#else
		if (!inet_aton(name, (struct in_addr *) & address->host))
#endif
			return -1;
		return 0;
	}

	address->host = *(mrtp_uint32 *)hostEntry->h_addr_list[0];

	return 0;
}

int mrtp_address_get_host_ip(const MRtpAddress * address, char * name, size_t nameLength) {
#ifdef HAS_INET_NTOP
	if (inet_ntop(AF_INET, &address->host, name, nameLength) == NULL)
#else
	char * addr = inet_ntoa(*(struct in_addr *) & address->host);
	if (addr != NULL) {
		size_t addrLen = strlen(addr);
		if (addrLen >= nameLength)
			return -1;
		memcpy(name, addr, addrLen + 1);
	}
	else
#endif
		return -1;
	return 0;
}

int mrtp_address_get_host(const MRtpAddress * address, char * name, size_t nameLength) {
	struct in_addr in;
	struct hostent * hostEntry = NULL;
#ifdef HAS_GETHOSTBYADDR_R
	struct hostent hostData;
	char buffer[2048];
	int errnum;

	in.s_addr = address->host;

#if defined(linux) || defined(__linux) || defined(__linux__) || defined(__FreeBSD__) || defined(__FreeBSD_kernel__) || defined(__DragonFly__)
	gethostbyaddr_r((char *)& in, sizeof(struct in_addr), AF_INET, &hostData, buffer, sizeof(buffer), &hostEntry, &errnum);
#else
	hostEntry = gethostbyaddr_r((char *)& in, sizeof(struct in_addr), AF_INET, &hostData, buffer, sizeof(buffer), &errnum);
#endif
#else
	in.s_addr = address->host;

	hostEntry = gethostbyaddr((char *)& in, sizeof(struct in_addr), AF_INET);
#endif

	if (hostEntry == NULL)
		return mrtp_address_get_host_ip(address, name, nameLength);
	else {
		size_t hostLen = strlen(hostEntry->h_name);
		if (hostLen >= nameLength)
			return -1;
		memcpy(name, hostEntry->h_name, hostLen + 1);
	}

	return 0;
}

int mrtp_socket_bind(MRtpSocket socket, const MRtpAddress * address) {
	struct sockaddr_in sin;

	memset(&sin, 0, sizeof(struct sockaddr_in));

	sin.sin_family = AF_INET;

	if (address != NULL) {
		sin.sin_port = MRTP_HOST_TO_NET_16(address->port);
		sin.sin_addr.s_addr = address->host;
	}
	else {
		sin.sin_port = 0;
		sin.sin_addr.s_addr = INADDR_ANY;
	}

	return bind(socket, (struct sockaddr *) & sin, sizeof(struct sockaddr_in));
}

int mrtp_socket_get_address(MRtpSocket socket, MRtpAddress * address) {
	struct sockaddr_in sin;
	socklen_t sinLength = sizeof(struct sockaddr_in);

	if (getsockname(socket, (struct sockaddr *) & sin, &sinLength) == -1)
		return -1;

	address->host = (mrtp_uint32)sin.sin_addr.s_addr;
	address->port = MRTP_NET_TO_HOST_16(sin.sin_port);

	return 0;
}

int mrtp_socket_listen(MRtpSocket socket, int backlog) {
	return listen(socket, backlog < 0 ? SOMAXCONN : backlog);
}

MRtpSocket mrtp_socket_create(MRtpSocketType type) {
	return socket(PF_INET, type == MRTP_SOCKET_TYPE_DATAGRAM ? SOCK_DGRAM : SOCK_STREAM, 0);
}

int mrtp_socket_set_option(MRtpSocket socket, MRtpSocketOption option, int value) {

	int result = -1;
	switch (option) {
	case MRTP_SOCKOPT_NONBLOCK:
#ifdef HAS_FCNTL
		result = fcntl(socket, F_SETFL, (value ? O_NONBLOCK : 0) | (fcntl(socket, F_GETFL) & ~O_NONBLOCK));
#else
		result = ioctl(socket, FIONBIO, &value);
#endif
		break;

	case MRTP_SOCKOPT_BROADCAST:
		result = setsockopt(socket, SOL_SOCKET, SO_BROADCAST, (char *)& value, sizeof(int));
		break;

	case MRTP_SOCKOPT_REUSEADDR:
		result = setsockopt(socket, SOL_SOCKET, SO_REUSEADDR, (char *)& value, sizeof(int));
		break;

	case MRTP_SOCKOPT_RCVBUF:
		result = setsockopt(socket, SOL_SOCKET, SO_RCVBUF, (char *)& value, sizeof(int));
		break;

	case MRTP_SOCKOPT_SNDBUF:
		result = setsockopt(socket, SOL_SOCKET, SO_SNDBUF, (char *)& value, sizeof(int));
		break;

	case MRTP_SOCKOPT_RCVTIMEO: {
		struct timeval timeVal;
		timeVal.tv_sec = value / 1000;
		timeVal.tv_usec = (value % 1000) * 1000;
		result = setsockopt(socket, SOL_SOCKET, SO_RCVTIMEO, (char *)& timeVal, sizeof(struct timeval));
		break;
	}

	case MRTP_SOCKOPT_SNDTIMEO: {
		struct timeval timeVal;
		timeVal.tv_sec = value / 1000;
		timeVal.tv_usec = (value % 1000) * 1000;
		result = setsockopt(socket, SOL_SOCKET, SO_SNDTIMEO, (char *)& timeVal, sizeof(struct timeval));
		break;
	}

	case MRTP_SOCKOPT_NODELAY:
		result = setsockopt(socket, IPPROTO_TCP, TCP_NODELAY, (char *)& value, sizeof(int));
		break;

	default:
		break;
	}
	return result == -1 ? -1 : 0;
}

int mrtp_socket_get_option(MRtpSocket socket, MRtpSocketOption option, int * value) {

	int result = -1;
	socklen_t len;
	switch (option)
	{
	case MRTP_SOCKOPT_ERROR:
		len = sizeof(int);
		result = getsockopt(socket, SOL_SOCKET, SO_ERROR, value, &len);
		break;

	default:
		break;
	}
	return result == -1 ? -1 : 0;
}

int mrtp_socket_connect(MRtpSocket socket, const MRtpAddress * address) {
	struct sockaddr_in sin;
	int result;

	memset(&sin, 0, sizeof(struct sockaddr_in));

	sin.sin_family = AF_INET;
	sin.sin_port = MRTP_HOST_TO_NET_16(address->port);
	sin.sin_addr.s_addr = address->host;

	result = connect(socket, (struct sockaddr *) & sin, sizeof(struct sockaddr_in));
	if (result == -1 && errno == EINPROGRESS)
		return 0;

	return result;
}

MRtpSocket mrtp_socket_accept(MRtpSocket socket, MRtpAddress * address) {
	int result;
	struct sockaddr_in sin;
	socklen_t sinLength = sizeof(struct sockaddr_in);

	result = accept(socket,
		address != NULL ? (struct sockaddr *) & sin : NULL,
		address != NULL ? &sinLength : NULL);

	if (result == -1)
		return MRTP_SOCKET_NULL;

	if (address != NULL) {
		address->host = (mrtp_uint32)sin.sin_addr.s_addr;
		address->port = MRTP_NET_TO_HOST_16(sin.sin_port);
	}

	return result;
}

int mrtp_socket_shutdown(MRtpSocket socket, MRtpSocketShutdown how) {
	return shutdown(socket, (int)how);
}

void mrtp_socket_destroy(MRtpSocket socket) {
	if (socket != -1)
		close(socket);
}

int mrtp_socket_send(MRtpSocket socket, const MRtpAddress * address, const MRtpBuffer * buffers, size_t bufferCount) {

	struct msghdr msgHdr;
	struct sockaddr_in sin;
	int sentLength;

	memset(&msgHdr, 0, sizeof(struct msghdr));

	if (address != NULL) {
		memset(&sin, 0, sizeof(struct sockaddr_in));

		sin.sin_family = AF_INET;
		sin.sin_port = MRTP_HOST_TO_NET_16(address->port);
		sin.sin_addr.s_addr = address->host;

		msgHdr.msg_name = &sin;
		msgHdr.msg_namelen = sizeof(struct sockaddr_in);
	}

	msgHdr.msg_iov = (struct iovec *) buffers;
	msgHdr.msg_iovlen = bufferCount;

	sentLength = sendmsg(socket, &msgHdr, MSG_NOSIGNAL);

	if (sentLength == -1) {
		if (errno == EWOULDBLOCK)
			return 0;

		return -1;
	}

	return sentLength;
}

int mrtp_socket_receive(MRtpSocket socket, MRtpAddress * address, MRtpBuffer * buffers, size_t bufferCount) {

	struct msghdr msgHdr;
	struct sockaddr_in sin;
	int recvLength;

	memset(&msgHdr, 0, sizeof(struct msghdr));

	if (address != NULL) {
		msgHdr.msg_name = &sin;
		msgHdr.msg_namelen = sizeof(struct sockaddr_in);
	}

	msgHdr.msg_iov = (struct iovec *) buffers;
	msgHdr.msg_iovlen = bufferCount;

	recvLength = recvmsg(socket, &msgHdr, MSG_NOSIGNAL);

	if (recvLength == -1) {
		if (errno == EWOULDBLOCK)
			return 0;

		return -1;
	}

#ifdef HAS_MSGHDR_FLAGS
	if (msgHdr.msg_flags & MSG_TRUNC)
		return -1;
#endif

	if (address != NULL)
	{
		address->host = (mrtp_uint32)sin.sin_addr.s_addr;
		address->port = MRTP_NET_TO_HOST_16(sin.sin_port);
	}

	return recvLength;
}

int mrtp_socketset_select(MRtpSocket maxSocket, MRtpSocketSet * readSet, MRtpSocketSet * writeSet, 
	mrtp_uint32 timeout)
{
	struct timeval timeVal;

	timeVal.tv_sec = timeout / 1000;
	timeVal.tv_usec = (timeout % 1000) * 1000;

	return select(maxSocket + 1, readSet, writeSet, NULL, &timeVal);
}

int mrtp_socket_wait(MRtpSocket socket, mrtp_uint32 * condition, mrtp_uint32 timeout) {

#ifdef HAS_POLL
	struct pollfd pollSocket;
	int pollCount;

	pollSocket.fd = socket;
	pollSocket.events = 0;

	if (*condition & MRTP_SOCKET_WAIT_SEND)
		pollSocket.events |= POLLOUT;

	if (*condition & MRTP_SOCKET_WAIT_RECEIVE)
		pollSocket.events |= POLLIN;

	pollCount = poll(&pollSocket, 1, timeout);

	if (pollCount < 0) {
		if (errno == EINTR && * condition & MRTP_SOCKET_WAIT_INTERRUPT) {
			*condition = MRTP_SOCKET_WAIT_INTERRUPT;

			return 0;
		}

		return -1;
	}

	*condition = MRTP_SOCKET_WAIT_NONE;

	if (pollCount == 0)
		return 0;

	if (pollSocket.revents & POLLOUT)
		* condition |= MRTP_SOCKET_WAIT_SEND;

	if (pollSocket.revents & POLLIN)
		* condition |= MRTP_SOCKET_WAIT_RECEIVE;

	return 0;
#else
	fd_set readSet, writeSet;
	struct timeval timeVal;
	int selectCount;

	timeVal.tv_sec = timeout / 1000;
	timeVal.tv_usec = (timeout % 1000) * 1000;

	FD_ZERO(&readSet);
	FD_ZERO(&writeSet);

	if (*condition & MRTP_SOCKET_WAIT_SEND)
		FD_SET(socket, &writeSet);

	if (*condition & MRTP_SOCKET_WAIT_RECEIVE)
		FD_SET(socket, &readSet);

	selectCount = select(socket + 1, &readSet, &writeSet, NULL, &timeVal);

	if (selectCount < 0) {
		if (errno == EINTR && * condition & MRTP_SOCKET_WAIT_INTERRUPT) {
			*condition = MRTP_SOCKET_WAIT_INTERRUPT;

			return 0;
		}

		return -1;
	}

	*condition = MRTP_SOCKET_WAIT_NONE;

	if (selectCount == 0)
		return 0;

	if (FD_ISSET(socket, &writeSet))
		* condition |= MRTP_SOCKET_WAIT_SEND;

	if (FD_ISSET(socket, &readSet))
		* condition |= MRTP_SOCKET_WAIT_RECEIVE;

	return 0;
#endif
}

#endif

