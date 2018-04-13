#include <stdio.h>
#include <string.h>
#include "utility.h"
#include "time.h"
#include "mrtp.h"
#ifdef SETPACKETLOSS
#include <assert.h>
#endif // SETPACKETLOSS


static size_t commandSizes[MRTP_PROTOCOL_COMMAND_COUNT] = {
	0,
	sizeof(MRtpProtocolAcknowledge),
	sizeof(MRtpProtocolConnect),
	sizeof(MRtpProtocolVerifyConnect),
	sizeof(MRtpProtocolDisconnect),
	sizeof(MRtpProtocolPing),
	sizeof(MRtpProtocolSendReliable),
	sizeof(MRtpProtocolSendFragment),
	sizeof(MRtpProtocolBandwidthLimit),
	sizeof(MRtpProtocolThrottleConfigure),
	sizeof(MRtpProtocolSendRedundancyNoAck),
	sizeof(MRtpProtocolSendRedundancyFragementNoAck),
	sizeof(MRtpProtocolSendRedundancy),
	sizeof(MRtpProtocolSendRedundancyFragment),
	sizeof(MRtpProtocolSetQuickRetransmit),
	sizeof(MRtpProtocolRedundancyAcknowledge),
	sizeof(MRtpProtocolRetransmitRedundancy),
};

mrtp_uint8 channelIDs[] = {
	0xFF,										//none
	0xFF,										//ack
	0xFF,										//connect
	0xFF,										//verify connect
	0xFF,										//disconnect
	0xFF,										//ping
	MRTP_PROTOCOL_RELIABLE_CHANNEL_NUM,			//send reliable
	MRTP_PROTOCOL_RELIABLE_CHANNEL_NUM,			//send fragement
	0xFF,										//bandwidth limit
	0xFF,										//throttleconfigure
	MRTP_PROTOCOL_REDUNDANCY_NOACK_CHANNEL_NUM,	//send redundancy no ack
	MRTP_PROTOCOL_REDUNDANCY_NOACK_CHANNEL_NUM, //send redundancy fragement no ack
	MRTP_PROTOCOL_REDUNDANCY_CHANNEL_NUM,		//send reliable redundancy
	MRTP_PROTOCOL_REDUNDANCY_CHANNEL_NUM,		//send reliable redundancy
	0xFF,										//send quick retransmit number
	0xFF,										//redundancy ack
	0xFF,										//retransmit Redundancy
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
	"RetransmitConfigure",
	"RedundancyAck",
	"RetransmitRedundancy",
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
	//需要重新计算带宽
	if (peer->state >= MRTP_PEER_STATE_CONNECTION_PENDING)
		host->recalculateBandwidthLimits = 1;

	// 如果三次握手还没握手完
	if (peer->state != MRTP_PEER_STATE_CONNECTING && peer->state < MRTP_PEER_STATE_CONNECTION_SUCCEEDED)
		mrtp_peer_reset(peer);
	else if (event != NULL) {
		event->type = MRTP_EVENT_TYPE_DISCONNECT;
		event->peer = peer;
		event->data = 0;

		mrtp_peer_reset(peer);
	}
	else {
		peer->eventData = 0;
		mrtp_protocol_dispatch_state(host, peer, MRTP_PEER_STATE_ZOMBIE);
	}
}

static void mrtp_protocol_notify_connect(MRtpHost * host, MRtpPeer * peer, MRtpEvent * event) {
	//需要重新计算带宽
	host->recalculateBandwidthLimits = 1;

	if (event != NULL) {
		mrtp_protocol_change_state(host, peer, MRTP_PEER_STATE_CONNECTED);

		event->type = MRTP_EVENT_TYPE_CONNECT;
		event->peer = peer;
		event->data = peer->eventData;
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

			// judeg whether a peer has disconnected
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
			++peer->packetsLost;
			// change the [rto * 2] to [rto * 1.5]
			outgoingCommand->roundTripTimeout += outgoingCommand->roundTripTimeout / 2;

#ifdef PACKETLOSSDEBUG
			printf("[%s]: [%d] Loss! change rto to: [%d]\n",
				commandName[outgoingCommand->command.header.command & MRTP_PROTOCOL_COMMAND_MASK],
				MRTP_NET_TO_HOST_16(outgoingCommand->command.header.sequenceNumber),
				outgoingCommand->roundTripTimeout);
#endif // PACKETLOSSDEBUG

			mrtp_list_insert(insertPosition, mrtp_list_remove(&outgoingCommand->outgoingCommandList));

			if (currentCommand == mrtp_list_begin(&peer->sentReliableCommands) &&
				!mrtp_list_empty(&peer->sentReliableCommands))
			{
				outgoingCommand = (MRtpOutgoingCommand *)currentCommand;
				peer->nextTimeout = outgoingCommand->sentTime + outgoingCommand->roundTripTimeout;
			}
		}
	}

	if (MRTP_TIME_GREATER_EQUAL(host->serviceTime, peer->nextRedundancyTimeout)) {

		currentCommand = mrtp_list_begin(&peer->sentRedundancyCommands);

		while (currentCommand != mrtp_list_end(&peer->sentRedundancyCommands)) {

			outgoingCommand = (MRtpOutgoingCommand *)currentCommand;

			currentCommand = mrtp_list_next(currentCommand);

			// if the outgoing command doexn't timeout
			if (MRTP_TIME_DIFFERENCE(host->serviceTime, outgoingCommand->sentTime) < outgoingCommand->roundTripTimeout)
				continue;

			if (peer->earliestTimeout == 0 || MRTP_TIME_LESS(outgoingCommand->sentTime, peer->earliestTimeout))
				peer->earliestTimeout = outgoingCommand->sentTime;

			// judeg whether a peer has disconnected
			if (MRTP_TIME_DIFFERENCE(host->serviceTime, peer->earliestTimeout) >= peer->timeoutMaximum ||
				(outgoingCommand->roundTripTimeout >= outgoingCommand->roundTripTimeoutLimit &&
					MRTP_TIME_DIFFERENCE(host->serviceTime, peer->earliestTimeout) >= peer->timeoutMinimum))
			{
				mrtp_protocol_notify_disconnect(host, peer, event);
				return 1;
			}

			// if a command is lost
			//if (outgoingCommand->packet != NULL)
			//	peer->reliableDataInTransit -= outgoingCommand->fragmentLength;
			++peer->packetsLost;

			mrtp_list_insert(mrtp_list_end(&peer->readytoDeleteRedundancyCommands),
				mrtp_list_remove(&outgoingCommand->outgoingCommandList));

			MRtpProtocol sendCommand;

			// change the outgoingCommand to readytoDeleteRedundancyCommands 
			// and use a reliable command to send it
			mrtp_uint8 commandNumber = outgoingCommand->command.header.command & MRTP_PROTOCOL_COMMAND_MASK;
			if (commandNumber == MRTP_PROTOCOL_COMMAND_SEND_REDUNDANCY) {

#ifdef PACKETLOSSDEBUG
				printf("[%s]: [%d] Loss!\n",
					commandName[outgoingCommand->command.header.command & MRTP_PROTOCOL_COMMAND_MASK],
					MRTP_NET_TO_HOST_16(outgoingCommand->command.header.sequenceNumber));
#endif // PACKETLOSSDEBUG

				sendCommand.header.command = MRTP_PROTOCOL_COMMAND_RETRANSMIT_REDUNDANCY | MRTP_PROTOCOL_COMMAND_FLAG_ACKNOWLEDGE;
				sendCommand.retransmitRedundancy.dataLength = outgoingCommand->command.sendRedundancy.dataLength;
				sendCommand.retransmitRedundancy.retransmitSequenceNumber = outgoingCommand->command.header.sequenceNumber;
				mrtp_peer_queue_outgoing_command(peer, &sendCommand, outgoingCommand->packet, 0, outgoingCommand->packet->dataLength);

			}
			else if (commandNumber == MRTP_PROTOCOL_COMMAND_SEND_REDUNDANCY_FRAGMENT) {

			}
		}

		if (!mrtp_list_empty(&peer->sentRedundancyCommands)) {
			currentCommand = mrtp_list_front(&peer->sentRedundancyCommands);
			outgoingCommand = (MRtpOutgoingCommand *)currentCommand;
			peer->nextRedundancyTimeout = outgoingCommand->sentTime + outgoingCommand->roundTripTimeout;
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

#ifdef SENDANDRECEIVE
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

		if (command >= &host->commands[sizeof(host->commands) / sizeof(MRtpProtocol)] ||
			buffer >= &host->buffers[sizeof(host->buffers) / sizeof(MRtpBuffer)] ||
			peer->mtu - host->packetSize < sizeof(MRtpProtocolRedundancyAcknowledge))
		{
			host->continueSending = 1;
			break;
		}

		acknowledgement = (MRtpAcknowledgement *)currentAcknowledgement;
		currentAcknowledgement = mrtp_list_next(currentAcknowledgement);

		buffer->data = command;
		buffer->dataLength = sizeof(MRtpProtocolRedundancyAcknowledge);

		host->packetSize += buffer->dataLength;

		sequenceNumber = MRTP_HOST_TO_NET_16(acknowledgement->command.header.sequenceNumber);

		command->header.command = MRTP_PROTOCOL_COMMAND_REDUNDANCY_ACKNOWLEDGE;
		command->header.sequenceNumber = sequenceNumber;
		command->redundancyAcknowledge.receivedSequenceNumber = sequenceNumber;
		command->redundancyAcknowledge.receivedSentTime = MRTP_HOST_TO_NET_16(acknowledgement->sentTime);
		// next sequence number to receive
		command->redundancyAcknowledge.nextUnackSequenceNumber = MRTP_HOST_TO_NET_16(nextRedundancyNumber);

#ifdef SENDANDRECEIVE
		printf("add buffer [redundancy ack]: (%d) at channel: [%d]\n",
			MRTP_NET_TO_HOST_16(command->header.sequenceNumber),
			channelIDs[acknowledgement->command.header.command & MRTP_PROTOCOL_COMMAND_MASK]);
#endif // SENDANDRECEIVE

		mrtp_list_remove(&acknowledgement->acknowledgementList);
		mrtp_free(acknowledgement);

		++command;
		++buffer;
	}

	host->commandCount = command - host->commands;
	host->bufferCount = buffer - host->buffers;
}

