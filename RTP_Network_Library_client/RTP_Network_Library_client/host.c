#include <string.h>
#include "mrtp.h"

MRtpHost * mrtp_host_create(const MRtpAddress * address, size_t peerCount,
	mrtp_uint32 incomingBandwidth, mrtp_uint32 outgoingBandwidth) {

	MRtpHost * host;
	MRtpPeer * currentPeer;

	if (peerCount > MRTP_PROTOCOL_MAXIMUM_PEER_ID)
		return NULL;

	host = (MRtpHost *)mrtp_malloc(sizeof(MRtpHost));
	if (host == NULL)
		return NULL;
	memset(host, 0, sizeof(MRtpHost));

	host->peers = (MRtpPeer *)mrtp_malloc(peerCount * sizeof(MRtpPeer));
	if (host->peers == NULL) {
		mrtp_free(host);

		return NULL;
	}
	memset(host->peers, 0, peerCount * sizeof(MRtpPeer));

	host->socket = mrtp_socket_create(MRTP_SOCKET_TYPE_DATAGRAM);
	if (host->socket == MRTP_SOCKET_NULL || (address != NULL && mrtp_socket_bind(host->socket, address) < 0)) {
		if (host->socket != MRTP_SOCKET_NULL)
			mrtp_socket_destroy(host->socket);

		mrtp_free(host->peers);
		mrtp_free(host);

		return NULL;
	}

	mrtp_socket_set_option(host->socket, MRTP_SOCKOPT_NONBLOCK, 1);
	mrtp_socket_set_option(host->socket, MRTP_SOCKOPT_BROADCAST, 1);
	mrtp_socket_set_option(host->socket, MRTP_SOCKOPT_RCVBUF, MRTP_HOST_RECEIVE_BUFFER_SIZE);
	mrtp_socket_set_option(host->socket, MRTP_SOCKOPT_SNDBUF, MRTP_HOST_SEND_BUFFER_SIZE);

	if (address != NULL && mrtp_socket_get_address(host->socket, &host->address) < 0)
		host->address = *address;

	host->randomSeed = (mrtp_uint32)(size_t)host;
	host->randomSeed += mrtp_host_random_seed();
	host->randomSeed = (host->randomSeed << 16) | (host->randomSeed >> 16);
	host->incomingBandwidth = incomingBandwidth;
	host->outgoingBandwidth = outgoingBandwidth;
	host->bandwidthThrottleEpoch = 0;
	host->recalculateBandwidthLimits = 0;
	host->mtu = MRTP_HOST_DEFAULT_MTU;
	host->peerCount = peerCount;
	host->commandCount = 0;
	host->bufferCount = 0;
	host->receivedAddress.host = MRTP_HOST_ANY;
	host->receivedAddress.port = 0;
	host->receivedData = NULL;
	host->receivedDataLength = 0;

	host->totalSentData = 0;
	host->totalSentPackets = 0;
	host->totalReceivedData = 0;
	host->totalReceivedPackets = 0;

	host->connectedPeers = 0;
	host->bandwidthLimitedPeers = 0;
	host->duplicatePeers = MRTP_PROTOCOL_MAXIMUM_PEER_ID;
	host->maximumPacketSize = MRTP_HOST_DEFAULT_MAXIMUM_PACKET_SIZE;
	host->maximumWaitingData = MRTP_HOST_DEFAULT_MAXIMUM_WAITING_DATA;

	host->redundancyNum = MRTP_PROTOCOL_DEFAULT_REDUNDANCY_NUM;
	host->openQuickRetransmit = 0;

	mrtp_list_clear(&host->dispatchQueue);

	//初始化peers数组的信息
	for (currentPeer = host->peers; currentPeer < &host->peers[host->peerCount]; ++currentPeer) {

		currentPeer->host = host;
		currentPeer->incomingPeerID = currentPeer - host->peers;
		currentPeer->outgoingSessionID = currentPeer->incomingSessionID = 0xFF;
		currentPeer->data = NULL;

		mrtp_list_clear(&currentPeer->acknowledgements);
		mrtp_list_clear(&currentPeer->redundancyAcknowledgemets);
		mrtp_list_clear(&currentPeer->sentReliableCommands);
		mrtp_list_clear(&currentPeer->sentRedundancyNoAckCommands);
		mrtp_list_clear(&currentPeer->outgoingReliableCommands);
		mrtp_list_clear(&currentPeer->dispatchedCommands);
		mrtp_list_clear(&currentPeer->outgoingRedundancyCommands);
		mrtp_list_clear(&currentPeer->outgoingRedundancyNoAckCommands);
		mrtp_list_clear(&currentPeer->sentRedundancyCommands);
		mrtp_list_clear(&currentPeer->readytoDeleteRedundancyCommands);

		mrtp_peer_reset(currentPeer);
	}

	return host;
}

