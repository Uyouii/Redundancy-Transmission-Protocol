#include <string.h>
#define MRTP_BUILDING_LIB 1
#include "mrtp.h"

void mrtp_peer_on_disconnect(MRtpPeer * peer) {

	if (peer->state == MRTP_PEER_STATE_CONNECTED || peer->state == MRTP_PEER_STATE_DISCONNECT_LATER) {

		if (peer->incomingBandwidth != 0)
			--peer->host->bandwidthLimitedPeers;

		--peer->host->connectedPeers;
	}
}

void mrtp_peer_on_connect(MRtpPeer * peer) {

	if (peer->state != MRTP_PEER_STATE_CONNECTED && peer->state != MRTP_PEER_STATE_DISCONNECT_LATER) {
		if (peer->incomingBandwidth != 0)
			++peer->host->bandwidthLimitedPeers;

		++peer->host->connectedPeers;
	}
}

static void mrtp_peer_remove_incoming_commands(MRtpList * queue, MRtpListIterator startCommand, 
	MRtpListIterator endCommand) {

	MRtpListIterator currentCommand;

	for (currentCommand = startCommand; currentCommand != endCommand; ) {

		MRtpIncomingCommand * incomingCommand = (MRtpIncomingCommand *)currentCommand;

		currentCommand = mrtp_list_next(currentCommand);

		mrtp_list_remove(&incomingCommand->incomingCommandList);

		if (incomingCommand->packet != NULL) {
			--incomingCommand->packet->referenceCount;
			// 在删除incomingCommand时如果发现packet引用次数为0，则删除packet
			if (incomingCommand->packet->referenceCount == 0)
				mrtp_packet_destroy(incomingCommand->packet);
		}

		if (incomingCommand->fragments != NULL)
			mrtp_free(incomingCommand->fragments);

		mrtp_free(incomingCommand);
	}
}

static void mrtp_peer_reset_incoming_commands(MRtpList * queue) {
	mrtp_peer_remove_incoming_commands(queue, mrtp_list_begin(queue), mrtp_list_end(queue));
}

static void mrtp_peer_reset_outgoing_commands(MRtpList * queue) {
	MRtpOutgoingCommand * outgoingCommand;

	while (!mrtp_list_empty(queue)) {
		outgoingCommand = (MRtpOutgoingCommand *)mrtp_list_remove(mrtp_list_begin(queue));

		if (outgoingCommand->packet != NULL) {
			--outgoingCommand->packet->referenceCount;

			if (outgoingCommand->packet->referenceCount == 0)
				mrtp_packet_destroy(outgoingCommand->packet);
		}

		mrtp_free(outgoingCommand);
	}
}

void mrtp_peer_reset_queues(MRtpPeer * peer) {
	MRtpChannel * channel;

	if (peer->needsDispatch) {
		mrtp_list_remove(&peer->dispatchList);

		peer->needsDispatch = 0;
	}

	while (!mrtp_list_empty(&peer->acknowledgements))
		mrtp_free(mrtp_list_remove(mrtp_list_begin(&peer->acknowledgements)));

	mrtp_peer_reset_outgoing_commands(&peer->sentReliableCommands);
	mrtp_peer_reset_outgoing_commands(&peer->outgoingReliableCommands);
	mrtp_peer_reset_incoming_commands(&peer->dispatchedCommands);

	if (peer->channels != NULL && peer->channelCount > 0) {
		for (channel = peer->channels;
			channel < &peer->channels[peer->channelCount];
			++channel) {
			mrtp_peer_reset_incoming_commands(&channel->incomingReliableCommands);
		}

		mrtp_free(peer->channels);
	}

	peer->channels = NULL;
	peer->channelCount = 0;
}

