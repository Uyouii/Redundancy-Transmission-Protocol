#include"mrtp.h"
#include<cstdlib>
#include<cstdio>
#include<cstring>
#include<string>
#include<iostream>
#include<fstream>

//#define SERVERADDRESS "10.240.66.57"
#define SERVERADDRESS "10.242.3.221"
//#define SERVERADDRESS "127.0.0.1"

MRtpHost* createClient() {
	MRtpHost * client;
	client = mrtp_host_create(
		NULL /* create a client host */,
		1 /* only allow 1 outgoing connection */,
		0 /* 56K modem with 56 Kbps downstream bandwidth */,
		0 /* 56K modem with 14 Kbps upstream bandwidth */
	);

	if (client == NULL) {
		printf("An error occurred while trying to create an MRtp client host.\n");
		exit(EXIT_FAILURE);
	}

	printf("create Client Successfully!\n");

	return client;
}

int main(int argc, char ** argv) {

	if (mrtp_initialize() != 0) {
		printf("An error occurred while initializing MRtp.\n");
		exit(EXIT_FAILURE);
	}
	MRtpHost* client = createClient();
	//mrtp_host_open_quick_retransmit(client, 3);

	MRtpAddress address;
	MRtpEvent event;
	MRtpPeer *peer;

	/* Connect to some.server.net:1234. */
	mrtp_address_set_host(&address, SERVERADDRESS);
	address.port = 1234;

	/* Initiate the connection, allocating the two channels 0 and 1. */
	peer = mrtp_host_connect(client, &address);


	if (peer == NULL) {
		printf("No available peers for initiating an MRtp connection.\n");
		system("pause");
		exit(EXIT_FAILURE);
	}

	bool disconnected = false;
	bool hasconnected = false;
	const int TOTALPACKET = 200;
	const int PACKELENGTH = 80;
	mrtp_uint32 packetNum = 1;
	mrtp_uint32 currentTime = (mrtp_uint32)timeGetTime();
	mrtp_uint32 slap = currentTime + 30;
	mrtp_uint32 totalRTT = 0, maxRTT = 0;
	mrtp_uint8 * buffer = (mrtp_uint8 *)malloc(PACKELENGTH);
	memset(buffer, 'a', PACKELENGTH);

	std::ofstream out_file("mrtp.csv");
	while (true) {

		currentTime = (mrtp_uint32)timeGetTime();

		while (mrtp_host_service(client, &event, 0) >= 1) {

			switch (event.type) {

			case MRTP_EVENT_TYPE_CONNECT:
				printf("connect to server %x:%u.\n", event.peer->address.host, event.peer->address.port);
				hasconnected = true;
				break;
			case MRTP_EVENT_TYPE_DISCONNECT:
				printf("Disconnection succeeded.\n");
				disconnected = true;
				break;
			case MRTP_EVENT_TYPE_RECEIVE:
				mrtp_uint32 seqNumber = *((mrtp_uint32*)event.packet->data);
				mrtp_uint32 sendTimeStamp = *((mrtp_uint32*)(event.packet->data + sizeof(mrtp_uint32)));
				
				mrtp_uint32 rtt = currentTime - sendTimeStamp;
				printf("Receive a Pakcet of length: %d. Sequence Number: %d, TimeStamp: %d rtt: %dms\n",
					event.packet->dataLength, seqNumber, sendTimeStamp, rtt);
				out_file << seqNumber << ", " << rtt << std::endl;
				totalRTT += rtt;
				if (rtt > maxRTT)
					maxRTT = rtt;
				/* Clean up the packet now that we're done using it. */
				mrtp_packet_destroy(event.packet);

				if (seqNumber >= TOTALPACKET) {
					printf("Total rtt: %d, average rtt: %f max rtt: %d\n", 
						totalRTT, totalRTT * 1.0 / TOTALPACKET, maxRTT);
					mrtp_peer_disconnect(peer, 0);
					disconnected = true;
				}

				break;
			}
		}
		if (disconnected)
			break;

		if (hasconnected && !disconnected && packetNum <= TOTALPACKET && currentTime >= slap) {
			((mrtp_uint32*)buffer)[0] = packetNum;
			((mrtp_uint32*)buffer)[1] = currentTime;
			slap += 30;
			packetNum++;
			MRtpPacket * packet = mrtp_packet_create(buffer, PACKELENGTH, MRTP_PACKET_FLAG_REDUNDANCY);
			mrtp_peer_send(peer, packet);
		}

		Sleep(1);
	}
	printf("totalData: %d totalPackets: %d\n", client->totalSentData, client->totalSentPackets);

	out_file.close();
	atexit(mrtp_deinitialize);

#ifdef _MSC_VER
	system("pause");
#endif // _MSC_VER
}