MRtpPeer *mrtp_host_connect(MRtpHost * host, const MRtpAddress * address) {
	MRtpPeer * currentPeer;
	MRtpChannel * channel;
	MRtpProtocol command;

	for (currentPeer = host->peers; currentPeer < &host->peers[host->peerCount]; ++currentPeer) {
		//找到第一个可用的peer
		if (currentPeer->state == MRTP_PEER_STATE_DISCONNECTED)
			break;
	}

	if (currentPeer >= &host->peers[host->peerCount])
		return NULL;

	currentPeer->channels = (MRtpChannel *)mrtp_malloc(MRTP_PROTOCOL_CHANNEL_COUNT * sizeof(MRtpChannel));
	if (currentPeer->channels == NULL)
		return NULL;
	currentPeer->channelCount = MRTP_PROTOCOL_CHANNEL_COUNT;
	currentPeer->state = MRTP_PEER_STATE_CONNECTING;
	currentPeer->address = *address;
	currentPeer->connectID = ++host->randomSeed;

	if (host->outgoingBandwidth == 0)
		currentPeer->windowSize = MRTP_PROTOCOL_MAXIMUM_WINDOW_SIZE;
	else
		currentPeer->windowSize = (host->outgoingBandwidth / MRTP_PEER_WINDOW_SIZE_SCALE) *
		MRTP_PROTOCOL_MINIMUM_WINDOW_SIZE;

	if (currentPeer->windowSize < MRTP_PROTOCOL_MINIMUM_WINDOW_SIZE)
		currentPeer->windowSize = MRTP_PROTOCOL_MINIMUM_WINDOW_SIZE;
	else if (currentPeer->windowSize > MRTP_PROTOCOL_MAXIMUM_WINDOW_SIZE)
		currentPeer->windowSize = MRTP_PROTOCOL_MAXIMUM_WINDOW_SIZE;

	for (channel = currentPeer->channels; channel < &currentPeer->channels[MRTP_PROTOCOL_CHANNEL_COUNT]; ++channel) {

		channel->outgoingSequenceNumber = 0;
		channel->incomingSequenceNumber = 0;

		mrtp_list_clear(&channel->incomingCommands);

		channel->usedReliableWindows = 0;
		memset(channel->commandWindows, 0, sizeof(channel->commandWindows));
	}

	command.header.command = MRTP_PROTOCOL_COMMAND_CONNECT | MRTP_PROTOCOL_COMMAND_FLAG_ACKNOWLEDGE;
	command.connect.outgoingPeerID = MRTP_HOST_TO_NET_16(currentPeer->incomingPeerID);
	command.connect.incomingSessionID = currentPeer->incomingSessionID;
	command.connect.outgoingSessionID = currentPeer->outgoingSessionID;
	command.connect.mtu = MRTP_HOST_TO_NET_32(currentPeer->mtu);
	command.connect.windowSize = MRTP_HOST_TO_NET_32(currentPeer->windowSize);
	command.connect.incomingBandwidth = MRTP_HOST_TO_NET_32(host->incomingBandwidth);
	command.connect.outgoingBandwidth = MRTP_HOST_TO_NET_32(host->outgoingBandwidth);
	command.connect.packetThrottleInterval = MRTP_HOST_TO_NET_32(currentPeer->packetThrottleInterval);
	command.connect.packetThrottleAcceleration = MRTP_HOST_TO_NET_32(currentPeer->packetThrottleAcceleration);
	command.connect.packetThrottleDeceleration = MRTP_HOST_TO_NET_32(currentPeer->packetThrottleDeceleration);
	command.connect.connectID = currentPeer->connectID;

	mrtp_peer_queue_outgoing_command(currentPeer, &command, NULL, 0, 0);

	return currentPeer;
}

