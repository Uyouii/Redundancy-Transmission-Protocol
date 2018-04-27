#include <cstdlib>
#include <cstdio>
#include "mrtp.h"

#define HOSTADDRESS "10.242.3.221"
//#define HOSTADDRESS "127.0.0.1"

MRtpHost* createServer() {
	MRtpAddress address;
	MRtpHost * server;

	mrtp_address_set_host(&address, HOSTADDRESS);
	address.port = 1234;

	server = mrtp_host_create(
		&address /* the address to bind the server host to */,
		8      /* allow up to 32 clients and/or outgoing connections */,
		0      /* assume any amount of incoming bandwidth */,
		0     /* assume any amount of outgoing bandwidth */
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
	//mrtp_host_open_quick_retransmit(server, 3);

	MRtpEvent event;
	/* Wait up to 1000 milliseconds for an event. */
	while (true) {
		while (mrtp_host_service(server, &event, 0) >= 1) {
			switch (event.type) {
			case MRTP_EVENT_TYPE_CONNECT:
				printf("A new client connected from %x:%u.\n",
					event.peer->address.host,
					event.peer->address.port);
				break;
			case MRTP_EVENT_TYPE_RECEIVE:
				printf("Receive a Pakcet of length: %d. Sequence Number: %d, TimeStamp: %d\n",
					event.packet->dataLength,
					*((mrtp_uint32*)event.packet->data),
					*((mrtp_uint32*)(event.packet->data + sizeof(mrtp_uint32))));
				/* Clean up the packet now that we're done using it. */
				event.packet->referenceCount = 0;
				mrtp_peer_send(event.peer, event.packet);
				break;

			case MRTP_EVENT_TYPE_DISCONNECT:
				printf("disconnected.\n");
				event.peer->data = NULL;
				break;
			}
		}
		Sleep(1);
	}
	
	mrtp_host_destroy(server);

	atexit(mrtp_deinitialize);

#ifdef _MSC_VER
	system("pause");
#endif // _MSC_VER

}