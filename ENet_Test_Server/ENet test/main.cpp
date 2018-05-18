#include <cstdlib>
#include <cstdio>
#include "enet/enet.h"

//#define HOSTADDRESS "10.242.3.221"
#define HOSTADDRESS "192.168.31.233"
//#define HOSTADDRESS "127.0.0.1"

ENetHost* createServer() {
	ENetAddress address;
	ENetHost * server;

	enet_address_set_host(&address, HOSTADDRESS);
	address.port = 1234;

	server = enet_host_create(
		&address /* the address to bind the server host to */,
		8      /* allow up to 32 clients and/or outgoing connections */,
		2,
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

	if (enet_initialize() != 0) {
		printf("An error occurred while initializing ENet.\n");
		system("pause");
		exit(EXIT_FAILURE);
	}

	ENetHost* server = createServer();

	ENetEvent event;
	/* Wait up to 1000 milliseconds for an event. */
	while (true) {
		while (enet_host_service(server, &event, 0) >= 1) {
			switch (event.type) {
			case ENET_EVENT_TYPE_CONNECT:
				printf("A new client connected from %x:%u.\n",
					event.peer->address.host,
					event.peer->address.port);
				break;
			case ENET_EVENT_TYPE_RECEIVE:
				printf("Receive a Pakcet of length: %d. Sequence Number: %d, TimeStamp: %d\n",
					event.packet->dataLength,
					*((enet_uint32*)event.packet->data),
					*((enet_uint32*)(event.packet->data + sizeof(enet_uint32))));
				/* Clean up the packet now that we're done using it. */
				event.packet->referenceCount = 0;
				enet_peer_send(event.peer, 0,event.packet);
				break;

			case ENET_EVENT_TYPE_DISCONNECT:
				printf("disconnected.\n");
				event.peer->data = NULL;
				break;
			}
		}
		Sleep(1);
	}

	enet_host_destroy(server);

	atexit(enet_deinitialize);

#ifdef _MSC_VER
	system("pause");
#endif // _MSC_VER

}