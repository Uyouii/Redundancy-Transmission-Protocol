#include <stdio.h>
#include <string.h>
#include "utility.h"
#include "time.h"
#include "mrtp.h"
#include <assert.h>



static size_t commandSizes[MRTP_PROTOCOL_COMMAND_COUNT] = {
	0,													// 0
	sizeof(MRtpProtocolAcknowledge),					// 1
	sizeof(MRtpProtocolConnect),						// 2
	sizeof(MRtpProtocolVerifyConnect),					// 3
	sizeof(MRtpProtocolDisconnect),						// 4
	sizeof(MRtpProtocolPing),							// 5
	sizeof(MRtpProtocolSend),							// 6
	sizeof(MRtpProtocolSendFragment),					// 7
	sizeof(MRtpProtocolBandwidthLimit),					// 8
	sizeof(MRtpProtocolThrottleConfigure),				// 9
	sizeof(MRtpProtocolSend),							// 10
	sizeof(MRtpProtocolSendFragment),					// 11
	sizeof(MRtpProtocolSend),							// 12
	sizeof(MRtpProtocolSendFragment),					// 13
	sizeof(MRtpProtocolRedundancyAcknowledge),			// 14
	sizeof(MRtpProtocolSendUnsequenced),				// 15
	sizeof(MRtpProtocolSendFragment),					// 16
};

mrtp_uint8 channelIDs[] = {
	0xFF,										// none
	0xFF,										// ack
	0xFF,										// connect
	0xFF,										// verify connect
	0xFF,										// disconnect
	0xFF,										// ping
	MRTP_PROTOCOL_RELIABLE_CHANNEL_NUM,			// send reliable
	MRTP_PROTOCOL_RELIABLE_CHANNEL_NUM,			// send fragement
	0xFF,										// bandwidth limit
	0xFF,										// throttleconfigure
	MRTP_PROTOCOL_REDUNDANCY_NOACK_CHANNEL_NUM,	// send redundancy no ack
	MRTP_PROTOCOL_REDUNDANCY_NOACK_CHANNEL_NUM, // send redundancy fragment no ack
	MRTP_PROTOCOL_REDUNDANCY_CHANNEL_NUM,		// send reliable redundancy 
	MRTP_PROTOCOL_REDUNDANCY_CHANNEL_NUM,		// send reliable redundancy fragment
	0xFF,										// redundancy ack
	MRTP_PROTOCOL_UNSEQUENCED_CHANNEL_NUM,		// send unsequenced
	MRTP_PROTOCOL_UNSEQUENCED_CHANNEL_NUM,		// sned unsequenced fragment
};

char* commandName[] = {
	"None",
	"Ack",
	"Connect",
	"VerifyConnect",
	"Disconnect",
	"Ping",
	"SendReliable",
	"SendFragement",
	"BandwidthLimit",
	"ThrottleConfigure",
	"SendRedundancyNoAck",
	"SendRedundancyFragementNoAck",
	"SendRedundancy",
	"SendRedundancyFragment",
	"RedundancyAck",
	"SendUnsequenced",
	"SendUnsequencedFragment",
};

size_t mrtp_protocol_command_size(mrtp_uint8 commandNumber) {
	return commandSizes[commandNumber & MRTP_PROTOCOL_COMMAND_MASK];
}

static void mrtp_protocol_change_state(MRtpHost * host, MRtpPeer * peer, MRtpPeerState state) {

	if (state == MRTP_PEER_STATE_CONNECTED || state == MRTP_PEER_STATE_DISCONNECT_LATER)
		mrtp_peer_on_connect(peer);
	else
		mrtp_peer_on_disconnect(peer);

	peer->state = state;
}

static void mrtp_protocol_dispatch_state(MRtpHost * host, MRtpPeer * peer, MRtpPeerState state) {

	mrtp_protocol_change_state(host, peer, state);

	if (!peer->needsDispatch) {
		mrtp_list_insert(mrtp_list_end(&host->dispatchQueue), &peer->dispatchList);

		peer->needsDispatch = 1;
	}
}

static void mrtp_protocol_notify_disconnect(MRtpHost * host, MRtpPeer * peer, MRtpEvent * event) {
	// need to reculate the bandwidth
	if (peer->state >= MRTP_PEER_STATE_CONNECTION_PENDING)
		host->recalculateBandwidthLimits = 1;

	// if the connection hasn't set up successfully, reset the peer directly
	if (peer->state != MRTP_PEER_STATE_CONNECTING && peer->state < MRTP_PEER_STATE_CONNECTION_SUCCEEDED)
		mrtp_peer_reset(peer);
	else if (event != NULL) {
		event->type = MRTP_EVENT_TYPE_DISCONNECT;
		event->peer = peer;

		mrtp_peer_reset(peer);
	}
	else {
		mrtp_protocol_dispatch_state(host, peer, MRTP_PEER_STATE_ZOMBIE);
	}
}

static void mrtp_protocol_notify_connect(MRtpHost * host, MRtpPeer * peer, MRtpEvent * event) {
	// need to reculate the bandwidth
	host->recalculateBandwidthLimits = 1;

	if (event != NULL) {
		mrtp_protocol_change_state(host, peer, MRTP_PEER_STATE_CONNECTED);

		event->type = MRTP_EVENT_TYPE_CONNECT;
		event->peer = peer;
	}
	else
		mrtp_protocol_dispatch_state(host, peer, peer->state == MRTP_PEER_STATE_CONNECTING ? MRTP_PEER_STATE_CONNECTION_SUCCEEDED : MRTP_PEER_STATE_CONNECTION_PENDING);
}

static int mrtp_protocol_check_timeouts(MRtpHost * host, MRtpPeer * peer, MRtpEvent * event) {

	MRtpOutgoingCommand * outgoingCommand;
	MRtpListIterator currentCommand, insertPosition;

	if (MRTP_TIME_GREATER_EQUAL(host->serviceTime, peer->nextTimeout)) {

		currentCommand = mrtp_list_begin(&peer->sentReliableCommands);
		insertPosition = mrtp_list_begin(&peer->outgoingReliableCommands);

		while (currentCommand != mrtp_list_end(&peer->sentReliableCommands)) {

			outgoingCommand = (MRtpOutgoingCommand *)currentCommand;

			currentCommand = mrtp_list_next(currentCommand);

			// if the outgoing command doexn't timeout
			if (MRTP_TIME_DIFFERENCE(host->serviceTime, outgoingCommand->sentTime) < outgoingCommand->roundTripTimeout)
				continue;

			// the peer time out after the last time receive acknowledge
			// the erliestTimeout will reset after receive acknowledge
			if (peer->earliestTimeout == 0 || MRTP_TIME_LESS(outgoingCommand->sentTime, peer->earliestTimeout))
				peer->earliestTimeout = outgoingCommand->sentTime;

			// judge whether a peer has disconnected
			if (MRTP_TIME_DIFFERENCE(host->serviceTime, peer->earliestTimeout) >= peer->timeoutMaximum ||
				(outgoingCommand->roundTripTimeout >= outgoingCommand->roundTripTimeoutLimit &&
					MRTP_TIME_DIFFERENCE(host->serviceTime, peer->earliestTimeout) >= peer->timeoutMinimum))
			{
				mrtp_protocol_notify_disconnect(host, peer, event);
				return 1;
			}

			// if a command is lost
			if (outgoingCommand->packet != NULL)
				peer->reliableDataInTransit -= outgoingCommand->fragmentLength;

			// change the [rto * 2] to [rto * 1.5]
			outgoingCommand->roundTripTimeout += outgoingCommand->roundTripTimeout / 2;


#if defined(PRINTLOG) && defined(PACKETLOSSDEBUG)
			fprintf(host->logFile, "[%s]: [%d] Loss! change rto to: [%d]\n",
				commandName[outgoingCommand->command.header.command & MRTP_PROTOCOL_COMMAND_MASK],
				MRTP_NET_TO_HOST_16(outgoingCommand->command.header.sequenceNumber),
				outgoingCommand->roundTripTimeout);
#endif // PACKETLOSSDEBUG
#ifdef PACKETLOSSDEBUG
			printf("[%s]: [%d] Loss! change rto to: [%d]\n",
				commandName[outgoingCommand->command.header.command & MRTP_PROTOCOL_COMMAND_MASK],
				MRTP_NET_TO_HOST_16(outgoingCommand->command.header.sequenceNumber),
				outgoingCommand->roundTripTimeout);
#endif

			mrtp_list_insert(insertPosition, mrtp_list_remove(&outgoingCommand->outgoingCommandList));

			if (currentCommand == mrtp_list_begin(&peer->sentReliableCommands) &&
				!mrtp_list_empty(&peer->sentReliableCommands))
			{
				outgoingCommand = (MRtpOutgoingCommand *)currentCommand;
				peer->nextTimeout = outgoingCommand->sentTime + outgoingCommand->roundTripTimeout;
			}
		}
	}

	return 0;
}

static int mrtp_protocol_check_redundancy_timeouts(MRtpHost * host, MRtpPeer * peer, MRtpEvent * event) {

	MRtpOutgoingCommand * outgoingCommand;
	MRtpListIterator currentCommand, insertPosition;

	if (MRTP_TIME_GREATER_EQUAL(host->serviceTime, peer->nextRedundancyTimeout)) {

		currentCommand = mrtp_list_begin(&peer->sentRedundancyLastTimeCommands);
		insertPosition = mrtp_list_begin(&peer->outgoingRedundancyCommands);

		while (currentCommand != mrtp_list_end(&peer->sentRedundancyLastTimeCommands)) {

			outgoingCommand = (MRtpOutgoingCommand *)currentCommand;

			currentCommand = mrtp_list_next(currentCommand);

			// if the outgoing command doexn't timeout
			if (MRTP_TIME_DIFFERENCE(host->serviceTime, outgoingCommand->sentTime) < outgoingCommand->roundTripTimeout)
				continue;

			// the peer time out after the last time receive acknowledge
			// the erliestTimeout will reset after receive acknowledge
			if (peer->earliestTimeout == 0 || MRTP_TIME_LESS(outgoingCommand->sentTime, peer->earliestTimeout))
				peer->earliestTimeout = outgoingCommand->sentTime;

			// judge whether a peer has disconnected
			if (peer->sentRedundancyLastTimeSize >= MRTP_PROTOCOL_MAXIMUM_REDUNDANCY_COMMAND_QUEUE_SiZE ||
				MRTP_TIME_DIFFERENCE(host->serviceTime, peer->earliestTimeout) >= peer->timeoutMaximum ||
				(outgoingCommand->roundTripTimeout >= outgoingCommand->roundTripTimeoutLimit &&
					MRTP_TIME_DIFFERENCE(host->serviceTime, peer->earliestTimeout) >= peer->timeoutMinimum))
			{
				mrtp_protocol_notify_disconnect(host, peer, event);
				return 1;
			}

			// if a command is lost
			if (outgoingCommand->packet != NULL)
				peer->reliableDataInTransit -= outgoingCommand->fragmentLength;

			// change the [rto * 2] to [rto * 1.5]
			outgoingCommand->roundTripTimeout += outgoingCommand->roundTripTimeout / 2;


#if defined(PRINTLOG) && defined(PACKETLOSSDEBUG)
			fprintf(host->logFile, "[%s]: [%d] Loss! change rto to: [%d]\n",
				commandName[outgoingCommand->command.header.command & MRTP_PROTOCOL_COMMAND_MASK],
				MRTP_NET_TO_HOST_16(outgoingCommand->command.header.sequenceNumber),
				outgoingCommand->roundTripTimeout);
#endif // PACKETLOSSDEBUG
#ifdef PACKETLOSSDEBUG
			printf("[%s]: [%d] Loss! change rto to: [%d]\n",
				commandName[outgoingCommand->command.header.command & MRTP_PROTOCOL_COMMAND_MASK],
				MRTP_NET_TO_HOST_16(outgoingCommand->command.header.sequenceNumber),
				outgoingCommand->roundTripTimeout);
#endif // DEBUG


			mrtp_list_insert(insertPosition, mrtp_list_remove(&outgoingCommand->outgoingCommandList));
			--peer->sentRedundancyLastTimeSize;

			if (currentCommand == mrtp_list_begin(&peer->sentRedundancyLastTimeCommands) &&
				!mrtp_list_empty(&peer->sentRedundancyLastTimeCommands))
			{
				outgoingCommand = (MRtpOutgoingCommand *)currentCommand;
				peer->nextTimeout = outgoingCommand->sentTime + outgoingCommand->roundTripTimeout;
			}
		}
	}

	return 0;
}

static void mrtp_protocol_remove_sent_redundancy_noack_commands(MRtpPeer * peer) {

	MRtpOutgoingCommand * outgoingCommand;

	while (!mrtp_list_empty(&peer->sentRedundancyNoAckCommands)) {

		outgoingCommand = (MRtpOutgoingCommand *)mrtp_list_front(&peer->sentRedundancyNoAckCommands);

		mrtp_list_remove(&outgoingCommand->outgoingCommandList);

		if (outgoingCommand->packet != NULL) {
			--outgoingCommand->packet->referenceCount;

			if (outgoingCommand->packet->referenceCount == 0) {

				outgoingCommand->packet->flags |= MRTP_PACKET_FLAG_SENT;
				mrtp_packet_destroy(outgoingCommand->packet);
			}
		}

		mrtp_free(outgoingCommand);
	}
}

void mrtp_protocol_remove_redundancy_buffer_commands(MRtpRedundancyNoAckBuffer* mrtpRedundancyBuffer) {

	MRtpOutgoingCommand* outgoingCommand;
	while (!mrtp_list_empty(&mrtpRedundancyBuffer->sentCommands)) {
		outgoingCommand = (MRtpOutgoingCommand *)mrtp_list_front(&mrtpRedundancyBuffer->sentCommands);
		mrtp_list_remove(&outgoingCommand->outgoingCommandList);

		if (outgoingCommand->packet != NULL) {
			--outgoingCommand->packet->referenceCount;
			if (outgoingCommand->packet->referenceCount == 0) {
				outgoingCommand->packet->flags |= MRTP_PACKET_FLAG_SENT;
				mrtp_packet_destroy(outgoingCommand->packet);
			}
		}
		mrtp_free(outgoingCommand);
	}
}

static void mrtp_protocol_send_acknowledgements(MRtpHost * host, MRtpPeer * peer) {

	MRtpProtocol *command = &host->commands[host->commandCount];
	MRtpBuffer *buffer = &host->buffers[host->bufferCount];
	MRtpAcknowledgement * acknowledgement;
	MRtpListIterator currentAcknowledgement;
	mrtp_uint16 reliableSequenceNumber;

	currentAcknowledgement = mrtp_list_begin(&peer->acknowledgements);

	while (currentAcknowledgement != mrtp_list_end(&peer->acknowledgements)) {

		if (command >= &host->commands[sizeof(host->commands) / sizeof(MRtpProtocol)] ||
			buffer >= &host->buffers[sizeof(host->buffers) / sizeof(MRtpBuffer)] ||
			peer->mtu - host->packetSize < sizeof(MRtpProtocolAcknowledge))
		{
			host->continueSending = 1;
			break;
		}

		acknowledgement = (MRtpAcknowledgement *)currentAcknowledgement;
		currentAcknowledgement = mrtp_list_next(currentAcknowledgement);

		buffer->data = command;
		buffer->dataLength = sizeof(MRtpProtocolAcknowledge);

		host->packetSize += buffer->dataLength;

		reliableSequenceNumber = MRTP_HOST_TO_NET_16(acknowledgement->command.header.sequenceNumber);

		command->header.command = MRTP_PROTOCOL_COMMAND_ACKNOWLEDGE;
		command->header.sequenceNumber = reliableSequenceNumber;
		command->acknowledge.receivedReliableSequenceNumber = reliableSequenceNumber;
		command->acknowledge.receivedSentTime = MRTP_HOST_TO_NET_16(acknowledgement->sentTime);
		command->acknowledge.channelID = channelIDs[acknowledgement->command.header.command & MRTP_PROTOCOL_COMMAND_MASK];
		if (command->acknowledge.channelID < peer->channelCount) {
			// maybe 
			if (peer->channels) {
				command->acknowledge.nextUnackSequenceNumber =
					MRTP_HOST_TO_NET_16(peer->channels[command->acknowledge.channelID].incomingSequenceNumber + 1);
			}
		}
		else {
			command->acknowledge.nextUnackSequenceNumber = 0;
		}

#if defined(PRINTLOG) && defined(SENDANDRECEIVE)
		fprintf(host->logFile, "add buffer [ack]: (%d) at channel: [%d]\n",
			MRTP_NET_TO_HOST_16(command->header.sequenceNumber),
			channelIDs[acknowledgement->command.header.command & MRTP_PROTOCOL_COMMAND_MASK]);
#endif // SENDANDRECEIVE
#if defined(SENDANDRECEIVE)
		printf("add buffer [ack]: (%d) at channel: [%d]\n",
			MRTP_NET_TO_HOST_16(command->header.sequenceNumber),
			channelIDs[acknowledgement->command.header.command & MRTP_PROTOCOL_COMMAND_MASK]);
#endif // SENDANDRECEIVE

		//if the command to ack is disconnect, change the peer state to ZOMBIE
		if ((acknowledgement->command.header.command & MRTP_PROTOCOL_COMMAND_MASK) == MRTP_PROTOCOL_COMMAND_DISCONNECT)
			mrtp_protocol_dispatch_state(host, peer, MRTP_PEER_STATE_ZOMBIE);

		mrtp_list_remove(&acknowledgement->acknowledgementList);
		mrtp_free(acknowledgement);

		++command;
		++buffer;
	}

	host->commandCount = command - host->commands;
	host->bufferCount = buffer - host->buffers;
}

