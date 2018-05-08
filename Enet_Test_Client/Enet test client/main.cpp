#include"enet/enet.h"
#include<cstdlib>
#include<cstdio>
#include<cstring>
#include<string>
#include<iostream>
#include<fstream>

//#define SERVERADDRESS "10.240.66.57"
#define SERVERADDRESS "10.242.3.221"
//#define SERVERADDRESS "127.0.0.1"

ENetHost* createClient() {
	ENetHost * client;
	client = enet_host_create(
		NULL /* create a client host */,
		1 /* only allow 1 outgoing connection */,
		2,
		0 /* 56K modem with 56 Kbps downstream bandwidth */,
		0 /* 56K modem with 14 Kbps upstream bandwidth */
	);

	if (client == NULL) {
		printf("An error occurred while trying to create an ENet client host.\n");
		exit(EXIT_FAILURE);
	}

	printf("create Client Successfully!\n");

	return client;
}

int main(int argc, char ** argv) {

	if (enet_initialize() != 0) {
		printf("An error occurred while initializing ENet.\n");
		exit(EXIT_FAILURE);
	}
	ENetHost* client = createClient();

	ENetAddress address;
	ENetEvent event;
	ENetPeer *peer;

	/* Connect to some.server.net:1234. */
	enet_address_set_host(&address, SERVERADDRESS);
	address.port = 1234;

	/* Initiate the connection, allocating the two channels 0 and 1. */
	peer = enet_host_connect(client, &address, 2, 0);


	if (peer == NULL) {
		printf("No available peers for initiating an ENet connection.\n");
		system("pause");
		exit(EXIT_FAILURE);
	}

	bool disconnected = false;
	bool hasconnected = false;
	const int TOTALPACKET = 2000;
	const int PACKELENGTH = 80;
	enet_uint32 packetNum = 1;
	enet_uint32 currentTime = (enet_uint32)timeGetTime();
	enet_uint32 slap = currentTime + 1000;
	enet_uint32 totalRTT = 0, maxRTT = 0;
	enet_uint8 * buffer = (enet_uint8 *)malloc(PACKELENGTH);
	memset(buffer, 'a', PACKELENGTH);

	std::ofstream out_file("enet.csv");
	while (true) {

		currentTime = (enet_uint32)timeGetTime();

		while (enet_host_service(client, &event, 0) >= 1) {

			switch (event.type) {

			case ENET_EVENT_TYPE_CONNECT:
				printf("connect to server %x:%u.\n", event.peer->address.host, event.peer->address.port);
				hasconnected = true;
				break;
			case ENET_EVENT_TYPE_DISCONNECT:
				printf("Disconnection succeeded.\n");
				disconnected = true;
				break;
			case ENET_EVENT_TYPE_RECEIVE:
				enet_uint32 seqNumber = *((enet_uint32*)event.packet->data);
				enet_uint32 sendTimeStamp = *((enet_uint32*)(event.packet->data + sizeof(enet_uint32)));

				enet_uint32 rtt = currentTime - sendTimeStamp;
				printf("Receive a Pakcet of length: %d. Sequence Number: %d, TimeStamp: %d rtt: %dms\n",
					event.packet->dataLength, seqNumber, sendTimeStamp, rtt);
				out_file << seqNumber << ", " << rtt << std::endl;
				totalRTT += rtt;
				if (rtt > maxRTT)
					maxRTT = rtt;
				/* Clean up the packet now that we're done using it. */
				enet_packet_destroy(event.packet);

				if (seqNumber >= TOTALPACKET) {
					printf("Total rtt: %d, average rtt: %f max rtt: %d\n",
						totalRTT, totalRTT * 1.0 / TOTALPACKET, maxRTT);
					enet_peer_disconnect(peer, 0);
					disconnected = true;
				}

				break;
			}
		}
		if (disconnected)
			break;

		if (hasconnected && !disconnected && packetNum <= TOTALPACKET && currentTime >= slap) {
			((enet_uint32*)buffer)[0] = packetNum;
			((enet_uint32*)buffer)[1] = currentTime;
			slap += 30;
			packetNum++;
			ENetPacket * packet = enet_packet_create(buffer, PACKELENGTH, ENET_PACKET_FLAG_RELIABLE);
			enet_peer_send(peer, 0,packet);
		}

		Sleep(1);
	}
	printf("totalData: %d totalPackets: %d\n", client->totalSentData, client->totalSentPackets);
	out_file.close();
	atexit(enet_deinitialize);

#ifdef _MSC_VER
	system("pause");
#endif // _MSC_VER
}