/** Forcefully disconnects a peer.
@param peer peer to forcefully disconnect
@remarks The foreign host represented by the peer is not notified of the disconnection and will timeout
on its connection to the local host.
*/
void mrtp_peer_reset(MRtpPeer * peer) {

	mrtp_peer_on_disconnect(peer);

	peer->outgoingPeerID = MRTP_PROTOCOL_MAXIMUM_PEER_ID;
	peer->connectID = 0;

	peer->state = MRTP_PEER_STATE_DISCONNECTED;

	peer->incomingBandwidth = 0;
	peer->outgoingBandwidth = 0;
	peer->incomingBandwidthThrottleEpoch = 0;
	peer->outgoingBandwidthThrottleEpoch = 0;
	peer->incomingDataTotal = 0;
	peer->outgoingDataTotal = 0;
	peer->lastSendTime = 0;
	peer->lastReceiveTime = 0;
	peer->nextTimeout = 0;
	peer->earliestTimeout = 0;
	peer->packetLossEpoch = 0;
	peer->packetsSent = 0;
	peer->packetsLost = 0;
	peer->packetLoss = 0;
	peer->packetLossVariance = 0;
	peer->packetThrottle = MRTP_PEER_DEFAULT_PACKET_THROTTLE;
	peer->packetThrottleLimit = MRTP_PEER_PACKET_THROTTLE_SCALE;
	peer->packetThrottleCounter = 0;
	peer->packetThrottleEpoch = 0;
	peer->packetThrottleAcceleration = MRTP_PEER_PACKET_THROTTLE_ACCELERATION;
	peer->packetThrottleDeceleration = MRTP_PEER_PACKET_THROTTLE_DECELERATION;
	peer->packetThrottleInterval = MRTP_PEER_PACKET_THROTTLE_INTERVAL;
	peer->pingInterval = MRTP_PEER_PING_INTERVAL;
	peer->timeoutLimit = MRTP_PEER_TIMEOUT_LIMIT;
	peer->timeoutMinimum = MRTP_PEER_TIMEOUT_MINIMUM;
	peer->timeoutMaximum = MRTP_PEER_TIMEOUT_MAXIMUM;
	peer->lastRoundTripTime = MRTP_PEER_DEFAULT_ROUND_TRIP_TIME;
	peer->lowestRoundTripTime = MRTP_PEER_DEFAULT_ROUND_TRIP_TIME;
	peer->lastRoundTripTimeVariance = 0;
	peer->highestRoundTripTimeVariance = 0;
	peer->roundTripTime = MRTP_PEER_DEFAULT_ROUND_TRIP_TIME;
	peer->roundTripTimeVariance = 0;
	peer->mtu = peer->host->mtu;
	peer->reliableDataInTransit = 0;
	peer->outgoingReliableSequenceNumber = 0;
	peer->windowSize = MRTP_PROTOCOL_MAXIMUM_WINDOW_SIZE;
	peer->eventData = 0;
	peer->totalWaitingData = 0;


	mrtp_peer_reset_queues(peer);
}

MRtpOutgoingCommand * mrtp_peer_queue_outgoing_command(MRtpPeer * peer, const MRtpProtocol * command,
	MRtpPacket * packet, mrtp_uint32 offset, mrtp_uint16 length) {

	MRtpOutgoingCommand * outgoingCommand = (MRtpOutgoingCommand *)mrtp_malloc(sizeof(MRtpOutgoingCommand));
	if (outgoingCommand == NULL)
		return NULL;

	outgoingCommand->command = *command;
	outgoingCommand->fragmentOffset = offset;
	outgoingCommand->fragmentLength = length;
	outgoingCommand->packet = packet;
	if (packet != NULL)
		++packet->referenceCount;

	mrtp_peer_setup_outgoing_command(peer, outgoingCommand);

	return outgoingCommand;
}