static int mrtp_protocol_send_redundancy_acknowledgements(MRtpHost* host, MRtpPeer* peer) {

	MRtpProtocol *command = &host->commands[host->commandCount];
	MRtpBuffer *buffer = &host->buffers[host->bufferCount];
	MRtpAcknowledgement * acknowledgement;
	MRtpListIterator currentAcknowledgement;
	mrtp_uint16 sequenceNumber;

	mrtp_uint16 nextRedundancyNumber = peer->channels[MRTP_PROTOCOL_REDUNDANCY_CHANNEL_NUM].incomingSequenceNumber + 1;

	currentAcknowledgement = mrtp_list_begin(&peer->redundancyAcknowledgemets);

	while (currentAcknowledgement != mrtp_list_end(&peer->redundancyAcknowledgemets)) {

		acknowledgement = (MRtpAcknowledgement *)currentAcknowledgement;
		sequenceNumber = acknowledgement->command.header.sequenceNumber;
		currentAcknowledgement = mrtp_list_next(currentAcknowledgement);

		if (command >= &host->commands[sizeof(host->commands) / sizeof(MRtpProtocol)] ||
			buffer >= &host->buffers[sizeof(host->buffers) / sizeof(MRtpBuffer)] ||
			peer->mtu - host->packetSize < sizeof(MRtpProtocolRedundancyAcknowledge))
		{
			host->continueSending = 1;
			break;
		}


		buffer->data = command;
		buffer->dataLength = sizeof(MRtpProtocolRedundancyAcknowledge);

		host->packetSize += buffer->dataLength;

		command->header.command = MRTP_PROTOCOL_COMMAND_REDUNDANCY_ACKNOWLEDGE;
		command->header.sequenceNumber = MRTP_HOST_TO_NET_16(sequenceNumber);
		command->redundancyAcknowledge.receivedSequenceNumber = MRTP_HOST_TO_NET_16(sequenceNumber);
		command->redundancyAcknowledge.receivedSentTime = MRTP_HOST_TO_NET_16(acknowledgement->sentTime);
		// next sequence number to receive
		command->redundancyAcknowledge.nextUnackSequenceNumber = MRTP_HOST_TO_NET_16(nextRedundancyNumber);

#if defined(PRINTLOG) && defined(SENDANDRECEIVE)
		fprintf(host->logFile, "add buffer [redundancy ack]: (%d) nextunack: [%d] at channel: [%d]\n",
			MRTP_NET_TO_HOST_16(command->header.sequenceNumber),
			MRTP_NET_TO_HOST_16(command->redundancyAcknowledge.nextUnackSequenceNumber),
			channelIDs[acknowledgement->command.header.command & MRTP_PROTOCOL_COMMAND_MASK]);
#endif // SENDANDRECEIVE
#if defined(SENDANDRECEIVE)
		printf("add buffer [redundancy ack]: (%d) nextunack: [%d] at channel: [%d]\n",
			MRTP_NET_TO_HOST_16(command->header.sequenceNumber),
			MRTP_NET_TO_HOST_16(command->redundancyAcknowledge.nextUnackSequenceNumber),
			channelIDs[acknowledgement->command.header.command & MRTP_PROTOCOL_COMMAND_MASK]);
#endif // SENDANDRECEIVE

		++command;
		++buffer;


		mrtp_list_remove(&acknowledgement->acknowledgementList);
		mrtp_free(acknowledgement);

	}

	host->commandCount = command - host->commands;
	host->bufferCount = buffer - host->buffers;
}

static int mrtp_protocol_send_reliable_commands(MRtpHost * host, MRtpPeer * peer) {

	MRtpProtocol * command = &host->commands[host->commandCount];
	MRtpBuffer * buffer = &host->buffers[host->bufferCount];
	MRtpOutgoingCommand * outgoingCommand;
	MRtpListIterator currentCommand;
	MRtpChannel *channel;
	mrtp_uint16 commandWindow;
	size_t commandSize;
	mrtp_uint8 channelID;
	int windowExceeded = 0, windowWrap = 0, canPing = 1;

	currentCommand = mrtp_list_begin(&peer->outgoingReliableCommands);

	while (currentCommand != mrtp_list_end(&peer->outgoingReliableCommands)) {

		outgoingCommand = (MRtpOutgoingCommand *)currentCommand;
		channelID = channelIDs[outgoingCommand->command.header.command & MRTP_PROTOCOL_COMMAND_MASK];
		channel = channelID < peer->channelCount ? &peer->channels[channelID] : NULL;
		commandWindow = outgoingCommand->sequenceNumber / MRTP_PEER_WINDOW_SIZE;

		if (channel != NULL) {
			if (!windowWrap &&
				outgoingCommand->sendAttempts < 1 &&
				!(outgoingCommand->sequenceNumber % MRTP_PEER_WINDOW_SIZE) &&
				(channel->commandWindows[(commandWindow + MRTP_PEER_WINDOWS - 1) % MRTP_PEER_WINDOWS] >= MRTP_PEER_WINDOW_SIZE ||
					channel->usedWindows & ((((1 << MRTP_PEER_FREE_WINDOWS) - 1) << commandWindow) |
					(((1 << MRTP_PEER_FREE_WINDOWS) - 1) >> (MRTP_PEER_WINDOWS - commandWindow)))))
				windowWrap = 1;
#if defined(PRINTLOG) && defined(RELIABLEWINDOWDEBUG)
			fprintf(host->logFile, "channel: %d,realiableSeqNum: %d, reliableWindow: %d, number in window:[%d]\n",
				channelID, outgoingCommand->sequenceNumber, commandWindow,
				channel->commandWindows[(commandWindow + MRTP_PEER_WINDOWS - 1) % MRTP_PEER_WINDOWS]);
#endif
#if defined(RELIABLEWINDOWDEBUG)
			printf("channel: %d,realiableSeqNum: %d, reliableWindow: %d, number in window:[%d]\n",
				channelID, outgoingCommand->sequenceNumber, commandWindow,
				channel->commandWindows[(commandWindow + MRTP_PEER_WINDOWS - 1) % MRTP_PEER_WINDOWS]);
#endif
			if (windowWrap) {
				currentCommand = mrtp_list_next(currentCommand);
				continue;	//maybe there is some system command or other channel command need to send
			}
		}

		// if the data in transmit is lager than the window size
		if (outgoingCommand->packet != NULL) {
			if (!windowExceeded) {
				mrtp_uint32 windowSize = (peer->packetThrottle * peer->windowSize) / MRTP_PEER_PACKET_THROTTLE_SCALE;

				if (peer->reliableDataInTransit + outgoingCommand->fragmentLength > MRTP_MAX(windowSize, peer->mtu))
					windowExceeded = 1;
			}
			if (windowExceeded) {
				currentCommand = mrtp_list_next(currentCommand);

				continue;	//may be there is also some system command need to send
			}
		}

		canPing = 0;

		// if the data in host buffer is full
		commandSize = commandSizes[outgoingCommand->command.header.command & MRTP_PROTOCOL_COMMAND_MASK];
		if (command >= &host->commands[sizeof(host->commands) / sizeof(MRtpProtocol)] ||	//	the command buffer is full
			buffer + 1 >= &host->buffers[sizeof(host->buffers) / sizeof(MRtpBuffer)] ||	// host's send buffer is full
			peer->mtu - host->packetSize < commandSize ||	// the total send packetSize is larger than mtu
			(outgoingCommand->packet != NULL &&
			(mrtp_uint16)(peer->mtu - host->packetSize) < (mrtp_uint16)(commandSize + outgoingCommand->fragmentLength)))
		{
			host->continueSending = 1;

			break;
		}

		currentCommand = mrtp_list_next(currentCommand);

		if (channel != NULL && outgoingCommand->sendAttempts < 1) {
			channel->usedWindows |= 1 << commandWindow;
			++channel->commandWindows[commandWindow];
		}

		++outgoingCommand->sendAttempts;

		// set the rtt for the outgoing command
		if (outgoingCommand->roundTripTimeout == 0) {
			outgoingCommand->roundTripTimeout = peer->roundTripTime + 4 * peer->roundTripTimeVariance;
			outgoingCommand->roundTripTimeoutLimit = peer->timeoutLimit * outgoingCommand->roundTripTimeout;
		}

		if (mrtp_list_empty(&peer->sentReliableCommands))
			peer->nextTimeout = host->serviceTime + outgoingCommand->roundTripTimeout;

		// transter command from outgoing queue to sent queue
		mrtp_list_insert(mrtp_list_end(&peer->sentReliableCommands), mrtp_list_remove(&outgoingCommand->outgoingCommandList));

		outgoingCommand->sentTime = host->serviceTime;

		buffer->data = command;
		buffer->dataLength = commandSize;

		host->packetSize += buffer->dataLength;
		host->headerFlags |= MRTP_PROTOCOL_HEADER_FLAG_SENT_TIME;

		*command = outgoingCommand->command;

		// if there is a packet need to send, then put the packet to buffer too.
		if (outgoingCommand->packet != NULL) {
			++buffer;

			buffer->data = outgoingCommand->packet->data + outgoingCommand->fragmentOffset;
			buffer->dataLength = outgoingCommand->fragmentLength;

			host->packetSize += outgoingCommand->fragmentLength;

			peer->reliableDataInTransit += outgoingCommand->fragmentLength;
		}
#if defined(PRINTLOG) && defined(SENDANDRECEIVE)
		fprintf(host->logFile, "add buffer [%s]: (%d) at channel[%d]\n",
			commandName[outgoingCommand->command.header.command & MRTP_PROTOCOL_COMMAND_MASK],
			MRTP_NET_TO_HOST_16(outgoingCommand->command.header.sequenceNumber), channelID);
#endif // SENDANDRECEIVE
#if defined(SENDANDRECEIVE)
		printf("add buffer [%s]: (%d) at channel[%d]\n",
			commandName[outgoingCommand->command.header.command & MRTP_PROTOCOL_COMMAND_MASK],
			MRTP_NET_TO_HOST_16(outgoingCommand->command.header.sequenceNumber), channelID);
#endif // SENDANDRECEIVE
		++command;
		++buffer;
	}

	host->commandCount = command - host->commands;
	host->bufferCount = buffer - host->buffers;

	return canPing;
}

// use redundancy to send and receiver will not send acknowledges
static int mrtp_protocol_send_redundancy_noack_commands(MRtpHost * host, MRtpPeer * peer) {

	size_t redundancyNum = peer->currentRedundancyNoAckBufferNum;
	MRtpRedundancyNoAckBuffer* currentRedundancyBuffer = &peer->redundancyNoAckBuffers[redundancyNum];

	if (currentRedundancyBuffer->buffercount != 0) {
		return 0;	// if the command put in the buffer havn't sent
	}

	MRtpBuffer * buffer = &currentRedundancyBuffer->buffers[currentRedundancyBuffer->buffercount];
	MRtpOutgoingCommand * outgoingCommand;
	MRtpListIterator currentCommand;
	size_t commandSize;
	mrtp_uint32 redundancyMtu = (peer->mtu - sizeof(MRtpProtocolHeader) - 1) / peer->redundancyNum;

	currentCommand = mrtp_list_begin(&peer->outgoingRedundancyNoAckCommands);

	while (currentCommand != mrtp_list_end(&peer->outgoingRedundancyNoAckCommands)) {

		outgoingCommand = (MRtpOutgoingCommand *)currentCommand;
		commandSize = commandSizes[outgoingCommand->command.header.command & MRTP_PROTOCOL_COMMAND_MASK];

		if (buffer + 1 >= &currentRedundancyBuffer->buffers[(MRTP_BUFFER_MAXIMUM - 1) / peer->redundancyNum] ||
			redundancyMtu - currentRedundancyBuffer->packetSize < commandSize ||
			(outgoingCommand->packet != NULL &&
				redundancyMtu - currentRedundancyBuffer->packetSize < commandSize + outgoingCommand->fragmentLength))
		{
			host->continueSending = 1;
			break;
		}

		currentCommand = mrtp_list_next(currentCommand);

		buffer->data = &outgoingCommand->command;
		buffer->dataLength = commandSize;

		currentRedundancyBuffer->packetSize += buffer->dataLength;

		mrtp_list_insert(mrtp_list_end(&currentRedundancyBuffer->sentCommands), mrtp_list_remove(&outgoingCommand->outgoingCommandList));

		if (outgoingCommand->packet != NULL) {
			++buffer;
			buffer->data = outgoingCommand->packet->data + outgoingCommand->fragmentOffset;
			buffer->dataLength = outgoingCommand->fragmentLength;
			currentRedundancyBuffer->packetSize += buffer->dataLength;
		}

		++buffer;

#if defined(PRINTLOG) && defined(SENDANDRECEIVE)
		fprintf(host->logFile, "add buffer [%s]: (%d) at channel[%d]\n",
			commandName[outgoingCommand->command.header.command & MRTP_PROTOCOL_COMMAND_MASK],
			MRTP_NET_TO_HOST_16(outgoingCommand->command.header.sequenceNumber),
			channelIDs[outgoingCommand->command.header.command & MRTP_PROTOCOL_COMMAND_MASK]);
#endif // SENDANDRECEIVE
#if defined(SENDANDRECEIVE)
		printf("add buffer [%s]: (%d) at channel[%d]\n",
			commandName[outgoingCommand->command.header.command & MRTP_PROTOCOL_COMMAND_MASK],
			MRTP_NET_TO_HOST_16(outgoingCommand->command.header.sequenceNumber),
			channelIDs[outgoingCommand->command.header.command & MRTP_PROTOCOL_COMMAND_MASK]);
#endif // SENDANDRECEIVE

	}
	currentRedundancyBuffer->buffercount = buffer - currentRedundancyBuffer->buffers;

	if (peer->state == MRTP_PEER_STATE_DISCONNECT_LATER &&
		mrtp_list_empty(&peer->outgoingReliableCommands) &&
		mrtp_list_empty(&peer->outgoingUnsequencedCommands) &&
		mrtp_list_empty(&peer->sentReliableCommands) &&
		mrtp_list_empty(&peer->outgoingRedundancyCommands) &&
		mrtp_list_empty(&peer->sentRedundancyLastTimeCommands) &&
		mrtp_list_empty(&peer->outgoingRedundancyNoAckCommands))
		mrtp_peer_disconnect(peer);

	return 0;
}

