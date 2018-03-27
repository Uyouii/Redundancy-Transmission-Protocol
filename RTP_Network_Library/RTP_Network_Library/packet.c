#include <string.h>
#define MRTP_BUILDING_LIB 1
#include "mrtp.h"

MRtpPacket * mrtp_packet_create(const void * data, size_t dataLength, mrtp_uint32 flags) {

	MRtpPacket * packet = (MRtpPacket *)mrtp_malloc(sizeof(MRtpPacket));
	if (packet == NULL)
		return NULL;

	if (flags & MRTP_PACKET_FLAG_NO_ALLOCATE)
		packet->data = (mrtp_uint8 *)data;
	else if (dataLength <= 0)
		packet->data = NULL;
	else {
		packet->data = (mrtp_uint8 *)mrtp_malloc(dataLength);
		if (packet->data == NULL) {
			mrtp_free(packet);
			return NULL;
		}
		if (data != NULL)
			memcpy(packet->data, data, dataLength);
	}

	packet->referenceCount = 0;
	packet->flags = flags;
	packet->dataLength = dataLength;
	packet->freeCallback = NULL;
	packet->userData = NULL;

	return packet;
}

int mrtp_packet_resize(MRtpPacket * packet, size_t dataLength) {

	mrtp_uint8 * newData;

	if (dataLength <= packet->dataLength || (packet->flags & MRTP_PACKET_FLAG_NO_ALLOCATE)) {
		packet->dataLength = dataLength;
		return 0;
	}

	newData = (mrtp_uint8 *)mrtp_malloc(dataLength);
	if (newData == NULL)
		return -1;

	memcpy(newData, packet->data, packet->dataLength);
	mrtp_free(packet->data);

	packet->data = newData;
	packet->dataLength = dataLength;

	return 0;
}

void mrtp_packet_destroy(MRtpPacket * packet) {
	if (packet == NULL)
		return;
	if (packet->freeCallback != NULL)
		(*packet->freeCallback) (packet);
	if (!(packet->flags & MRTP_PACKET_FLAG_NO_ALLOCATE) &&
		packet->data != NULL)
		mrtp_free(packet->data);
	mrtp_free(packet);
}