void mrtp_host_free_redundancy_buffers(MRtpHost* host) {

	MRtpPeer* currentPeer;
	for (currentPeer = host->peers; currentPeer < &host->peers[host->peerCount]; ++currentPeer) {
		if (currentPeer->redundancyNoAckBuffers) {

			for (int i = 0; i < currentPeer->redundancyNum; i++) {
				mrtp_protocol_remove_redundancy_buffer_commands(&currentPeer->redundancyNoAckBuffers[i]);
			}
			mrtp_free(currentPeer->redundancyNoAckBuffers);

			currentPeer->redundancyNoAckBuffers = NULL;
			currentPeer->currentRedundancyNoAckBufferNum = 0;
		}

		if (currentPeer->redundancyBuffers) {

			for (int i = 0; i < MRTP_PROTOCOL_MAXIMUM_REDUNDNACY_BUFFER_SIZE; i++) {
				mrtp_protocol_remove_redundancy_buffer_commands(&currentPeer->redundancyBuffers[i]);
			}
			mrtp_free(currentPeer->redundancyBuffers);

			currentPeer->redundancyBuffers = NULL;
			currentPeer->currentRedundancyBufferNum = 0;
		}
	}

}

void mrtp_host_destroy(MRtpHost * host) {
	MRtpPeer * currentPeer;

	if (host == NULL)
		return;

	mrtp_socket_destroy(host->socket);

	mrtp_host_free_redundancy_buffers(host);

	for (currentPeer = host->peers; currentPeer < &host->peers[host->peerCount]; ++currentPeer) {
		mrtp_peer_reset(currentPeer);
	}

	mrtp_free(host->peers);
	mrtp_free(host);
}