static int mrtp_protocol_send_redundancy_commands(MRtpHost * host, MRtpPeer * peer, MRtpEvent* event) {

	MRtpProtocol * command = &host->commands[host->commandCount];
	MRtpBuffer * buffer = &host->buffers[host->bufferCount];
	MRtpOutgoingCommand * outgoingCommand;
	MRtpListIterator currentCommand;
	MRtpChannel *channel;
	mrtp_uint16 commandWindow;
	size_t commandSize;
	mrtp_uint8 channelID;
	int windowWrap = 0, windowExceeded = 0;

	// if too many command can't get ack from the peer
	if (peer->sentRedundancyLastTimeSize >= MRTP_PROTOCOL_MAXIMUM_REDUNDANCY_COMMAND_QUEUE_SiZE) {
		mrtp_protocol_notify_disconnect(host, peer, event);
		return 1;
	}

	// if hasn't resend the command after last receive
	// or need to send new command and maybe already has received the ack, but the ack command has lost
	if (!mrtp_list_empty(&peer->sentRedundancyLastTimeCommands) && (peer->sendRedundancyAfterReceive == FALSE ||
		(!mrtp_list_empty(&peer->outgoingRedundancyCommands) && (peer->redundancyLastSentTimeStamp == 0 ||
		(MRTP_TIME_DIFFERENCE(host->serviceTime, peer->redundancyLastSentTimeStamp) >= peer->roundTripTime) ||
			(peer->roundTripTime > 2 * peer->roundTripTimeVariance &&
				MRTP_TIME_DIFFERENCE(host->serviceTime, peer->redundancyLastSentTimeStamp) >=
				peer->roundTripTime - 2 * peer->roundTripTimeVariance)))))
	{
		currentCommand = mrtp_list_begin(&peer->sentRedundancyLastTimeCommands);

		while (currentCommand != mrtp_list_end(&peer->sentRedundancyLastTimeCommands)) {

			outgoingCommand = (MRtpOutgoingCommand *)currentCommand;
			channelID = channelIDs[outgoingCommand->command.header.command & MRTP_PROTOCOL_COMMAND_MASK];

			// maybe this command is put in the queue just now
			if (peer->roundTripTime > peer->roundTripTimeVariance &&
				MRTP_TIME_DIFFERENCE(host->serviceTime, outgoingCommand->sentTime) <
				peer->roundTripTime - 2 * peer->roundTripTimeVariance)
			{
				currentCommand = mrtp_list_next(currentCommand);
				continue;
			}

			commandSize = commandSizes[outgoingCommand->command.header.command & MRTP_PROTOCOL_COMMAND_MASK];

			if (command >= &host->commands[sizeof(host->commands) / sizeof(MRtpProtocol)] ||
				buffer + 1 >= &host->buffers[sizeof(host->buffers) / sizeof(MRtpBuffer)] ||
				peer->mtu - host->packetSize < commandSize ||
				(outgoingCommand->packet != NULL &&
				(mrtp_uint16)(peer->mtu - host->packetSize) < (mrtp_uint16)(commandSize + outgoingCommand->fragmentLength)))
			{
				host->continueSending = 1;

				break;
			}

			currentCommand = mrtp_list_next(currentCommand);

			++outgoingCommand->sendAttempts;

			// if retransmit too many times, then disconnect
			if (outgoingCommand->sendAttempts >= MRTP_PROTOCOL_MAXIMUM_REDUNDANCY_COMMAND_RETRANSMIT_TIME) {
				mrtp_protocol_notify_disconnect(host, peer, event);
				return 1;
			}

			// set the rtt for the outgoing command
			if (outgoingCommand->roundTripTimeout == 0) {
				outgoingCommand->roundTripTimeout = peer->roundTripTime + 4 * peer->roundTripTimeVariance;
				outgoingCommand->roundTripTimeoutLimit = peer->timeoutLimit * outgoingCommand->roundTripTimeout;
			}

			mrtp_list_insert(mrtp_list_end(&peer->sentRedundancyThisTimeCommands), mrtp_list_remove(&outgoingCommand->outgoingCommandList));
			--peer->sentRedundancyLastTimeSize;
			++peer->sentRedundancyThisTimeSize;

			outgoingCommand->sentTime = host->serviceTime;

			buffer->data = command;
			buffer->dataLength = commandSize;

			host->packetSize += buffer->dataLength;
			host->headerFlags |= MRTP_PROTOCOL_HEADER_FLAG_SENT_TIME;

			*command = outgoingCommand->command;

			if (outgoingCommand->packet != NULL) {
				++buffer;

				buffer->data = outgoingCommand->packet->data + outgoingCommand->fragmentOffset;
				buffer->dataLength = outgoingCommand->fragmentLength;

				host->packetSize += outgoingCommand->fragmentLength;

			}

#if defined(PRINTLOG) && defined(SENDANDRECEIVE)
			fprintf(host->logFile, "add buffer [%s]: (%d) at channel[%d]\n",
				commandName[outgoingCommand->command.header.command & MRTP_PROTOCOL_COMMAND_MASK],
				MRTP_NET_TO_HOST_16(outgoingCommand->command.header.sequenceNumber), channelID);
#endif // SENDANDRECEIVE
#if defined(SENDANDRECEIVE)
			printf("add buffer [%s]: (%d) at channel[%d]\n",
				commandName[outgoingCommand->command.header.command & MRTP_PROTOCOL_COMMAND_MASK],
				MRTP_NET_TO_HOST_16(outgoingCommand->command.header.sequenceNumber), channelID);
#endif // SENDANDRECEIVE
			++command;
			++buffer;
		}

		if (mrtp_list_empty(&peer->sentRedundancyLastTimeCommands)) {
			outgoingCommand = (MRtpOutgoingCommand *)mrtp_list_begin(&peer->sentRedundancyLastTimeCommands);
			peer->nextRedundancyTimeout = host->serviceTime + outgoingCommand->roundTripTimeout;
		}

		// after all command has send, then refresh the sent time stamp
		if (host->continueSending == 0) {
			peer->redundancyLastSentTimeStamp = host->serviceTime;
		}
	}

	currentCommand = mrtp_list_begin(&peer->outgoingRedundancyCommands);

	// if there is some commands need to send in outgoingRedundancyCommands queue
	while (currentCommand != mrtp_list_end(&peer->outgoingRedundancyCommands)) {

		outgoingCommand = (MRtpOutgoingCommand *)currentCommand;
		channelID = channelIDs[outgoingCommand->command.header.command & MRTP_PROTOCOL_COMMAND_MASK];
		channel = channelID < peer->channelCount ? &peer->channels[channelID] : NULL;
		commandWindow = outgoingCommand->sequenceNumber / MRTP_PEER_WINDOW_SIZE;

		if (!windowWrap &&
			outgoingCommand->sendAttempts < 1 &&
			!(outgoingCommand->sequenceNumber % MRTP_PEER_WINDOW_SIZE) &&
			(channel->commandWindows[(commandWindow + MRTP_PEER_WINDOWS - 1) % MRTP_PEER_WINDOWS] >= MRTP_PEER_WINDOW_SIZE ||
				channel->usedWindows & ((((1 << MRTP_PEER_FREE_WINDOWS) - 1) << commandWindow) |
				(((1 << MRTP_PEER_FREE_WINDOWS) - 1) >> (MRTP_PEER_WINDOWS - commandWindow)))))
			windowWrap = 1;

		if (windowWrap) {
			break;
		}

		// if the data in transmit is lager than the window size
		// to ensure that the data in transmit is not too mush
		if (outgoingCommand->packet != NULL) {
			if (!windowExceeded) {
				mrtp_uint32 windowSize = (peer->packetThrottle * peer->windowSize) / MRTP_PEER_PACKET_THROTTLE_SCALE;

				if (peer->reliableDataInTransit + outgoingCommand->fragmentLength > MRTP_MAX(windowSize, peer->mtu))
					windowExceeded = 1;
			}
			else break;
		}

		// if the data in host buffer is full
		commandSize = commandSizes[outgoingCommand->command.header.command & MRTP_PROTOCOL_COMMAND_MASK];

		if (command >= &host->commands[sizeof(host->commands) / sizeof(MRtpProtocol)] ||// the host->commands is full
			buffer + 1 >= &host->buffers[sizeof(host->buffers) / sizeof(MRtpBuffer)] ||	// the host->buffers is full
			peer->mtu - host->packetSize < commandSize ||	// total size is larger than mtu
			(outgoingCommand->packet != NULL &&
			(mrtp_uint16)(peer->mtu - host->packetSize) < (mrtp_uint16)(commandSize + outgoingCommand->fragmentLength)))
		{
			host->continueSending = 1;

			break;
		}

		currentCommand = mrtp_list_next(currentCommand);

		if (channel != NULL && outgoingCommand->sendAttempts < 1) {
			channel->usedWindows |= 1 << commandWindow;
			++channel->commandWindows[commandWindow];
		}

		++outgoingCommand->sendAttempts;

		// if retransmit too many times, then disconnect
		if (outgoingCommand->sendAttempts >= MRTP_PROTOCOL_MAXIMUM_REDUNDANCY_COMMAND_RETRANSMIT_TIME) {
			mrtp_protocol_notify_disconnect(host, peer, event);
			return 1;
		}

		// set the rtt for the outgoing command
		if (outgoingCommand->roundTripTimeout == 0) {
			outgoingCommand->roundTripTimeout = peer->roundTripTime + 4 * peer->roundTripTimeVariance;
			outgoingCommand->roundTripTimeoutLimit = peer->timeoutLimit * outgoingCommand->roundTripTimeout;
		}

		if (mrtp_list_empty(&peer->sentRedundancyLastTimeCommands) &&
			mrtp_list_empty(&peer->sentRedundancyThisTimeCommands))
		{
			peer->nextRedundancyTimeout = host->serviceTime + outgoingCommand->roundTripTimeout;
		}

		// move the command from outgoing queue to sent queue
		mrtp_list_insert(mrtp_list_end(&peer->sentRedundancyThisTimeCommands), mrtp_list_remove(&outgoingCommand->outgoingCommandList));
		++peer->sentRedundancyThisTimeSize;

		outgoingCommand->sentTime = host->serviceTime;

		// add the command to host->buffer
		buffer->data = command;
		buffer->dataLength = commandSize;

		host->packetSize += buffer->dataLength;
		host->headerFlags |= MRTP_PROTOCOL_HEADER_FLAG_SENT_TIME;

		*command = outgoingCommand->command;

		// if there is a packet in command, then put the packet to buffer
		if (outgoingCommand->packet != NULL) {
			++buffer;

			buffer->data = outgoingCommand->packet->data + outgoingCommand->fragmentOffset;
			buffer->dataLength = outgoingCommand->fragmentLength;

			host->packetSize += outgoingCommand->fragmentLength;

			peer->reliableDataInTransit += outgoingCommand->fragmentLength;
		}

#if defined(PRINTLOG) && defined(SENDANDRECEIVE)
		fprintf(host->logFile, "add buffer [%s]: (%d) at channel[%d]\n",
			commandName[outgoingCommand->command.header.command & MRTP_PROTOCOL_COMMAND_MASK],
			MRTP_NET_TO_HOST_16(outgoingCommand->command.header.sequenceNumber), channelID);
#endif // SENDANDRECEIVE
#if defined(SENDANDRECEIVE)
		printf("add buffer [%s]: (%d) at channel[%d]\n",
			commandName[outgoingCommand->command.header.command & MRTP_PROTOCOL_COMMAND_MASK],
			MRTP_NET_TO_HOST_16(outgoingCommand->command.header.sequenceNumber), channelID);
#endif // SENDANDRECEIVE

		++command;
		++buffer;
	}


	host->commandCount = command - host->commands;
	host->bufferCount = buffer - host->buffers;

	return 1;
}

static int mrtp_protocol_send_unsequenced_commands(MRtpHost * host, MRtpPeer * peer) {

	MRtpProtocol * command = &host->commands[host->commandCount];
	MRtpBuffer * buffer = &host->buffers[host->bufferCount];
	MRtpOutgoingCommand * outgoingCommand;
	MRtpListIterator currentCommand;

	currentCommand = mrtp_list_begin(&peer->outgoingUnsequencedCommands);

	while (currentCommand != mrtp_list_end(&peer->outgoingUnsequencedCommands)) {

		size_t commandSize;

		outgoingCommand = (MRtpOutgoingCommand *)currentCommand;

		commandSize = commandSizes[outgoingCommand->command.header.command & MRTP_PROTOCOL_COMMAND_MASK];

		if (command >= &host->commands[sizeof(host->commands) / sizeof(MRtpProtocol)] ||
			buffer + 1 >= &host->buffers[sizeof(host->buffers) / sizeof(MRtpBuffer)] ||
			peer->mtu - host->packetSize < commandSize ||
			(outgoingCommand->packet != NULL &&
				peer->mtu - host->packetSize < commandSize + outgoingCommand->fragmentLength))
		{
			host->continueSending = 1;

			break;
		}

		currentCommand = mrtp_list_next(currentCommand);


		// according to the network condition, randomly discard some unsequenced commands
		if (outgoingCommand->packet != NULL && outgoingCommand->fragmentOffset == 0) {

			peer->packetThrottleCounter += MRTP_PEER_PACKET_THROTTLE_COUNTER;
			peer->packetThrottleCounter %= MRTP_PEER_PACKET_THROTTLE_SCALE;

			if (peer->packetThrottleCounter > peer->packetThrottle) {

				--outgoingCommand->packet->referenceCount;
				if (outgoingCommand->packet->referenceCount == 0) {
					mrtp_packet_destroy(outgoingCommand->packet);
				}
				mrtp_list_remove(&outgoingCommand->outgoingCommandList);
				mrtp_free(outgoingCommand);

				continue;
			}
		}


		buffer->data = command;
		buffer->dataLength = commandSize;

		host->packetSize += buffer->dataLength;

		*command = outgoingCommand->command;

		mrtp_list_remove(&outgoingCommand->outgoingCommandList);

		if (outgoingCommand->packet != NULL) {

			++buffer;

			buffer->data = outgoingCommand->packet->data + outgoingCommand->fragmentOffset;
			buffer->dataLength = outgoingCommand->fragmentLength;

			host->packetSize += buffer->dataLength;

			// if there is a packet need to send, then put it in the sent queue
			mrtp_list_insert(mrtp_list_end(&peer->sentUnsequencedCommands), outgoingCommand);
		}
		else
			mrtp_free(outgoingCommand);

#if defined(PRINTLOG) && defined(SENDANDRECEIVE)
		if (outgoingCommand->command.header.command & MRTP_PROTOCOL_COMMAND_MASK == MRTP_PROTOCOL_COMMAND_SEND_UNSEQUENCED) {
			fprintf(host->logFile, "add buffer [%s]: (%d) at channel[%d] ",
				commandName[outgoingCommand->command.header.command & MRTP_PROTOCOL_COMMAND_MASK],
				MRTP_NET_TO_HOST_16(outgoingCommand->command.sendUnsequenced.unsequencedGroup),
				channelIDs[outgoingCommand->command.header.command & MRTP_PROTOCOL_COMMAND_MASK]);
		}
		else {
			fprintf(host->logFile, "add buffer [%s]: (%d) at channel[%d] ",
				commandName[outgoingCommand->command.header.command & MRTP_PROTOCOL_COMMAND_MASK],
				MRTP_NET_TO_HOST_16(outgoingCommand->command.header.sequenceNumber),
				channelIDs[outgoingCommand->command.header.command & MRTP_PROTOCOL_COMMAND_MASK]);
		}

		fprintf(host->logFile, "\n");
#endif // SENDANDRECEIVE
#if defined(SENDANDRECEIVE)
		if (outgoingCommand->command.header.command & MRTP_PROTOCOL_COMMAND_MASK == MRTP_PROTOCOL_COMMAND_SEND_UNSEQUENCED) {
			printf("add buffer [%s]: (%d) at channel[%d] ",
				commandName[outgoingCommand->command.header.command & MRTP_PROTOCOL_COMMAND_MASK],
				MRTP_NET_TO_HOST_16(outgoingCommand->command.sendUnsequenced.unsequencedGroup),
				channelIDs[outgoingCommand->command.header.command & MRTP_PROTOCOL_COMMAND_MASK]);
		}
		else {
			printf("add buffer [%s]: (%d) at channel[%d] ",
				commandName[outgoingCommand->command.header.command & MRTP_PROTOCOL_COMMAND_MASK],
				MRTP_NET_TO_HOST_16(outgoingCommand->command.header.sequenceNumber),
				channelIDs[outgoingCommand->command.header.command & MRTP_PROTOCOL_COMMAND_MASK]);
		}

		printf("\n");
#endif // SENDANDRECEIVE

		++command;
		++buffer;

	}
	host->commandCount = command - host->commands;
	host->bufferCount = buffer - host->buffers;

	if (peer->state == MRTP_PEER_STATE_DISCONNECT_LATER &&
		mrtp_list_empty(&peer->outgoingReliableCommands) &&
		mrtp_list_empty(&peer->outgoingUnsequencedCommands) &&
		mrtp_list_empty(&peer->sentReliableCommands) &&
		mrtp_list_empty(&peer->outgoingRedundancyCommands) &&
		mrtp_list_empty(&peer->sentRedundancyLastTimeCommands) &&
		mrtp_list_empty(&peer->outgoingRedundancyNoAckCommands))
		mrtp_peer_disconnect(peer);

}

