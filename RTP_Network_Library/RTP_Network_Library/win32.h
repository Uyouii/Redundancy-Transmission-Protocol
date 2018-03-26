#ifndef __MRTP_WIN32_H__
#define __MRTP_WIN32_H__

#ifdef _MSC_VER

#define _WINSOCK_DEPRECATED_NO_WARNINGS 1

#ifdef MRTP_BUILDING_LIB
#pragma warning (disable: 4267) // size_t to int conversion
#pragma warning (disable: 4244) // 64bit to 32bit int
#pragma warning (disable: 4018) // signed/unsigned mismatch
#pragma warning (disable: 4146) // unary minus operator applied to unsigned type
#endif
#endif

#include <stdlib.h>
#include <winsock2.h>

typedef SOCKET MRtpSocket;

#define MRTP_SOCKET_NULL INVALID_SOCKET

#define MRTP_HOST_TO_NET_16(value) (htons (value))
#define MRTP_HOST_TO_NET_32(value) (htonl (value))

#define MRTP_NET_TO_HOST_16(value) (ntohs (value))
#define MRTP_NET_TO_HOST_32(value) (ntohl (value))

typedef struct {
	size_t dataLength;
	void * data;
} MRtpBuffer;

#define MRTP_CALLBACK __cdecl	//c语言默认的函数调用方法

#ifdef MRTP_DLL
#ifdef MRTP_BUILDING_LIB
//在使用Windows DLL编程时，可以使用__declspec( dllexport )关键字导入函数或者变量
#define MRTP_API __declspec( dllexport ) 
#else
#define MRTP_API __declspec( dllimport )
#endif /* MRTP_BUILDING_LIB */
#else /* !MRTP_DLL */
#define MRTP_API extern
#endif /* MRTP_DLL */

typedef fd_set MRtpSocketSet;

#define MRTP_SOCKETSET_EMPTY(sockset)          FD_ZERO (& (sockset))
#define MRTP_SOCKETSET_ADD(sockset, socket)    FD_SET (socket, & (sockset))
#define MRTP_SOCKETSET_REMOVE(sockset, socket) FD_CLR (socket, & (sockset))
#define MRTP_SOCKETSET_CHECK(sockset, socket)  FD_ISSET (socket, & (sockset))

#endif /* __MRTP_WIN32_H__ */