void mrtp_peer_setup_outgoing_command(MRtpPeer * peer, MRtpOutgoingCommand * outgoingCommand) {

	MRtpChannel * channel = &peer->channels[outgoingCommand->command.header.channelID];

	peer->outgoingDataTotal += mrtp_protocol_command_size(outgoingCommand->command.header.command) + 
		outgoingCommand->fragmentLength;

	// 根据指令类型增加相应的序号
	if (outgoingCommand->command.header.channelID == 0xFF) {
		++peer->outgoingReliableSequenceNumber;
		outgoingCommand->reliableSequenceNumber = peer->outgoingReliableSequenceNumber;
	}
	else if (outgoingCommand->command.header.command & MRTP_PROTOCOL_COMMAND_FLAG_ACKNOWLEDGE) {
			++channel->outgoingReliableSequenceNumber;
			outgoingCommand->reliableSequenceNumber = channel->outgoingReliableSequenceNumber;
	}
	else {
		outgoingCommand->reliableSequenceNumber = channel->outgoingReliableSequenceNumber;
	}

	outgoingCommand->sendAttempts = 0;
	outgoingCommand->sentTime = 0;
	outgoingCommand->roundTripTimeout = 0;
	outgoingCommand->roundTripTimeoutLimit = 0;
	outgoingCommand->command.header.reliableSequenceNumber = MRTP_HOST_TO_NET_16(outgoingCommand->reliableSequenceNumber);

	if (outgoingCommand->command.header.command & MRTP_PROTOCOL_COMMAND_FLAG_ACKNOWLEDGE)
		mrtp_list_insert(mrtp_list_end(&peer->outgoingReliableCommands), outgoingCommand);
}

void mrtp_peer_ping(MRtpPeer * peer) {
	MRtpProtocol command;

	if (peer->state != MRTP_PEER_STATE_CONNECTED)
		return;

	command.header.command = MRTP_PROTOCOL_COMMAND_PING | MRTP_PROTOCOL_COMMAND_FLAG_ACKNOWLEDGE;
	command.header.channelID = 0xFF;

	mrtp_peer_queue_outgoing_command(peer, &command, NULL, 0, 0);
}

MRtpAcknowledgement * mrtp_peer_queue_acknowledgement(MRtpPeer * peer, const MRtpProtocol * command,
	mrtp_uint16 sentTime)
{
	MRtpAcknowledgement * acknowledgement;

	if (command->header.channelID < peer->channelCount) {
		MRtpChannel * channel = &peer->channels[command->header.channelID];
		mrtp_uint16 reliableWindow = command->header.reliableSequenceNumber / MRTP_PEER_RELIABLE_WINDOW_SIZE,
			currentWindow = channel->incomingReliableSequenceNumber / MRTP_PEER_RELIABLE_WINDOW_SIZE;

		if (command->header.reliableSequenceNumber < channel->incomingReliableSequenceNumber)
			reliableWindow += MRTP_PEER_RELIABLE_WINDOWS;

		if (reliableWindow >= currentWindow + MRTP_PEER_FREE_RELIABLE_WINDOWS - 1 && reliableWindow <= currentWindow + MRTP_PEER_FREE_RELIABLE_WINDOWS)
			return NULL;
	}

	acknowledgement = (MRtpAcknowledgement *)mrtp_malloc(sizeof(MRtpAcknowledgement));
	if (acknowledgement == NULL)
		return NULL;

	peer->outgoingDataTotal += sizeof(MRtpProtocolAcknowledge);

	acknowledgement->sentTime = sentTime;
	acknowledgement->command = *command;

	mrtp_list_insert(mrtp_list_end(&peer->acknowledgements), acknowledgement);

	return acknowledgement;
}

//调节peer->packetThrottle
int mrtp_peer_throttle(MRtpPeer * peer, mrtp_uint32 rtt) {
	//上次传输的速度非常快
	if (peer->lastRoundTripTime <= peer->lastRoundTripTimeVariance) {
		peer->packetThrottle = peer->packetThrottleLimit;
	}
	else if (rtt < peer->lastRoundTripTime) {
		peer->packetThrottle += peer->packetThrottleAcceleration;

		if (peer->packetThrottle > peer->packetThrottleLimit)
			peer->packetThrottle = peer->packetThrottleLimit;

		return 1;
	} 
	else if (rtt > peer->lastRoundTripTime + 2 * peer->lastRoundTripTimeVariance) {
		if (peer->packetThrottle > peer->packetThrottleDeceleration)
			peer->packetThrottle -= peer->packetThrottleDeceleration;
		else
			peer->packetThrottle = 0;
		return -1;
	}

	return 0;
}