static int mrtp_protocol_send_outgoing_commands(MRtpHost * host, MRtpEvent * event, int checkForTimeouts) {

	mrtp_uint8 headerData[sizeof(MRtpProtocolHeader) + sizeof(mrtp_uint32)];
	MRtpProtocolHeader *header = (MRtpProtocolHeader *)headerData;
	MRtpPeer * currentPeer;
	int sentLength;

	host->continueSending = 1;

	while (host->continueSending) {

		host->continueSending = 0;
		for (currentPeer = host->peers; currentPeer < &host->peers[host->peerCount]; ++currentPeer) {

			if (currentPeer->state == MRTP_PEER_STATE_DISCONNECTED || currentPeer->state == MRTP_PEER_STATE_ZOMBIE)
				continue;

			host->headerFlags = 0;
			host->commandCount = 0;
			host->bufferCount = 1;
			host->packetSize = sizeof(MRtpProtocolHeader);

			// first to hanle the acknowledgements
			if (!mrtp_list_empty(&currentPeer->acknowledgements))
				mrtp_protocol_send_acknowledgements(host, currentPeer);

			if (!mrtp_list_empty(&currentPeer->redundancyAcknowledgemets))
				mrtp_protocol_send_redundancy_acknowledgements(host, currentPeer);

			if (checkForTimeouts != 0 &&
				!mrtp_list_empty(&currentPeer->sentReliableCommands) &&
				MRTP_TIME_GREATER_EQUAL(host->serviceTime, currentPeer->nextTimeout) &&
				mrtp_protocol_check_timeouts(host, currentPeer, event) == 1)
			{
				if (event != NULL && event->type != MRTP_EVENT_TYPE_NONE)
					return 1;
				else
					continue;
			}

			if (checkForTimeouts != 0 && !mrtp_list_empty(&currentPeer->sentRedundancyLastTimeCommands) &&
				MRTP_TIME_GREATER_EQUAL(host->serviceTime, currentPeer->nextRedundancyTimeout) &&
				mrtp_protocol_check_redundancy_timeouts(host, currentPeer, event) == 1)
			{
				if (event != NULL && event->type != MRTP_EVENT_TYPE_NONE)
					return 1;
				else
					continue;
			}

			if ((mrtp_list_empty(&currentPeer->outgoingReliableCommands) ||
				mrtp_protocol_send_reliable_commands(host, currentPeer)) && // try to send data
				mrtp_list_empty(&currentPeer->sentReliableCommands) &&		// nothing to send
				MRTP_TIME_DIFFERENCE(host->serviceTime, currentPeer->lastReceiveTime) >= currentPeer->pingInterval && // the interval to last receive command > pingInterval
				currentPeer->mtu - host->packetSize >= sizeof(MRtpProtocolPing))	// there is still space for ping command
			{
				mrtp_peer_ping(currentPeer);
				mrtp_protocol_send_reliable_commands(host, currentPeer);
			}


			if (!mrtp_list_empty(&currentPeer->outgoingRedundancyCommands) ||
				(!mrtp_list_empty(&currentPeer->sentRedundancyLastTimeCommands) && currentPeer->sendRedundancyAfterReceive == FALSE))
			{
				if (mrtp_protocol_send_redundancy_commands(host, currentPeer, event) == 1) {
					if (event != NULL && event->type != MRTP_EVENT_TYPE_NONE)
						return 1;
				}
			}

			if (!mrtp_list_empty(&currentPeer->outgoingUnsequencedCommands))
				mrtp_protocol_send_unsequenced_commands(host, currentPeer);

			if (!mrtp_list_empty(&currentPeer->outgoingRedundancyNoAckCommands))
				mrtp_protocol_send_redundancy_noack_commands(host, currentPeer);

			MRtpRedundancyNoAckBuffer* currentRedundancyNoackBuffer =
				&currentPeer->redundancyNoAckBuffers[currentPeer->currentRedundancyNoAckBufferNum];


			// there are some data need to send
			if (host->bufferCount > 1 ||
				(currentRedundancyNoackBuffer && currentRedundancyNoackBuffer->buffercount > 0))
			{
				host->buffers->data = headerData;
				if (host->headerFlags & MRTP_PROTOCOL_HEADER_FLAG_SENT_TIME) {
					header->sentTime = MRTP_HOST_TO_NET_16(host->serviceTime & 0xFFFF);
					host->buffers->dataLength = sizeof(MRtpProtocolHeader);
				}
				else host->buffers->dataLength = (size_t) & ((MRtpProtocolHeader *)0)->sentTime;

				if (currentPeer->outgoingPeerID < MRTP_PROTOCOL_MAXIMUM_PEER_ID)
					host->headerFlags |= currentPeer->outgoingSessionID << MRTP_PROTOCOL_HEADER_SESSION_SHIFT;
				header->peerID = MRTP_HOST_TO_NET_16(currentPeer->outgoingPeerID | host->headerFlags);

				currentPeer->lastSendTime = host->serviceTime;

				MRtpBuffer* buffer = &host->buffers[host->bufferCount];

				// send the redundancy noack buffer data
				if (currentRedundancyNoackBuffer && currentRedundancyNoackBuffer->buffercount > 0) {

					int redundancyNoackBufferCount = 0;
					int redundancyNoackPacketSize = 0;

					for (int i = currentPeer->redundancyNum - 1; i >= 0; i--) {
						int num = (currentPeer->currentRedundancyNoAckBufferNum - i + currentPeer->redundancyNum + 1)
							% (currentPeer->redundancyNum + 1);
						if (currentPeer->redundancyNoAckBuffers[num].buffercount > 0) {
							redundancyNoackBufferCount += currentPeer->redundancyNoAckBuffers[num].buffercount;
							redundancyNoackPacketSize += currentPeer->redundancyNoAckBuffers[num].packetSize;
						}
					}

					if (host->bufferCount + redundancyNoackBufferCount <= sizeof(host->buffers) / sizeof(MRtpBuffer) &&
						host->mtu > host->packetSize + redundancyNoackPacketSize)
					{
						// copy the peer redundancy buffer to host buffer to send 
						for (int i = currentPeer->redundancyNum - 1; i >= 0; i--) {
							int num = (currentPeer->currentRedundancyNoAckBufferNum - i + currentPeer->redundancyNum + 1)
								% (currentPeer->redundancyNum + 1);
							if (currentPeer->redundancyNoAckBuffers[num].buffercount > 0) {
								for (int j = 0; j < currentPeer->redundancyNoAckBuffers[num].buffercount; j++) {
									buffer->data = currentPeer->redundancyNoAckBuffers[num].buffers[j].data;
									buffer->dataLength = currentPeer->redundancyNoAckBuffers[num].buffers[j].dataLength;

									++buffer;
									++host->bufferCount;
								}
							}
						}

						// clear current redundancy noack buffer
						currentPeer->currentRedundancyNoAckBufferNum = (currentPeer->currentRedundancyNoAckBufferNum + 1)
							% (currentPeer->redundancyNum + 1);
						currentRedundancyNoackBuffer = &currentPeer->redundancyNoAckBuffers[currentPeer->currentRedundancyNoAckBufferNum];
						if (currentRedundancyNoackBuffer->buffercount != 0) {
							mrtp_protocol_remove_redundancy_buffer_commands(currentRedundancyNoackBuffer);
							currentRedundancyNoackBuffer->buffercount = 0;
							currentRedundancyNoackBuffer->packetSize = 0;
						}
					}
					else {
						host->continueSending = 1;
					}

				}

				sentLength = mrtp_socket_send(host->socket, &currentPeer->address, host->buffers, host->bufferCount);
				assert(sentLength != 2);
#if defined(PRINTLOG) && defined(SENDANDRECEIVE)
				fprintf(host->logFile, "send: %d to peer: <%d> at {%d}\n",
					sentLength, currentPeer->incomingPeerID, host->serviceTime);
#endif // SENDANDRECEIVE
#ifdef SENDANDRECEIVE
				printf("send: %d to peer: <%d> at {%d}\n",
					sentLength, currentPeer->incomingPeerID, host->serviceTime);
#endif // SENDANDRECEIVE


				if (sentLength < 0)
					return -1;

				host->totalSentData += sentLength;
				host->totalSentPackets++;
			}
		}
	}

	for (currentPeer = host->peers; currentPeer < &host->peers[host->peerCount]; ++currentPeer) {

		if (currentPeer->state == MRTP_PEER_STATE_DISCONNECTED || currentPeer->state == MRTP_PEER_STATE_ZOMBIE)
			continue;

		if (!mrtp_list_empty(&currentPeer->sentRedundancyThisTimeCommands)) {
			mrtp_list_move(mrtp_list_end(&currentPeer->sentRedundancyLastTimeCommands),
				mrtp_list_begin(&currentPeer->sentRedundancyThisTimeCommands),
				mrtp_list_previous(mrtp_list_end(&currentPeer->sentRedundancyThisTimeCommands)));

			currentPeer->sentRedundancyLastTimeSize += currentPeer->sentRedundancyThisTimeSize;
			currentPeer->sentRedundancyThisTimeSize = 0;
		}

		currentPeer->sendRedundancyAfterReceive = TRUE;
	}

	return 0;
}

static int mrtp_protocol_delete_reliable_command(MRtpHost * host, MRtpPeer * peer, MRtpEvent * event,
	mrtp_uint16 reliableSequenceNumber, mrtp_uint8 channelID, MRtpOutgoingCommand * outgoingCommand)
{
	MRtpProtocolCommand commandNumber;

	if (channelID < peer->channelCount) {

		MRtpChannel * channel = &peer->channels[channelID];
		mrtp_uint16 reliableWindow = reliableSequenceNumber / MRTP_PEER_WINDOW_SIZE;

		if (channel->commandWindows[reliableWindow] > 0) {
			--channel->commandWindows[reliableWindow];
			if (!channel->commandWindows[reliableWindow])
				channel->usedWindows &= ~(1 << reliableWindow);
		}
	}

	commandNumber = (MRtpProtocolCommand)(outgoingCommand->command.header.command & MRTP_PROTOCOL_COMMAND_MASK);

	//remove this command from it's queue
	mrtp_list_remove(&outgoingCommand->outgoingCommandList);

	if (outgoingCommand->packet != NULL) {

		peer->reliableDataInTransit -= outgoingCommand->fragmentLength;
		--outgoingCommand->packet->referenceCount;

		if (outgoingCommand->packet->referenceCount == 0) {
			outgoingCommand->packet->flags |= MRTP_PACKET_FLAG_SENT;

			mrtp_packet_destroy(outgoingCommand->packet);
		}
	}

	mrtp_free(outgoingCommand);

	switch (peer->state)
	{
	case MRTP_PEER_STATE_ACKNOWLEDGING_CONNECT:
		if (commandNumber != MRTP_PROTOCOL_COMMAND_VERIFY_CONNECT)
			return -1;

		mrtp_protocol_notify_connect(host, peer, event);
		break;

	case MRTP_PEER_STATE_DISCONNECTING:
		if (commandNumber != MRTP_PROTOCOL_COMMAND_DISCONNECT)
			return -1;

		mrtp_protocol_notify_disconnect(host, peer, event);
		break;

	case MRTP_PEER_STATE_DISCONNECT_LATER:
		// after send all the outgoing data then disconnect
		if (mrtp_list_empty(&peer->outgoingReliableCommands) && mrtp_list_empty(&peer->sentReliableCommands) &&
			mrtp_list_empty(&peer->outgoingRedundancyCommands) && mrtp_list_empty(&peer->sentRedundancyLastTimeCommands))
			mrtp_peer_disconnect(peer);
		break;

	default:
		break;
	}

	if (mrtp_list_empty(&peer->sentReliableCommands))
		return 0;

	// set the next timeout stamp for peer
	outgoingCommand = (MRtpOutgoingCommand *)mrtp_list_front(&peer->sentReliableCommands);
	peer->nextTimeout = outgoingCommand->sentTime + outgoingCommand->roundTripTimeout;

	return 0;
}

static int mrtp_protocol_remove_sent_reliable_command(MRtpHost* host, MRtpPeer * peer, MRtpEvent * event,
	mrtp_uint16 reliableSequenceNumber, mrtp_uint32 nextUnackSequenceNumber, mrtp_uint8 channelID)
{
	MRtpOutgoingCommand * outgoingCommand = NULL;
	MRtpListIterator currentCommand, nextCommand;
	mrtp_uint32 currentSequenceNumber, waitNum;
	int wasSent = 1;
	int result = 0;
	BOOL hasFound = FALSE;

	for (currentCommand = mrtp_list_begin(&peer->sentReliableCommands);
		currentCommand != mrtp_list_end(&peer->sentReliableCommands);
		currentCommand = nextCommand)
	{
		outgoingCommand = (MRtpOutgoingCommand *)currentCommand;
		nextCommand = mrtp_list_next(currentCommand);
		currentSequenceNumber = outgoingCommand->sequenceNumber;

		if (channelIDs[outgoingCommand->command.header.command & MRTP_PROTOCOL_COMMAND_MASK] != channelID)
			continue;

		if (outgoingCommand->sequenceNumber == reliableSequenceNumber) {
			hasFound = TRUE;
			result |= mrtp_protocol_delete_reliable_command(host, peer, event, reliableSequenceNumber, channelID,
				outgoingCommand, wasSent);
		}
		// remove the already received command and the outgoing command is not disconnect
		else if (channelID < peer->channelCount && (currentSequenceNumber < nextUnackSequenceNumber ||
			(currentSequenceNumber - nextUnackSequenceNumber > MRTP_PROTOCOL_MAXIMUM_WINDOW_SIZE / 2 &&
				currentSequenceNumber < nextUnackSequenceNumber + MRTP_PROTOCOL_MAXIMUM_WINDOW_SIZE)))
		{
			result |= mrtp_protocol_delete_reliable_command(host, peer, event, reliableSequenceNumber, channelID,
				outgoingCommand);
		}
		else if (peer->host->openQuickRetransmit) {

			++outgoingCommand->fastAck;
			waitNum = 1;
			if ((outgoingCommand->command.header.command & MRTP_PROTOCOL_COMMAND_MASK) ==
				MRTP_PROTOCOL_COMMAND_SEND_FRAGMENT)
			{
				waitNum = MRTP_NET_TO_HOST_32(outgoingCommand->command.sendFragment.fragmentCount);
			}

			// quickly retransmit, if the command are jumped quickRetransmitNum, then quickly retransmit
			if (outgoingCommand->fastAck > peer->quickRetransmitNum + waitNum - 1) {

				outgoingCommand->fastAck = 0;
				mrtp_list_insert(mrtp_list_begin(&peer->outgoingReliableCommands), mrtp_list_remove(&outgoingCommand->outgoingCommandList));

#if defined(PRINTLOG) && defined(PACKETLOSSDEBUG)
				fprintf(host->logFile, "[%s]: [%d] Loss!\n",
					commandName[outgoingCommand->command.header.command & MRTP_PROTOCOL_COMMAND_MASK],
					MRTP_NET_TO_HOST_16(outgoingCommand->command.header.sequenceNumber));
#endif // PACKETLOSSDEBUG
#ifdef PACKETLOSSDEBUG
				printf("[%s]: [%d] Loss!\n",
					commandName[outgoingCommand->command.header.command & MRTP_PROTOCOL_COMMAND_MASK],
					MRTP_NET_TO_HOST_16(outgoingCommand->command.header.sequenceNumber));
#endif // PACKETLOSSDEBUG


				if (currentCommand == mrtp_list_begin(&peer->sentReliableCommands) &&
					!mrtp_list_empty(&peer->sentReliableCommands))
				{
					outgoingCommand = (MRtpOutgoingCommand *)currentCommand;
					peer->nextTimeout = outgoingCommand->sentTime + outgoingCommand->roundTripTimeout;
				}

			}
		}
	}

	// if can't find command in sentReliableCommands queue
	// then find the command in outgoingReliableCommands queue(maybe the command has been retransmited）
	if (hasFound == FALSE) {

		for (currentCommand = mrtp_list_begin(&peer->outgoingReliableCommands);
			currentCommand != mrtp_list_end(&peer->outgoingReliableCommands);
			currentCommand = mrtp_list_next(currentCommand))
		{
			outgoingCommand = (MRtpOutgoingCommand *)currentCommand;

			if (outgoingCommand->sendAttempts < 1) return MRTP_PROTOCOL_COMMAND_NONE;

			if (outgoingCommand->sequenceNumber == reliableSequenceNumber &&
				channelIDs[outgoingCommand->command.header.command & MRTP_PROTOCOL_COMMAND_MASK] == channelID)
			{
				wasSent = 0;
				result |= mrtp_protocol_delete_reliable_command(host, peer, event, reliableSequenceNumber, channelID,
					outgoingCommand, wasSent);
				break;
			}
		}

		if (currentCommand == mrtp_list_end(&peer->outgoingReliableCommands))
			return result;
	}


	return result;
}