static int mrtp_protocol_send_redundancy_outgoing_commands(MRtpHost * host, MRtpEvent * event, int checkForTimeouts) {

	mrtp_uint8 headerData[sizeof(MRtpProtocolHeader) + sizeof(mrtp_uint32)];
	MRtpProtocolHeader *header = (MRtpProtocolHeader *)headerData;
	MRtpPeer *currentPeer;
	int sentLength;

	host->continueSending = 1;
	while (host->continueSending) {
		host->continueSending = 0;
		for (currentPeer = host->peers; currentPeer < &host->peers[host->peerCount]; ++currentPeer) {

			if (!currentPeer->redundancyNoAckBuffers || currentPeer->state == MRTP_PEER_STATE_DISCONNECTED || currentPeer->state == MRTP_PEER_STATE_ZOMBIE)
				continue;

			host->headerFlags = 0;
			host->commandCount = 0;
			host->bufferCount = 1;
			host->packetSize = sizeof(MRtpProtocolHeader);

			if (!mrtp_list_empty(&currentPeer->outgoingRedundancyNoAckCommands))
				mrtp_protocol_send_redundancy_noack_commands(host, currentPeer);

			MRtpRedundancyNoAckBuffer* currentRedundancyNoackBuffer = &currentPeer->redundancyNoAckBuffers[currentPeer->currentRedundancyNoAckBufferNum];

			if (currentRedundancyNoackBuffer->buffercount > 0) {

				host->buffers->data = headerData;
				host->buffers->dataLength = (size_t) & ((MRtpProtocolHeader *)0)->sentTime;
				MRtpBuffer * buffer = &host->buffers[host->bufferCount];

				if (currentPeer->outgoingPeerID < MRTP_PROTOCOL_MAXIMUM_PEER_ID)
					host->headerFlags |= currentPeer->outgoingSessionID << MRTP_PROTOCOL_HEADER_SESSION_SHIFT;
				header->peerID = MRTP_HOST_TO_NET_16(currentPeer->outgoingPeerID | host->headerFlags);

				currentPeer->lastSendTime = host->serviceTime;

				// 将每个peer 缓存的buffer中的数据移动到host的buffer中
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

				sentLength = mrtp_socket_send(host->socket, &currentPeer->address, host->buffers, host->bufferCount);

#ifdef SENDANDRECEIVE
				printf("send redundancy %d to peer: <%d>\n", sentLength, currentPeer->incomingPeerID);
#endif // SENDANDRECEIVE

				if (sentLength < 0)
					return -1;

				host->totalSentData += sentLength;
				host->totalSentPackets++;
				currentPeer->currentRedundancyNoAckBufferNum = (currentPeer->currentRedundancyNoAckBufferNum + 1)
					% (currentPeer->redundancyNum + 1);
				currentRedundancyNoackBuffer = &currentPeer->redundancyNoAckBuffers[currentPeer->currentRedundancyNoAckBufferNum];
				if (currentRedundancyNoackBuffer->buffercount >= 0) {
					mrtp_protocol_remove_redundancy_buffer_commands(currentRedundancyNoackBuffer);
					currentRedundancyNoackBuffer->buffercount = 0;
					currentRedundancyNoackBuffer->packetSize = 0;
				}
			}
		}
	}
	return 0;
}

