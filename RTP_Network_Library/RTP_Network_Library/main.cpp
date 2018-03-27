#include <cstdlib>
#include <cstdio>
#include "mrtp.h"

#define HOSTADDRESS "10.242.3.221"

MRtpHost* createServer() {
	MRtpAddress address;
	MRtpHost * server;

	mrtp_address_set_host(&address, HOSTADDRESS);
	address.port = 1234;

	server = mrtp_host_create(
		&address /* the address to bind the server host to */,
		128      /* allow up to 32 clients and/or outgoing connections */,
		0      /* assume any amount of incoming bandwidth */,
		0      /* assume any amount of outgoing bandwidth */
	);

	if (server == NULL) {
		printf("An error occurred while initializing Server.\n");
		system("pause");
		exit(EXIT_FAILURE);
	}

	printf("Create Server Succellfully!\n");
	return server;
}


int main(int argc, char ** argv) {

	if (mrtp_initialize() != 0) {
		printf("An error occurred while initializing MRtp.\n");
		system("pause");
		exit(EXIT_FAILURE);
	}

	MRtpHost* server = createServer();

	MRtpEvent event;
	/* Wait up to 1000 milliseconds for an event. */
	while (mrtp_host_service(server, &event, 100) >= 0) {
		switch (event.type) {
		case MRTP_EVENT_TYPE_CONNECT:
			printf("A new client connected from %x:%u.\n",
				event.peer->address.host,
				event.peer->address.port);
			printf("%d\n", event.peer->state);
			break;
		case MRTP_EVENT_TYPE_RECEIVE:
			printf("A packet of length %u containing %s was received from %s on channel %u.\n",
				event.packet->dataLength,
				event.packet->data,
				event.peer->data,
				event.channelID);
			/* Clean up the packet now that we're done using it. */
			mrtp_packet_destroy(event.packet);
			printf("%d\n", event.peer->state);
			break;

		case MRTP_EVENT_TYPE_DISCONNECT:
			printf("disconnected.\n");
			event.peer->data = NULL;
			printf("%d\n", event.peer->state);
			break;
		}
	}

	mrtp_host_destroy(server);

	atexit(mrtp_deinitialize);
	system("pause");

}