// handle the acknowledge
static int mrtp_protocol_handle_acknowledge(MRtpHost * host, MRtpEvent * event,
	MRtpPeer * peer, const MRtpProtocol * command)
{
	mrtp_uint32 roundTripTime, receivedSentTime, receivedReliableSequenceNumber;
	mrtp_uint16 nextUnackSequenceNumber;
	MRtpProtocolCommand commandNumber;
	mrtp_uint8 channelID;

	//if peer is already disconnected, then do nothing
	if (peer->state == MRTP_PEER_STATE_DISCONNECTED || peer->state == MRTP_PEER_STATE_ZOMBIE)
		return 0;

	receivedSentTime = MRTP_NET_TO_HOST_16(command->acknowledge.receivedSentTime);
	receivedSentTime |= host->serviceTime & 0xFFFF0000;	// or operation with sent time and service time high bits
	if ((receivedSentTime & 0x8000) > (host->serviceTime & 0x8000))	// if the senttime has already overflowd
		receivedSentTime -= 0x10000;

	if (MRTP_TIME_LESS(host->serviceTime, receivedSentTime))
		return 0;

	peer->earliestTimeout = 0;

	roundTripTime = MRTP_TIME_DIFFERENCE(host->serviceTime, receivedSentTime);
	// adjust the throttle according to the roundTripTime
	mrtp_peer_throttle(peer, roundTripTime);

	peer->roundTripTimeVariance -= peer->roundTripTimeVariance / 4;

	if (roundTripTime >= peer->roundTripTime) {
		peer->roundTripTime += (roundTripTime - peer->roundTripTime) / 8;
		peer->roundTripTimeVariance += (roundTripTime - peer->roundTripTime) / 4;
	}
	else {
		peer->roundTripTime -= (peer->roundTripTime - roundTripTime) / 8;
		peer->roundTripTimeVariance += (peer->roundTripTime - roundTripTime) / 4;
	}

	if (peer->roundTripTime < peer->lowestRoundTripTime)
		peer->lowestRoundTripTime = peer->roundTripTime;

	if (peer->roundTripTimeVariance > peer->highestRoundTripTimeVariance)
		peer->highestRoundTripTimeVariance = peer->roundTripTimeVariance;

	// if hasn't do flow-control before or the interval of flow control is larger than throttle interval
	if (peer->packetThrottleEpoch == 0 ||
		MRTP_TIME_DIFFERENCE(host->serviceTime, peer->packetThrottleEpoch) >= peer->packetThrottleInterval)
	{
		peer->lastRoundTripTime = peer->lowestRoundTripTime;
		peer->lastRoundTripTimeVariance = peer->highestRoundTripTimeVariance;
		peer->lowestRoundTripTime = peer->roundTripTime;
		peer->highestRoundTripTimeVariance = peer->roundTripTimeVariance;
		peer->packetThrottleEpoch = host->serviceTime;
	}

	receivedReliableSequenceNumber = MRTP_NET_TO_HOST_16(command->acknowledge.receivedReliableSequenceNumber);
	nextUnackSequenceNumber = MRTP_NET_TO_HOST_16(command->acknowledge.nextUnackSequenceNumber);
	channelID = command->acknowledge.channelID;

	return mrtp_protocol_remove_sent_reliable_command(host, peer, event, receivedReliableSequenceNumber,
		nextUnackSequenceNumber, channelID);

}

static void mrtp_protocol_delete_redundancy_command(MRtpPeer * peer, MRtpOutgoingCommand* outgoingCommand,
	mrtp_uint16 sequenceNumber) {

	mrtp_uint8 channelID = channelIDs[outgoingCommand->command.header.command];

	if (channelID < peer->channelCount) {

		MRtpChannel * channel = &peer->channels[channelID];
		mrtp_uint16 window = sequenceNumber / MRTP_PEER_WINDOW_SIZE;

		if (channel->commandWindows[window] > 0) {
			--channel->commandWindows[window];
			if (!channel->commandWindows[window])
				channel->usedWindows &= ~(1 << window);
		}
	}

	mrtp_list_remove(&outgoingCommand->outgoingCommandList);

	if (outgoingCommand->packet != NULL) {

		peer->reliableDataInTransit -= outgoingCommand->fragmentLength;
		--outgoingCommand->packet->referenceCount;

		if (outgoingCommand->packet->referenceCount == 0) {
			outgoingCommand->packet->flags |= MRTP_PACKET_FLAG_SENT;

			mrtp_packet_destroy(outgoingCommand->packet);
		}
	}
	mrtp_free(outgoingCommand);

	if (peer->state == MRTP_PEER_STATE_DISCONNECT_LATER) {
		if (mrtp_list_empty(&peer->outgoingReliableCommands) && mrtp_list_empty(&peer->sentReliableCommands) &&
			mrtp_list_empty(&peer->outgoingRedundancyCommands) && mrtp_list_empty(&peer->sentRedundancyLastTimeCommands))
			mrtp_peer_disconnect(peer);
	}
}

// because all the retransmit commands are using the reliable
// so the commands in the sentRedundancyCommands are ascending
static void mrtp_protocol_remove_sent_redundancy_command(MRtpHost * host, MRtpPeer * peer,
	mrtp_uint16 sequenceNumber, mrtp_uint32 nextUnackSequenceNumber)
{
	MRtpOutgoingCommand * outgoingCommand = NULL;
	MRtpOutgoingCommand * alreadyReceivedCommand = NULL;
	MRtpListIterator currentCommand, nextCommand = NULL;
	MRtpProtocolCommand commandNumber;
	mrtp_uint32 currentSequenceNumber;
	mrtp_uint32 waitNum;

	if (mrtp_list_empty(&peer->sentRedundancyLastTimeCommands))
		return MRTP_PROTOCOL_COMMAND_NONE;

	for (currentCommand = mrtp_list_begin(&peer->sentRedundancyLastTimeCommands);
		currentCommand != mrtp_list_end(&peer->sentRedundancyLastTimeCommands);
		currentCommand = nextCommand)
	{
		outgoingCommand = (MRtpOutgoingCommand *)currentCommand;
		nextCommand = mrtp_list_next(currentCommand);
		currentSequenceNumber = outgoingCommand->sequenceNumber;

		if (currentSequenceNumber == sequenceNumber) {
			mrtp_protocol_delete_redundancy_command(peer, outgoingCommand, sequenceNumber);
			--peer->sentRedundancyLastTimeSize;
		}
		// if peer already receive the command
		else if (currentSequenceNumber < nextUnackSequenceNumber ||
			(currentSequenceNumber - nextUnackSequenceNumber > MRTP_PROTOCOL_MAXIMUM_WINDOW_SIZE / 2 &&
				currentSequenceNumber < nextUnackSequenceNumber + MRTP_PROTOCOL_MAXIMUM_WINDOW_SIZE))
		{
			mrtp_protocol_delete_redundancy_command(peer, outgoingCommand, sequenceNumber);
			--peer->sentRedundancyLastTimeSize;
		}

	}

	if (currentCommand == mrtp_list_end(&peer->sentRedundancyLastTimeCommands)) {

		for (currentCommand = mrtp_list_begin(&peer->sentRedundancyThisTimeCommands);
			currentCommand != mrtp_list_end(&peer->sentRedundancyThisTimeCommands);
			currentCommand = nextCommand)
		{
			outgoingCommand = (MRtpOutgoingCommand *)currentCommand;
			nextCommand = mrtp_list_next(currentCommand);
			currentSequenceNumber = outgoingCommand->sequenceNumber;

			if (currentSequenceNumber == sequenceNumber) {
				mrtp_protocol_delete_redundancy_command(peer, outgoingCommand, sequenceNumber);
				--peer->sentRedundancyThisTimeSize;
			}
			// if peer already receive the command
			else if (currentSequenceNumber < nextUnackSequenceNumber ||
				(currentSequenceNumber - nextUnackSequenceNumber > MRTP_PROTOCOL_MAXIMUM_WINDOW_SIZE / 2 &&
					currentSequenceNumber < nextUnackSequenceNumber + MRTP_PROTOCOL_MAXIMUM_WINDOW_SIZE))
			{
				mrtp_protocol_delete_redundancy_command(peer, outgoingCommand, sequenceNumber);
				--peer->sentRedundancyThisTimeSize;
			}

		}
	}

	// maybe has been retransmit
	if (currentCommand == mrtp_list_end(&peer->sentRedundancyThisTimeCommands)) {

		for (currentCommand = mrtp_list_begin(&peer->outgoingRedundancyCommands);
			currentCommand != mrtp_list_end(&peer->outgoingRedundancyCommands);
			currentCommand = mrtp_list_next(currentCommand))
		{
			outgoingCommand = (MRtpOutgoingCommand *)currentCommand;

			if (outgoingCommand->sendAttempts < 1) continue;

			if (outgoingCommand->sequenceNumber == sequenceNumber) {
				mrtp_protocol_delete_redundancy_command(peer, outgoingCommand, sequenceNumber);
			}
			else if (currentSequenceNumber < nextUnackSequenceNumber ||
				(currentSequenceNumber - nextUnackSequenceNumber > MRTP_PROTOCOL_MAXIMUM_WINDOW_SIZE / 2 &&
					currentSequenceNumber < nextUnackSequenceNumber + MRTP_PROTOCOL_MAXIMUM_WINDOW_SIZE))
			{
				mrtp_protocol_delete_redundancy_command(peer, outgoingCommand, sequenceNumber);
			}
		}

	}

	if (mrtp_list_empty(&peer->sentRedundancyLastTimeCommands)) {
		if (mrtp_list_empty(&peer->sentRedundancyThisTimeCommands)) {
			return;
		}
		else {
			outgoingCommand = (MRtpOutgoingCommand *)mrtp_list_begin(&peer->sentRedundancyThisTimeCommands);
		}
	}
	else {
		outgoingCommand = (MRtpOutgoingCommand *)mrtp_list_begin(&peer->sentRedundancyLastTimeCommands);
	}

	peer->nextRedundancyTimeout = outgoingCommand->sentTime + outgoingCommand->roundTripTimeout;

#if defined(PRINTLOG) && defined(PACKETLOSSDEBUG)
	fprintf(host->logFile, "peer nextRedundancyTimeout: %d host service time: %d\n",
		peer->nextRedundancyTimeout, peer->host->serviceTime);
#endif // PACKETLOSSDEBUG
#ifdef PACKETLOSSDEBUG
	printf("peer nextRedundancyTimeout: %d host service time: %d\n",
		peer->nextRedundancyTimeout, peer->host->serviceTime);
#endif // PACKETLOSSDEBUG

}

// handle the redundancy acknowledge
static int mrtp_protocol_handle_redundancy_acknowledge(MRtpHost * host, MRtpEvent * event,
	MRtpPeer * peer, const MRtpProtocol * command)
{
	mrtp_uint32 roundTripTime, receivedSentTime;
	mrtp_uint16 nextUnackSequenceNumber, receivedSequenceNumber;
	MRtpProtocolCommand commandNumber;

	//if peer is already disconnected, then do nothing
	if (peer->state == MRTP_PEER_STATE_DISCONNECTED || peer->state == MRTP_PEER_STATE_ZOMBIE)
		return 0;

	receivedSentTime = MRTP_NET_TO_HOST_16(command->redundancyAcknowledge.receivedSentTime);
	receivedSentTime |= host->serviceTime & 0xFFFF0000;	// or operation with sent time and service time high bits
	if ((receivedSentTime & 0x8000) > (host->serviceTime & 0x8000))	// if the senttime has already overflowd
		receivedSentTime -= 0x10000;

	if (MRTP_TIME_LESS(host->serviceTime, receivedSentTime))
		return 0;

	peer->earliestTimeout = 0;

	roundTripTime = MRTP_TIME_DIFFERENCE(host->serviceTime, receivedSentTime);
	mrtp_peer_throttle(peer, roundTripTime);


	// change the rtt and rtt_var smoothly
	peer->roundTripTimeVariance -= peer->roundTripTimeVariance / 4;

	if (roundTripTime >= peer->roundTripTime) {
		peer->roundTripTime += (roundTripTime - peer->roundTripTime) / 8;
		peer->roundTripTimeVariance += (roundTripTime - peer->roundTripTime) / 4;
	}
	else {
		peer->roundTripTime -= (peer->roundTripTime - roundTripTime) / 8;
		peer->roundTripTimeVariance += (peer->roundTripTime - roundTripTime) / 4;
	}

	if (peer->roundTripTime < peer->lowestRoundTripTime)
		peer->lowestRoundTripTime = peer->roundTripTime;

	if (peer->roundTripTimeVariance > peer->highestRoundTripTimeVariance)
		peer->highestRoundTripTimeVariance = peer->roundTripTimeVariance;

	// if hasn't do flow-control before or the interval of flow control is larger than throttle interval
	if (peer->packetThrottleEpoch == 0 ||
		MRTP_TIME_DIFFERENCE(host->serviceTime, peer->packetThrottleEpoch) >= peer->packetThrottleInterval)
	{
		peer->lastRoundTripTime = peer->lowestRoundTripTime;
		peer->lastRoundTripTimeVariance = peer->highestRoundTripTimeVariance;
		peer->lowestRoundTripTime = peer->roundTripTime;
		peer->highestRoundTripTimeVariance = peer->roundTripTimeVariance;
		peer->packetThrottleEpoch = host->serviceTime;
	}

	receivedSequenceNumber = MRTP_NET_TO_HOST_16(command->redundancyAcknowledge.receivedSequenceNumber);
	nextUnackSequenceNumber = MRTP_NET_TO_HOST_16(command->redundancyAcknowledge.nextUnackSequenceNumber);
	/*
	越界的情况先没考虑
	*/
	//if (receivedSequenceNumber >= nextUnackSequenceNumber - 1) {
	mrtp_protocol_remove_sent_redundancy_command(host, peer, receivedSequenceNumber, nextUnackSequenceNumber);
	//}

	return 0;
}

static MRtpPeer * mrtp_protocol_handle_connect(MRtpHost * host, MRtpProtocolHeader * header, MRtpProtocol * command)
{
	mrtp_uint8 incomingSessionID, outgoingSessionID;
	mrtp_uint32 mtu, windowSize;
	size_t duplicatePeers = 0;
	MRtpPeer * currentPeer, *peer = NULL;
	MRtpProtocol verifyCommand;

	for (currentPeer = host->peers; currentPeer < &host->peers[host->peerCount]; ++currentPeer) {
		// find the first disconnected location in peers
		if (currentPeer->state == MRTP_PEER_STATE_DISCONNECTED) {
			if (peer == NULL)
				peer = currentPeer;
		}
		else if (currentPeer->state != MRTP_PEER_STATE_CONNECTING &&
			currentPeer->address.host == host->receivedAddress.host)
		{
			if (currentPeer->address.port == host->receivedAddress.port &&
				currentPeer->connectID == command->connect.connectID)
				return NULL;
			++duplicatePeers;
		}
	}

	if (peer == NULL || duplicatePeers >= host->duplicatePeers)
		return NULL;

	peer->state = MRTP_PEER_STATE_ACKNOWLEDGING_CONNECT;
	peer->connectID = command->connect.connectID;
	peer->address = host->receivedAddress;
	peer->outgoingPeerID = MRTP_NET_TO_HOST_16(command->connect.outgoingPeerID);
	peer->incomingBandwidth = MRTP_NET_TO_HOST_32(command->connect.incomingBandwidth);
	peer->outgoingBandwidth = MRTP_NET_TO_HOST_32(command->connect.outgoingBandwidth);

	incomingSessionID = command->connect.incomingSessionID == 0xFF ? peer->outgoingSessionID : command->connect.incomingSessionID;
	incomingSessionID = (incomingSessionID + 1) & (MRTP_PROTOCOL_HEADER_SESSION_MASK >> MRTP_PROTOCOL_HEADER_SESSION_SHIFT);
	if (incomingSessionID == peer->outgoingSessionID)
		incomingSessionID = (incomingSessionID + 1) & (MRTP_PROTOCOL_HEADER_SESSION_MASK >> MRTP_PROTOCOL_HEADER_SESSION_SHIFT);
	peer->outgoingSessionID = incomingSessionID;

	outgoingSessionID = command->connect.outgoingSessionID == 0xFF ? peer->incomingSessionID : command->connect.outgoingSessionID;
	outgoingSessionID = (outgoingSessionID + 1) & (MRTP_PROTOCOL_HEADER_SESSION_MASK >> MRTP_PROTOCOL_HEADER_SESSION_SHIFT);
	if (outgoingSessionID == peer->incomingSessionID)
		outgoingSessionID = (outgoingSessionID + 1) & (MRTP_PROTOCOL_HEADER_SESSION_MASK >> MRTP_PROTOCOL_HEADER_SESSION_SHIFT);
	peer->incomingSessionID = outgoingSessionID;

	mtu = MRTP_NET_TO_HOST_32(command->connect.mtu);

	if (mtu < MRTP_PROTOCOL_MINIMUM_MTU)
		mtu = MRTP_PROTOCOL_MINIMUM_MTU;
	else if (mtu > MRTP_PROTOCOL_MAXIMUM_MTU)
		mtu = MRTP_PROTOCOL_MAXIMUM_MTU;

	peer->mtu = mtu;

	if (host->outgoingBandwidth == 0 && peer->incomingBandwidth == 0)
		peer->windowSize = MRTP_PROTOCOL_MAXIMUM_WINDOW_SIZE;
	else if (host->outgoingBandwidth == 0 || peer->incomingBandwidth == 0)
		peer->windowSize = (MRTP_MAX(host->outgoingBandwidth, peer->incomingBandwidth) /
			MRTP_PEER_WINDOW_SIZE_SCALE) * MRTP_PROTOCOL_MINIMUM_WINDOW_SIZE;
	else
		peer->windowSize = (MRTP_MIN(host->outgoingBandwidth, peer->incomingBandwidth) /
			MRTP_PEER_WINDOW_SIZE_SCALE) * MRTP_PROTOCOL_MINIMUM_WINDOW_SIZE;

	if (peer->windowSize < MRTP_PROTOCOL_MINIMUM_WINDOW_SIZE)
		peer->windowSize = MRTP_PROTOCOL_MINIMUM_WINDOW_SIZE;
	else if (peer->windowSize > MRTP_PROTOCOL_MAXIMUM_WINDOW_SIZE)
		peer->windowSize = MRTP_PROTOCOL_MAXIMUM_WINDOW_SIZE;

	if (host->incomingBandwidth == 0)
		windowSize = MRTP_PROTOCOL_MAXIMUM_WINDOW_SIZE;
	else
		windowSize = (host->incomingBandwidth / MRTP_PEER_WINDOW_SIZE_SCALE) *
		MRTP_PROTOCOL_MINIMUM_WINDOW_SIZE;

	if (windowSize > MRTP_NET_TO_HOST_32(command->connect.windowSize))
		windowSize = MRTP_NET_TO_HOST_32(command->connect.windowSize);

	if (windowSize < MRTP_PROTOCOL_MINIMUM_WINDOW_SIZE)
		windowSize = MRTP_PROTOCOL_MINIMUM_WINDOW_SIZE;
	else if (windowSize > MRTP_PROTOCOL_MAXIMUM_WINDOW_SIZE)
		windowSize = MRTP_PROTOCOL_MAXIMUM_WINDOW_SIZE;

	verifyCommand.header.command = MRTP_PROTOCOL_COMMAND_VERIFY_CONNECT;
	verifyCommand.header.flag = MRTP_PROTOCOL_COMMAND_FLAG_ACKNOWLEDGE;
	verifyCommand.verifyConnect.outgoingPeerID = MRTP_HOST_TO_NET_16(peer->incomingPeerID);
	verifyCommand.verifyConnect.incomingSessionID = incomingSessionID;
	verifyCommand.verifyConnect.outgoingSessionID = outgoingSessionID;
	verifyCommand.verifyConnect.mtu = MRTP_HOST_TO_NET_32(peer->mtu);
	verifyCommand.verifyConnect.windowSize = MRTP_HOST_TO_NET_32(windowSize);
	verifyCommand.verifyConnect.incomingBandwidth = MRTP_HOST_TO_NET_32(host->incomingBandwidth);
	verifyCommand.verifyConnect.outgoingBandwidth = MRTP_HOST_TO_NET_32(host->outgoingBandwidth);
	verifyCommand.verifyConnect.connectID = peer->connectID;

	// sent the verify connect comand
	mrtp_peer_queue_outgoing_command(peer, &verifyCommand, NULL, 0, 0);

	return peer;
}