//发送reliable outgoing commands
//如果发送的数据没有超过发送窗口的大小的限制或者host的commands和buffer没有超过限制
//则将该command从outgoingReliableCommands转移到sentReliableCommands
//并设置该command的roundTripTimeout
//并将要发送的command和command中的packet存放到host的buffer中
//如果存在可以发送的command，则返回1，否则返回0
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
		commandWindow = outgoingCommand->sequenceNumber / MRTP_PEER_RELIABLE_WINDOW_SIZE;

		if (channel != NULL) {
			if (!windowWrap &&
				outgoingCommand->sendAttempts < 1 &&	//第一次被发送的sequence number
				!(outgoingCommand->sequenceNumber % MRTP_PEER_RELIABLE_WINDOW_SIZE) &&	//正好要到一个新的窗口
																						// 上一个窗口满了（ps：一个ack都没收到）
				(channel->commandWindows[(commandWindow + MRTP_PEER_RELIABLE_WINDOWS - 1) % MRTP_PEER_RELIABLE_WINDOWS] >= MRTP_PEER_RELIABLE_WINDOW_SIZE ||
					channel->usedReliableWindows & ((((1 << MRTP_PEER_FREE_RELIABLE_WINDOWS) - 1) << commandWindow) |	// 发送窗口和空闲窗口重叠了
					(((1 << MRTP_PEER_FREE_RELIABLE_WINDOWS) - 1) >> (MRTP_PEER_RELIABLE_WINDOWS - commandWindow)))))
				windowWrap = 1;
#ifdef RELIABLEWINDOWDEBUG
			printf("channel: %d,realiableSeqNum: %d, reliableWindow: %d, number in window:[%d]\n",
				outgoingCommand->command.header.channelID, outgoingCommand->sequenceNumber, commandWindow,
				channel->commandWindows[(commandWindow + MRTP_PEER_RELIABLE_WINDOWS - 1) % MRTP_PEER_RELIABLE_WINDOWS]);
#endif
			if (windowWrap) {
				currentCommand = mrtp_list_next(currentCommand);
				continue;
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
		if (command >= &host->commands[sizeof(host->commands) / sizeof(MRtpProtocol)] ||	//host的command满了
			buffer + 1 >= &host->buffers[sizeof(host->buffers) / sizeof(MRtpBuffer)] ||	//host的buffer满了
			peer->mtu - host->packetSize < commandSize ||	//commandSize的大小超过限制了
			(outgoingCommand->packet != NULL &&
			(mrtp_uint16)(peer->mtu - host->packetSize) < (mrtp_uint16)(commandSize + outgoingCommand->fragmentLength)))
		{
			host->continueSending = 1;

			break;
		}

		currentCommand = mrtp_list_next(currentCommand);

		if (channel != NULL && outgoingCommand->sendAttempts < 1) {
			channel->usedReliableWindows |= 1 << commandWindow;
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

		//将该command从outgoing队列放到sent队列
		mrtp_list_insert(mrtp_list_end(&peer->sentReliableCommands), mrtp_list_remove(&outgoingCommand->outgoingCommandList));

		outgoingCommand->sentTime = host->serviceTime;

		//把buffer的指针指向command
		buffer->data = command;
		buffer->dataLength = commandSize;

		host->packetSize += buffer->dataLength;
		host->headerFlags |= MRTP_PROTOCOL_HEADER_FLAG_SENT_TIME;

		*command = outgoingCommand->command;

		//如果command需要发送packet，则将packet存到buffer中
		if (outgoingCommand->packet != NULL) {
			++buffer;

			buffer->data = outgoingCommand->packet->data + outgoingCommand->fragmentOffset;
			buffer->dataLength = outgoingCommand->fragmentLength;

			host->packetSize += outgoingCommand->fragmentLength;

			peer->reliableDataInTransit += outgoingCommand->fragmentLength;
		}
#ifdef SENDANDRECEIVE
		printf("add buffer [%s]: (%d) at channel[%d] ",
			commandName[outgoingCommand->command.header.command & MRTP_PROTOCOL_COMMAND_MASK],
			MRTP_NET_TO_HOST_16(outgoingCommand->command.header.sequenceNumber), channelID);
		if (commandName[outgoingCommand->command.header.command & MRTP_PROTOCOL_COMMAND_MASK] ==
			MRTP_PROTOCOL_COMMAND_RETRANSMIT_REDUNDANCY)
		{
			printf("for redundancy seq: [%d]",
				MRTP_NET_TO_HOST_16(outgoingCommand->command.retransmitRedundancy.retransmitSequenceNumber));
		}
		printf("\n");
#endif // SENDANDRECEIVE

		++peer->packetsSent;

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
		return 0;	//之前的还没发送
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
		++peer->packetsSent;

#ifdef SENDANDRECEIVE
		printf("add buffer [%s]: (%d) at channel[%d]\n",
			commandName[outgoingCommand->command.header.command & MRTP_PROTOCOL_COMMAND_MASK],
			MRTP_NET_TO_HOST_16(outgoingCommand->command.header.sequenceNumber),
			channelIDs[outgoingCommand->command.header.command & MRTP_PROTOCOL_COMMAND_MASK]);
#endif // SENDANDRECEIVE

	}
	currentRedundancyBuffer->buffercount = buffer - currentRedundancyBuffer->buffers;

	if (peer->state == MRTP_PEER_STATE_DISCONNECT_LATER &&
		mrtp_list_empty(&peer->outgoingReliableCommands) &&
		mrtp_list_empty(&peer->outgoingRedundancyCommands) &&
		mrtp_list_empty(&peer->outgoingRedundancyNoAckCommands) &&
		mrtp_list_empty(&peer->sentReliableCommands))
		mrtp_peer_disconnect(peer, peer->eventData);

	return 0;
}

static int mrtp_protocol_send_redundancy_commands(MRtpHost * host, MRtpPeer * peer) {

	MRtpRedundancyBuffer* currentRedundancyBuffer = &peer->redundancyBuffers[peer->currentRedundancyBufferNum];

	// the data in current redundancy buffer hasn't been sent
	if (currentRedundancyBuffer->buffercount != 0)
		return 0;

	MRtpBuffer * buffer = &currentRedundancyBuffer->buffers[currentRedundancyBuffer->buffercount];
	MRtpOutgoingCommand * outgoingCommand;
	MRtpListIterator currentCommand;
	size_t commandSize;
	mrtp_uint32 redundancyMtu = (peer->mtu - sizeof(MRtpProtocolHeader) - 1) / peer->redundancyNum;

	currentCommand = mrtp_list_begin(&peer->outgoingRedundancyCommands);

	while (currentCommand != mrtp_list_end(&peer->outgoingRedundancyCommands)) {

		outgoingCommand = (MRtpOutgoingCommand *)currentCommand;
		commandSize = commandSizes[outgoingCommand->command.header.command & MRTP_PROTOCOL_COMMAND_MASK];

		/*
		some code to judge whether the data in transmit has exceed the window size
		*/

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

		++outgoingCommand->sendAttempts;

		// set the rtt for the outgoing command
		if (outgoingCommand->roundTripTimeout == 0) {
			outgoingCommand->roundTripTimeout = peer->roundTripTime + 4 * peer->roundTripTimeVariance;
			outgoingCommand->roundTripTimeoutLimit = peer->timeoutLimit * outgoingCommand->roundTripTimeout;
		}
		// set the next redundancy timeout
		if (mrtp_list_empty(&peer->sentRedundancyCommands))
			peer->nextRedundancyTimeout = host->serviceTime + outgoingCommand->roundTripTimeout;

		mrtp_list_insert(mrtp_list_end(&peer->sentRedundancyCommands), mrtp_list_remove(&outgoingCommand->outgoingCommandList));

		outgoingCommand->redundancyBufferNum = peer->currentRedundancyBufferNum;
		outgoingCommand->sentTime = host->serviceTime;

		host->headerFlags |= MRTP_PROTOCOL_HEADER_FLAG_SENT_TIME;

		if (outgoingCommand->packet != NULL) {
			++buffer;
			buffer->data = outgoingCommand->packet->data + outgoingCommand->fragmentOffset;
			buffer->dataLength = outgoingCommand->fragmentLength;
			currentRedundancyBuffer->packetSize += buffer->dataLength;
		}

		++buffer;
		++peer->packetsSent;

#ifdef SENDANDRECEIVE
		printf("add buffer [%s]: (%d) at channel[%d]\n",
			commandName[outgoingCommand->command.header.command & MRTP_PROTOCOL_COMMAND_MASK],
			MRTP_NET_TO_HOST_16(outgoingCommand->command.header.sequenceNumber),
			channelIDs[outgoingCommand->command.header.command & MRTP_PROTOCOL_COMMAND_MASK]);
#endif // SENDANDRECEIVE

	}
	currentRedundancyBuffer->buffercount = buffer - currentRedundancyBuffer->buffers;

	return 0;
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
				(MRTP_TIME_GREATER_EQUAL(host->serviceTime, currentPeer->nextTimeout) ||
					MRTP_TIME_GREATER_EQUAL(host->serviceTime, currentPeer->nextRedundancyTimeout)) &&
				mrtp_protocol_check_timeouts(host, currentPeer, event) == 1)
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

			if (!mrtp_list_empty(&currentPeer->outgoingRedundancyNoAckCommands))
				mrtp_protocol_send_redundancy_noack_commands(host, currentPeer);

			if (!mrtp_list_empty(&currentPeer->outgoingRedundancyCommands))
				mrtp_protocol_send_redundancy_commands(host, currentPeer);

			MRtpRedundancyNoAckBuffer* currentRedundancyNoackBuffer =
				&currentPeer->redundancyNoAckBuffers[currentPeer->currentRedundancyNoAckBufferNum];

			MRtpRedundancyBuffer* currentRedundancyBuffer =
				&currentPeer->redundancyBuffers[currentPeer->currentRedundancyBufferNum];

			// there are some data need to send
			if (host->bufferCount > 1 ||
				(currentRedundancyNoackBuffer && currentRedundancyNoackBuffer->buffercount > 0) ||
				(currentRedundancyBuffer && currentRedundancyBuffer->buffercount > 0))
			{
				if (currentPeer->packetLossEpoch == 0)
					currentPeer->packetLossEpoch = host->serviceTime;
				else if (MRTP_TIME_DIFFERENCE(host->serviceTime, currentPeer->packetLossEpoch) >= MRTP_PEER_PACKET_LOSS_INTERVAL &&
					currentPeer->packetsSent > 0) {

					mrtp_uint32 packetLoss = currentPeer->packetsLost * MRTP_PEER_PACKET_LOSS_SCALE / currentPeer->packetsSent;

					currentPeer->packetLossVariance -= currentPeer->packetLossVariance / 4;

					if (packetLoss >= currentPeer->packetLoss) {
						currentPeer->packetLoss += (packetLoss - currentPeer->packetLoss) / 8;
						currentPeer->packetLossVariance += (packetLoss - currentPeer->packetLoss) / 4;
					}
					else {
						currentPeer->packetLoss -= (currentPeer->packetLoss - packetLoss) / 8;
						currentPeer->packetLossVariance += (currentPeer->packetLoss - packetLoss) / 4;
					}

					currentPeer->packetLossEpoch = host->serviceTime;
					currentPeer->packetsSent = 0;
					currentPeer->packetsLost = 0;
				}

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

				//send the redundnacy buffer data
				if (currentRedundancyBuffer && currentRedundancyBuffer->buffercount > 0) {

					int redundancyBufferCount = 0;
					int redundancyPacketSize = 0;

					for (int i = currentPeer->redundancyNum - 1; i >= 0; i--) {
						int num = (currentPeer->currentRedundancyBufferNum - i + MRTP_PROTOCOL_MAXIMUM_REDUNDNACY_BUFFER_SIZE)
							% MRTP_PROTOCOL_MAXIMUM_REDUNDNACY_BUFFER_SIZE;
						if (currentPeer->redundancyBuffers[num].buffercount > 0) {
							redundancyBufferCount += currentPeer->redundancyBuffers[num].buffercount;
							redundancyPacketSize += currentPeer->redundancyBuffers[num].packetSize;
						}
					}

					if (host->bufferCount + redundancyBufferCount <= sizeof(host->buffers) / sizeof(MRtpBuffer) &&
						host->mtu > host->packetSize + redundancyPacketSize)
					{
						// copy the peer redundancy buffer to host buffer to send 
						for (int i = currentPeer->redundancyNum - 1; i >= 0; i--) {
							int num = (currentPeer->currentRedundancyBufferNum - i + MRTP_PROTOCOL_MAXIMUM_REDUNDNACY_BUFFER_SIZE)
								% MRTP_PROTOCOL_MAXIMUM_REDUNDNACY_BUFFER_SIZE;
							if (currentPeer->redundancyBuffers[num].buffercount > 0) {
								for (int j = 0; j < currentPeer->redundancyBuffers[num].buffercount; j++) {
									buffer->data = currentPeer->redundancyBuffers[num].buffers[j].data;
									buffer->dataLength = currentPeer->redundancyBuffers[num].buffers[j].dataLength;

									++buffer;
									++host->bufferCount;
								}
							}
						}

						// clear current redundancy buffer
						currentPeer->currentRedundancyBufferNum = (currentPeer->currentRedundancyBufferNum + 1)
							% MRTP_PROTOCOL_MAXIMUM_REDUNDNACY_BUFFER_SIZE;
						currentRedundancyBuffer = &currentPeer->redundancyBuffers[currentPeer->currentRedundancyBufferNum];
						if (currentRedundancyBuffer->buffercount != 0) {
							currentRedundancyBuffer->buffercount = 0;
							currentRedundancyBuffer->packetSize = 0;
						}
					}
					else {
						host->continueSending = 1;
					}
				}

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
#ifdef SENDANDRECEIVE
				printf("send: %d to peer: <%d>\n", sentLength, currentPeer->incomingPeerID);
#endif // SENDANDRECEIVE

				if (sentLength < 0)
					return -1;

				host->totalSentData += sentLength;
				host->totalSentPackets++;
			}
		}
	}

	return 0;
}