MRtpPacket * mrtp_peer_receive(MRtpPeer * peer, mrtp_uint8 * channelID) {

	MRtpIncomingCommand * incomingCommand;
	MRtpPacket * packet;

	if (mrtp_list_empty(&peer->dispatchedCommands))
		return NULL;

	incomingCommand = (MRtpIncomingCommand *)mrtp_list_remove(mrtp_list_begin(&peer->dispatchedCommands));

	if (channelID != NULL)
		* channelID = incomingCommand->command.header.channelID;

	packet = incomingCommand->packet;

	--packet->referenceCount;

	if (incomingCommand->fragments != NULL)
		mrtp_free(incomingCommand->fragments);

	mrtp_free(incomingCommand);

	peer->totalWaitingData -= packet->dataLength;

	return packet;
}

void mrtp_peer_disconnect(MRtpPeer * peer, mrtp_uint32 data) {
	MRtpProtocol command;

	if (peer->state == MRTP_PEER_STATE_DISCONNECTING ||
		peer->state == MRTP_PEER_STATE_DISCONNECTED ||
		peer->state == MRTP_PEER_STATE_ACKNOWLEDGING_DISCONNECT ||
		peer->state == MRTP_PEER_STATE_ZOMBIE)
		return;

	mrtp_peer_reset_queues(peer);

	command.header.command = MRTP_PROTOCOL_COMMAND_DISCONNECT;
	command.header.channelID = 0xFF;
	command.disconnect.data = MRTP_HOST_TO_NET_32(data);

	if (peer->state == MRTP_PEER_STATE_CONNECTED || peer->state == MRTP_PEER_STATE_DISCONNECT_LATER)
		command.header.command |= MRTP_PROTOCOL_COMMAND_FLAG_ACKNOWLEDGE;

	mrtp_peer_queue_outgoing_command(peer, &command, NULL, 0, 0);

	if (peer->state == MRTP_PEER_STATE_CONNECTED || peer->state == MRTP_PEER_STATE_DISCONNECT_LATER) {
		mrtp_peer_on_disconnect(peer);

		peer->state = MRTP_PEER_STATE_DISCONNECTING;
	}
	else {
		mrtp_host_flush(peer->host);
		mrtp_peer_reset(peer);
	}
}

void mrtp_peer_disconnect_later(MRtpPeer * peer, mrtp_uint32 data) {
	if ((peer->state == MRTP_PEER_STATE_CONNECTED || peer->state == MRTP_PEER_STATE_DISCONNECT_LATER) &&
		!(mrtp_list_empty(&peer->outgoingReliableCommands) &&  mrtp_list_empty(&peer->sentReliableCommands)))
	{
		peer->state = MRTP_PEER_STATE_DISCONNECT_LATER;
		peer->eventData = data;
	}
	else
		mrtp_peer_disconnect(peer, data);
}

// 发送一个不需要ack的断开连接的命令，随后重置该peer
void mrtp_peer_disconnect_now(MRtpPeer * peer, mrtp_uint32 data) {
	MRtpProtocol command;

	if (peer->state == MRTP_PEER_STATE_DISCONNECTED)
		return;

	if (peer->state != MRTP_PEER_STATE_ZOMBIE && peer->state != MRTP_PEER_STATE_DISCONNECTING) {
		mrtp_peer_reset_queues(peer);

		command.header.command = MRTP_PROTOCOL_COMMAND_DISCONNECT;
		command.header.channelID = 0xFF;
		command.disconnect.data = MRTP_HOST_TO_NET_32(data);

		mrtp_peer_queue_outgoing_command(peer, &command, NULL, 0, 0);

		mrtp_host_flush(peer->host);
	}

	mrtp_peer_reset(peer);
}