static int mrtp_protocol_handle_verify_connect(MRtpHost * host, MRtpEvent * event,
	MRtpPeer * peer, const MRtpProtocol * command) {

	mrtp_uint32 mtu, windowSize;
	size_t channelCount;

	if (peer->state != MRTP_PEER_STATE_CONNECTING)
		return 0;

	channelCount = MRTP_PROTOCOL_CHANNEL_COUNT;

	if (command->verifyConnect.connectID != peer->connectID) {
		mrtp_protocol_dispatch_state(host, peer, MRTP_PEER_STATE_ZOMBIE);
		return -1;
	}

	// the sequence of connect is 1, and next unack sequence number is 2
	mrtp_protocol_remove_sent_reliable_command(host, peer, event, 1, 2, 0xFF);

	peer->outgoingPeerID = MRTP_NET_TO_HOST_16(command->verifyConnect.outgoingPeerID);
	peer->incomingSessionID = command->verifyConnect.incomingSessionID;
	peer->outgoingSessionID = command->verifyConnect.outgoingSessionID;

	mtu = MRTP_NET_TO_HOST_32(command->verifyConnect.mtu);

	if (mtu < MRTP_PROTOCOL_MINIMUM_MTU)
		mtu = MRTP_PROTOCOL_MINIMUM_MTU;
	else
		if (mtu > MRTP_PROTOCOL_MAXIMUM_MTU)
			mtu = MRTP_PROTOCOL_MAXIMUM_MTU;

	if (mtu < peer->mtu)
		peer->mtu = mtu;

	windowSize = MRTP_NET_TO_HOST_32(command->verifyConnect.windowSize);

	if (windowSize < MRTP_PROTOCOL_MINIMUM_WINDOW_SIZE)
		windowSize = MRTP_PROTOCOL_MINIMUM_WINDOW_SIZE;

	if (windowSize > MRTP_PROTOCOL_MAXIMUM_WINDOW_SIZE)
		windowSize = MRTP_PROTOCOL_MAXIMUM_WINDOW_SIZE;

	if (windowSize < peer->windowSize)
		peer->windowSize = windowSize;

	peer->incomingBandwidth = MRTP_NET_TO_HOST_32(command->verifyConnect.incomingBandwidth);
	peer->outgoingBandwidth = MRTP_NET_TO_HOST_32(command->verifyConnect.outgoingBandwidth);

	mrtp_protocol_notify_connect(host, peer, event);
	return 0;
}

static int mrtp_protocol_handle_disconnect(MRtpHost * host, MRtpPeer * peer, const MRtpProtocol * command) {

	if (peer->state == MRTP_PEER_STATE_DISCONNECTED || peer->state == MRTP_PEER_STATE_ZOMBIE
		|| peer->state == MRTP_PEER_STATE_ACKNOWLEDGING_DISCONNECT)
		return 0;

	// reset the peer queue
	mrtp_peer_reset_queues(peer);

	if (peer->state == MRTP_PEER_STATE_CONNECTION_SUCCEEDED || peer->state == MRTP_PEER_STATE_DISCONNECTING
		|| peer->state == MRTP_PEER_STATE_CONNECTING)
	{
		mrtp_protocol_dispatch_state(host, peer, MRTP_PEER_STATE_ZOMBIE);
	}
	else if (peer->state != MRTP_PEER_STATE_CONNECTED && peer->state != MRTP_PEER_STATE_DISCONNECT_LATER) {
		if (peer->state == MRTP_PEER_STATE_CONNECTION_PENDING)
			host->recalculateBandwidthLimits = 1;
		mrtp_peer_reset(peer);
	}
	else if (command->header.flag == MRTP_PROTOCOL_COMMAND_FLAG_ACKNOWLEDGE)
		mrtp_protocol_change_state(host, peer, MRTP_PEER_STATE_ACKNOWLEDGING_DISCONNECT);
	else
		mrtp_protocol_dispatch_state(host, peer, MRTP_PEER_STATE_ZOMBIE);

	return 0;
}

static int mrtp_protocol_handle_ping(MRtpHost * host, MRtpPeer * peer, const MRtpProtocol * command) {
	if (peer->state != MRTP_PEER_STATE_CONNECTED && peer->state != MRTP_PEER_STATE_DISCONNECT_LATER)
		return -1;

	return 0;
}

static int mrtp_protocol_handle_bandwidth_limit(MRtpHost * host, MRtpPeer * peer, const MRtpProtocol * command) {

	if (peer->state != MRTP_PEER_STATE_CONNECTED && peer->state != MRTP_PEER_STATE_DISCONNECT_LATER)
		return -1;

	if (peer->incomingBandwidth != 0)
		--host->bandwidthLimitedPeers;

	peer->incomingBandwidth = MRTP_NET_TO_HOST_32(command->bandwidthLimit.incomingBandwidth);
	peer->outgoingBandwidth = MRTP_NET_TO_HOST_32(command->bandwidthLimit.outgoingBandwidth);

	if (peer->incomingBandwidth != 0)
		++host->bandwidthLimitedPeers;

	if (peer->incomingBandwidth == 0 && host->outgoingBandwidth == 0)
		peer->windowSize = MRTP_PROTOCOL_MAXIMUM_WINDOW_SIZE;
	else if (peer->incomingBandwidth == 0 || host->outgoingBandwidth == 0)
		peer->windowSize = (MRTP_MAX(peer->incomingBandwidth, host->outgoingBandwidth) /
			MRTP_PEER_WINDOW_SIZE_SCALE) * MRTP_PROTOCOL_MINIMUM_WINDOW_SIZE;
	else
		peer->windowSize = (MRTP_MIN(peer->incomingBandwidth, host->outgoingBandwidth) /
			MRTP_PEER_WINDOW_SIZE_SCALE) * MRTP_PROTOCOL_MINIMUM_WINDOW_SIZE;

	if (peer->windowSize < MRTP_PROTOCOL_MINIMUM_WINDOW_SIZE)
		peer->windowSize = MRTP_PROTOCOL_MINIMUM_WINDOW_SIZE;
	else if (peer->windowSize > MRTP_PROTOCOL_MAXIMUM_WINDOW_SIZE)
		peer->windowSize = MRTP_PROTOCOL_MAXIMUM_WINDOW_SIZE;

	return 0;
}

static int mrtp_protocol_handle_send_reliable(MRtpHost * host, MRtpPeer * peer, const MRtpProtocol * command,
	mrtp_uint8 ** currentData)
{
	size_t dataLength;

	if (peer->state != MRTP_PEER_STATE_CONNECTED && peer->state != MRTP_PEER_STATE_DISCONNECT_LATER)
		return -1;

	dataLength = MRTP_NET_TO_HOST_16(command->send.dataLength);
	*currentData += dataLength;
	if (dataLength > host->maximumPacketSize ||
		*currentData < host->receivedData ||
		*currentData > & host->receivedData[host->receivedDataLength])
		return -1;

	if (mrtp_peer_queue_incoming_command(peer, command, (const mrtp_uint8 *)command + sizeof(MRtpProtocolSend),
		dataLength, MRTP_PACKET_FLAG_RELIABLE, 0) == NULL)
		return -1;

	return 0;
}

static int mrtp_protocol_handle_send_fragment(MRtpHost * host, MRtpPeer * peer,
	const MRtpProtocol * command, mrtp_uint8 ** currentData)
{
	mrtp_uint32 fragmentNumber,
		fragmentCount,
		fragmentOffset,
		fragmentLength,
		startSequenceNumber,
		totalLength;
	MRtpChannel * channel;
	mrtp_uint16 startWindow, currentWindow;
	MRtpListIterator currentCommand;
	MRtpIncomingCommand * startCommand = NULL;

	if (peer->state != MRTP_PEER_STATE_CONNECTED && peer->state != MRTP_PEER_STATE_DISCONNECT_LATER)
		return -1;

	fragmentLength = MRTP_NET_TO_HOST_16(command->sendFragment.dataLength);
	*currentData += fragmentLength;
	if (fragmentLength > host->maximumPacketSize ||
		*currentData < host->receivedData ||
		*currentData > & host->receivedData[host->receivedDataLength])
		return -1;

	mrtp_uint8 channelID = channelIDs[command->header.command & MRTP_PROTOCOL_COMMAND_MASK];

	channel = &peer->channels[channelID];
	startSequenceNumber = MRTP_NET_TO_HOST_16(command->sendFragment.startSequenceNumber);
	startWindow = startSequenceNumber / MRTP_PEER_WINDOW_SIZE;
	currentWindow = channel->incomingSequenceNumber / MRTP_PEER_WINDOW_SIZE;

	if (startSequenceNumber < channel->incomingSequenceNumber)
		startWindow += MRTP_PEER_WINDOWS;
	// if the received command isn't in window
	if (startWindow < currentWindow || startWindow >= currentWindow + MRTP_PEER_FREE_WINDOWS - 1)
		return 0;

	fragmentNumber = MRTP_NET_TO_HOST_32(command->sendFragment.fragmentNumber);
	fragmentCount = MRTP_NET_TO_HOST_32(command->sendFragment.fragmentCount);
	fragmentOffset = MRTP_NET_TO_HOST_32(command->sendFragment.fragmentOffset);
	totalLength = MRTP_NET_TO_HOST_32(command->sendFragment.totalLength);

	if (fragmentCount > MRTP_PROTOCOL_MAXIMUM_FRAGMENT_COUNT ||
		fragmentNumber >= fragmentCount ||
		totalLength > host->maximumPacketSize ||
		fragmentOffset >= totalLength ||
		fragmentLength > totalLength - fragmentOffset)
		return -1;

	// first try to find the start command in incomingReliableCommands queue
	for (currentCommand = mrtp_list_previous(mrtp_list_end(&channel->incomingCommands));
		currentCommand != mrtp_list_end(&channel->incomingCommands);
		currentCommand = mrtp_list_previous(currentCommand))
	{
		MRtpIncomingCommand * incomingCommand = (MRtpIncomingCommand *)currentCommand;

		if (startSequenceNumber >= channel->incomingSequenceNumber) {
			if (incomingCommand->sequenceNumber < channel->incomingSequenceNumber)
				continue;
		}
		else if (incomingCommand->sequenceNumber >= channel->incomingSequenceNumber)
			break;

		if (incomingCommand->sequenceNumber <= startSequenceNumber) {
			if (incomingCommand->sequenceNumber < startSequenceNumber)
				break;

			if ((incomingCommand->command.header.command & MRTP_PROTOCOL_COMMAND_MASK) != MRTP_PROTOCOL_COMMAND_SEND_FRAGMENT ||
				totalLength != incomingCommand->packet->dataLength ||
				fragmentCount != incomingCommand->fragmentCount)
				return -1;
			// find the startCommand
			startCommand = incomingCommand;
			break;
		}
	}

	if (startCommand == NULL) {
		MRtpProtocol hostCommand = *command;

		hostCommand.header.sequenceNumber = startSequenceNumber;

		startCommand = mrtp_peer_queue_incoming_command(peer, &hostCommand, NULL, totalLength,
			MRTP_PACKET_FLAG_RELIABLE, fragmentCount);
		if (startCommand == NULL)
			return -1;
	}

	// move all the packet data to one command with start command sequence number
	if ((startCommand->fragments[fragmentNumber / 32] & (1 << (fragmentNumber % 32))) == 0) {
		--startCommand->fragmentsRemaining;

		startCommand->fragments[fragmentNumber / 32] |= (1 << (fragmentNumber % 32));

		if (fragmentOffset + fragmentLength > startCommand->packet->dataLength)
			fragmentLength = startCommand->packet->dataLength - fragmentOffset;

		memcpy(startCommand->packet->data + fragmentOffset,
			(mrtp_uint8 *)command + sizeof(MRtpProtocolSendFragment),
			fragmentLength);

		// after all fragments have received, then dispatch
		if (startCommand->fragmentsRemaining <= 0)
			mrtp_peer_dispatch_incoming_reliable_commands(peer, channel);
	}

	return 0;
}

static int mrtp_protocol_handle_send_redundancy_noack(MRtpHost * host, MRtpPeer * peer,
	const MRtpProtocol * command, mrtp_uint8 ** currentData)
{
	size_t dataLength;
	if (peer->state != MRTP_PEER_STATE_CONNECTED && peer->state != MRTP_PEER_STATE_DISCONNECT_LATER)
		return -1;
	dataLength = MRTP_NET_TO_HOST_16(command->send.dataLength);

	*currentData += dataLength;
	if (dataLength > host->maximumPacketSize ||
		*currentData < host->receivedData ||
		*currentData > & host->receivedData[host->receivedDataLength])
		return -1;

	if (mrtp_peer_queue_incoming_command(peer, command, (const mrtp_uint8 *)command + sizeof(MRtpProtocolSend),
		dataLength, MRTP_PACKET_FLAG_REDUNDANCY_NO_ACK, 0) == NULL)
	{
		return -1;
	}

	return 0;

}