static MRtpProtocolCommand mrtp_protocol_remove_sent_reliable_command(MRtpPeer * peer,
	mrtp_uint16 reliableSequenceNumber, mrtp_uint8 channelID)
{
	MRtpOutgoingCommand * outgoingCommand = NULL;
	MRtpListIterator currentCommand, nextCommand;
	MRtpProtocolCommand commandNumber;
	int wasSent = 1;

	for (currentCommand = mrtp_list_begin(&peer->sentReliableCommands);
		currentCommand != mrtp_list_end(&peer->sentReliableCommands);
		currentCommand = nextCommand)
	{
		outgoingCommand = (MRtpOutgoingCommand *)currentCommand;
		nextCommand = mrtp_list_next(currentCommand);

		if (outgoingCommand->sequenceNumber == reliableSequenceNumber &&
			channelIDs[outgoingCommand->command.header.command & MRTP_PROTOCOL_COMMAND_MASK] == channelID)
			break;
		else if (peer->host->openQuickRetransmit) {
			outgoingCommand->fastAck++;

			// quickly retransmit, if the command are jumped quickRetransmitNum, then quickly retransmit
			if (outgoingCommand->fastAck > peer->quickRetransmitNum) {

				++peer->packetsLost;

				outgoingCommand->fastAck = 0;
				mrtp_list_insert(mrtp_list_begin(&peer->outgoingReliableCommands), mrtp_list_remove(&outgoingCommand->outgoingCommandList));

				if (currentCommand == mrtp_list_begin(&peer->sentReliableCommands) &&
					!mrtp_list_empty(&peer->sentReliableCommands))
				{
					outgoingCommand = (MRtpOutgoingCommand *)currentCommand;
					peer->nextTimeout = outgoingCommand->sentTime + outgoingCommand->roundTripTimeout;
				}

#ifdef PACKETLOSSDEBUG
				printf("[%s]: [%d] Loss!\n",
					commandName[outgoingCommand->command.header.command & MRTP_PROTOCOL_COMMAND_MASK],
					MRTP_NET_TO_HOST_16(outgoingCommand->command.header.sequenceNumber));
#endif // PACKETLOSSDEBUG

			}
		}
	}

	//if can't find command in sentReliableCommands queue
	//then find the command in outgoingReliableCommands queue(maybe the command has retransmited）
	if (currentCommand == mrtp_list_end(&peer->sentReliableCommands)) {

		for (currentCommand = mrtp_list_begin(&peer->outgoingReliableCommands);
			currentCommand != mrtp_list_end(&peer->outgoingReliableCommands);
			currentCommand = mrtp_list_next(currentCommand))
		{
			outgoingCommand = (MRtpOutgoingCommand *)currentCommand;

			if (outgoingCommand->sendAttempts < 1) return MRTP_PROTOCOL_COMMAND_NONE;

			if (outgoingCommand->sequenceNumber == reliableSequenceNumber &&
				channelIDs[outgoingCommand->command.header.command & MRTP_PROTOCOL_COMMAND_MASK] == channelID)
				break;
		}

		if (currentCommand == mrtp_list_end(&peer->outgoingReliableCommands))
			return MRTP_PROTOCOL_COMMAND_NONE;

		wasSent = 0;
	}

	if (outgoingCommand == NULL)
		return MRTP_PROTOCOL_COMMAND_NONE;

	if (channelID < peer->channelCount) {

		MRtpChannel * channel = &peer->channels[channelID];
		mrtp_uint16 reliableWindow = reliableSequenceNumber / MRTP_PEER_RELIABLE_WINDOW_SIZE;

		if (channel->commandWindows[reliableWindow] > 0) {
			--channel->commandWindows[reliableWindow];
			if (!channel->commandWindows[reliableWindow])
				channel->usedReliableWindows &= ~(1 << reliableWindow);
		}
	}

	commandNumber = (MRtpProtocolCommand)(outgoingCommand->command.header.command & MRTP_PROTOCOL_COMMAND_MASK);

	//remove this command from it's queue
	mrtp_list_remove(&outgoingCommand->outgoingCommandList);

	if (outgoingCommand->packet != NULL) {
		if (wasSent)
			peer->reliableDataInTransit -= outgoingCommand->fragmentLength;
		--outgoingCommand->packet->referenceCount;

		if (outgoingCommand->packet->referenceCount == 0) {
			outgoingCommand->packet->flags |= MRTP_PACKET_FLAG_SENT;

			mrtp_packet_destroy(outgoingCommand->packet);
		}
	}

	mrtp_free(outgoingCommand);

	if (mrtp_list_empty(&peer->sentReliableCommands))
		return commandNumber;

	// set the next timeout stamp for peer
	outgoingCommand = (MRtpOutgoingCommand *)mrtp_list_front(&peer->sentReliableCommands);
	peer->nextTimeout = outgoingCommand->sentTime + outgoingCommand->roundTripTimeout;

	return commandNumber;
}

