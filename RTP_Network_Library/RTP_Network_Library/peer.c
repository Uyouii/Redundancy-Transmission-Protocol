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
		!(mrtp_list_empty(&peer->outgoingReliableCommands) && mrtp_list_empty(&peer->sentReliableCommands)))
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

int mrtp_peer_send_reliable(MRtpPeer * peer, MRtpPacket * packet) {

	if (peer->state != MRTP_PEER_STATE_CONNECTED || packet->dataLength > peer->host->maximumPacketSize)
		return -1;

	MRtpChannel * channel = &peer->channels[MRTP_PROTOCOL_RELIABLE_CHANNEL_NUM];
	MRtpProtocol command;
	size_t fragmentLength;

	fragmentLength = peer->mtu - sizeof(MRtpProtocolHeader) - sizeof(MRtpProtocolSendFragment);
	if (peer->host->checksum != NULL)
		fragmentLength -= sizeof(mrtp_uint32);

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
		startSequenceNumber = MRTP_HOST_TO_NET_16(channel->outgoingReliableSequenceNumber + 1);

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
			fragment->command.header.channelID = MRTP_PROTOCOL_RELIABLE_CHANNEL_NUM;
			fragment->command.sendFragment.startSequenceNumber = startSequenceNumber;
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
		command.header.channelID = MRTP_PROTOCOL_RELIABLE_CHANNEL_NUM;

		command.header.command = MRTP_PROTOCOL_COMMAND_SEND_RELIABLE | MRTP_PROTOCOL_COMMAND_FLAG_ACKNOWLEDGE;
		command.sendReliable.dataLength = MRTP_HOST_TO_NET_16(packet->dataLength);

		if (mrtp_peer_queue_outgoing_command(peer, &command, packet, 0, packet->dataLength) == NULL)
			return -1;
	}

	return 0;
}

MRtpIncomingCommand *mrtp_peer_queue_incoming_command(MRtpPeer * peer, const MRtpProtocol * command,
	const void * data, size_t dataLength, mrtp_uint32 flags, mrtp_uint32 fragmentCount)
{
	static MRtpIncomingCommand dummyCommand;

	MRtpChannel * channel = &peer->channels[command->header.channelID];
	mrtp_uint32 unreliableSequenceNumber = 0, reliableSequenceNumber = 0;
	mrtp_uint16 reliableWindow, currentWindow;
	MRtpIncomingCommand * incomingCommand;
	MRtpListIterator currentCommand;
	MRtpPacket * packet = NULL;

	if (peer->state == MRTP_PEER_STATE_DISCONNECT_LATER)
		goto discardCommand;

	reliableSequenceNumber = command->header.reliableSequenceNumber;
	reliableWindow = reliableSequenceNumber / MRTP_PEER_RELIABLE_WINDOW_SIZE;
	currentWindow = channel->incomingReliableSequenceNumber / MRTP_PEER_RELIABLE_WINDOW_SIZE;

	// 如果sequenceNumber 大小溢出了
	if (reliableSequenceNumber < channel->incomingReliableSequenceNumber)
		reliableWindow += MRTP_PEER_RELIABLE_WINDOWS;

	if (reliableWindow < currentWindow || reliableWindow >= currentWindow + MRTP_PEER_FREE_RELIABLE_WINDOWS - 1)
		goto discardCommand;

	switch (command->header.command & MRTP_PROTOCOL_COMMAND_MASK)
	{
	case MRTP_PROTOCOL_COMMAND_SEND_FRAGMENT:
	case MRTP_PROTOCOL_COMMAND_SEND_RELIABLE:

		if (reliableSequenceNumber == channel->incomingReliableSequenceNumber)
			goto discardCommand;

		for (currentCommand = mrtp_list_previous(mrtp_list_end(&channel->incomingReliableCommands));
			currentCommand != mrtp_list_end(&channel->incomingReliableCommands);
			currentCommand = mrtp_list_previous(currentCommand))
		{
			incomingCommand = (MRtpIncomingCommand *)currentCommand;

			// 大神级操作！！！句句精辟
			if (reliableSequenceNumber >= channel->incomingReliableSequenceNumber) {
				if (incomingCommand->reliableSequenceNumber < channel->incomingReliableSequenceNumber)
					continue;
			}
			else if (incomingCommand->reliableSequenceNumber >= channel->incomingReliableSequenceNumber)
				break;

			//找到把command插入到合适的位置
			if (incomingCommand->reliableSequenceNumber <= reliableSequenceNumber) {
				if (incomingCommand->reliableSequenceNumber < reliableSequenceNumber)
					break;
				// 如果相等
				goto discardCommand;
			}
		}
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

	incomingCommand->reliableSequenceNumber = command->header.reliableSequenceNumber;
	incomingCommand->command = *command;
	incomingCommand->fragmentCount = fragmentCount;
	incomingCommand->fragmentsRemaining = fragmentCount;
	incomingCommand->packet = packet;
	incomingCommand->fragments = NULL;

	if (fragmentCount > 0) {
		//分配fragments用来记录已经到的fragment
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

// 将已经收到的序号连续的command移动到dispatchCommand队列中
void mrtp_peer_dispatch_incoming_reliable_commands(MRtpPeer * peer, MRtpChannel * channel) {

	MRtpListIterator currentCommand;

	for (currentCommand = mrtp_list_begin(&channel->incomingReliableCommands);
		currentCommand != mrtp_list_end(&channel->incomingReliableCommands);
		currentCommand = mrtp_list_next(currentCommand))
	{
		MRtpIncomingCommand * incomingCommand = (MRtpIncomingCommand *)currentCommand;

		//如果fragement还有剩余或者序号不对就跳出
		if (incomingCommand->fragmentsRemaining > 0 ||
			incomingCommand->reliableSequenceNumber != (mrtp_uint16)(channel->incomingReliableSequenceNumber + 1))
			break;

		channel->incomingReliableSequenceNumber = incomingCommand->reliableSequenceNumber;
		//将fragement的序号加上
		if (incomingCommand->fragmentCount > 0)
			channel->incomingReliableSequenceNumber += incomingCommand->fragmentCount - 1;
	}

	if (currentCommand == mrtp_list_begin(&channel->incomingReliableCommands))
		return;

	//移动到dispatch队列
	mrtp_list_move(mrtp_list_end(&peer->dispatchedCommands), mrtp_list_begin(&channel->incomingReliableCommands), mrtp_list_previous(currentCommand));

	if (!peer->needsDispatch) {
		mrtp_list_insert(mrtp_list_end(&peer->host->dispatchQueue), &peer->dispatchList);

		peer->needsDispatch = 1;
	}
}