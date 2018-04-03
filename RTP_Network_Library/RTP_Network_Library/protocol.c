#include <stdio.h>
#include <string.h>
#define MRTP_BUILDING_LIB 1
#include "utility.h"
#include "time.h"
#include "mrtp.h"

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
	sizeof(MRtpProtocolSendRedundancyFragementNoAck)
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
	"SendRedundancyFragementNoAck"
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

#ifdef SENDANDRECEIVE
		printf("add buffer [ack]: (%d) at channel: [%d]\n",
			MRTP_NET_TO_HOST_16(command->header.sequenceNumber),
			channelIDs[acknowledgement->command.header.command & MRTP_PROTOCOL_COMMAND_MASK]);
#endif // SENDANDRECEIVE

		//如果是断开连接的确认，将peer状态改为ZOMBIE
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

// 检测peer->sentReliable队列中的所有command是否超时
// 如果超时则判断是否断开连接或者重发该包
static int mrtp_protocol_check_timeouts(MRtpHost * host, MRtpPeer * peer, MRtpEvent * event) {

	MRtpOutgoingCommand * outgoingCommand;
	MRtpListIterator currentCommand, insertPosition;

	currentCommand = mrtp_list_begin(&peer->sentReliableCommands);
	insertPosition = mrtp_list_begin(&peer->outgoingReliableCommands);

	while (currentCommand != mrtp_list_end(&peer->sentReliableCommands)) {

		outgoingCommand = (MRtpOutgoingCommand *)currentCommand;

		currentCommand = mrtp_list_next(currentCommand);

		// 如果发送的包没有超时
		if (MRTP_TIME_DIFFERENCE(host->serviceTime, outgoingCommand->sentTime) < outgoingCommand->roundTripTimeout)
			continue;

		//设置peer距上次收到ack后的超时的时间
		//erliestTimeout在收到ack时会重置
		if (peer->earliestTimeout == 0 || MRTP_TIME_LESS(outgoingCommand->sentTime, peer->earliestTimeout))
			peer->earliestTimeout = outgoingCommand->sentTime;

		// 判断是否断开连接
		if (MRTP_TIME_DIFFERENCE(host->serviceTime, peer->earliestTimeout) >= peer->timeoutMaximum ||
			(outgoingCommand->roundTripTimeout >= outgoingCommand->roundTripTimeoutLimit &&
				MRTP_TIME_DIFFERENCE(host->serviceTime, peer->earliestTimeout) >= peer->timeoutMinimum))
		{
			mrtp_protocol_notify_disconnect(host, peer, event);
			return 1;
		}

		//接下来是丢包的情况
		if (outgoingCommand->packet != NULL)
			peer->reliableDataInTransit -= outgoingCommand->fragmentLength;
		++peer->packetsLost;

		outgoingCommand->roundTripTimeout *= 2;

#ifdef PACKETLOSSDEBUG
		printf("seqnum [%d] Loss! change rto to: [%d]\n",
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

void mrtp_protocol_remove_redundancy_buffer_commands(MRtpRedundancyBuffer* mrtpRedundancyBuffer) {

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

		//要发送数据的大小超过了总的发送窗口大小的限制
		if (outgoingCommand->packet != NULL) {
			if (!windowExceeded) {
				mrtp_uint32 windowSize = (peer->packetThrottle * peer->windowSize) / MRTP_PEER_PACKET_THROTTLE_SCALE;

				if (peer->reliableDataInTransit + outgoingCommand->fragmentLength > MRTP_MAX(windowSize, peer->mtu))
					windowExceeded = 1;
			}
			if (windowExceeded) {
				currentCommand = mrtp_list_next(currentCommand);

				continue;	//可能还有需要发送的指令command
			}
		}

		canPing = 0;

		//host需要发送的数据满了，跳出循环
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

		//设置该command的roundTripTimeout
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
		printf("add buffer [%s]: (%d) at channel[%d]\n",
			commandName[outgoingCommand->command.header.command & MRTP_PROTOCOL_COMMAND_MASK],
			MRTP_NET_TO_HOST_16(outgoingCommand->command.header.sequenceNumber), channelID);
#endif // SENDANDRECEIVE

		++peer->packetsSent;

		++command;
		++buffer;
	}

	host->commandCount = command - host->commands;
	host->bufferCount = buffer - host->buffers;

	return canPing;
}

// 发送的时候采用冗余发送的方式
static int mrtp_protocol_send_redundancy_noack_commands(MRtpHost * host, MRtpPeer * peer) {

	size_t redundancyNum = peer->currentRedundancyNoAckBufferNum;
	MRtpRedundancyBuffer* currentRedundancyBuffer = &peer->redundancyNoAckBuffers[redundancyNum];

	if (currentRedundancyBuffer->buffercount != 0) {
		mrtp_protocol_remove_redundancy_buffer_commands(currentRedundancyBuffer);
		currentRedundancyBuffer->buffercount = 0;
		currentRedundancyBuffer->packetSize = 0;
	}

	MRtpBuffer * buffer = &currentRedundancyBuffer->buffers[currentRedundancyBuffer->buffercount];
	MRtpOutgoingCommand * outgoingCommand;
	MRtpListIterator currentCommand;
	size_t commandSize;
	mrtp_uint32 redundancyMtu = (peer->mtu - sizeof(MRtpProtocolHeader)) / peer->redundancyNum;

	currentCommand = mrtp_list_begin(&peer->outgoingRedundancyNoAckCommands);
	while (currentCommand != mrtp_list_end(&peer->outgoingRedundancyNoAckCommands)) {

		outgoingCommand = (MRtpOutgoingCommand *)currentCommand;
		commandSize = commandSizes[outgoingCommand->command.header.command & MRTP_PROTOCOL_COMMAND_MASK];

		if (buffer + 1 >= &currentRedundancyBuffer->buffers[sizeof(currentRedundancyBuffer->buffers) / sizeof(MRtpBuffer)] ||
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

			MRtpRedundancyBuffer* currentRedundancyNoackBuffer = &currentPeer->redundancyNoAckBuffers[currentPeer->currentRedundancyNoAckBufferNum];

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
					int num = (currentPeer->currentRedundancyNoAckBufferNum - i + currentPeer->redundancyNum)
						% currentPeer->redundancyNum;
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
					% currentPeer->redundancyNum;
				currentRedundancyNoackBuffer = &currentPeer->redundancyNoAckBuffers[currentPeer->currentRedundancyNoAckBufferNum];
				if (currentRedundancyNoackBuffer->buffercount != 0) {
					mrtp_protocol_remove_redundancy_buffer_commands(currentRedundancyNoackBuffer);
					currentRedundancyNoackBuffer->buffercount = 0;
					currentRedundancyNoackBuffer->packetSize = 0;
				}
			}
		}
	}
	return 0;
}

static int mrtp_protocol_send_reliable_outgoing_commands(MRtpHost * host, MRtpEvent * event, int checkForTimeouts) {

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

			// 首先处理ack
			if (!mrtp_list_empty(&currentPeer->acknowledgements))
				mrtp_protocol_send_acknowledgements(host, currentPeer);

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

			if ((mrtp_list_empty(&currentPeer->outgoingReliableCommands) ||
				mrtp_protocol_send_reliable_commands(host, currentPeer)) && //尝试将outging中的command转移到sent中
				mrtp_list_empty(&currentPeer->sentReliableCommands) &&				//转移后sent还是空的
				MRTP_TIME_DIFFERENCE(host->serviceTime, currentPeer->lastReceiveTime) >= currentPeer->pingInterval && //距离上次接收包的时间 > pingInterval
				currentPeer->mtu - host->packetSize >= sizeof(MRtpProtocolPing))	//还有空间ping一下
			{
				mrtp_peer_ping(currentPeer);								//将ping的包放在outgoing command下
				mrtp_protocol_send_reliable_commands(host, currentPeer);	//将outgoing command下的command放到sent队列下
			}

			if (host->commandCount == 0)
				continue;

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

			if (host->bufferCount > 1) {
				//设置host的header
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

				sentLength = mrtp_socket_send(host->socket, &currentPeer->address, host->buffers, host->bufferCount);
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

//处理确认请求
static int mrtp_protocol_handle_acknowledge(MRtpHost * host, MRtpEvent * event,
	MRtpPeer * peer, const MRtpProtocol * command)
{
	mrtp_uint32 roundTripTime, receivedSentTime, receivedReliableSequenceNumber;
	MRtpProtocolCommand commandNumber;

	//如果断开连接或者准备断开连接，则不处理
	if (peer->state == MRTP_PEER_STATE_DISCONNECTED || peer->state == MRTP_PEER_STATE_ZOMBIE)
		return 0;

	receivedSentTime = MRTP_NET_TO_HOST_16(command->acknowledge.receivedSentTime);
	receivedSentTime |= host->serviceTime & 0xFFFF0000;	//将时间|上服务器时间的高位
	if ((receivedSentTime & 0x8000) > (host->serviceTime & 0x8000))	//如果时间轮了一圈，因为只有前16位是对端的
		receivedSentTime -= 0x10000;

	if (MRTP_TIME_LESS(host->serviceTime, receivedSentTime))
		return 0;

	peer->lastReceiveTime = host->serviceTime;
	peer->earliestTimeout = 0;
	//command的传输时间
	roundTripTime = MRTP_TIME_DIFFERENCE(host->serviceTime, receivedSentTime);
	//根据roundTripTime调节throttle
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

	//如果之前没有进行过流量控制，或者流量控制的时间间隔超过一定的时间
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

	commandNumber = mrtp_protocol_remove_sent_reliable_command(peer, receivedReliableSequenceNumber);

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
		// 将数据发送完后再断开连接
		if (mrtp_list_empty(&peer->outgoingReliableCommands) && mrtp_list_empty(&peer->sentReliableCommands))
			mrtp_peer_disconnect(peer, peer->eventData);
		break;

	default:
		break;
	}

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

//从sentReliableCommands队列中确认之前发送的command
static MRtpProtocolCommand mrtp_protocol_remove_sent_reliable_command(MRtpPeer * peer,
	mrtp_uint16 reliableSequenceNumber) {

	MRtpOutgoingCommand * outgoingCommand = NULL;
	MRtpListIterator currentCommand;
	MRtpProtocolCommand commandNumber;
	int wasSent = 1;

	for (currentCommand = mrtp_list_begin(&peer->sentReliableCommands);
		currentCommand != mrtp_list_end(&peer->sentReliableCommands);
		currentCommand = mrtp_list_next(currentCommand))
	{
		outgoingCommand = (MRtpOutgoingCommand *)currentCommand;
		if (outgoingCommand->sequenceNumber == reliableSequenceNumber)
			break;
	}

	//如果sentReliableCommands中没有找到响应的command，
	//则在outgoingReliableCommands中继续检索（可能已经被判定为超时准备重新发送了）
	if (currentCommand == mrtp_list_end(&peer->sentReliableCommands)) {

		for (currentCommand = mrtp_list_begin(&peer->outgoingReliableCommands);
			currentCommand != mrtp_list_end(&peer->outgoingReliableCommands);
			currentCommand = mrtp_list_next(currentCommand))
		{
			outgoingCommand = (MRtpOutgoingCommand *)currentCommand;

			if (outgoingCommand->sendAttempts < 1) return MRTP_PROTOCOL_COMMAND_NONE;

			if (outgoingCommand->sequenceNumber == reliableSequenceNumber)
				break;
		}

		if (currentCommand == mrtp_list_end(&peer->outgoingReliableCommands))
			return MRTP_PROTOCOL_COMMAND_NONE;

		wasSent = 0;
	}

	if (outgoingCommand == NULL)
		return MRTP_PROTOCOL_COMMAND_NONE;

	mrtp_uint8 channelID = channelIDs[outgoingCommand->command.header.command& MRTP_PROTOCOL_COMMAND_MASK];

	if (channelID < peer->channelCount) {

		MRtpChannel * channel = &peer->channels[channelID];
		mrtp_uint16 reliableWindow = reliableSequenceNumber / MRTP_PEER_RELIABLE_WINDOW_SIZE;
		//将其占用的相应的窗口移除
		if (channel->commandWindows[reliableWindow] > 0) {
			--channel->commandWindows[reliableWindow];
			if (!channel->commandWindows[reliableWindow])
				channel->usedReliableWindows &= ~(1 << reliableWindow);
		}
	}

	commandNumber = (MRtpProtocolCommand)(outgoingCommand->command.header.command & MRTP_PROTOCOL_COMMAND_MASK);

	//从相应的队列中移除这个command
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

	//设置发送队列的下次包的超时时间
	outgoingCommand = (MRtpOutgoingCommand *)mrtp_list_front(&peer->sentReliableCommands);
	peer->nextTimeout = outgoingCommand->sentTime + outgoingCommand->roundTripTimeout;

	//返回相应的command number
	return commandNumber;
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
	else
		if (peer->incomingBandwidth == 0 || host->outgoingBandwidth == 0)
			peer->windowSize = (MRTP_MAX(peer->incomingBandwidth, host->outgoingBandwidth) /
				MRTP_PEER_WINDOW_SIZE_SCALE) * MRTP_PROTOCOL_MINIMUM_WINDOW_SIZE;
		else
			peer->windowSize = (MRTP_MIN(peer->incomingBandwidth, host->outgoingBandwidth) /
				MRTP_PEER_WINDOW_SIZE_SCALE) * MRTP_PROTOCOL_MINIMUM_WINDOW_SIZE;

	if (peer->windowSize < MRTP_PROTOCOL_MINIMUM_WINDOW_SIZE)
		peer->windowSize = MRTP_PROTOCOL_MINIMUM_WINDOW_SIZE;
	else
		if (peer->windowSize > MRTP_PROTOCOL_MAXIMUM_WINDOW_SIZE)
			peer->windowSize = MRTP_PROTOCOL_MAXIMUM_WINDOW_SIZE;

	return 0;
}

static int mrtp_protocol_handle_send_reliable(MRtpHost * host, MRtpPeer * peer, const MRtpProtocol * command,
	mrtp_uint8 ** currentData)
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

		startCommand = mrtp_peer_queue_incoming_command(peer, &hostCommand, NULL, totalLength, MRTP_PACKET_FLAG_RELIABLE, fragmentCount);
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
	const MRtpProtocol * command, mrtp_uint8 ** currentData)
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

	if (mrtp_peer_queue_incoming_command(peer, command, (const mrtp_uint8 *)command + sizeof(MRtpProtocolSendReliable),
		dataLength, MRTP_PACKET_FLAG_RELIABLE, 0) == NULL)
		return -1;

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

		command->header.sequenceNumber = MRTP_NET_TO_HOST_16(command->header.sequenceNumber);
#ifdef SENDANDRECEIVE
		printf("receive [%s]: (%d) from peer: <%d> at channel: [%d]\n", commandName[commandNumber],
			command->header.sequenceNumber, peerID, channelIDs[command->header.command & MRTP_PROTOCOL_COMMAND_MASK]);
#endif // SENDANDRECEIVE

		switch (commandNumber) {

		case MRTP_PROTOCOL_COMMAND_ACKNOWLEDGE:
			if (mrtp_protocol_handle_acknowledge(host, event, peer, command))
				goto commandError;
			break;

			//处理连接请求
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

			mrtp_uint16 sentTime;

			if (!(flags & MRTP_PROTOCOL_HEADER_FLAG_SENT_TIME))
				break;

			sentTime = MRTP_NET_TO_HOST_16(header->sentTime);

			switch (peer->state) {

			case MRTP_PEER_STATE_DISCONNECTING:
			case MRTP_PEER_STATE_ACKNOWLEDGING_CONNECT:	//不用发ack了
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

		//如果遇到event，则返回
		//将与host相连的peer中的所有outgoingcommand发送出去
		switch (mrtp_protocol_send_reliable_outgoing_commands(host, event, 1)) {
		case 1:
			return 1;
		case -1:
			return -1;
		default:
			break;
		}

		switch (mrtp_protocol_send_redundancy_outgoing_commands(host, event, 1)) {
		case 1:
			return 1;
		case -1:
			return -1;
		default:
			break;
		}

		//接收command
		switch (mrtp_protocol_receive_incoming_commands(host, event)) {
		case 1:
			return 1;
		case -1:
			return -1;
		default:
			break;
		}
		//刚才接收command后将需要发送的ack发送出去
		switch (mrtp_protocol_send_reliable_outgoing_commands(host, event, 1)) {
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

	mrtp_protocol_send_reliable_outgoing_commands(host, NULL, 0);
	mrtp_protocol_send_redundancy_outgoing_commands(host, NULL, 0);
}