// handle the acknowledge
static int mrtp_protocol_handle_acknowledge(MRtpHost * host, MRtpEvent * event,
	MRtpPeer * peer, const MRtpProtocol * command)
{
	mrtp_uint32 roundTripTime, receivedSentTime, receivedReliableSequenceNumber;
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

	peer->lastReceiveTime = host->serviceTime;
	peer->earliestTimeout = 0;

	roundTripTime = MRTP_TIME_DIFFERENCE(host->serviceTime, receivedSentTime);
	// just the throttle according to the roundTripTime
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

	channelID = command->acknowledge.channelID;
	commandNumber = mrtp_protocol_remove_sent_reliable_command(peer, receivedReliableSequenceNumber, channelID);

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
		if (mrtp_list_empty(&peer->outgoingReliableCommands) && mrtp_list_empty(&peer->sentReliableCommands))
			mrtp_peer_disconnect(peer, peer->eventData);
		break;

	default:
		break;
	}

	return 0;
}

static void mrtp_protocol_delete_redundancy_command(MRtpPeer * peer) {

	MRtpListIterator currentCommand;
	MRtpOutgoingCommand * outgoingCommand = NULL;
	size_t currentRedundancyBufferNum = peer->currentRedundancyBufferNum;

	if (mrtp_list_empty(&peer->readytoDeleteRedundancyCommands))
		return;

	for (currentCommand = mrtp_list_begin(&peer->readytoDeleteRedundancyCommands);
		currentCommand != mrtp_list_end(&peer->readytoDeleteRedundancyCommands);)
	{
		outgoingCommand = (MRtpOutgoingCommand *)currentCommand;
		currentCommand = mrtp_list_next(currentCommand);

		if (currentRedundancyBufferNum < outgoingCommand->redundancyBufferNum) {
			currentRedundancyBufferNum += MRTP_PROTOCOL_MAXIMUM_REDUNDNACY_BUFFER_SIZE;
		}
		if (currentRedundancyBufferNum - outgoingCommand->redundancyBufferNum >= peer->redundancyNum) {
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

}

// because all the retransmit commands are using the reliable
// so the commands in the sentRedundancyCommands are ascending
static void mrtp_protocol_remove_sent_redundancy_command(MRtpPeer * peer,
	mrtp_uint16 sequenceNumber, mrtp_uint16 nextUnackSequenceNumber)
{
	MRtpOutgoingCommand * outgoingCommand = NULL;
	MRtpOutgoingCommand * alreadyReceivedCommand = NULL;
	MRtpListIterator currentCommand, nextCommand;
	MRtpProtocolCommand commandNumber;
	mrtp_uint8 needDelete = FALSE;

	if (mrtp_list_empty(&peer->sentRedundancyCommands))
		return MRTP_PROTOCOL_COMMAND_NONE;

	//MRtpOutgoingCommand * firstSentCommand = mrtp_list_front(&peer->sentRedundancyCommands);
	//mrtp_uint16 alreadySentNumber = firstSentCommand->sequenceNumber - 1;
	//MRtpOutgoingCommand * endSentCommand = mrtp_list_previous(mrtp_list_end(&peer->sentRedundancyCommands));
	//mrtp_uint16 endSentNumber = endSentCommand->sequenceNumber;

	/*
	so don't consider overflow first
	nextUnackSequenceNumber to delete the command
	*/

	for (currentCommand = mrtp_list_begin(&peer->sentRedundancyCommands);
		currentCommand != mrtp_list_end(&peer->sentRedundancyCommands);
		currentCommand = nextCommand)
	{
		outgoingCommand = (MRtpOutgoingCommand *)currentCommand;
		nextCommand = mrtp_list_next(currentCommand);

		if (outgoingCommand->sequenceNumber == sequenceNumber) {
			needDelete = TRUE;
			break;
		}
		else if (outgoingCommand->sequenceNumber < nextUnackSequenceNumber) {
			needDelete = TRUE;
			mrtp_list_insert(mrtp_list_end(&peer->readytoDeleteRedundancyCommands),
				mrtp_list_remove(&outgoingCommand->outgoingCommandList));
		}
		else {
			outgoingCommand->fastAck++;

			// quickly retransmit, if the command are jumped quickRetransmitNum, then quickly retransmit
			if (outgoingCommand->fastAck > peer->quickRetransmitNum) {

				++peer->packetsLost;

				mrtp_list_insert(mrtp_list_end(&peer->readytoDeleteRedundancyCommands),
					mrtp_list_remove(&outgoingCommand->outgoingCommandList));

#ifdef PACKETLOSSDEBUG
				printf("[%s]: [%d] Loss!\n",
					commandName[outgoingCommand->command.header.command & MRTP_PROTOCOL_COMMAND_MASK],
					MRTP_NET_TO_HOST_16(outgoingCommand->command.header.sequenceNumber));
#endif // PACKETLOSSDEBUG

				MRtpProtocol sendCommand;
				sendCommand.header.command = MRTP_PROTOCOL_COMMAND_RETRANSMIT_REDUNDANCY | MRTP_PROTOCOL_COMMAND_FLAG_ACKNOWLEDGE;
				sendCommand.retransmitRedundancy.dataLength = outgoingCommand->command.sendRedundancy.dataLength;
				sendCommand.retransmitRedundancy.retransmitSequenceNumber = outgoingCommand->command.header.sequenceNumber;
				mrtp_peer_queue_outgoing_command(peer, &sendCommand, outgoingCommand->packet, 0, outgoingCommand->packet->dataLength);

			}
		}
	}

	if (currentCommand != mrtp_list_end(&peer->sentRedundancyCommands)) {
		mrtp_list_insert(mrtp_list_end(&peer->readytoDeleteRedundancyCommands),
			mrtp_list_remove(&outgoingCommand->outgoingCommandList));
	}

	if (needDelete)
		mrtp_protocol_delete_redundancy_command(peer);

	if (mrtp_list_empty(&peer->sentRedundancyCommands))
		return;

	// set the next timeout stamp for peer
	outgoingCommand = (MRtpOutgoingCommand *)mrtp_list_begin(&peer->sentRedundancyCommands);
	peer->nextRedundancyTimeout = outgoingCommand->sentTime + outgoingCommand->roundTripTimeout;

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

	peer->lastReceiveTime = host->serviceTime;
	peer->earliestTimeout = 0;

	/*
	just the throttle and roundtriptime
	*/

	receivedSequenceNumber = MRTP_NET_TO_HOST_16(command->redundancyAcknowledge.receivedSequenceNumber);
	nextUnackSequenceNumber = MRTP_NET_TO_HOST_16(command->redundancyAcknowledge.nextUnackSequenceNumber);

	mrtp_protocol_remove_sent_redundancy_command(peer, receivedSequenceNumber, nextUnackSequenceNumber);

	return 0;
}

//为host初始化一个peer
//处理连接请求，如果连接成功则将连接确认的command（verify command）移动到outgoing队列
//为host创建一个新的peer对象处理该连接
static MRtpPeer * mrtp_protocol_handle_connect(MRtpHost * host, MRtpProtocolHeader * header, MRtpProtocol * command)
{
	mrtp_uint8 incomingSessionID, outgoingSessionID;
	mrtp_uint32 mtu, windowSize;
	MRtpChannel * channel;
	size_t duplicatePeers = 0;
	MRtpPeer * currentPeer, *peer = NULL;
	MRtpProtocol verifyCommand;

	size_t channelCount = MRTP_PROTOCOL_CHANNEL_COUNT;


	for (currentPeer = host->peers; currentPeer < &host->peers[host->peerCount]; ++currentPeer) {
		//找到第一个已经断开连接的或者没有分配的位置
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

	peer->channels = (MRtpChannel *)mrtp_malloc(channelCount * sizeof(MRtpChannel));
	if (peer->channels == NULL)
		return NULL;
	peer->channelCount = channelCount;
	peer->state = MRTP_PEER_STATE_ACKNOWLEDGING_CONNECT;
	peer->connectID = command->connect.connectID;
	peer->address = host->receivedAddress;
	peer->outgoingPeerID = MRTP_NET_TO_HOST_16(command->connect.outgoingPeerID);
	peer->incomingBandwidth = MRTP_NET_TO_HOST_32(command->connect.incomingBandwidth);
	peer->outgoingBandwidth = MRTP_NET_TO_HOST_32(command->connect.outgoingBandwidth);
	peer->packetThrottleInterval = MRTP_NET_TO_HOST_32(command->connect.packetThrottleInterval);
	peer->packetThrottleAcceleration = MRTP_NET_TO_HOST_32(command->connect.packetThrottleAcceleration);
	peer->packetThrottleDeceleration = MRTP_NET_TO_HOST_32(command->connect.packetThrottleDeceleration);
	peer->eventData = MRTP_NET_TO_HOST_32(command->connect.data);

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

	for (channel = peer->channels; channel < &peer->channels[channelCount]; ++channel) {
		channel->outgoingSequenceNumber = 0;
		channel->incomingSequenceNumber = 0;

		mrtp_list_clear(&channel->incomingCommands);

		channel->usedReliableWindows = 0;
		memset(channel->commandWindows, 0, sizeof(channel->commandWindows));
	}

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

	verifyCommand.header.command = MRTP_PROTOCOL_COMMAND_VERIFY_CONNECT | MRTP_PROTOCOL_COMMAND_FLAG_ACKNOWLEDGE;
	verifyCommand.verifyConnect.outgoingPeerID = MRTP_HOST_TO_NET_16(peer->incomingPeerID);
	verifyCommand.verifyConnect.incomingSessionID = incomingSessionID;
	verifyCommand.verifyConnect.outgoingSessionID = outgoingSessionID;
	verifyCommand.verifyConnect.mtu = MRTP_HOST_TO_NET_32(peer->mtu);
	verifyCommand.verifyConnect.windowSize = MRTP_HOST_TO_NET_32(windowSize);
	verifyCommand.verifyConnect.incomingBandwidth = MRTP_HOST_TO_NET_32(host->incomingBandwidth);
	verifyCommand.verifyConnect.outgoingBandwidth = MRTP_HOST_TO_NET_32(host->outgoingBandwidth);
	verifyCommand.verifyConnect.packetThrottleInterval = MRTP_HOST_TO_NET_32(peer->packetThrottleInterval);
	verifyCommand.verifyConnect.packetThrottleAcceleration = MRTP_HOST_TO_NET_32(peer->packetThrottleAcceleration);
	verifyCommand.verifyConnect.packetThrottleDeceleration = MRTP_HOST_TO_NET_32(peer->packetThrottleDeceleration);
	verifyCommand.verifyConnect.connectID = peer->connectID;

	//发送连接确认的command
	mrtp_peer_queue_outgoing_command(peer, &verifyCommand, NULL, 0, 0);

	return peer;
}

//收到确认连接的command
//确认连接建立，确认重新分配带宽
static int mrtp_protocol_handle_verify_connect(MRtpHost * host, MRtpEvent * event,
	MRtpPeer * peer, const MRtpProtocol * command) {

	mrtp_uint32 mtu, windowSize;
	size_t channelCount;

	if (peer->state != MRTP_PEER_STATE_CONNECTING)
		return 0;

	channelCount = MRTP_PROTOCOL_CHANNEL_COUNT;

	//如果信息对不上= =。
	if (MRTP_NET_TO_HOST_32(command->verifyConnect.packetThrottleInterval) != peer->packetThrottleInterval ||
		MRTP_NET_TO_HOST_32(command->verifyConnect.packetThrottleAcceleration) != peer->packetThrottleAcceleration ||
		MRTP_NET_TO_HOST_32(command->verifyConnect.packetThrottleDeceleration) != peer->packetThrottleDeceleration ||
		command->verifyConnect.connectID != peer->connectID)
	{
		peer->eventData = 0;
		mrtp_protocol_dispatch_state(host, peer, MRTP_PEER_STATE_ZOMBIE);

		return -1;
	}

	// connect command的sequence number一定是1，所以这里删除sequence number为1的command
	mrtp_protocol_remove_sent_reliable_command(peer, 1, 0xFF);

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

	//清空peer的各种队列
	mrtp_peer_reset_queues(peer);

	if (peer->state == MRTP_PEER_STATE_CONNECTION_SUCCEEDED || peer->state == MRTP_PEER_STATE_DISCONNECTING
		|| peer->state == MRTP_PEER_STATE_CONNECTING)
		mrtp_protocol_dispatch_state(host, peer, MRTP_PEER_STATE_ZOMBIE);
	else if (peer->state != MRTP_PEER_STATE_CONNECTED && peer->state != MRTP_PEER_STATE_DISCONNECT_LATER) {
		if (peer->state == MRTP_PEER_STATE_CONNECTION_PENDING)
			host->recalculateBandwidthLimits = 1;
		mrtp_peer_reset(peer);
	}
	else if (command->header.command & MRTP_PROTOCOL_COMMAND_FLAG_ACKNOWLEDGE)
		mrtp_protocol_change_state(host, peer, MRTP_PEER_STATE_ACKNOWLEDGING_DISCONNECT);
	else
		mrtp_protocol_dispatch_state(host, peer, MRTP_PEER_STATE_ZOMBIE);

	if (peer->state != MRTP_PEER_STATE_DISCONNECTED)
		peer->eventData = MRTP_NET_TO_HOST_32(command->disconnect.data);

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
	mrtp_uint8 ** currentData, mrtp_uint16 sentTime)
{
	size_t dataLength;

	if (peer->state != MRTP_PEER_STATE_CONNECTED && peer->state != MRTP_PEER_STATE_DISCONNECT_LATER)
		return -1;

	dataLength = MRTP_NET_TO_HOST_16(command->sendReliable.dataLength);
	*currentData += dataLength;
	if (dataLength > host->maximumPacketSize ||
		*currentData < host->receivedData ||
		*currentData > & host->receivedData[host->receivedDataLength])
		return -1;

	if (mrtp_peer_queue_incoming_command(peer, command, (const mrtp_uint8 *)command + sizeof(MRtpProtocolSendReliable),
		dataLength, MRTP_PACKET_FLAG_RELIABLE, 0, sentTime) == NULL)
		return -1;

	return 0;
}

static int mrtp_protocol_handle_send_fragment(MRtpHost * host, MRtpPeer * peer,
	const MRtpProtocol * command, mrtp_uint8 ** currentData, mrtp_uint16 sentTime)
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
	startWindow = startSequenceNumber / MRTP_PEER_RELIABLE_WINDOW_SIZE;
	currentWindow = channel->incomingSequenceNumber / MRTP_PEER_RELIABLE_WINDOW_SIZE;

	if (startSequenceNumber < channel->incomingSequenceNumber)
		startWindow += MRTP_PEER_RELIABLE_WINDOWS;
	// 发送的数据包不在发送窗口的范围内
	if (startWindow < currentWindow || startWindow >= currentWindow + MRTP_PEER_FREE_RELIABLE_WINDOWS - 1)
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

	//首先去incomingReliableCommands中寻找stratCommand
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
			//找到了
			startCommand = incomingCommand;
			break;
		}
	}

	if (startCommand == NULL) {
		MRtpProtocol hostCommand = *command;

		hostCommand.header.sequenceNumber = startSequenceNumber;

		startCommand = mrtp_peer_queue_incoming_command(peer, &hostCommand, NULL, totalLength,
			MRTP_PACKET_FLAG_RELIABLE, fragmentCount, sentTime);
		if (startCommand == NULL)
			return -1;
	}

	//这里把所有的数据都集中到一个command上了
	if ((startCommand->fragments[fragmentNumber / 32] & (1 << (fragmentNumber % 32))) == 0) {
		--startCommand->fragmentsRemaining;

		startCommand->fragments[fragmentNumber / 32] |= (1 << (fragmentNumber % 32));

		if (fragmentOffset + fragmentLength > startCommand->packet->dataLength)
			fragmentLength = startCommand->packet->dataLength - fragmentOffset;

		memcpy(startCommand->packet->data + fragmentOffset,
			(mrtp_uint8 *)command + sizeof(MRtpProtocolSendFragment),
			fragmentLength);

		//等到所有的fragment都到了才dispatch
		if (startCommand->fragmentsRemaining <= 0)
			mrtp_peer_dispatch_incoming_reliable_commands(peer, channel);
	}

	return 0;
}