void mrtp_host_bandwidth_throttle(MRtpHost * host) {

	mrtp_uint32 timeCurrent = mrtp_time_get();
	mrtp_uint32 elapsedTime = timeCurrent - host->bandwidthThrottleEpoch;//距离上次流量控制的时间
	mrtp_uint32	peersRemaining = (mrtp_uint32)host->connectedPeers;
	mrtp_uint32 dataTotal = ~0;
	mrtp_uint32 bandwidth = ~0;
	mrtp_uint32 throttle = 0;
	mrtp_uint32 bandwidthLimit = 0;
	int needsAdjustment = host->bandwidthLimitedPeers > 0 ? 1 : 0;
	MRtpPeer * peer;
	MRtpProtocol command;

	if (elapsedTime < MRTP_HOST_BANDWIDTH_THROTTLE_INTERVAL)
		return;
	//重置做流量控制的时间
	host->bandwidthThrottleEpoch = timeCurrent;

	if (peersRemaining == 0)
		return;

	if (host->outgoingBandwidth != 0) {
		dataTotal = 0;

		//在全带宽时，在间隔时间内传输的数据量
		bandwidth = (host->outgoingBandwidth * elapsedTime) / 1000;

		for (peer = host->peers; peer < &host->peers[host->peerCount]; ++peer) {
			if (peer->state != MRTP_PEER_STATE_CONNECTED && peer->state != MRTP_PEER_STATE_DISCONNECT_LATER)
				continue;

			dataTotal += peer->outgoingDataTotal;	//向peer发送的数据
		}
	}

	//调节peer -> packetThrottleLimit 和 peer -> packetThrottle
	while (peersRemaining > 0 && needsAdjustment != 0) {
		needsAdjustment = 0;

		if (dataTotal <= bandwidth)
			throttle = MRTP_PEER_PACKET_THROTTLE_SCALE;
		else
			throttle = (bandwidth * MRTP_PEER_PACKET_THROTTLE_SCALE) / dataTotal;

		for (peer = host->peers; peer < &host->peers[host->peerCount]; ++peer) {
			mrtp_uint32 peerBandwidth;

			if ((peer->state != MRTP_PEER_STATE_CONNECTED &&
				peer->state != MRTP_PEER_STATE_DISCONNECT_LATER) ||	//保证peer是连接状态
				peer->incomingBandwidth == 0 ||						//允从接收数据
				peer->outgoingBandwidthThrottleEpoch == timeCurrent)	//不是刚刚调节过
				continue;

			peerBandwidth = (peer->incomingBandwidth * elapsedTime) / 1000;	//peer在间隔时间内能接收的最大数据

																			// if((peerBandwidth * MRTP_PEER_PACKET_THROTTLE_SCALE)/ peer->outgoingDataTotal >= throttle)
																			// 如果peer接收数据的能力/peer接收的数据量大于host发送数据的能力/host发送的数据量
																			// 即peer接收能力大于平均水平，则不调节
			if ((throttle * peer->outgoingDataTotal) / MRTP_PEER_PACKET_THROTTLE_SCALE <= peerBandwidth)
				continue;

			// 低于平均水平时
			peer->packetThrottleLimit = (peerBandwidth * MRTP_PEER_PACKET_THROTTLE_SCALE) / peer->outgoingDataTotal;

			//packThrottleLimit最小值为1
			if (peer->packetThrottleLimit == 0)
				peer->packetThrottleLimit = 1;

			//设置packetThrottle
			if (peer->packetThrottle > peer->packetThrottleLimit)
				peer->packetThrottle = peer->packetThrottleLimit;

			peer->outgoingBandwidthThrottleEpoch = timeCurrent;

			peer->incomingDataTotal = 0;
			peer->outgoingDataTotal = 0;

			needsAdjustment = 1;
			--peersRemaining;
			bandwidth -= peerBandwidth;
			dataTotal -= peerBandwidth;
		}
	}

	//调节incomingBandwidth为0的peer的throttle
	//以及上面没有调节的大于平均水平的peer，将其throttle设置为throttle
	if (peersRemaining > 0) {
		if (dataTotal <= bandwidth)
			throttle = MRTP_PEER_PACKET_THROTTLE_SCALE;
		else
			throttle = (bandwidth * MRTP_PEER_PACKET_THROTTLE_SCALE) / dataTotal;

		for (peer = host->peers; peer < &host->peers[host->peerCount]; ++peer) {
			if ((peer->state != MRTP_PEER_STATE_CONNECTED &&
				peer->state != MRTP_PEER_STATE_DISCONNECT_LATER) ||
				peer->outgoingBandwidthThrottleEpoch == timeCurrent)
				continue;

			peer->packetThrottleLimit = throttle;

			if (peer->packetThrottle > peer->packetThrottleLimit)
				peer->packetThrottle = peer->packetThrottleLimit;

			peer->incomingDataTotal = 0;
			peer->outgoingDataTotal = 0;
		}
	}

#ifdef FLOWCONTROLDEBUG
	for (int i = 0; i < host->peerCount; i++) {
		peer = &host->peers[i];
		printf("peer [%d]: packetThrottleLimit [%d], packetThrottle [%d], incomingBandwidth [%d]\n",
			i, peer->packetThrottleLimit, peer->packetThrottle, peer->incomingBandwidth);
	}
	printf("\n");
#endif // FLOWCONTROLDEBUG


	//如果需要重新计算带宽限制
	if (host->recalculateBandwidthLimits) {
		host->recalculateBandwidthLimits = 0;

		peersRemaining = (mrtp_uint32)host->connectedPeers;
		bandwidth = host->incomingBandwidth;
		needsAdjustment = 1;

		if (bandwidth == 0)
			bandwidthLimit = 0;
		else {
			while (peersRemaining > 0 && needsAdjustment != 0) {
				needsAdjustment = 0;
				//取平均数，会随着循环增大
				bandwidthLimit = bandwidth / peersRemaining;

				for (peer = host->peers; peer < &host->peers[host->peerCount]; ++peer) {
					if ((peer->state != MRTP_PEER_STATE_CONNECTED &&
						peer->state != MRTP_PEER_STATE_DISCONNECT_LATER) ||	//是已经连接的peer
						peer->incomingBandwidthThrottleEpoch == timeCurrent)	//之前没有处理过
						continue;

					if (peer->outgoingBandwidth > 0 && peer->outgoingBandwidth >= bandwidthLimit)
						continue;
					//将outgoingBandwidth小于平均水平的peer标记出来
					peer->incomingBandwidthThrottleEpoch = timeCurrent;

					needsAdjustment = 1;
					--peersRemaining;
					bandwidth -= peer->outgoingBandwidth;
				}
			}
		}

		for (peer = host->peers; peer < &host->peers[host->peerCount]; ++peer) {

			if (peer->state != MRTP_PEER_STATE_CONNECTED && peer->state != MRTP_PEER_STATE_DISCONNECT_LATER)
				continue;

			//向带宽大于平均水平的peer发送流量控制命令
			command.header.command = MRTP_PROTOCOL_COMMAND_BANDWIDTH_LIMIT | MRTP_PROTOCOL_COMMAND_FLAG_ACKNOWLEDGE;
			command.bandwidthLimit.outgoingBandwidth = MRTP_HOST_TO_NET_32(host->outgoingBandwidth);

			// 将超过平均水平的设置为平均水平，否则不变，发送带宽控制命令
			if (peer->incomingBandwidthThrottleEpoch == timeCurrent)
				command.bandwidthLimit.incomingBandwidth = MRTP_HOST_TO_NET_32(peer->outgoingBandwidth);
			else
				command.bandwidthLimit.incomingBandwidth = MRTP_HOST_TO_NET_32(bandwidthLimit);

			//设置发送给peer的command，并将该command添加到peer的command处理队列中
			mrtp_peer_queue_outgoing_command(peer, &command, NULL, 0, 0);
		}
	}
}


