#include"mrtp.h"
#include<cstdlib>
#include<cstdio>
#include<cstring>
#include<string>
#include<iostream>

//#define SERVERADDRESS "10.240.66.57"
#define SERVERADDRESS "10.242.3.221"

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
	int packetNum = 0;
	/* Wait up to 5 seconds for the connection attempt to succeed. */
	while (true) {
		while (mrtp_host_service(client, &event, 0) >= 1) {
			switch (event.type) {
			case MRTP_EVENT_TYPE_CONNECT:
				printf("connect to server %x:%u.\n", event.peer->address.host, event.peer->address.port);
				mrtp_peer_quick_restransmit_configure(peer, 5);
				hasconnected = true;
				break;
			case MRTP_EVENT_TYPE_DISCONNECT:
				printf("Disconnection succeeded.\n");
				disconnected = true;
				break;
			}
		}
		if (disconnected)
			break;
		if (hasconnected && !disconnected && (peer->outgoingReliableSequenceNumber > 10 ||
			peer->channels[MRTP_PROTOCOL_RELIABLE_CHANNEL_NUM].outgoingSequenceNumber > 10) ||
			(peer->channels[MRTP_PROTOCOL_REDUNDANCY_NOACK_CHANNEL_NUM].outgoingSequenceNumber > 10) ||
			(peer->channels[MRTP_PROTOCOL_REDUNDANCY_CHANNEL_NUM].outgoingSequenceNumber > 100)) {
			mrtp_peer_disconnect(peer, 0);
			disconnected = true;
		}
		if (hasconnected && !disconnected) {
			std::string packet_str = "packct" + std::to_string(packetNum) + " at peer" + std::to_string(peer->outgoingPeerID);
			packet_str += std::string(5, 'a');
			MRtpPacket * packet = mrtp_packet_create(packet_str.c_str(), packet_str.size() + 1, MRTP_PACKET_FLAG_REDUNDANCY);
			mrtp_peer_send(peer, packet);
			packetNum++;
		}
		Sleep(30);
	}
	
	atexit(mrtp_deinitialize);
#ifdef _MSC_VER
	system("pause");
#endif // _MSC_VER
}