static int mrtp_protocol_handle_send_redundancy_noack(MRtpHost * host, MRtpPeer * peer,
	const MRtpProtocol * command, mrtp_uint8 ** currentData, mrtp_uint16 sentTime)
{
	size_t dataLength;
	if (peer->state != MRTP_PEER_STATE_CONNECTED && peer->state != MRTP_PEER_STATE_DISCONNECT_LATER)
		return -1;
	dataLength = MRTP_NET_TO_HOST_16(command->sendRedundancyNoAck.datalength);

	*currentData += dataLength;
	if (dataLength > host->maximumPacketSize ||
		*currentData < host->receivedData ||
		*currentData > & host->receivedData[host->receivedDataLength])
		return -1;

	if (mrtp_peer_queue_incoming_command(peer, command, (const mrtp_uint8 *)command + sizeof(MRtpProtocolSendRedundancyNoAck),
		dataLength, MRTP_PACKET_FLAG_REDUNDANCY_NO_ACK, 0, sentTime) == NULL)
	{
		return -1;
	}

	return 0;

}

static int mrtp_protocol_handle_send_redundancy_fragment_noack(MRtpHost* host, MRtpPeer * peer,
	const MRtpProtocol * command, mrtp_uint8 ** currentData, mrtp_uint16 sentTime)
{
	mrtp_uint32 fragmentNumber, fragmentCount, fragmentOffset, fragmentLength;
	mrtp_uint32 sequenceNumber, startSequenceNumber;
	mrtp_uint32 totalLength;
	mrtp_uint16 startWindow, currentWindow;
	MRtpChannel * channel;
	MRtpListIterator currentCommand;
	MRtpIncomingCommand * startCommand = NULL;

	fragmentLength = MRTP_NET_TO_HOST_16(command->sendRedundancyFragementNoAck.dataLength);

	*currentData += fragmentLength;
	if (fragmentLength > host->maximumPacketSize || *currentData < host->receivedData ||
		*currentData > & host->receivedData[host->receivedDataLength])
		return -1;

	mrtp_uint8 channelID = channelIDs[command->header.command & MRTP_PROTOCOL_COMMAND_MASK];
	channel = &peer->channels[channelID];
	sequenceNumber = command->header.sequenceNumber;
	startSequenceNumber = MRTP_NET_TO_HOST_16(command->sendRedundancyFragementNoAck.startSequenceNumber);
	startWindow = startSequenceNumber / MRTP_PEER_RELIABLE_WINDOW_SIZE;

	currentWindow = channel->incomingSequenceNumber / MRTP_PEER_RELIABLE_WINDOW_SIZE;
	if (startSequenceNumber < channel->incomingSequenceNumber)
		startWindow += MRTP_PEER_RELIABLE_WINDOWS;
	if (startWindow < currentWindow || startWindow >= currentWindow + MRTP_PEER_FREE_RELIABLE_WINDOWS - 1)
		return 0;

	fragmentNumber = MRTP_NET_TO_HOST_32(command->sendRedundancyFragementNoAck.fragmentNumber);
	fragmentCount = MRTP_NET_TO_HOST_32(command->sendRedundancyFragementNoAck.fragmentCount);
	fragmentOffset = MRTP_NET_TO_HOST_32(command->sendRedundancyFragementNoAck.fragmentOffset);
	totalLength = MRTP_NET_TO_HOST_32(command->sendRedundancyFragementNoAck.totalLength);

	if (fragmentCount > MRTP_PROTOCOL_MAXIMUM_FRAGMENT_COUNT || fragmentNumber >= fragmentCount ||
		totalLength > host->maximumPacketSize || fragmentOffset >= totalLength ||
		fragmentLength > totalLength - fragmentOffset)
		return -1;

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
			MRTP_PACKET_FLAG_REDUNDANCY_NO_ACK, fragmentCount, sentTime);

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
			(mrtp_uint8 *)command + sizeof(MRtpProtocolSendRedundancyFragementNoAck),
			fragmentLength);

		if (startCommand->fragmentsRemaining <= 0)
			mrtp_peer_dispatch_incoming_redundancy_noack_commands(peer, channel);
	}

	return 0;

}

