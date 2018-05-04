
#define _WINSOCK_DEPRECATED_NO_WARNINGS 

#include <stdlib.h>
#include <winsock2.h>
#include <iostream>
#include <vector>

#define SERVERPORT 6666
#define SERVERADDR "10.242.3.221"

int main() {

	WORD versionRequested = MAKEWORD(1, 1);
	WSADATA wsaData;

	if (WSAStartup(versionRequested, &wsaData)) {
		std::cout << "WSAStartup failed." << std::endl;
		return -1;
	}

	if (LOBYTE(wsaData.wVersion) != 1 || HIBYTE(wsaData.wVersion) != 1) {
		WSACleanup();
		return -1;
	}

	SOCKET sockfd = socket(AF_INET, SOCK_STREAM, 0);
	sockaddr_in  servaddr;

	servaddr.sin_family = AF_INET;
	servaddr.sin_port = htons(SERVERPORT);
	servaddr.sin_addr.S_un.S_addr = inet_addr(SERVERADDR);

	if (connect(sockfd, (sockaddr *)&servaddr, sizeof(sockaddr)) == SOCKET_ERROR) {
		std::cout << "connect failed with code: " << WSAGetLastError() << std::endl;
	}

	FD_SET readSet;

	bool isconnect = true;
	timeval waittime;

	const int TOTALPACKET = 2000;
	const int PACKELENGTH = 80;
	char * sendBuffer = (char *)malloc(PACKELENGTH);
	memset(sendBuffer, 'a', PACKELENGTH);
	char buffer[1024];
	size_t packetNum = 1;
	size_t currentTime = (size_t)timeGetTime();
	size_t slap = currentTime + 30;
	size_t totalRTT = 0, maxRTT = 0;

	std::vector<char> recvBuffer;

	while (isconnect) {
		FD_ZERO(&readSet);
		FD_SET(sockfd, &readSet);

		waittime.tv_sec = 0;
		waittime.tv_usec = 1000;
		currentTime = (size_t)timeGetTime();

		if (isconnect && packetNum <= TOTALPACKET && currentTime >= slap) {
			((size_t*)sendBuffer)[0] = packetNum;
			((size_t*)sendBuffer)[1] = currentTime;
			slap += 30;
			++packetNum;
			send(sockfd, sendBuffer, TOTALPACKET, 0);
		}

		int nready = select(0, &readSet, NULL, NULL, &waittime);
		if (nready > 0) {
			if (FD_ISSET(sockfd, &readSet)) {
				int n;
				if ((n = recv(sockfd, buffer, 1024, 0)) == 0) {
					std::cout << "Server disconnect." << std::endl;
					break;
				}
				else {
					// first store the data in the recv buffer, then read data from the recv buffer
					std::vector<char> temp(buffer, buffer + n);
					recvBuffer.insert(recvBuffer.end(), temp.begin(), temp.end());

					size_t loc = 0;
					while (recvBuffer.size() - loc >= TOTALPACKET) {
						size_t seqNumber = *((size_t*)(&recvBuffer[loc]));
						size_t sendTimeStamp = *((size_t*)(&recvBuffer[loc + sizeof(size_t)]));
						currentTime = (size_t)timeGetTime();
						size_t rtt = currentTime - sendTimeStamp;
						std::cout << "Receive a packet of length: " << PACKELENGTH <<
							" Sequence Number: " << seqNumber <<
							" TimeStamp: " << sendTimeStamp <<
							" rtt: " << rtt << std::endl;
						totalRTT += rtt;
						if (rtt > maxRTT)
							maxRTT = rtt;
						if (seqNumber >= TOTALPACKET) {
							std::cout << "Total rtt: " << totalRTT <<
								" average rtt: " << totalRTT * 1.0 / TOTALPACKET <<
								" max rtt: " << maxRTT << std::endl;
							closesocket(sockfd);
							isconnect = false;
						}

						loc += TOTALPACKET;
					}
					recvBuffer.erase(recvBuffer.begin(), recvBuffer.begin() + loc);
				}
			}
		}
	}

	closesocket(sockfd);
	WSACleanup();
#ifdef _MSC_VER
	system("pause");
#endif // _MSC_VER
	return 0;
}