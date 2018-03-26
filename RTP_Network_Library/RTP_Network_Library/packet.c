#include <string.h>
#define MRTP_BUILDING_LIB 1
#include "mrtp.h"

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