static int mrtp_protocol_handle_send_redundancy(MRtpHost * host, MRtpPeer * peer,
	const MRtpProtocol * command, mrtp_uint8 ** currentData, mrtp_uint16 sentTime)
{
	size_t dataLength;
	if (peer->state != MRTP_PEER_STATE_CONNECTED && peer->state != MRTP_PEER_STATE_DISCONNECT_LATER)
		return -1;
	dataLength = MRTP_NET_TO_HOST_16(command->sendRedundancy.dataLength);

	*currentData += dataLength;
	if (dataLength > host->maximumPacketSize ||
		*currentData < host->receivedData ||
		*currentData > & host->receivedData[host->receivedDataLength])
		return -1;

	if (mrtp_peer_queue_incoming_command(peer, command, (const mrtp_uint8 *)command + sizeof(MRtpProtocolSendRedundancy),
		dataLength, MRTP_PACKET_FLAG_REDUNDANCY, 0, sentTime) == NULL)
	{
		return -1;
	}

	return 0;
}

static int mrtp_protocol_handle_set_quick_retransmit(MRtpHost * host, MRtpPeer * peer, const MRtpProtocol * command) {

	if (peer->state != MRTP_PEER_STATE_CONNECTED && peer->state != MRTP_PEER_STATE_DISCONNECT_LATER)
		return -1;

	peer->quickRetransmitNum = MRTP_NET_TO_HOST_16(command->setQuickRestrnsmit.quickRetransmit);

	if (peer->quickRetransmitNum > MRTP_PROTOCOL_MAXIMUM_QUICK_RETRANSMIT)
		peer->quickRetransmitNum = MRTP_PROTOCOL_MAXIMUM_QUICK_RETRANSMIT;
	else if (peer->quickRetransmitNum < MRTP_PROTOCOL_MINIMUM_QUICK_RETRANSMIT)
		peer->quickRetransmitNum = MRTP_PROTOCOL_MINIMUM_QUICK_RETRANSMIT;

#ifdef SENDANDRECEIVE
	printf("set quickRetrainsmitNum: %d successfully.\n", peer->quickRetransmitNum);
#endif

	return 0;
}

