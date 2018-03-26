#include"mrtp.h"
#include<cstdlib>
#include<cstdio>
#include<cstring>

#define SERVERADDRESS "10.242.3.221"

MRtpHost* createClient() {
	MRtpHost * client;
	client = mrtp_host_create(
		NULL /* create a client host */,
		1 /* only allow 1 outgoing connection */,
		57600 / 8 /* 56K modem with 56 Kbps downstream bandwidth */,
		14400 / 8 /* 56K modem with 14 Kbps upstream bandwidth */
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
	peer = mrtp_host_connect(client, &address, 0);

	if (peer == NULL) {
		printf("No available peers for initiating an MRtp connection.\n");
		system("pause");
		exit(EXIT_FAILURE);
	}

	bool disconnected = false;
	/* Wait up to 5 seconds for the connection attempt to succeed. */
	while (mrtp_host_service(client, &event, 5000) >= 0) {
		switch (event.type) {
		case MRTP_EVENT_TYPE_CONNECT:
			printf("connect to server %x:%u.\n", event.peer->address.host, event.peer->address.port);
			printf("%d\n", event.peer->state);
			break;
		case MRTP_EVENT_TYPE_DISCONNECT:
			printf("Disconnection succeeded.\n");
			disconnected = true;
			break;
		}
		if (disconnected)
			break;
		if (peer->outgoingReliableSequenceNumber > 5) {
			mrtp_peer_disconnect(peer, 0);
		}
	}
	atexit(mrtp_deinitialize);
	system("pause");

}