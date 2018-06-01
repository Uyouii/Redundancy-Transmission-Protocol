
#define _WINSOCK_DEPRECATED_NO_WARNINGS 

#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <ctype.h>
#include <string.h>
#include <winsock2.h>
#include <iostream>
#include <fstream>
#include <string>
#include <vector>

#include "ikcp.h"

#define SERVERPORT 6666
//#define SERVERADDR "10.242.3.221"
#define SERVERADDR "192.168.31.233"

#define GENERATECSVFILE

sockaddr_in servAddr;
SOCKADDR_IN peeraddr;
int peeraddr_len = sizeof(peeraddr);
size_t totalSendData = 0;
size_t totalReceiveData = 0;
size_t totalSendPacket = 0;
size_t totalReceivePacket = 0;

SOCKET set_socket() {

	WORD versionRequested = MAKEWORD(1, 1);
	WSADATA wsaData;

	if (WSAStartup(versionRequested, &wsaData)) {
		std::cout << "WSAStartup failed." << std::endl;
		exit(-1);
	}
	SOCKET socketfd = socket(AF_INET, SOCK_DGRAM, 0);

	servAddr.sin_family = AF_INET;
	servAddr.sin_port = htons(SERVERPORT);
	servAddr.sin_addr.S_un.S_addr = inet_addr(SERVERADDR);

	// set nonBlocking
	u_long nonBlocking = 1;
	if (ioctlsocket(socketfd, FIONBIO, &nonBlocking) == SOCKET_ERROR) {
		std::cout << "set nonblocking failed with code: " << WSAGetLastError() << std::endl;
		closesocket(socketfd);
		exit(-1);
	}

	// The state of the SO_BROADCAST socket option determines whether broadcast messages 
	// can be transmitted over a datagram socket.This socket option applies only to datagram sockets.
	int value = 1;
	if (setsockopt(socketfd, SOL_SOCKET, SO_BROADCAST, (char *)&value, sizeof(int)) == SOCKET_ERROR) {
		std::cout << "set broadcast failed with code: " << WSAGetLastError() << std::endl;
		closesocket(socketfd);
		exit(-1);
	}

	return socketfd;
}


int udp_output(const char *buf, int len, ikcpcb *kcp, void * user) {
	SOCKET socket = *((SOCKET*)user);
	int sendLength = 0;
	if ((sendLength = sendto(socket, buf, len, 0, (sockaddr *)&servAddr, sizeof(servAddr))) == SOCKET_ERROR) {
		std::cout << "send error with code: " << WSAGetLastError() << std::endl;
		closesocket(socket);
		exit(-1);
	}
	//std::cout << "send " << sendLength << std::endl;
	totalSendData += sendLength;
	totalSendPacket++;
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
		case WSAEINVAL:
			return 0;
		}
		exit(1);
	}
	//std::cout << "receive " << receiveLength << std::endl;
	totalReceiveData += receiveLength;
	totalReceivePacket++;
	return receiveLength;
}