static int mrtp_protocol_handle_send_redundancy_fragment_noack(MRtpHost* host, MRtpPeer * peer,
	const MRtpProtocol * command, mrtp_uint8 ** currentData)
{
	mrtp_uint32 fragmentNumber, fragmentCount, fragmentOffset, fragmentLength;
	mrtp_uint32 sequenceNumber, startSequenceNumber;
	mrtp_uint32 totalLength;
	mrtp_uint16 startWindow, currentWindow;
	MRtpChannel * channel;
	MRtpListIterator currentCommand;
	MRtpIncomingCommand * startCommand = NULL;

	if (peer->state != MRTP_PEER_STATE_CONNECTED && peer->state != MRTP_PEER_STATE_DISCONNECT_LATER)
		return -1;

	fragmentLength = MRTP_NET_TO_HOST_16(command->sendFragment.dataLength);

	*currentData += fragmentLength;
	if (fragmentLength > host->maximumPacketSize || *currentData < host->receivedData ||
		*currentData > & host->receivedData[host->receivedDataLength])
		return -1;

	mrtp_uint8 channelID = channelIDs[command->header.command & MRTP_PROTOCOL_COMMAND_MASK];
	channel = &peer->channels[channelID];
	sequenceNumber = command->header.sequenceNumber;
	startSequenceNumber = MRTP_NET_TO_HOST_16(command->sendFragment.startSequenceNumber);
	startWindow = startSequenceNumber / MRTP_PEER_WINDOW_SIZE;

	currentWindow = channel->incomingSequenceNumber / MRTP_PEER_WINDOW_SIZE;
	if (startSequenceNumber < channel->incomingSequenceNumber)
		startWindow += MRTP_PEER_WINDOWS;
	if (startWindow < currentWindow || startWindow >= currentWindow + MRTP_PEER_FREE_WINDOWS - 1)
		return 0;

	fragmentNumber = MRTP_NET_TO_HOST_32(command->sendFragment.fragmentNumber);
	fragmentCount = MRTP_NET_TO_HOST_32(command->sendFragment.fragmentCount);
	fragmentOffset = MRTP_NET_TO_HOST_32(command->sendFragment.fragmentOffset);
	totalLength = MRTP_NET_TO_HOST_32(command->sendFragment.totalLength);

	if (fragmentCount > MRTP_PROTOCOL_MAXIMUM_FRAGMENT_COUNT || fragmentNumber >= fragmentCount ||
		totalLength > host->maximumPacketSize || fragmentOffset >= totalLength ||
		fragmentLength > totalLength - fragmentOffset)
	{
		return -1;
	}

	for (currentCommand = mrtp_list_previous(mrtp_list_end(&channel->incomingCommands));
		currentCommand != mrtp_list_end(&channel->incomingCommands);
		currentCommand = mrtp_list_previous(currentCommand))
	{
		MRtpIncomingCommand * incomingCommand = (MRtpIncomingCommand *)currentCommand;

		if (startSequenceNumber >= channel->incomingSequenceNumber) {
			if (incomingCommand->sequenceNumber < channel->incomingSequenceNumber)
				continue;
		}
		else if (incomingCommand->sequenceNumber >= channel->incomingSequenceNumber)
			break;

		if (incomingCommand->sequenceNumber <= startSequenceNumber) {

			if (incomingCommand->sequenceNumber < startSequenceNumber)
				break;

			if ((incomingCommand->command.header.command & MRTP_PROTOCOL_COMMAND_MASK) != MRTP_PROTOCOL_COMMAND_SEND_REDUNDANCY_FRAGEMENT_NO_ACK ||
				totalLength != incomingCommand->packet->dataLength ||
				fragmentCount != incomingCommand->fragmentCount)
				return -1;

			startCommand = incomingCommand;
			break;
		}
	}

	if (startCommand == NULL) {

		MRtpProtocol hostCommand = *command;
		hostCommand.header.sequenceNumber = startSequenceNumber;

		startCommand = mrtp_peer_queue_incoming_command(peer, &hostCommand, NULL, totalLength,
			MRTP_PACKET_FLAG_REDUNDANCY_NO_ACK, fragmentCount);

		// maybe the startCommand has already been dispatched
		if (startCommand == NULL)
			return 0;
	}

	if ((startCommand->fragments[fragmentNumber / 32] & (1 << (fragmentNumber % 32))) == 0) {

		--startCommand->fragmentsRemaining;

		startCommand->fragments[fragmentNumber / 32] |= (1 << (fragmentNumber % 32));

		if (fragmentOffset + fragmentLength > startCommand->packet->dataLength)
			fragmentLength = startCommand->packet->dataLength - fragmentOffset;

		memcpy(startCommand->packet->data + fragmentOffset,
			(mrtp_uint8 *)command + sizeof(MRtpProtocolSendFragment),
			fragmentLength);

		if (startCommand->fragmentsRemaining <= 0)
			mrtp_peer_dispatch_incoming_redundancy_noack_commands(peer, channel);
	}

	return 0;

}

static int mrtp_protocol_handle_send_redundancy(MRtpHost * host, MRtpPeer * peer,
	const MRtpProtocol * command, mrtp_uint8 ** currentData)
{
	size_t dataLength;
	if (peer->state != MRTP_PEER_STATE_CONNECTED && peer->state != MRTP_PEER_STATE_DISCONNECT_LATER)
		return -1;
	dataLength = MRTP_NET_TO_HOST_16(command->send.dataLength);

	*currentData += dataLength;
	if (dataLength > host->maximumPacketSize ||
		*currentData < host->receivedData ||
		*currentData > & host->receivedData[host->receivedDataLength])
		return -1;

	if (mrtp_peer_queue_incoming_command(peer, command, (const mrtp_uint8 *)command + sizeof(MRtpProtocolSend),
		dataLength, MRTP_PACKET_FLAG_REDUNDANCY, 0) == NULL)
	{
		return -1;
	}

	return 0;
}

static int mrtp_protocol_handle_send_redundancy_fragment(MRtpHost * host, MRtpPeer * peer,
	const MRtpProtocol * command, mrtp_uint8 ** currentData)
{
	mrtp_uint32 fragmentNumber, fragmentCount, fragmentOffset, fragmentLength;
	mrtp_uint32 sequenceNumber, startSequenceNumber;
	mrtp_uint32 totalLength;
	mrtp_uint16 startWindow, currentWindow;
	MRtpChannel * channel;
	MRtpListIterator currentCommand;
	MRtpIncomingCommand * startCommand = NULL;

	if (peer->state != MRTP_PEER_STATE_CONNECTED && peer->state != MRTP_PEER_STATE_DISCONNECT_LATER)
		return -1;

	fragmentLength = MRTP_NET_TO_HOST_16(command->sendFragment.dataLength);

	*currentData += fragmentLength;
	if (fragmentLength > host->maximumPacketSize || *currentData < host->receivedData ||
		*currentData > & host->receivedData[host->receivedDataLength])
		return -1;

	mrtp_uint8 channelID = channelIDs[command->header.command & MRTP_PROTOCOL_COMMAND_MASK];
	channel = &peer->channels[channelID];

	sequenceNumber = command->header.sequenceNumber;
	startSequenceNumber = MRTP_NET_TO_HOST_16(command->sendFragment.startSequenceNumber);
	startWindow = startSequenceNumber / MRTP_PEER_WINDOW_SIZE;

	currentWindow = channel->incomingSequenceNumber / MRTP_PEER_WINDOW_SIZE;
	if (startSequenceNumber < channel->incomingSequenceNumber)
		startWindow += MRTP_PEER_WINDOWS;
	if (startWindow < currentWindow || startWindow >= currentWindow + MRTP_PEER_FREE_WINDOWS - 1)
		return 0;

	fragmentNumber = MRTP_NET_TO_HOST_32(command->sendFragment.fragmentNumber);
	fragmentCount = MRTP_NET_TO_HOST_32(command->sendFragment.fragmentCount);
	fragmentOffset = MRTP_NET_TO_HOST_32(command->sendFragment.fragmentOffset);
	totalLength = MRTP_NET_TO_HOST_32(command->sendFragment.totalLength);

	if (fragmentCount > MRTP_PROTOCOL_MAXIMUM_FRAGMENT_COUNT || fragmentNumber >= fragmentCount ||
		totalLength > host->maximumPacketSize || fragmentOffset >= totalLength ||
		fragmentLength > totalLength - fragmentOffset)
	{
		return -1;
	}

	// first try to find the start command
	for (currentCommand = mrtp_list_previous(mrtp_list_end(&channel->incomingCommands));
		currentCommand != mrtp_list_end(&channel->incomingCommands);
		currentCommand = mrtp_list_previous(currentCommand))
	{
		MRtpIncomingCommand * incomingCommand = (MRtpIncomingCommand *)currentCommand;

		if (startSequenceNumber >= channel->incomingSequenceNumber) {
			if (incomingCommand->sequenceNumber < channel->incomingSequenceNumber)
				continue;
		}
		else if (incomingCommand->sequenceNumber >= channel->incomingSequenceNumber)
			break;

		if (incomingCommand->sequenceNumber <= startSequenceNumber) {

			if (incomingCommand->sequenceNumber < startSequenceNumber)
				break;

			if ((incomingCommand->command.header.command & MRTP_PROTOCOL_COMMAND_MASK) != MRTP_PROTOCOL_COMMAND_SEND_REDUNDANCY_FRAGMENT ||
				totalLength != incomingCommand->packet->dataLength ||
				fragmentCount != incomingCommand->fragmentCount)
				return -1;

			startCommand = incomingCommand;
			break;
		}
	}

	if (startCommand == NULL) {

		MRtpProtocol hostCommand = *command;
		hostCommand.header.sequenceNumber = startSequenceNumber;

		startCommand = mrtp_peer_queue_incoming_command(peer, &hostCommand, NULL, totalLength,
			MRTP_PACKET_FLAG_REDUNDANCY, fragmentCount);

		// maybe the startCommand has already been dispatched
		if (startCommand == NULL)
			return 0;
	}
	if ((startCommand->fragments[fragmentNumber / 32] & (1 << (fragmentNumber % 32))) == 0) {

		--startCommand->fragmentsRemaining;

		startCommand->fragments[fragmentNumber / 32] |= (1 << (fragmentNumber % 32));

		if (fragmentOffset + fragmentLength > startCommand->packet->dataLength)
			fragmentLength = startCommand->packet->dataLength - fragmentOffset;

		memcpy(startCommand->packet->data + fragmentOffset,
			(mrtp_uint8 *)command + sizeof(MRtpProtocolSendFragment),
			fragmentLength);

		if (startCommand->fragmentsRemaining <= 0)
			mrtp_peer_dispatch_incoming_redundancy_commands(peer, channel);
	}

	return 0;
}

static int mrtp_protocol_handle_send_unsequenced(MRtpHost * host, MRtpPeer * peer,
	const MRtpProtocol * command, mrtp_uint8 ** currentData)
{
	mrtp_uint32 unsequencedGroup, index;
	size_t dataLength;

	if (peer->state != MRTP_PEER_STATE_CONNECTED && peer->state != MRTP_PEER_STATE_DISCONNECT_LATER) {
		return -1;
	}

	dataLength = MRTP_NET_TO_HOST_16(command->sendUnsequenced.dataLength);
	*currentData += dataLength;

	if (dataLength > host->maximumPacketSize ||
		*currentData < host->receivedData ||
		*currentData > & host->receivedData[host->receivedDataLength])
		return -1;

	unsequencedGroup = MRTP_NET_TO_HOST_16(command->sendUnsequenced.unsequencedGroup);
	index = unsequencedGroup % MRTP_PEER_UNSEQUENCED_WINDOW_SIZE;

	if (unsequencedGroup < peer->incomingUnsequencedGroup)
		unsequencedGroup += 0x10000;

	if (unsequencedGroup >= (mrtp_uint32)peer->incomingUnsequencedGroup + MRTP_PEER_FREE_UNSEQUENCED_WINDOWS * MRTP_PEER_UNSEQUENCED_WINDOW_SIZE)
		return 0;

	unsequencedGroup &= 0xFFFF;

	if (unsequencedGroup - index != peer->incomingUnsequencedGroup) {
		peer->incomingUnsequencedGroup = unsequencedGroup - index;

		memset(peer->unsequencedWindow, 0, sizeof(peer->unsequencedWindow));
	}
	else if (peer->unsequencedWindow[index / 32] & (1 << (index % 32)))
		return 0;

	if (mrtp_peer_queue_incoming_command(peer, command, (const mrtp_uint8 *)command + sizeof(MRtpProtocolSendUnsequenced),
		dataLength, MRTP_PACKET_FLAG_UNSEQUENCED, 0) == NULL)
		return -1;

	peer->unsequencedWindow[index / 32] |= 1 << (index % 32);

	return 0;

}

static int mrtp_protocol_handle_send_unsequenced_fragment(MRtpHost * host, MRtpPeer * peer,
	const MRtpProtocol * command, mrtp_uint8 ** currentData)
{
	mrtp_uint32 fragmentNumber, fragmentCount, fragmentOffset, fragmentLength;
	mrtp_uint32 sequenceNumber, startSequenceNumber;
	mrtp_uint32 totalLength;
	mrtp_uint16 startWindow, currentWindow;
	MRtpChannel * channel;
	MRtpListIterator currentCommand;
	MRtpIncomingCommand * startCommand = NULL;

	if (peer->state != MRTP_PEER_STATE_CONNECTED && peer->state != MRTP_PEER_STATE_DISCONNECT_LATER)
		return -1;

	fragmentLength = MRTP_NET_TO_HOST_16(command->sendFragment.dataLength);

	*currentData += fragmentLength;
	if (fragmentLength > host->maximumPacketSize || *currentData < host->receivedData ||
		*currentData > & host->receivedData[host->receivedDataLength])
		return -1;

	mrtp_uint8 channelID = channelIDs[command->header.command & MRTP_PROTOCOL_COMMAND_MASK];
	channel = &peer->channels[channelID];
	sequenceNumber = command->header.sequenceNumber;
	startSequenceNumber = MRTP_NET_TO_HOST_16(command->sendFragment.startSequenceNumber);
	startWindow = startSequenceNumber / MRTP_PEER_WINDOW_SIZE;

	currentWindow = channel->incomingSequenceNumber / MRTP_PEER_WINDOW_SIZE;
	if (startSequenceNumber < channel->incomingSequenceNumber)
		startWindow += MRTP_PEER_WINDOWS;
	if (startWindow < currentWindow || startWindow >= currentWindow + MRTP_PEER_FREE_WINDOWS - 1)
		return 0;

	fragmentNumber = MRTP_NET_TO_HOST_32(command->sendFragment.fragmentNumber);
	fragmentCount = MRTP_NET_TO_HOST_32(command->sendFragment.fragmentCount);
	fragmentOffset = MRTP_NET_TO_HOST_32(command->sendFragment.fragmentOffset);
	totalLength = MRTP_NET_TO_HOST_32(command->sendFragment.totalLength);

	if (fragmentCount > MRTP_PROTOCOL_MAXIMUM_FRAGMENT_COUNT || fragmentNumber >= fragmentCount ||
		totalLength > host->maximumPacketSize || fragmentOffset >= totalLength ||
		fragmentLength > totalLength - fragmentOffset)
	{
		return -1;
	}

	for (currentCommand = mrtp_list_previous(mrtp_list_end(&channel->incomingCommands));
		currentCommand != mrtp_list_end(&channel->incomingCommands);
		currentCommand = mrtp_list_previous(currentCommand))
	{
		MRtpIncomingCommand * incomingCommand = (MRtpIncomingCommand *)currentCommand;

		if (startSequenceNumber >= channel->incomingSequenceNumber) {
			if (incomingCommand->sequenceNumber < channel->incomingSequenceNumber)
				continue;
		}
		else if (incomingCommand->sequenceNumber >= channel->incomingSequenceNumber)
			break;

		if (incomingCommand->sequenceNumber <= startSequenceNumber) {

			if (incomingCommand->sequenceNumber < startSequenceNumber)
				break;

			if ((incomingCommand->command.header.command & MRTP_PROTOCOL_COMMAND_MASK) != MRTP_PROTOCOL_COMMAND_SEND_UNSEQUENCED_FRAGMENT ||
				totalLength != incomingCommand->packet->dataLength ||
				fragmentCount != incomingCommand->fragmentCount)
				return -1;

			startCommand = incomingCommand;
			break;
		}
	}

	if (startCommand == NULL) {

		MRtpProtocol hostCommand = *command;
		hostCommand.header.sequenceNumber = startSequenceNumber;

		startCommand = mrtp_peer_queue_incoming_command(peer, &hostCommand, NULL, totalLength,
			MRTP_PACKET_FLAG_UNSEQUENCED, fragmentCount);

		// maybe the startCommand has already been dispatched
		if (startCommand == NULL)
			return 0;
	}

	if ((startCommand->fragments[fragmentNumber / 32] & (1 << (fragmentNumber % 32))) == 0) {

		--startCommand->fragmentsRemaining;

		startCommand->fragments[fragmentNumber / 32] |= (1 << (fragmentNumber % 32));

		if (fragmentOffset + fragmentLength > startCommand->packet->dataLength)
			fragmentLength = startCommand->packet->dataLength - fragmentOffset;

		memcpy(startCommand->packet->data + fragmentOffset,
			(mrtp_uint8 *)command + sizeof(MRtpProtocolSendFragment),
			fragmentLength);

		if (startCommand->fragmentsRemaining <= 0)
			mrtp_peer_dispatch_incoming_unsequenced_commands(peer, channel);
	}

	return 0;
}