static int mrtp_protocol_handle_send_retransmit_redundancy(MRtpHost * host, MRtpPeer * peer,
	const MRtpProtocol * command, mrtp_uint8 ** currentData, mrtp_uint16 sentTime)
{
	size_t dataLength;
	if (peer->state != MRTP_PEER_STATE_CONNECTED && peer->state != MRTP_PEER_STATE_DISCONNECT_LATER)
		return -1;
	dataLength = MRTP_NET_TO_HOST_16(command->retransmitRedundancy.dataLength);

	*currentData += dataLength;
	if (dataLength > host->maximumPacketSize ||
		*currentData < host->receivedData ||
		*currentData > & host->receivedData[host->receivedDataLength])
		return -1;

	if (mrtp_peer_queue_retransmit_redundancy_command(peer, command, (const mrtp_uint8 *)command + sizeof(MRtpProtocolRetransmitRedundancy),
		dataLength, MRTP_PACKET_FLAG_REDUNDANCY, 0, sentTime) == NULL)
	{
		return -1;
	}

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
			((host->receivedAddress.host != peer->address.host ||
				host->receivedAddress.port != peer->address.port) &&
				peer->address.host != MRTP_HOST_BROADCAST) ||
				(peer->outgoingPeerID < MRTP_PROTOCOL_MAXIMUM_PEER_ID &&
					sessionID != peer->incomingSessionID))
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

		//如果peer不存在且不是请求连接的command，则跳出循环
		if (peer == NULL && commandNumber != MRTP_PROTOCOL_COMMAND_CONNECT)
			break;
		else if (peer != NULL) {
			peer->lastReceiveTime = host->serviceTime;
		}

		command->header.sequenceNumber = MRTP_NET_TO_HOST_16(command->header.sequenceNumber);
#ifdef SENDANDRECEIVE
		printf("receive [%s]: (%d) from peer: <%d> at channel: [%d]", commandName[commandNumber],
			command->header.sequenceNumber, peerID, channelIDs[command->header.command & MRTP_PROTOCOL_COMMAND_MASK]);
		if (command->header.command & MRTP_PROTOCOL_COMMAND_MASK == MRTP_PROTOCOL_COMMAND_RETRANSMIT_REDUNDANCY) {
			printf(" for redundancy seq: [%d]", MRTP_NET_TO_HOST_16(command->retransmitRedundancy.retransmitSequenceNumber));
		}
		printf("\n");
#endif // SENDANDRECEIVE

		mrtp_uint16 sentTime;
		sentTime = (flags & MRTP_PROTOCOL_HEADER_FLAG_SENT_TIME) ? MRTP_NET_TO_HOST_16(header->sentTime) : 0;

		switch (commandNumber) {

		case MRTP_PROTOCOL_COMMAND_ACKNOWLEDGE:
			if (mrtp_protocol_handle_acknowledge(host, event, peer, command))
				goto commandError;
			break;

		case MRTP_PROTOCOL_COMMAND_REDUNDANCY_ACKNOWLEDGE:
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
			if (mrtp_protocol_handle_send_reliable(host, peer, command, &currentData, sentTime))
				goto commandError;
			break;

		case MRTP_PROTOCOL_COMMAND_SEND_FRAGMENT:
			if (mrtp_protocol_handle_send_fragment(host, peer, command, &currentData, sentTime))
				goto commandError;
			break;

		case MRTP_PROTOCOL_COMMAND_SEND_REDUNDANCY_NO_ACK:
			if (mrtp_protocol_handle_send_redundancy_noack(host, peer, command, &currentData, sentTime))
				goto commandError;
			break;

		case MRTP_PROTOCOL_COMMAND_SEND_REDUNDANCY_FRAGEMENT_NO_ACK:
			if (mrtp_protocol_handle_send_redundancy_fragment_noack(host, peer, command, &currentData, sentTime))
				goto commandError;
			break;

		case MRTP_PROTOCOL_COMMAND_SET_QUICK_RETRANSMIT:
			if (mrtp_protocol_handle_set_quick_retransmit(host, peer, command))
				goto commandError;
			break;

		case MRTP_PROTOCOL_COMMAND_SEND_REDUNDANCY:
			if (mrtp_protocol_handle_send_redundancy(host, peer, command, &currentData, sentTime))
				goto commandError;
			break;

		case MRTP_PROTOCOL_COMMAND_RETRANSMIT_REDUNDANCY:
			if (mrtp_protocol_handle_send_retransmit_redundancy(host, peer, command, &currentData, sentTime))
				goto commandError;
			break;
			/*
			case MRTP_PROTOCOL_COMMAND_THROTTLE_CONFIGURE:
			if (mrtp_protocol_handle_throttle_configure(host, peer, command))
			goto commandError;
			break;*/

		default:
			goto commandError;
		}

		//如果该command需要发送确认请求并且该peer的状态没有断开，则将该command放在acknowledge中
		if (peer != NULL && (command->header.command & MRTP_PROTOCOL_COMMAND_FLAG_ACKNOWLEDGE) != 0) {

			if (!(flags & MRTP_PROTOCOL_HEADER_FLAG_SENT_TIME))
				break;

			sentTime = MRTP_NET_TO_HOST_16(header->sentTime);

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
	}

commandError:
	if (event != NULL && event->type != MRTP_EVENT_TYPE_NONE)
		return 1;

	return 0;
}

static int mrtp_protocol_receive_incoming_commands(MRtpHost * host, MRtpEvent * event) {

	int packets;

	// 至多接收256次，如果遇到错误或者没有待接收的数据就返回
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

		if (receivedLength == 0)
			return 0;

#ifdef SENDANDRECEIVE
		printf("receive %d\n", receivedLength);
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

			//等待连接或者连接成功，host、peer状态变为已经连接，事件类型为连接
		case MRTP_PEER_STATE_CONNECTION_PENDING:
		case MRTP_PEER_STATE_CONNECTION_SUCCEEDED:
			mrtp_protocol_change_state(host, peer, MRTP_PEER_STATE_CONNECTED);

			event->type = MRTP_EVENT_TYPE_CONNECT;
			event->peer = peer;
			event->data = peer->eventData;

			return 1;

		case MRTP_PEER_STATE_ZOMBIE:
			host->recalculateBandwidthLimits = 1;

			event->type = MRTP_EVENT_TYPE_DISCONNECT;
			event->peer = peer;
			event->data = peer->eventData;

			mrtp_peer_reset(peer);

			return 1;

		case MRTP_PEER_STATE_CONNECTED:
			if (mrtp_list_empty(&peer->dispatchedCommands))
				continue;
			//设置包的同时修改event的channelID
			event->packet = mrtp_peer_receive(peer, &event->channelID);
			if (event->packet == NULL)
				continue;

			event->type = MRTP_EVENT_TYPE_RECEIVE;
			event->peer = peer;

			if (!mrtp_list_empty(&peer->dispatchedCommands)) {
				peer->needsDispatch = 1;
				//把该peer加入到host的处理队列中
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
		//距离上次做流量控制经过的时间大于1秒，则进行流量控制
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

		//switch (mrtp_protocol_send_redundancy_outgoing_commands(host, event, 1)) {
		//case 1:
		//	return 1;
		//case -1:
		//	return -1;
		//default:
		//	break;
		//}

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
		//如果超时了没有任何事件产生，则返回
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