int main() {

	SOCKET socketfd = set_socket();
	ikcpcb *kcp_client = ikcp_create(0x11223344, &socketfd);

	kcp_client->output = udp_output;

	IUINT32 current = (IUINT32)timeGetTime();
	IUINT32 slap = current + 30;
	IUINT32 index = 0, next = 0;
	const int PACKELENGTH = 80;
	const int TOTALPACKET = 1000;
	const int sendSlap = 30;
	IINT64 sumrtt = 0;
	int count = 0, maxrtt = 0;

	ikcp_wndsize(kcp_client, 128, 128);

	// 启动快速模式，
	ikcp_nodelay(kcp_client, 1, 10, 2, 1);

	char buffer[2000];
	memset(buffer, 'a', 2000);
	int hr;
#ifdef GENERATECSVFILE
	std::ofstream out_file("kcp.csv");
	std::vector<std::vector<int>> rttData(TOTALPACKET, std::vector<int>(2, 0));
	for (int i = 1; i <= rttData.size(); i++) {
		rttData[i - 1][0] = i;
	}
#endif // GENERATECSVFILE

	while (true) {
		Sleep(1);
		current = (IUINT32)timeGetTime();

		ikcp_update(kcp_client, current);

		for (; current >= slap; slap += sendSlap) {
			((IUINT32*)buffer)[0] = index++;
			((IUINT32*)buffer)[1] = current;

			ikcp_send(kcp_client, buffer, PACKELENGTH);
		}

		while (true) {
			hr = udp_input(socketfd, buffer, 2000);
			if (hr <= 0)
				break;
			ikcp_input(kcp_client, buffer, hr);
		}

		current = (IUINT32)timeGetTime();
		while (true) {
			hr = ikcp_recv(kcp_client, buffer, PACKELENGTH);
			if (hr < 0)
				break;
			IUINT32 sn = *(IUINT32*)(buffer + 0);
			IUINT32 ts = *(IUINT32*)(buffer + 4);
			IUINT32 rtt = current - ts;

			if (sn != next) {
				// 如果收到的包不连续
				std::cout << "ERROR sn " << count << "<->" << next << std::endl;
				return 0;
			}

			next++;
			sumrtt += rtt;
			count++;
			if (rtt > (IUINT32)maxrtt) maxrtt = rtt;

			std::cout << "[RECV] sn=" << sn << " rtt=" << rtt << std::endl;
#ifdef GENERATECSVFILE
			//out_file << seqNumber << ", " << rtt << std::endl;
			if(sn < TOTALPACKET)
				rttData[sn][1] = rtt;
#endif
		}
		if (next > TOTALPACKET)
			break;
	}

	std::cout << "avgrtt=" << 1.0 * sumrtt / count << " maxrtt=" << maxrtt << std::endl;
#ifdef GENERATECSVFILE
	out_file << "library, " << "kcp" << std::endl;
	out_file << "totalNumber, " << TOTALPACKET << std::endl;
	out_file << "totalReceive, " << TOTALPACKET << std::endl;
	out_file << "packetLength, " << PACKELENGTH << std::endl;
	size_t totalRtt = 0, maxRtt = 0;
	double averRtt = 0;
	for (int i = 0; i < rttData.size(); i++) {
		totalRtt += rttData[i][1];
		if (rttData[i][1] > maxRtt)
			maxRtt = rttData[i][1];
	}
	averRtt = 1.0 * totalRtt / rttData.size();
	out_file << "totalRtt, " << totalRtt << std::endl;
	out_file << "averageRtt, " << averRtt << std::endl;
	out_file << "maxRtt, " << maxRtt << std::endl;
	out_file << "needSendData, " << TOTALPACKET * PACKELENGTH << std::endl;
	out_file << "totalSendData, " << totalSendData << std::endl;
	out_file << "totalReceiveData, " << totalReceiveData << std::endl;
	out_file << "totalSendUdpPacket, " << totalSendPacket << std::endl;
	out_file << "totalReceiveUdpPacket, " << totalReceivePacket << std::endl;
	out_file << "upstreamLoss, 6.25" << std::endl;
	out_file << "upstreamLatency, 20" << std::endl;
	out_file << "upstreamDeviation, 10" << std::endl;
	out_file << "downstreamLoss, 6.25" << std::endl;
	out_file << "downstreamLatency, 20" << std::endl;
	out_file << "downstreamDeviation, 10" << std::endl;
	out_file << "timeStamp, " << (size_t)timeGetTime() << std::endl;
	out_file << "sendSlap, " << sendSlap << std::endl;
	for (int i = 0; i < rttData.size(); i++) {
		out_file << i + 1 << ", " << rttData[i][1] << std::endl;
	}
	out_file.close();
#endif // GENERATECSVFILE


#ifdef _MSC_VER
	system("pause");
#endif // _MSC_VER
	return 0;
}
