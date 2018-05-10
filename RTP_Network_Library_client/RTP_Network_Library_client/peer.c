#include <string.h>
#include "mrtp.h"

extern mrtp_uint8 channelIDs[];
extern char* commandName[];

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

	while (!mrtp_list_empty(&peer->redundancyAcknowledgemets))
		mrtp_free(mrtp_list_remove(mrtp_list_begin(&peer->redundancyAcknowledgemets)));

	mrtp_peer_reset_outgoing_commands(&peer->sentReliableCommands);
	mrtp_peer_reset_outgoing_commands(&peer->sentRedundancyNoAckCommands);
	mrtp_peer_reset_outgoing_commands(&peer->outgoingReliableCommands);
	mrtp_peer_reset_outgoing_commands(&peer->outgoingRedundancyCommands);
	mrtp_peer_reset_outgoing_commands(&peer->outgoingRedundancyNoAckCommands);
	mrtp_peer_reset_outgoing_commands(&peer->sentRedundancyLastTimeCommands);
	mrtp_peer_reset_outgoing_commands(&peer->sentRedundancyThisTimeCommands);
	mrtp_peer_reset_outgoing_commands(&peer->outgoingUnsequencedCommands);
	mrtp_peer_reset_outgoing_commands(&peer->sentUnsequencedCommands);
	mrtp_peer_reset_incoming_commands(&peer->dispatchedCommands);


	for (channel = peer->channels; channel < &peer->channels[peer->channelCount]; ++channel) {
		mrtp_peer_reset_incoming_commands(&channel->incomingCommands);
		channel->outgoingSequenceNumber = 0;
		channel->incomingSequenceNumber = 0;

		channel->usedWindows = 0;
		memset(channel->commandWindows, 0, sizeof(channel->commandWindows));
	}

}

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
	peer->quickRetransmitNum = MRTP_PROTOCOL_DEFAULT_QUICK_RETRANSMIT;

	peer->redundancyLastSentTimeStamp = 0;

	peer->sendRedundancyAfterReceive = TRUE;
	peer->sentRedundancyLastTimeSize = 0;
	peer->sentRedundancyThisTimeSize = 0;

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

	mrtp_uint8 channelID = channelIDs[outgoingCommand->command.header.command & MRTP_PROTOCOL_COMMAND_MASK];
	mrtp_uint8 commandNumber;
	MRtpChannel * channel = &peer->channels[channelID];

	peer->outgoingDataTotal += mrtp_protocol_command_size(outgoingCommand->command.header.command) +
		outgoingCommand->fragmentLength;

	// if channel is 0xFF, it is a system command, so we add the sequence number of peer
	// or add sequence number of channel
	if (channelID == 0xFF) {
		++peer->outgoingReliableSequenceNumber;
		outgoingCommand->sequenceNumber = peer->outgoingReliableSequenceNumber;
	}
	else if (outgoingCommand->command.header.command & MRTP_PROTOCOL_COMMAND_FLAG_UNSEQUENCED) {
		if ((outgoingCommand->command.header.command & MRTP_PROTOCOL_COMMAND_MASK) == MRTP_PROTOCOL_COMMAND_SEND_UNSEQUENCED_FRAGMENT) {
			++channel->outgoingSequenceNumber;
			outgoingCommand->sequenceNumber = channel->outgoingSequenceNumber;
		}
		else {
			++peer->outgoingUnsequencedGroup;
			outgoingCommand->sequenceNumber = 0;
		}

	}
	else {
		++channel->outgoingSequenceNumber;
		outgoingCommand->sequenceNumber = channel->outgoingSequenceNumber;
	}

	outgoingCommand->sendAttempts = 0;
	outgoingCommand->sentTime = 0;
	outgoingCommand->roundTripTimeout = 0;
	outgoingCommand->roundTripTimeoutLimit = 0;
	outgoingCommand->fastAck = 0;
	outgoingCommand->redundancyBufferNum = 0xFFFF;
	outgoingCommand->command.header.sequenceNumber = MRTP_HOST_TO_NET_16(outgoingCommand->sequenceNumber);

	if (channelID == MRTP_PROTOCOL_RELIABLE_CHANNEL_NUM || channelID == 0xFF) {

		mrtp_list_insert(mrtp_list_end(&peer->outgoingReliableCommands), outgoingCommand);

	}
	else if (channelID == MRTP_PROTOCOL_REDUNDANCY_CHANNEL_NUM) {
		mrtp_list_insert(mrtp_list_end(&peer->outgoingRedundancyCommands), outgoingCommand);
	}
	else if (channelID == MRTP_PROTOCOL_REDUNDANCY_NOACK_CHANNEL_NUM) {
		mrtp_list_insert(mrtp_list_end(&peer->outgoingRedundancyNoAckCommands), outgoingCommand);
	}
	else if (channelID == MRTP_PROTOCOL_UNSEQUENCED_CHANNEL_NUM) {
		if ((outgoingCommand->command.header.command & MRTP_PROTOCOL_COMMAND_MASK) == MRTP_PROTOCOL_COMMAND_SEND_UNSEQUENCED) {
			outgoingCommand->command.sendUnsequenced.unsequencedGroup = MRTP_HOST_TO_NET_16(peer->outgoingUnsequencedGroup);
		}
		mrtp_list_insert(mrtp_list_end(&peer->outgoingUnsequencedCommands), outgoingCommand);
	}
}