void mrtp_host_bandwidth_limit(MRtpHost * host, mrtp_uint32 incomingBandwidth, mrtp_uint32 outgoingBandwidth)
{
	host->incomingBandwidth = incomingBandwidth;
	host->outgoingBandwidth = outgoingBandwidth;
	host->recalculateBandwidthLimits = 1;
}

// don't change redundancy_num when you send a packet
void mrtp_host_set_redundancy_num(MRtpHost *host, mrtp_uint32 redundancy_num) {
	if (redundancy_num > MRTP_PROTOCOL_MAXIMUM_REDUNDANCY_NUM) {
		redundancy_num = MRTP_PROTOCOL_MAXIMUM_REDUNDANCY_NUM;
	}
	else if (redundancy_num < MRTP_PROTOCOL_MINIMUM_REDUNDANCY_NUM) {
		redundancy_num = MRTP_PROTOCOL_MINIMUM_REDUNDANCY_NUM;
	}
	host->redundancyNum = redundancy_num;
}

void mrtp_host_shutdown_quick_retransmit(MRtpHost * host) {
	host->openQuickRetransmit = 0;
}

void mrtp_host_open_quick_retransmit(MRtpHost *host, mrtp_uint32 quickRetransmit) {

	MRtpPeer * currentPeer;

	host->openQuickRetransmit = 1;

	if (quickRetransmit > 0) {
		if (quickRetransmit > MRTP_PROTOCOL_MAXIMUM_QUICK_RETRANSMIT) {
			quickRetransmit = MRTP_PROTOCOL_MAXIMUM_QUICK_RETRANSMIT;
		}
		else if (quickRetransmit < MRTP_PROTOCOL_MINIMUM_QUICK_RETRANSMIT) {
			quickRetransmit = MRTP_PROTOCOL_MINIMUM_QUICK_RETRANSMIT;
		}

		for (currentPeer = host->peers; currentPeer < &host->peers[host->peerCount]; ++currentPeer) {
			currentPeer->quickRetransmitNum = quickRetransmit;
		}
	}
}