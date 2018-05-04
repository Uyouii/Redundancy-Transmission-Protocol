#include <stdlib.h>
#include <winsock2.h>
#include <iostream>
#include <vector>

#define SERVERPORT 6666


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

	SOCKET listenfd = socket(AF_INET, SOCK_STREAM, 0);
	sockaddr_in  servaddr, cliaddr;

	servaddr.sin_family = AF_INET;
	servaddr.sin_port = htons(SERVERPORT);
	servaddr.sin_addr.S_un.S_addr = htonl(INADDR_ANY);

	if (bind(listenfd, (sockaddr*)&servaddr, sizeof(sockaddr)) == SOCKET_ERROR) {
		std::cout << "bind failed with code: " << WSAGetLastError() << std::endl;
		return -1;
	}

	if (listen(listenfd, 8) == SOCKET_ERROR) {
		std::cout << "listen failed with code: " << WSAGetLastError() << std::endl;
		return -1;
	}

	SOCKET connfd, sockfd;
	int client[FD_SETSIZE];
	int nready, i, maxi;
	FD_SET readSet;
	FD_SET allSet;

	char buffer[1024];

	FD_ZERO(&allSet);
	FD_SET(listenfd, &allSet);

	for (i = 0; i < FD_SETSIZE; i++) {
		client[i] = -1;
	}
	maxi = -1;

	while (true) {

		readSet = allSet;

		if ((nready = select(0, &readSet, NULL, NULL, NULL)) == SOCKET_ERROR) {
			std::cout << "select error with code: " << WSAGetLastError() << std::endl;
			return -1;
		}

		sockaddr_in addr;
		int len = sizeof(addr);

		if (FD_ISSET(listenfd, &readSet)) {
			connfd = accept(listenfd, (sockaddr*)&addr, &len);
			if (connfd == INVALID_SOCKET) {
				std::cout << "accpet error" << std::endl;
				return -1;
			}
			else {
				for(i = 0; i < FD_SETSIZE; i++)
					if (client[i] < 0) {
						client[i] = connfd;
						break;
					}
				if (i == FD_SETSIZE) {
					std::cout << "too many clients" << std::endl;
					return -1;
				}

				FD_SET(connfd, &allSet);
				std::cout << "new client connect" << std::endl;
				if (i > maxi)
					maxi = i;
				if (--nready <= 0)
					continue;
			}
		}

		for (i = 0; i <= maxi; i++) {
			if ((sockfd = client[i]) < 0)
				continue;
			if (FD_ISSET(sockfd, &readSet)) {
				int n;
				if ((n = recv(sockfd, buffer, 1024, 0)) == 0) {
					closesocket(sockfd);
					FD_CLR(sockfd, &allSet);
					client[i] = -1;
				}
				else {
					send(sockfd, buffer, n, 0);
				}

				if (--nready <= 0)
					break;
			}
		}

	}

	WSACleanup();
#ifdef _MSC_VER
	system("pause");
#endif // _MSC_VER
	return 0;
}