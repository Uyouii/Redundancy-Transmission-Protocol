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

	// initilize the peers array
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
		mrtp_list_clear(&currentPeer->sentRedundancyLastTimeCommands);
		mrtp_list_clear(&currentPeer->sentRedundancyThisTimeCommands);
		mrtp_list_clear(&currentPeer->outgoingUnsequencedCommands);
		mrtp_list_clear(&currentPeer->sentUnsequencedCommands);

		currentPeer->channels = (MRtpChannel *)mrtp_malloc(MRTP_PROTOCOL_CHANNEL_COUNT * sizeof(MRtpChannel));
		if (currentPeer->channels == NULL)
			return NULL;
		currentPeer->channelCount = MRTP_PROTOCOL_CHANNEL_COUNT;

		for (MRtpChannel *channel = currentPeer->channels;
			channel < &currentPeer->channels[currentPeer->channelCount]; ++channel)
		{
			mrtp_list_clear(&channel->incomingCommands);
		}

		mrtp_peer_reset(currentPeer);
	}

	return host;
}

MRtpPeer *mrtp_host_connect(MRtpHost * host, const MRtpAddress * address) {
	MRtpPeer * currentPeer;
	MRtpChannel * channel;
	MRtpProtocol command;

	for (currentPeer = host->peers; currentPeer < &host->peers[host->peerCount]; ++currentPeer) {
		// find the first peer which state is disconnected
		if (currentPeer->state == MRTP_PEER_STATE_DISCONNECTED)
			break;
	}

	if (currentPeer >= &host->peers[host->peerCount])
		return NULL;

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
		mrtp_free(currentPeer->channels);
	}

	mrtp_free(host->peers);
	mrtp_free(host);
}

void mrtp_host_bandwidth_throttle(MRtpHost * host) {

	mrtp_uint32 timeCurrent = mrtp_time_get();
	mrtp_uint32 elapsedTime = timeCurrent - host->bandwidthThrottleEpoch; // elapsed time from last bandwidth throttle
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

	host->bandwidthThrottleEpoch = timeCurrent;

	if (peersRemaining == 0)
		return;

	if (host->outgoingBandwidth != 0) {
		dataTotal = 0;

		//the data size at full outgoingBandwidth during the elapsed time
		bandwidth = (host->outgoingBandwidth * elapsedTime) / 1000;

		for (peer = host->peers; peer < &host->peers[host->peerCount]; ++peer) {
			if (peer->state != MRTP_PEER_STATE_CONNECTED && peer->state != MRTP_PEER_STATE_DISCONNECT_LATER)
				continue;

			dataTotal += peer->outgoingDataTotal;	// the total data send to peer during the elapsed time
		}
	}

	// adjust peer -> packetThrottleLimit and peer -> packetThrottle
	while (peersRemaining > 0 && needsAdjustment != 0) {
		needsAdjustment = 0;

		if (dataTotal <= bandwidth)
			throttle = MRTP_PEER_PACKET_THROTTLE_SCALE;
		else
			throttle = (bandwidth * MRTP_PEER_PACKET_THROTTLE_SCALE) / dataTotal;

		for (peer = host->peers; peer < &host->peers[host->peerCount]; ++peer) {
			mrtp_uint32 peerBandwidth;

			if ((peer->state != MRTP_PEER_STATE_CONNECTED &&
				peer->state != MRTP_PEER_STATE_DISCONNECT_LATER) ||	// peer is conected
				peer->incomingBandwidth == 0 ||						// peer open flow control
				peer->outgoingBandwidthThrottleEpoch == timeCurrent)// hasn't been adjusted before
				continue;

			peerBandwidth = (peer->incomingBandwidth * elapsedTime) / 1000;	// the largerest data peer can receive in the elapsed time

																			// if((peerBandwidth * MRTP_PEER_PACKET_THROTTLE_SCALE)/ peer->outgoingDataTotal >= throttle)
																			// if peer's receive abliity / peer receive data > host send ability / host send data
																			// means peer receive ability can hold up host send ability now, so don't need adjust this time
			if ((throttle * peer->outgoingDataTotal) / MRTP_PEER_PACKET_THROTTLE_SCALE <= peerBandwidth)
				continue;

			// if peer receive ability lower than host average send ability
			// then set the palcetThrottleLimit to limit the value of packetThrottle to limit the speed of data send
			peer->packetThrottleLimit = (peerBandwidth * MRTP_PEER_PACKET_THROTTLE_SCALE) / peer->outgoingDataTotal;

			// ensure that packetThrottleLimit is larger than zero
			if (peer->packetThrottleLimit == 0)
				peer->packetThrottleLimit = 1;

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

	// if there are still some peer hasn't been adjusted
	// then set their packetThrottleLimit to the lagerest value
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


	// if need recalculateBandwidthLimit 
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
				// get the host incoming bandwidth average value each time
				bandwidthLimit = bandwidth / peersRemaining;

				for (peer = host->peers; peer < &host->peers[host->peerCount]; ++peer) {
					if ((peer->state != MRTP_PEER_STATE_CONNECTED &&
						peer->state != MRTP_PEER_STATE_DISCONNECT_LATER) ||		// peer has alerady connected
						peer->incomingBandwidthThrottleEpoch == timeCurrent)	// hasn't been handled before
						continue;

					if (peer->outgoingBandwidth > 0 && peer->outgoingBandwidth >= bandwidthLimit)
						continue;
					// sign the send data ability lower than the average of host receive ability peer
					// doesn't need to change their outgoing bandwith
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

			// send the bandwidth limit command to the connected peer
			command.header.command = MRTP_PROTOCOL_COMMAND_BANDWIDTH_LIMIT | MRTP_PROTOCOL_COMMAND_FLAG_ACKNOWLEDGE;
			command.bandwidthLimit.outgoingBandwidth = MRTP_HOST_TO_NET_32(host->outgoingBandwidth);

			// to prevent peer send data larger than host receive ability,
			// set the remainint peer's outgoing bandwidth to bandwidthLimit
			if (peer->incomingBandwidthThrottleEpoch == timeCurrent)
				command.bandwidthLimit.incomingBandwidth = MRTP_HOST_TO_NET_32(peer->outgoingBandwidth);
			else
				command.bandwidthLimit.incomingBandwidth = MRTP_HOST_TO_NET_32(bandwidthLimit);

			// add the bandwidth limit command to outgoing queue
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

void mrtp_host_compress(MRtpHost * host, const MRtpCompressor * compressor) {

	if (host->compressor.context != NULL && host->compressor.destroy)
		(*host->compressor.destroy) (host->compressor.context);

	if (compressor)
		host->compressor = *compressor;
	else
		host->compressor.context = NULL;
}