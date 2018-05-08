
#define _WINSOCK_DEPRECATED_NO_WARNINGS 

#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <ctype.h>
#include <string.h>
#include <winsock2.h>
#include <iostream>

#include "ikcp.h"

#define SERVERPORT 6666

sockaddr_in servAddr;
SOCKADDR_IN peeraddr;
int peeraddr_len = sizeof(peeraddr);

SOCKET create_socket() {

	WORD versionRequested = MAKEWORD(1, 1);
	WSADATA wsaData;

	if (WSAStartup(versionRequested, &wsaData)) {
		std::cout << "WSAStartup failed." << std::endl;
		return -1;
	}
	SOCKET serSocket = socket(AF_INET, SOCK_DGRAM, 0);

	servAddr.sin_family = AF_INET;
	servAddr.sin_port = htons(SERVERPORT);
	servAddr.sin_addr.S_un.S_addr = INADDR_ANY;

	// set nonBlocking
	u_long nonBlocking = 1;
	if (ioctlsocket(serSocket, FIONBIO, &nonBlocking) == SOCKET_ERROR) {
		std::cout << "set nonblocking failed with code: " << WSAGetLastError() << std::endl;
		closesocket(serSocket);
		return -1;
	}

	// The state of the SO_BROADCAST socket option determines whether broadcast messages 
	// can be transmitted over a datagram socket.This socket option applies only to datagram sockets.
	int value = 1;
	if (setsockopt(serSocket, SOL_SOCKET, SO_BROADCAST, (char *)&value, sizeof(int)) == SOCKET_ERROR) {
		std::cout << "set broadcast failed with code: " << WSAGetLastError() << std::endl;
		closesocket(serSocket);
		return -1;
	}

	if (bind(serSocket, (sockaddr *)&servAddr, sizeof(servAddr)) == SOCKET_ERROR) {
		std::cout << "bind failed with code: " << WSAGetLastError() << std::endl;
		closesocket(serSocket);
		return -1;
	}

	return serSocket;
}


int udp_output(const char *buf, int len, ikcpcb *kcp, void * user) {
	SOCKET socket = *((SOCKET*)user);
	int sendLength = 0;
	if ((sendLength = sendto(socket, buf, len, 0, (sockaddr *)&peeraddr, peeraddr_len)) == SOCKET_ERROR) {
		std::cout << "send error with code: " << WSAGetLastError() << std::endl;
		closesocket(socket);
		exit(-1);
	}
	//std::cout << "send " << sendLength << std::endl;
	return 0;
}

int udp_input(SOCKET socket, char *buf, int len) {

	int receiveLength = 0;

	if ((receiveLength = recvfrom(socket, buf, len, 0, (sockaddr *)&peeraddr, &peeraddr_len)) == SOCKET_ERROR) {

		int error = WSAGetLastError();
		switch (error)
		{
		case WSAEWOULDBLOCK:
		case WSAECONNRESET:
			return 0;
		}
		exit(-1);
	}
	//std::cout << "receive " << receiveLength << std::endl;
	return receiveLength;
}

int main() {
	SOCKET servsocket = create_socket();
	//SOCKET clisocket = socket(AF_INET, SOCK_DGRAM, 0);
	
	ikcpcb *kcp_server = ikcp_create(0x11223344, &servsocket);

	kcp_server->output = udp_output;

	IUINT32 current = (IUINT32)timeGetTime();
	IUINT32 slap = current + 30;
	IUINT32 index = 0, next = 0;
	const int PACKELENGTH = 80;
	const int TOTALPACKET = 1000;
	IINT64 sumrtt = 0;
	int count = 0, maxrtt = 0;

	ikcp_wndsize(kcp_server, 128, 128);	

	ikcp_nodelay(kcp_server, 1, 10, 2, 1);

	char buffer[2000];
	memset(buffer, 'a', 2000);
	int hr;

	while (true) {
		Sleep(1);
		current = (IUINT32)timeGetTime();
		ikcp_update(kcp_server, current);

		while (true) {
			hr = udp_input(servsocket, buffer, 2000);
			if (hr <= 0)
				break;
			ikcp_input(kcp_server, buffer, hr);
		}

		current = (IUINT32)timeGetTime();

		while (true) {
			hr = ikcp_recv(kcp_server, buffer, PACKELENGTH + 2);
			// 没有收到包就退出
			if (hr < 0) break;
			// 如果收到包就回射
			ikcp_send(kcp_server, buffer, hr);
		}
	}
	return 0;
}