void mrtp_peer_ping(MRtpPeer * peer) {
	MRtpProtocol command;

	if (peer->state != MRTP_PEER_STATE_CONNECTED)
		return;

	command.header.command = MRTP_PROTOCOL_COMMAND_PING | MRTP_PROTOCOL_COMMAND_FLAG_ACKNOWLEDGE;

	mrtp_peer_queue_outgoing_command(peer, &command, NULL, 0, 0);
}

MRtpAcknowledgement * mrtp_peer_queue_acknowledgement(MRtpPeer * peer, const MRtpProtocol * command,
	mrtp_uint16 sentTime)
{
	MRtpAcknowledgement * acknowledgement;
	mrtp_uint8 channelID = channelIDs[command->header.command & MRTP_PROTOCOL_COMMAND_MASK];

	if (channelID < peer->channelCount) {
		MRtpChannel * channel = &peer->channels[channelID];
		mrtp_uint16 reliableWindow = command->header.sequenceNumber / MRTP_PEER_WINDOW_SIZE,
			currentWindow = channel->incomingSequenceNumber / MRTP_PEER_WINDOW_SIZE;

		if (command->header.sequenceNumber < channel->incomingSequenceNumber)
			reliableWindow += MRTP_PEER_WINDOWS;

		if (reliableWindow >= currentWindow + MRTP_PEER_FREE_WINDOWS - 1 && reliableWindow <= currentWindow + MRTP_PEER_FREE_WINDOWS)
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

MRtpAcknowledgement * mrtp_peer_queue_redundancy_acknowldegement(MRtpPeer* peer, const MRtpProtocol * command,
	mrtp_uint16 sentTime)
{
	mrtp_uint16 nextRedundancyNumber = peer->channels[MRTP_PROTOCOL_REDUNDANCY_CHANNEL_NUM].incomingSequenceNumber + 1;
	mrtp_uint16 sequenceNumber = MRTP_NET_TO_HOST_16(command->header.sequenceNumber);

	//if (sequenceNumber >= nextRedundancyNumber - 1) {
	MRtpAcknowledgement * acknowledgement;

	acknowledgement = (MRtpAcknowledgement *)mrtp_malloc(sizeof(MRtpAcknowledgement));
	if (acknowledgement == NULL)
		return NULL;

	peer->outgoingDataTotal += sizeof(MRtpProtocolRedundancyAcknowledge);

	acknowledgement->sentTime = sentTime;
	acknowledgement->command = *command;

	mrtp_list_insert(mrtp_list_end(&peer->redundancyAcknowledgemets), acknowledgement);

	return acknowledgement;
	//}
	//return NULL;
}

//调节peer->packetThrottle
int mrtp_peer_throttle(MRtpPeer * peer, mrtp_uint32 rtt) {

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

void mrtp_peer_disconnect(MRtpPeer * peer) {
	MRtpProtocol command;

	if (peer->state == MRTP_PEER_STATE_DISCONNECTING ||
		peer->state == MRTP_PEER_STATE_DISCONNECTED ||
		peer->state == MRTP_PEER_STATE_ACKNOWLEDGING_DISCONNECT ||
		peer->state == MRTP_PEER_STATE_ZOMBIE)
		return;

	mrtp_peer_reset_queues(peer);

	command.header.command = MRTP_PROTOCOL_COMMAND_DISCONNECT;

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

void mrtp_peer_disconnect_later(MRtpPeer * peer) {
	if ((peer->state == MRTP_PEER_STATE_CONNECTED || peer->state == MRTP_PEER_STATE_DISCONNECT_LATER) &&
		!(mrtp_list_empty(&peer->outgoingReliableCommands) && mrtp_list_empty(&peer->sentReliableCommands)))
	{
		peer->state = MRTP_PEER_STATE_DISCONNECT_LATER;
	}
	else
		mrtp_peer_disconnect(peer);
}

// 发送一个不需要ack的断开连接的命令，随后重置该peer
void mrtp_peer_disconnect_now(MRtpPeer * peer) {
	MRtpProtocol command;

	if (peer->state == MRTP_PEER_STATE_DISCONNECTED)
		return;

	if (peer->state != MRTP_PEER_STATE_ZOMBIE && peer->state != MRTP_PEER_STATE_DISCONNECTING) {
		mrtp_peer_reset_queues(peer);

		command.header.command = MRTP_PROTOCOL_COMMAND_DISCONNECT;

		mrtp_peer_queue_outgoing_command(peer, &command, NULL, 0, 0);

		mrtp_host_flush(peer->host);
	}

	mrtp_peer_reset(peer);
}

MRtpPacket * mrtp_peer_receive(MRtpPeer * peer, mrtp_uint8 * channelID) {

	MRtpIncomingCommand * incomingCommand;
	MRtpPacket * packet;

	if (mrtp_list_empty(&peer->dispatchedCommands))
		return NULL;

	incomingCommand = (MRtpIncomingCommand *)mrtp_list_remove(mrtp_list_begin(&peer->dispatchedCommands));

	if (channelID != NULL)
		* channelID = channelIDs[incomingCommand->command.header.command & MRTP_PROTOCOL_COMMAND_MASK];

	packet = incomingCommand->packet;

	--packet->referenceCount;

	if (incomingCommand->fragments != NULL)
		mrtp_free(incomingCommand->fragments);

	mrtp_free(incomingCommand);

	peer->totalWaitingData -= packet->dataLength;

	return packet;
}

int mrtp_peer_send_redundancy_noack(MRtpPeer* peer, MRtpPacket* packet) {

	// if redundancyNoAckBuffers hasn't been initialized
	if (peer->redundancyNum == 0 || !peer->redundancyNoAckBuffers || peer->redundancyNum != peer->host->redundancyNum)
		mrtp_peer_reset_redundancy_noack_buffer(peer, peer->host->redundancyNum);

	MRtpChannel* channel = &peer->channels[MRTP_PROTOCOL_REDUNDANCY_NOACK_CHANNEL_NUM];
	MRtpProtocol command;
	size_t fragmentLength;

	fragmentLength = (peer->mtu - sizeof(MRtpProtocolHeader)) / peer->redundancyNum - sizeof(MRtpProtocolSendRedundancyFragementNoAck);

	if (packet->dataLength > fragmentLength) {

		mrtp_uint32 fragmentCount = (packet->dataLength + fragmentLength - 1) / fragmentLength;
		mrtp_uint32 fragmentNumber, fragmentOffset;
		mrtp_uint8 commandNumber;
		mrtp_uint16 startSequenceNumber;
		MRtpList fragments;
		MRtpOutgoingCommand * fragment;

		if (fragmentCount > MRTP_PROTOCOL_MAXIMUM_FRAGMENT_COUNT)
			return -1;

		commandNumber = MRTP_PROTOCOL_COMMAND_SEND_REDUNDANCY_FRAGEMENT_NO_ACK;
		startSequenceNumber = channel->outgoingSequenceNumber + 1;

		mrtp_list_clear(&fragments);

		for (fragmentNumber = 0, fragmentOffset = 0; fragmentOffset < packet->dataLength;
			++fragmentNumber, fragmentOffset += fragmentLength)
		{
			if (packet->dataLength - fragmentOffset < fragmentLength)
				fragmentLength = packet->dataLength - fragmentOffset;

			fragment = (MRtpOutgoingCommand *)mrtp_malloc(sizeof(MRtpOutgoingCommand));

			if (fragment == NULL) {
				while (!mrtp_list_empty(&fragments)) {
					fragment = (MRtpOutgoingCommand *)mrtp_list_remove(mrtp_list_begin(&fragments));

					mrtp_free(fragment);
				}
				return -1;
			}

			fragment->fragmentOffset = fragmentOffset;
			fragment->fragmentLength = fragmentLength;
			fragment->packet = packet;
			fragment->command.header.command = commandNumber;
			fragment->command.sendRedundancyFragementNoAck.startSequenceNumber = MRTP_HOST_TO_NET_16(startSequenceNumber);
			fragment->command.sendRedundancyFragementNoAck.dataLength = MRTP_HOST_TO_NET_16(fragmentLength);
			fragment->command.sendRedundancyFragementNoAck.fragmentCount = MRTP_HOST_TO_NET_32(fragmentCount);
			fragment->command.sendRedundancyFragementNoAck.fragmentNumber = MRTP_HOST_TO_NET_32(fragmentNumber);
			fragment->command.sendRedundancyFragementNoAck.totalLength = MRTP_HOST_TO_NET_32(packet->dataLength);
			fragment->command.sendRedundancyFragementNoAck.fragmentOffset = MRTP_HOST_TO_NET_32(fragmentOffset);

			mrtp_list_insert(mrtp_list_end(&fragments), fragment);
		}
		packet->referenceCount += fragmentNumber;

		while (!mrtp_list_empty(&fragments)) {

			fragment = (MRtpOutgoingCommand *)mrtp_list_remove(mrtp_list_begin(&fragments));

			mrtp_peer_setup_outgoing_command(peer, fragment);
		}

		return 0;

	}
	else {
		command.header.command = MRTP_PROTOCOL_COMMAND_SEND_REDUNDANCY_NO_ACK;
		command.sendRedundancyNoAck.datalength = MRTP_HOST_TO_NET_16(packet->dataLength);
		if (mrtp_peer_queue_outgoing_command(peer, &command, packet, 0, packet->dataLength) == NULL)
			return -1;
	}
	return 0;
}

int mrtp_peer_send_redundancy(MRtpPeer* peer, MRtpPacket* packet) {

	//// if redundancyBuffers hasn't been initialized
	//if (peer->redundancyNum == 0 || !peer->redundancyBuffers || peer->redundancyNum != peer->host->redundancyNum)
	//	mrtp_peer_reset_reduandancy_buffer(peer, peer->host->redundancyNum);

	MRtpChannel* channel = &peer->channels[MRTP_PROTOCOL_REDUNDANCY_CHANNEL_NUM];
	MRtpProtocol command;
	size_t fragmentLength;

	fragmentLength = peer->mtu - sizeof(MRtpProtocolSendRedundancyFragment) - sizeof(MRtpProtocolHeader);

	if (packet->dataLength > fragmentLength) {

		mrtp_uint32 fragmentCount = (packet->dataLength + fragmentLength - 1) / fragmentLength;
		mrtp_uint32 fragmentNumber, fragmentOffset;
		mrtp_uint8 commandNumber;
		mrtp_uint16 startSequenceNumber;
		MRtpList fragments;
		MRtpOutgoingCommand * fragment;

		if (fragmentCount > MRTP_PROTOCOL_MAXIMUM_FRAGMENT_COUNT)
			return -1;

		commandNumber = MRTP_PROTOCOL_COMMAND_SEND_REDUNDANCY_FRAGMENT | MRTP_PROTOCOL_COMMAND_FLAG_REDUNDANCY_ACKNOWLEDGE;
		startSequenceNumber = channel->outgoingSequenceNumber + 1;

		mrtp_list_clear(&fragments);

		for (fragmentNumber = 0, fragmentOffset = 0; fragmentOffset < packet->dataLength;
			++fragmentNumber, fragmentOffset += fragmentLength)
		{
			if (packet->dataLength - fragmentOffset < fragmentLength)
				fragmentLength = packet->dataLength - fragmentOffset;

			fragment = (MRtpOutgoingCommand *)mrtp_malloc(sizeof(MRtpOutgoingCommand));

			if (fragment == NULL) {
				while (!mrtp_list_empty(&fragments)) {
					fragment = (MRtpOutgoingCommand *)mrtp_list_remove(mrtp_list_begin(&fragments));
					mrtp_free(fragment);
				}
				return -1;
			}

			fragment->fragmentOffset = fragmentOffset;
			fragment->fragmentLength = fragmentLength;
			fragment->packet = packet;
			fragment->command.header.command = commandNumber;
			fragment->command.sendRedundancyFragment.startSequenceNumber = MRTP_HOST_TO_NET_16(startSequenceNumber);
			fragment->command.sendRedundancyFragment.dataLength = MRTP_HOST_TO_NET_16(fragmentLength);
			fragment->command.sendRedundancyFragment.fragmentCount = MRTP_HOST_TO_NET_32(fragmentCount);
			fragment->command.sendRedundancyFragment.fragmentNumber = MRTP_HOST_TO_NET_32(fragmentNumber);
			fragment->command.sendRedundancyFragment.totalLength = MRTP_HOST_TO_NET_32(packet->dataLength);
			fragment->command.sendRedundancyFragment.fragmentOffset = MRTP_HOST_TO_NET_32(fragmentOffset);

			mrtp_list_insert(mrtp_list_end(&fragments), fragment);
		}
		packet->referenceCount += fragmentNumber;

		while (!mrtp_list_empty(&fragments)) {

			fragment = (MRtpOutgoingCommand *)mrtp_list_remove(mrtp_list_begin(&fragments));

			mrtp_peer_setup_outgoing_command(peer, fragment);
		}

		return 0;

	}
	else {
		command.header.command = MRTP_PROTOCOL_COMMAND_SEND_REDUNDANCY | MRTP_PROTOCOL_COMMAND_FLAG_REDUNDANCY_ACKNOWLEDGE;
		command.sendRedundancy.dataLength = MRTP_HOST_TO_NET_16(packet->dataLength);
		if (mrtp_peer_queue_outgoing_command(peer, &command, packet, 0, packet->dataLength) == NULL)
			return -1;
	}

	return 0;

}

int mrtp_peer_send_reliable(MRtpPeer * peer, MRtpPacket * packet) {

	MRtpChannel * channel = &peer->channels[MRTP_PROTOCOL_RELIABLE_CHANNEL_NUM];
	MRtpProtocol command;
	size_t fragmentLength;

	fragmentLength = peer->mtu - sizeof(MRtpProtocolHeader) - sizeof(MRtpProtocolSendFragment);

	// 如果需要分片
	if (packet->dataLength > fragmentLength) {
		mrtp_uint32 fragmentCount = (packet->dataLength + fragmentLength - 1) / fragmentLength,
			fragmentNumber,
			fragmentOffset;
		mrtp_uint8 commandNumber;
		mrtp_uint16 startSequenceNumber;
		MRtpList fragments;
		MRtpOutgoingCommand * fragment;

		if (fragmentCount > MRTP_PROTOCOL_MAXIMUM_FRAGMENT_COUNT)
			return -1;

		commandNumber = MRTP_PROTOCOL_COMMAND_SEND_FRAGMENT | MRTP_PROTOCOL_COMMAND_FLAG_ACKNOWLEDGE;
		startSequenceNumber = channel->outgoingSequenceNumber + 1;

		mrtp_list_clear(&fragments);

		for (fragmentNumber = 0, fragmentOffset = 0; fragmentOffset < packet->dataLength;
			++fragmentNumber, fragmentOffset += fragmentLength)
		{
			if (packet->dataLength - fragmentOffset < fragmentLength)
				fragmentLength = packet->dataLength - fragmentOffset;

			fragment = (MRtpOutgoingCommand *)mrtp_malloc(sizeof(MRtpOutgoingCommand));
			if (fragment == NULL) {
				while (!mrtp_list_empty(&fragments)) {
					fragment = (MRtpOutgoingCommand *)mrtp_list_remove(mrtp_list_begin(&fragments));

					mrtp_free(fragment);
				}

				return -1;
			}

			fragment->fragmentOffset = fragmentOffset;
			fragment->fragmentLength = fragmentLength;
			fragment->packet = packet;
			fragment->command.header.command = commandNumber;
			fragment->command.sendFragment.startSequenceNumber = MRTP_HOST_TO_NET_16(startSequenceNumber);
			fragment->command.sendFragment.dataLength = MRTP_HOST_TO_NET_16(fragmentLength);
			fragment->command.sendFragment.fragmentCount = MRTP_HOST_TO_NET_32(fragmentCount);
			fragment->command.sendFragment.fragmentNumber = MRTP_HOST_TO_NET_32(fragmentNumber);
			fragment->command.sendFragment.totalLength = MRTP_HOST_TO_NET_32(packet->dataLength);
			fragment->command.sendFragment.fragmentOffset = MRTP_NET_TO_HOST_32(fragmentOffset);

			mrtp_list_insert(mrtp_list_end(&fragments), fragment);
		}

		packet->referenceCount += fragmentNumber;

		while (!mrtp_list_empty(&fragments)) {
			fragment = (MRtpOutgoingCommand *)mrtp_list_remove(mrtp_list_begin(&fragments));

			mrtp_peer_setup_outgoing_command(peer, fragment);
		}

	}
	else {

		command.header.command = MRTP_PROTOCOL_COMMAND_SEND_RELIABLE | MRTP_PROTOCOL_COMMAND_FLAG_ACKNOWLEDGE;
		command.sendReliable.dataLength = MRTP_HOST_TO_NET_16(packet->dataLength);

		if (mrtp_peer_queue_outgoing_command(peer, &command, packet, 0, packet->dataLength) == NULL)
			return -1;
	}

	return 0;
}

int mrtp_peer_send_unsequenced(MRtpPeer * peer, MRtpPacket * packet) {

	MRtpChannel * channel = &peer->channels[MRTP_PROTOCOL_UNSEQUENCED_CHANNEL_NUM];
	MRtpProtocol command;
	size_t fragmentLength;

	fragmentLength = peer->mtu - sizeof(MRtpProtocolHeader) - sizeof(MRtpProtocolSendUnsequencedFragment);

	if (packet->dataLength > fragmentLength) {

		mrtp_uint32 fragmentCount = (packet->dataLength + fragmentLength - 1) / fragmentLength,
			fragmentNumber,
			fragmentOffset;
		mrtp_uint8 commandNumber;
		mrtp_uint16 startSequenceNumber;
		MRtpList fragments;
		MRtpOutgoingCommand * fragment;

		if (fragmentCount > MRTP_PROTOCOL_MAXIMUM_FRAGMENT_COUNT)
			return -1;

		commandNumber = MRTP_PROTOCOL_COMMAND_SEND_UNSEQUENCED_FRAGMENT | MRTP_PROTOCOL_COMMAND_FLAG_UNSEQUENCED;
		startSequenceNumber = channel->outgoingSequenceNumber + 1;

		mrtp_list_clear(&fragments);

		for (fragmentNumber = 0, fragmentOffset = 0; fragmentOffset < packet->dataLength;
			++fragmentNumber, fragmentOffset += fragmentLength)
		{
			if (packet->dataLength - fragmentOffset < fragmentLength)
				fragmentLength = packet->dataLength - fragmentOffset;

			fragment = (MRtpOutgoingCommand *)mrtp_malloc(sizeof(MRtpOutgoingCommand));
			if (fragment == NULL) {

				while (!mrtp_list_empty(&fragments)) {

					fragment = (MRtpOutgoingCommand *)mrtp_list_remove(mrtp_list_begin(&fragments));
					mrtp_free(fragment);
				}

				return -1;
			}

			fragment->fragmentOffset = fragmentOffset;
			fragment->fragmentLength = fragmentLength;
			fragment->packet = packet;
			fragment->command.header.command = commandNumber;
			fragment->command.sendUnsequencedFragment.startSequenceNumber = MRTP_HOST_TO_NET_16(startSequenceNumber);
			fragment->command.sendUnsequencedFragment.dataLength = MRTP_HOST_TO_NET_16(fragmentLength);
			fragment->command.sendUnsequencedFragment.fragmentCount = MRTP_HOST_TO_NET_32(fragmentCount);
			fragment->command.sendUnsequencedFragment.fragmentNumber = MRTP_HOST_TO_NET_32(fragmentNumber);
			fragment->command.sendUnsequencedFragment.totalLength = MRTP_HOST_TO_NET_32(packet->dataLength);
			fragment->command.sendUnsequencedFragment.fragmentOffset = MRTP_NET_TO_HOST_32(fragmentOffset);

			mrtp_list_insert(mrtp_list_end(&fragments), fragment);
		}

		packet->referenceCount += fragmentNumber;

		while (!mrtp_list_empty(&fragments)) {
			fragment = (MRtpOutgoingCommand *)mrtp_list_remove(mrtp_list_begin(&fragments));

			mrtp_peer_setup_outgoing_command(peer, fragment);
		}

	}
	else {

		command.header.command = MRTP_PROTOCOL_COMMAND_SEND_UNSEQUENCED | MRTP_PROTOCOL_COMMAND_FLAG_UNSEQUENCED;
		command.sendUnsequenced.dataLength = MRTP_HOST_TO_NET_16(packet->dataLength);

		if (mrtp_peer_queue_outgoing_command(peer, &command, packet, 0, packet->dataLength) == NULL)
			return -1;
	}

	return 0;
}

int mrtp_peer_send(MRtpPeer *peer, MRtpPacket *packet) {

	if (peer->state != MRTP_PEER_STATE_CONNECTED || packet->dataLength > peer->host->maximumPacketSize)
		return -1;

	if (packet->flags & MRTP_PACKET_FLAG_RELIABLE) {
		return mrtp_peer_send_reliable(peer, packet);
	}
	else if (packet->flags & MRTP_PACKET_FLAG_REDUNDANCY) {
		return mrtp_peer_send_redundancy(peer, packet);
	}
	else if (packet->flags & MRTP_PACKET_FLAG_REDUNDANCY_NO_ACK) {
		return mrtp_peer_send_redundancy_noack(peer, packet);
	}
	else {
		return mrtp_peer_send_unsequenced(peer, packet);
	}

}

// move the continual command to dispatchCommand queue
void mrtp_peer_dispatch_incoming_reliable_commands(MRtpPeer * peer, MRtpChannel * channel) {

	MRtpListIterator currentCommand;

	for (currentCommand = mrtp_list_begin(&channel->incomingCommands);
		currentCommand != mrtp_list_end(&channel->incomingCommands);
		currentCommand = mrtp_list_next(currentCommand))
	{
		MRtpIncomingCommand * incomingCommand = (MRtpIncomingCommand *)currentCommand;

		//if fragment hasn't been received entirly or the sequence number is not contunual
		if (incomingCommand->fragmentsRemaining > 0 ||
			incomingCommand->sequenceNumber != (mrtp_uint16)(channel->incomingSequenceNumber + 1))
			break;

		channel->incomingSequenceNumber = incomingCommand->sequenceNumber;

		if (incomingCommand->fragmentCount > 0)
			channel->incomingSequenceNumber += incomingCommand->fragmentCount - 1;
	}

	if (currentCommand == mrtp_list_begin(&channel->incomingCommands))
		return;

	// move command from incomingCommand queue to dispatchedCommand queue
	mrtp_list_move(mrtp_list_end(&peer->dispatchedCommands), mrtp_list_begin(&channel->incomingCommands), mrtp_list_previous(currentCommand));

	if (!peer->needsDispatch) {
		mrtp_list_insert(mrtp_list_end(&peer->host->dispatchQueue), &peer->dispatchList);

		peer->needsDispatch = 1;
	}
}

void mrtp_peer_dispatch_incoming_redundancy_noack_commands(MRtpPeer * peer, MRtpChannel * channel) {

	MRtpListIterator currentCommand, startCommand, droppedCommand;

	currentCommand = startCommand = droppedCommand = mrtp_list_begin(&channel->incomingCommands);
	for (; currentCommand != mrtp_list_end(&channel->incomingCommands);
		currentCommand = mrtp_list_next(currentCommand))
	{
		MRtpIncomingCommand * incomingCommand = (MRtpIncomingCommand*)currentCommand;

		if (incomingCommand->fragmentsRemaining <= 0) {
			channel->incomingSequenceNumber = incomingCommand->sequenceNumber;
			continue;
		}

		if (startCommand != currentCommand) {
			mrtp_list_move(mrtp_list_end(&peer->dispatchedCommands), startCommand, mrtp_list_previous(currentCommand));

			if (!peer->needsDispatch) {
				mrtp_list_insert(mrtp_list_end(&peer->host->dispatchQueue), &peer->dispatchList);
				peer->needsDispatch = 1;
			}
			droppedCommand = currentCommand;
		}
		else if (droppedCommand != currentCommand) {
			droppedCommand = mrtp_list_previous(currentCommand);
		}
		startCommand = mrtp_list_next(currentCommand);
	}

	if (startCommand != currentCommand) {

		mrtp_list_move(mrtp_list_end(&peer->dispatchedCommands), startCommand, mrtp_list_previous(currentCommand));

		if (!peer->needsDispatch) {
			mrtp_list_insert(mrtp_list_end(&peer->host->dispatchQueue), &peer->dispatchList);

			peer->needsDispatch = 1;
		}

		droppedCommand = currentCommand;
	}

	mrtp_peer_remove_incoming_commands(&channel->incomingCommands,
		mrtp_list_begin(&channel->incomingCommands), droppedCommand);

}

void mrtp_peer_dispatch_incoming_redundancy_commands(MRtpPeer * peer, MRtpChannel * channel) {

	MRtpListIterator currentCommand;

	for (currentCommand = mrtp_list_begin(&channel->incomingCommands);
		currentCommand != mrtp_list_end(&channel->incomingCommands);
		currentCommand = mrtp_list_next(currentCommand))
	{
		MRtpIncomingCommand * incomingCommand = (MRtpIncomingCommand *)currentCommand;

		//if fragment hasn't been received entirly or the sequence number is not contunual
		if (incomingCommand->fragmentsRemaining > 0 ||
			incomingCommand->sequenceNumber != (mrtp_uint16)(channel->incomingSequenceNumber + 1))
			break;

		channel->incomingSequenceNumber = incomingCommand->sequenceNumber;

		if (incomingCommand->fragmentCount > 0)
			channel->incomingSequenceNumber += incomingCommand->fragmentCount - 1;
	}

	// the incomingCommand queue is empty
	if (currentCommand == mrtp_list_begin(&channel->incomingCommands))
		return;

	// move command from incomingCommand queue to dispatchedCommand queue
	mrtp_list_move(mrtp_list_end(&peer->dispatchedCommands), mrtp_list_begin(&channel->incomingCommands), mrtp_list_previous(currentCommand));

	if (!peer->needsDispatch) {
		mrtp_list_insert(mrtp_list_end(&peer->host->dispatchQueue), &peer->dispatchList);

		peer->needsDispatch = 1;
	}

}

void mrtp_peer_dispatch_incoming_unsequenced_commands(MRtpPeer * peer, MRtpChannel * channel) {

	MRtpListIterator droppedCommand, startCommand, currentCommand;

	for (droppedCommand = startCommand = currentCommand = mrtp_list_begin(&channel->incomingCommands);
		currentCommand != mrtp_list_end(&channel->incomingCommands);
		currentCommand = mrtp_list_next(currentCommand))
	{
		MRtpIncomingCommand * incomingCommand = (MRtpIncomingCommand *)currentCommand;

		if ((incomingCommand->command.header.command & MRTP_PROTOCOL_COMMAND_MASK) ==
			MRTP_PROTOCOL_COMMAND_SEND_UNSEQUENCED)
			continue;

		//if remaining <= 0, then dispatch
		if (incomingCommand->fragmentsRemaining <= 0) {
			channel->incomingSequenceNumber = incomingCommand->sequenceNumber;
			continue;
		}

		if (startCommand != currentCommand) {
			mrtp_list_move(mrtp_list_end(&peer->dispatchedCommands), startCommand, mrtp_list_previous(currentCommand));

			if (!peer->needsDispatch) {

				mrtp_list_insert(mrtp_list_end(&peer->host->dispatchQueue), &peer->dispatchList);

				peer->needsDispatch = 1;
			}

			droppedCommand = currentCommand;
		}
		else if (droppedCommand != currentCommand)
			droppedCommand = mrtp_list_previous(currentCommand);

		startCommand = mrtp_list_next(currentCommand);
	}

	if (startCommand != currentCommand) {

		mrtp_list_move(mrtp_list_end(&peer->dispatchedCommands), startCommand, mrtp_list_previous(currentCommand));

		if (!peer->needsDispatch) {
			mrtp_list_insert(mrtp_list_end(&peer->host->dispatchQueue), &peer->dispatchList);

			peer->needsDispatch = 1;
		}

		droppedCommand = currentCommand;
	}

	mrtp_peer_remove_incoming_commands(&channel->incomingCommands, mrtp_list_begin(&channel->incomingCommands), droppedCommand);
}

MRtpIncomingCommand *mrtp_peer_queue_incoming_command(MRtpPeer * peer, const MRtpProtocol * command,
	const void * data, size_t dataLength, mrtp_uint32 flags, mrtp_uint32 fragmentCount)
{
	static MRtpIncomingCommand dummyCommand;
	mrtp_uint8 channelID = channelIDs[command->header.command & MRTP_PROTOCOL_COMMAND_MASK];
	MRtpChannel * channel = &peer->channels[channelID];
	mrtp_uint32 sequenceNumber = 0;
	mrtp_uint16 commandWindow, currentWindow;
	MRtpIncomingCommand * incomingCommand;
	MRtpListIterator currentCommand;
	MRtpPacket * packet = NULL;

	if (peer->state == MRTP_PEER_STATE_DISCONNECT_LATER)
		goto discardCommand;

	sequenceNumber = command->header.sequenceNumber;
	commandWindow = sequenceNumber / MRTP_PEER_WINDOW_SIZE;
	currentWindow = channel->incomingSequenceNumber / MRTP_PEER_WINDOW_SIZE;

	if (sequenceNumber < channel->incomingSequenceNumber)
		commandWindow += MRTP_PEER_WINDOWS;

	if (commandWindow < currentWindow || commandWindow >= currentWindow + MRTP_PEER_FREE_WINDOWS - 1)
		goto discardCommand;

	switch (command->header.command & MRTP_PROTOCOL_COMMAND_MASK)
	{
	case MRTP_PROTOCOL_COMMAND_SEND_FRAGMENT:
	case MRTP_PROTOCOL_COMMAND_SEND_RELIABLE:
	case MRTP_PROTOCOL_COMMAND_SEND_REDUNDANCY_NO_ACK:
	case MRTP_PROTOCOL_COMMAND_SEND_REDUNDANCY_FRAGEMENT_NO_ACK:
	case MRTP_PROTOCOL_COMMAND_SEND_REDUNDANCY:
	case MRTP_PROTOCOL_COMMAND_SEND_REDUNDANCY_FRAGMENT:

		if (sequenceNumber == channel->incomingSequenceNumber)
			goto discardCommand;

		for (currentCommand = mrtp_list_previous(mrtp_list_end(&channel->incomingCommands));
			currentCommand != mrtp_list_end(&channel->incomingCommands);
			currentCommand = mrtp_list_previous(currentCommand))
		{
			incomingCommand = (MRtpIncomingCommand *)currentCommand;

			if (sequenceNumber >= channel->incomingSequenceNumber) {
				if (incomingCommand->sequenceNumber < channel->incomingSequenceNumber)
					continue;
			}
			else if (incomingCommand->sequenceNumber >= channel->incomingSequenceNumber)
				break;

			if (incomingCommand->sequenceNumber <= sequenceNumber) {
				if (incomingCommand->sequenceNumber < sequenceNumber)
					break;
				// if command already exists, then discard the command
				goto discardCommand;
			}
		}
		break;

	case MRTP_PROTOCOL_COMMAND_SEND_UNSEQUENCED:
	case MRTP_PROTOCOL_COMMAND_SEND_UNSEQUENCED_FRAGMENT:
		currentCommand = mrtp_list_end(&channel->incomingCommands);
		break;

	default:
		goto discardCommand;
	}

	if (peer->totalWaitingData >= peer->host->maximumWaitingData)
		goto notifyError;

	packet = mrtp_packet_create(data, dataLength, flags);
	if (packet == NULL)
		goto notifyError;

	incomingCommand = (MRtpIncomingCommand *)mrtp_malloc(sizeof(MRtpIncomingCommand));
	if (incomingCommand == NULL)
		goto notifyError;

	incomingCommand->sequenceNumber = command->header.sequenceNumber;
	incomingCommand->command = *command;
	incomingCommand->fragmentCount = fragmentCount;
	incomingCommand->fragmentsRemaining = fragmentCount;
	incomingCommand->packet = packet;
	incomingCommand->fragments = NULL;

	if (fragmentCount > 0) {

		//use fragments(byte map) to record the already received fragment
		if (fragmentCount <= MRTP_PROTOCOL_MAXIMUM_FRAGMENT_COUNT)
			incomingCommand->fragments = (mrtp_uint32 *)mrtp_malloc((fragmentCount + 31) / 32 * sizeof(mrtp_uint32));
		if (incomingCommand->fragments == NULL) {
			mrtp_free(incomingCommand);

			goto notifyError;
		}
		memset(incomingCommand->fragments, 0, (fragmentCount + 31) / 32 * sizeof(mrtp_uint32));
	}

	if (packet != NULL) {
		++packet->referenceCount;
		peer->totalWaitingData += packet->dataLength;
	}

	mrtp_list_insert(mrtp_list_next(currentCommand), incomingCommand);

	switch (command->header.command & MRTP_PROTOCOL_COMMAND_MASK)
	{
	case MRTP_PROTOCOL_COMMAND_SEND_FRAGMENT:
	case MRTP_PROTOCOL_COMMAND_SEND_RELIABLE:
		mrtp_peer_dispatch_incoming_reliable_commands(peer, channel);
		break;

	case MRTP_PROTOCOL_COMMAND_SEND_REDUNDANCY_NO_ACK:
	case MRTP_PROTOCOL_COMMAND_SEND_REDUNDANCY_FRAGEMENT_NO_ACK:
		mrtp_peer_dispatch_incoming_redundancy_noack_commands(peer, channel);
		break;

	case MRTP_PROTOCOL_COMMAND_SEND_REDUNDANCY:
	case MRTP_PROTOCOL_COMMAND_SEND_REDUNDANCY_FRAGMENT:
		// the priority of & is lower than == !!!!
		mrtp_peer_dispatch_incoming_redundancy_commands(peer, channel);
		break;

	case MRTP_PROTOCOL_COMMAND_SEND_UNSEQUENCED:
	case MRTP_PROTOCOL_COMMAND_SEND_UNSEQUENCED_FRAGMENT:
		mrtp_peer_dispatch_incoming_unsequenced_commands(peer, channel);
		break;

	default:
		break;
	}

	return incomingCommand;

discardCommand:
	if (fragmentCount > 0)
		goto notifyError;

	if (packet != NULL && packet->referenceCount == 0)
		mrtp_packet_destroy(packet);

	return &dummyCommand;

notifyError:
	if (packet != NULL && packet->referenceCount == 0)
		mrtp_packet_destroy(packet);

	return NULL;
}

void mrtp_peer_reset_redundancy_noack_buffer(MRtpPeer* peer, size_t redundancyNum) {

	if (redundancyNum > MRTP_PROTOCOL_MAXIMUM_REDUNDANCY_NUM)
		redundancyNum = MRTP_PROTOCOL_MAXIMUM_REDUNDANCY_NUM;
	else if (redundancyNum < MRTP_PROTOCOL_MINIMUM_REDUNDANCY_NUM)
		redundancyNum = MRTP_PROTOCOL_MINIMUM_REDUNDANCY_NUM;

	if (redundancyNum == peer->redundancyNum && !peer->redundancyNoAckBuffers) {
		peer->currentRedundancyNoAckBufferNum = 0;

		for (int i = 0; i < redundancyNum + 1; i++) {
			mrtp_protocol_remove_redundancy_buffer_commands(&peer->redundancyNoAckBuffers[i]);
			memset(&peer->redundancyNoAckBuffers[i], 0, sizeof(MRtpRedundancyNoAckBuffer));
			mrtp_list_clear(&peer->redundancyNoAckBuffers[i].sentCommands);
		}
	}
	else if (peer->redundancyNoAckBuffers != NULL) {
		for (int i = 0; i < redundancyNum + 1; i++) {
			mrtp_protocol_remove_redundancy_buffer_commands(&peer->redundancyNoAckBuffers[i]);
		}
		mrtp_free(peer->redundancyNoAckBuffers);
		peer->redundancyNum = redundancyNum;
		peer->redundancyNoAckBuffers = mrtp_malloc((peer->redundancyNum + 1) * sizeof(MRtpRedundancyNoAckBuffer));
		for (int i = 0; i < peer->redundancyNum + 1; i++) {
			memset(&peer->redundancyNoAckBuffers[i], 0, sizeof(MRtpRedundancyNoAckBuffer));
			mrtp_list_clear(&peer->redundancyNoAckBuffers[i].sentCommands);
		}
		peer->currentRedundancyNoAckBufferNum = 0;
	}
	else {
		peer->redundancyNum = redundancyNum;
		peer->redundancyNoAckBuffers = mrtp_malloc((peer->redundancyNum + 1) * sizeof(MRtpRedundancyNoAckBuffer));
		for (int i = 0; i < peer->redundancyNum + 1; i++) {
			memset(&peer->redundancyNoAckBuffers[i], 0, sizeof(MRtpRedundancyNoAckBuffer));
			mrtp_list_clear(&peer->redundancyNoAckBuffers[i].sentCommands);
		}
		peer->currentRedundancyNoAckBufferNum = 0;
	}
}

void mrtp_peer_throttle_configure(MRtpPeer * peer, mrtp_uint32 interval, mrtp_uint32 acceleration,
	mrtp_uint32 deceleration)
{
	MRtpProtocol command;

	peer->packetThrottleInterval = interval;
	peer->packetThrottleAcceleration = acceleration;
	peer->packetThrottleDeceleration = deceleration;

	command.header.command = MRTP_PROTOCOL_COMMAND_THROTTLE_CONFIGURE | MRTP_PROTOCOL_COMMAND_FLAG_ACKNOWLEDGE;

	command.throttleConfigure.packetThrottleInterval = MRTP_HOST_TO_NET_32(interval);
	command.throttleConfigure.packetThrottleAcceleration = MRTP_HOST_TO_NET_32(acceleration);
	command.throttleConfigure.packetThrottleDeceleration = MRTP_HOST_TO_NET_32(deceleration);

	mrtp_peer_queue_outgoing_command(peer, &command, NULL, 0, 0);
}