static int mrtp_protocol_handle_throttle_configure(MRtpHost * host, MRtpPeer * peer,
	const MRtpProtocol * command)
{
	if (peer->state != MRTP_PEER_STATE_CONNECTED && peer->state != MRTP_PEER_STATE_DISCONNECT_LATER)
		return -1;

	peer->packetThrottleInterval = MRTP_NET_TO_HOST_32(command->throttleConfigure.packetThrottleInterval);
	peer->packetThrottleAcceleration = MRTP_NET_TO_HOST_32(command->throttleConfigure.packetThrottleAcceleration);
	peer->packetThrottleDeceleration = MRTP_NET_TO_HOST_32(command->throttleConfigure.packetThrottleDeceleration);

	return 0;
}

static int mrtp_protocol_handle_incoming_commands(MRtpHost * host, MRtpEvent * event) {
	MRtpProtocolHeader * header;
	MRtpProtocol * command;
	MRtpPeer * peer;
	mrtp_uint8 * currentData;
	size_t headerSize;
	mrtp_uint16 peerID, flags;
	mrtp_uint8 sessionID;

	if (host->receivedDataLength < (size_t) & ((MRtpProtocolHeader *)0)->sentTime)
		return 0;

	header = (MRtpProtocolHeader *)host->receivedData;


	peerID = MRTP_NET_TO_HOST_16(header->peerID);
	sessionID = (peerID & MRTP_PROTOCOL_HEADER_SESSION_MASK) >> MRTP_PROTOCOL_HEADER_SESSION_SHIFT;
	flags = peerID & MRTP_PROTOCOL_HEADER_FLAG_MASK;
	peerID &= ~(MRTP_PROTOCOL_HEADER_FLAG_MASK | MRTP_PROTOCOL_HEADER_SESSION_MASK);

	headerSize = (flags & MRTP_PROTOCOL_HEADER_FLAG_SENT_TIME ? sizeof(MRtpProtocolHeader) : (size_t) & ((MRtpProtocolHeader *)0)->sentTime);

	if (peerID == MRTP_PROTOCOL_MAXIMUM_PEER_ID)
		peer = NULL;
	else if (peerID >= host->peerCount)
		return 0;
	else {
		peer = &host->peers[peerID];

		if (peer->state == MRTP_PEER_STATE_DISCONNECTED || peer->state == MRTP_PEER_STATE_ZOMBIE ||
			((host->receivedAddress.host != peer->address.host || host->receivedAddress.port != peer->address.port) &&
				peer->address.host != MRTP_HOST_BROADCAST) ||
				(peer->outgoingPeerID < MRTP_PROTOCOL_MAXIMUM_PEER_ID && sessionID != peer->incomingSessionID))
			return 0;
	}

	if (peer != NULL) {
		peer->address.host = host->receivedAddress.host;
		peer->address.port = host->receivedAddress.port;
		peer->incomingDataTotal += host->receivedDataLength;
	}

	currentData = host->receivedData + headerSize;

	while (currentData < &host->receivedData[host->receivedDataLength]) {

		mrtp_uint8 commandNumber;
		size_t commandSize;

		command = (MRtpProtocol *)currentData;

		if (currentData + sizeof(MRtpProtocolCommandHeader) > & host->receivedData[host->receivedDataLength])
			break;

		commandNumber = command->header.command & MRTP_PROTOCOL_COMMAND_MASK;
		if (commandNumber >= MRTP_PROTOCOL_COMMAND_COUNT)
			break;

		commandSize = commandSizes[commandNumber];
		if (commandSize == 0 || currentData + commandSize > & host->receivedData[host->receivedDataLength])
			break;

		currentData += commandSize;

		// if peer is NULL and the command isn't the connect command, then break
		if (peer == NULL && commandNumber != MRTP_PROTOCOL_COMMAND_CONNECT)
			break;
		else if (peer != NULL) {
			peer->lastReceiveTime = host->serviceTime;
		}

		command->header.sequenceNumber = MRTP_NET_TO_HOST_16(command->header.sequenceNumber);

#if defined(PRINTLOG) && defined(SENDANDRECEIVE)
		fprintf(host->logFile, "receive [%s]: (%d) from peer: <%d> at channel: [%d]", commandName[commandNumber],
			command->header.sequenceNumber, peerID, channelIDs[command->header.command & MRTP_PROTOCOL_COMMAND_MASK]);
		if (commandNumber == MRTP_PROTOCOL_COMMAND_REDUNDANCY_ACKNOWLEDGE) {
			fprintf(host->logFile, " next unack: [%d]", MRTP_NET_TO_HOST_16(command->redundancyAcknowledge.nextUnackSequenceNumber));
		}
		fprintf(host->logFile, "\n");
#endif // SENDANDRECEIVE
#ifdef SENDANDRECEIVE
		printf("receive [%s]: (%d) from peer: <%d> at channel: [%d]", commandName[commandNumber],
			command->header.sequenceNumber, peerID, channelIDs[command->header.command & MRTP_PROTOCOL_COMMAND_MASK]);
		if (commandNumber == MRTP_PROTOCOL_COMMAND_REDUNDANCY_ACKNOWLEDGE) {
			printf(" next unack: [%d]", MRTP_NET_TO_HOST_16(command->redundancyAcknowledge.nextUnackSequenceNumber));
		}
		printf("\n");
#endif // SENDANDRECEIVE

		mrtp_uint16 sentTime;
		sentTime = (flags & MRTP_PROTOCOL_HEADER_FLAG_SENT_TIME) ? MRTP_NET_TO_HOST_16(header->sentTime) : 0;

		switch (commandNumber) {

		case MRTP_PROTOCOL_COMMAND_ACKNOWLEDGE:
			peer->sendRedundancyAfterReceive = FALSE;
			if (mrtp_protocol_handle_acknowledge(host, event, peer, command))
				goto commandError;
			break;

		case MRTP_PROTOCOL_COMMAND_REDUNDANCY_ACKNOWLEDGE:
			peer->sendRedundancyAfterReceive = FALSE;
			if (mrtp_protocol_handle_redundancy_acknowledge(host, event, peer, command))
				goto commandError;
			break;

		case MRTP_PROTOCOL_COMMAND_CONNECT:
			if (peer != NULL)
				goto commandError;
			peer = mrtp_protocol_handle_connect(host, header, command);
			if (peer == NULL)
				goto commandError;
			break;

		case MRTP_PROTOCOL_COMMAND_VERIFY_CONNECT:
			if (mrtp_protocol_handle_verify_connect(host, event, peer, command))
				goto commandError;
			break;

		case MRTP_PROTOCOL_COMMAND_DISCONNECT:
			if (mrtp_protocol_handle_disconnect(host, peer, command))
				goto commandError;
			break;

		case MRTP_PROTOCOL_COMMAND_PING:
			if (mrtp_protocol_handle_ping(host, peer, command))
				goto commandError;
			break;

		case MRTP_PROTOCOL_COMMAND_BANDWIDTH_LIMIT:
			if (mrtp_protocol_handle_bandwidth_limit(host, peer, command))
				goto commandError;
			break;

		case MRTP_PROTOCOL_COMMAND_SEND_RELIABLE:
			if (mrtp_protocol_handle_send_reliable(host, peer, command, &currentData))
				goto commandError;
			break;

		case MRTP_PROTOCOL_COMMAND_SEND_FRAGMENT:
			if (mrtp_protocol_handle_send_fragment(host, peer, command, &currentData))
				goto commandError;
			break;

		case MRTP_PROTOCOL_COMMAND_SEND_REDUNDANCY_NO_ACK:
			if (mrtp_protocol_handle_send_redundancy_noack(host, peer, command, &currentData))
				goto commandError;
			break;

		case MRTP_PROTOCOL_COMMAND_SEND_REDUNDANCY_FRAGEMENT_NO_ACK:
			if (mrtp_protocol_handle_send_redundancy_fragment_noack(host, peer, command, &currentData))
				goto commandError;
			break;

		case MRTP_PROTOCOL_COMMAND_SEND_REDUNDANCY:
			if (mrtp_protocol_handle_send_redundancy(host, peer, command, &currentData))
				goto commandError;
			break;

		case MRTP_PROTOCOL_COMMAND_SEND_REDUNDANCY_FRAGMENT:
			if (mrtp_protocol_handle_send_redundancy_fragment(host, peer, command, &currentData))
				goto commandError;
			break;

		case MRTP_PROTOCOL_COMMAND_SEND_UNSEQUENCED:
			if (mrtp_protocol_handle_send_unsequenced(host, peer, command, &currentData))
				goto commandError;
			break;

		case MRTP_PROTOCOL_COMMAND_SEND_UNSEQUENCED_FRAGMENT:
			if (mrtp_protocol_handle_send_unsequenced_fragment(host, peer, command, &currentData))
				goto commandError;
			break;

		case MRTP_PROTOCOL_COMMAND_THROTTLE_CONFIGURE:
			if (mrtp_protocol_handle_throttle_configure(host, peer, command))
				goto commandError;
			break;

		default:
			goto commandError;
		}

		// if this command need acknowledge and peer is not disconnected
		// then add ack to acknowledge queue
		if (peer != NULL && (flags & MRTP_PROTOCOL_HEADER_FLAG_SENT_TIME)) {

			sentTime = MRTP_NET_TO_HOST_16(header->sentTime);

			if (command->header.flag == MRTP_PROTOCOL_COMMAND_FLAG_ACKNOWLEDGE) {

				switch (peer->state) {

				case MRTP_PEER_STATE_DISCONNECTING:
				case MRTP_PEER_STATE_ACKNOWLEDGING_CONNECT:
				case MRTP_PEER_STATE_DISCONNECTED:
				case MRTP_PEER_STATE_ZOMBIE:
					break;

				case MRTP_PEER_STATE_ACKNOWLEDGING_DISCONNECT:
					if ((command->header.command & MRTP_PROTOCOL_COMMAND_MASK) == MRTP_PROTOCOL_COMMAND_DISCONNECT)
						mrtp_peer_queue_acknowledgement(peer, command, sentTime);
					break;

				default:
					mrtp_peer_queue_acknowledgement(peer, command, sentTime);
					break;
				}
			}
			else if (command->header.flag == MRTP_PROTOCOL_COMMAND_FLAG_REDUNDANCY_ACKNOWLEDGE) {

				mrtp_uint16 nextRedundancyNumber = peer->channels[MRTP_PROTOCOL_REDUNDANCY_CHANNEL_NUM].incomingSequenceNumber + 1;

				switch (peer->state) {

				case MRTP_PEER_STATE_DISCONNECTING:
				case MRTP_PEER_STATE_ACKNOWLEDGING_CONNECT:
				case MRTP_PEER_STATE_DISCONNECTED:
				case MRTP_PEER_STATE_ZOMBIE:
					break;

				default:
					//if (command->header.sequenceNumber >= nextRedundancyNumber - 1) {
					mrtp_peer_queue_redundancy_acknowldegement(peer, command, sentTime);
					//}
					break;
				}
			}
		}
	}

commandError:
	if (event != NULL && event->type != MRTP_EVENT_TYPE_NONE)
		return 1;

	return 0;
}

static int mrtp_protocol_receive_incoming_commands(MRtpHost * host, MRtpEvent * event) {

	int packets;

	// at most receive 256 times
	// if there is none data to receive or there is an error, then return
	for (packets = 0; packets < 256; ++packets) {

		int receivedLength;
		MRtpBuffer buffer;

		buffer.data = host->packetData[0];
		buffer.dataLength = sizeof(host->packetData[0]);

		receivedLength = mrtp_socket_receive(host->socket, &host->receivedAddress, &buffer, 1);

		if (receivedLength < 0) {
			printf("socket receive error!\n");
			return -1;
		}

		if (receivedLength == 0) {
			return 0;
		}


#if defined(PRINTLOG) && defined(SENDANDRECEIVE)
		fprintf(host->logFile, "receive %d at {%d}\n", receivedLength, host->serviceTime);
#endif
#ifdef SENDANDRECEIVE
		printf("receive %d at {%d}\n", receivedLength, host->serviceTime);
#endif // SENDANDRECEIVE


		host->receivedData = host->packetData[0];
		host->receivedDataLength = receivedLength;

		host->totalReceivedData += receivedLength;
		host->totalReceivedPackets++;

		switch (mrtp_protocol_handle_incoming_commands(host, event)) {
		case 1:
			return 1;

		case -1:
			return -1;

		default:
			break;
		}
	}
	return -1;
}

static int mrtp_protocol_dispatch_incoming_commands(MRtpHost * host, MRtpEvent * event) {

	while (!mrtp_list_empty(&host->dispatchQueue)) {

		MRtpPeer * peer = (MRtpPeer *)mrtp_list_remove(mrtp_list_begin(&host->dispatchQueue));

		peer->needsDispatch = 0;

		switch (peer->state) {

			// if there is an connection event to dispatch
		case MRTP_PEER_STATE_CONNECTION_PENDING:
		case MRTP_PEER_STATE_CONNECTION_SUCCEEDED:
			mrtp_protocol_change_state(host, peer, MRTP_PEER_STATE_CONNECTED);

			event->type = MRTP_EVENT_TYPE_CONNECT;
			event->peer = peer;

			return 1;

			// if there is an disconnection event to dispatch
		case MRTP_PEER_STATE_ZOMBIE:
			host->recalculateBandwidthLimits = 1;

			event->type = MRTP_EVENT_TYPE_DISCONNECT;
			event->peer = peer;

			mrtp_peer_reset(peer);

			return 1;

			// if there is some data to receive
		case MRTP_PEER_STATE_CONNECTED:
			if (mrtp_list_empty(&peer->dispatchedCommands))
				continue;

			event->packet = mrtp_peer_receive(peer, &event->channelID);
			if (event->packet == NULL)
				continue;

			event->type = MRTP_EVENT_TYPE_RECEIVE;
			event->peer = peer;

			if (!mrtp_list_empty(&peer->dispatchedCommands)) {
				peer->needsDispatch = 1;
				// if there are still some enent to dispatch of this peer, add peer to host dispatch queue
				mrtp_list_insert(mrtp_list_end(&host->dispatchQueue), &peer->dispatchList);
			}

			return 1;

		default:
			break;
		}
	}

	return 0;
}

int mrtp_host_service(MRtpHost * host, MRtpEvent * event, mrtp_uint32 timeout) {

	mrtp_uint32 waitCondition;

	if (event != NULL) {
		event->type = MRTP_EVENT_TYPE_NONE;
		event->peer = NULL;
		event->packet = NULL;

		switch (mrtp_protocol_dispatch_incoming_commands(host, event)) {
		case 1:
			return 1;
		case -1:
			return -1;
		default:
			break;
		}
	}

	host->serviceTime = mrtp_time_get();
	timeout += host->serviceTime;

	do {
		if (MRTP_TIME_DIFFERENCE(host->serviceTime, host->bandwidthThrottleEpoch) >=
			MRTP_HOST_BANDWIDTH_THROTTLE_INTERVAL)
			mrtp_host_bandwidth_throttle(host);

		switch (mrtp_protocol_send_outgoing_commands(host, event, 1)) {
		case 1:
			return 1;
		case -1:
			return -1;
		default:
			break;
		}

		switch (mrtp_protocol_receive_incoming_commands(host, event)) {
		case 1:
			return 1;
		case -1:
			return -1;
		default:
			break;
		}

		switch (mrtp_protocol_send_outgoing_commands(host, event, 1)) {
		case 1:
			return 1;
		case -1:
			return -1;
		default:
			break;
		}


		if (event != NULL) {
			switch (mrtp_protocol_dispatch_incoming_commands(host, event)) {
			case 1:
				return 1;
			case -1:
				return -1;
			default:
				break;
			}
		}

		if (MRTP_TIME_GREATER_EQUAL(host->serviceTime, timeout))
			return 0;

		do {
			host->serviceTime = mrtp_time_get();

			if (MRTP_TIME_GREATER_EQUAL(host->serviceTime, timeout))
				return 0;

			waitCondition = MRTP_SOCKET_WAIT_RECEIVE | MRTP_SOCKET_WAIT_INTERRUPT;

			if (mrtp_socket_wait(host->socket, &waitCondition, MRTP_TIME_DIFFERENCE(timeout, host->serviceTime)) != 0)
				return -1;

		} while (waitCondition & MRTP_SOCKET_WAIT_INTERRUPT);

		host->serviceTime = mrtp_time_get();

	} while (waitCondition & MRTP_SOCKET_WAIT_RECEIVE);
	return 0;
}

void mrtp_host_flush(MRtpHost * host) {
	host->serviceTime = mrtp_time_get();

	mrtp_protocol_send_outgoing_commands(host, NULL, 0);
	//mrtp_protocol_send_redundancy_outgoing_commands(host, NULL, 0);
}