#ifndef _MRTP_H_
#define _MRTP_H_

// for debug
#define SENDANDRECEIVE
//#define FLOWCONTROLDEBUG
//#define RELIABLEWINDOWDEBUG
//#define PACKETLOSSDEBUG

#ifdef __cplusplus
extern "C"
{
#endif

#include <stdlib.h>

#ifdef _WIN32
#include "win32.h"
#else
#include "unix.h"
#endif

#include "types.h"
#include "list.h"
#include "protocol.h"
#include "callbacks.h"

	typedef enum _MRtpSocketType {
		MRTP_SOCKET_TYPE_STREAM = 1,
		MRTP_SOCKET_TYPE_DATAGRAM = 2
	} MRtpSocketType;

	typedef enum _MRtpSocketWait {
		MRTP_SOCKET_WAIT_NONE = 0,
		MRTP_SOCKET_WAIT_SEND = (1 << 0),
		MRTP_SOCKET_WAIT_RECEIVE = (1 << 1),
		MRTP_SOCKET_WAIT_INTERRUPT = (1 << 2)
	} MRtpSocketWait;

	typedef enum _MRtpSocketOption {
		MRTP_SOCKOPT_NONBLOCK = 1,
		MRTP_SOCKOPT_BROADCAST = 2,
		MRTP_SOCKOPT_RCVBUF = 3,
		MRTP_SOCKOPT_SNDBUF = 4,
		MRTP_SOCKOPT_REUSEADDR = 5,
		MRTP_SOCKOPT_RCVTIMEO = 6,
		MRTP_SOCKOPT_SNDTIMEO = 7,
		MRTP_SOCKOPT_ERROR = 8,
		MRTP_SOCKOPT_NODELAY = 9
	} MRtpSocketOption;

	typedef enum _MRtpSocketShutdown {
		MRTP_SOCKET_SHUTDOWN_READ = 0,
		MRTP_SOCKET_SHUTDOWN_WRITE = 1,
		MRTP_SOCKET_SHUTDOWN_READ_WRITE = 2
	} MRtpSocketShutdown;


#define MRTP_HOST_ANY       0
#define MRTP_HOST_BROADCAST 0xFFFFFFFFU
#define MRTP_PORT_ANY       0

	typedef struct _MRtpAddress {
		mrtp_uint32 host;
		mrtp_uint16 port;
	} MRtpAddress;

	typedef enum _MRtpPacketFlag {
		MRTP_PACKET_FLAG_RELIABLE = (1 << 0),
		MRTP_PACKET_FLAG_NO_ALLOCATE = (1 << 2),
		MRTP_PACKET_FLAG_REDUNDANCY = (1 << 3),
		MRTP_PACKET_FLAG_REDUNDANCY_NO_ACK = (1 << 4),


		MRTP_PACKET_FLAG_SENT = (1 << 8)
	} MRtpPacketFlag;

	typedef void (MRTP_CALLBACK * MRtpPacketFreeCallback) (struct _MRtpPacket *);

	typedef struct _MRtpPacket {
		size_t                   referenceCount;  /**< internal use only */
		mrtp_uint32              flags;           /**< bitwise-or of MRtpPacketFlag constants */
		mrtp_uint8 *             data;            /**< allocated data for packet */
		size_t                   dataLength;      /**< length of data */
		MRtpPacketFreeCallback   freeCallback;    /**< function to be called when the packet is no longer in use */
	} MRtpPacket;

	typedef struct _MRtpAcknowledgement
	{
		MRtpListNode acknowledgementList;
		mrtp_uint32  sentTime;
		MRtpProtocol command;
	} MRtpAcknowledgement;

	typedef struct _MRtpOutgoingCommand
	{
		MRtpListNode outgoingCommandList;
		mrtp_uint16  sequenceNumber;
		mrtp_uint32  sentTime;
		mrtp_uint32  roundTripTimeout;
		mrtp_uint32  roundTripTimeoutLimit;
		mrtp_uint32  fragmentOffset;
		mrtp_uint16  fragmentLength;
		mrtp_uint16  sendAttempts;
		MRtpProtocol command;
		MRtpPacket * packet;
	} MRtpOutgoingCommand;

	typedef struct _MRtpIncomingCommand
	{
		MRtpListNode incomingCommandList;
		mrtp_uint16 sequenceNumber;
		MRtpProtocol command;
		mrtp_uint32 fragmentCount;
		mrtp_uint32 fragmentsRemaining;
		mrtp_uint32 * fragments;
		MRtpPacket * packet;
	} MRtpIncomingCommand;

	typedef enum _MRtpPeerState {
		MRTP_PEER_STATE_DISCONNECTED = 0,
		MRTP_PEER_STATE_CONNECTING = 1,
		MRTP_PEER_STATE_ACKNOWLEDGING_CONNECT = 2,
		MRTP_PEER_STATE_CONNECTION_PENDING = 3,
		MRTP_PEER_STATE_CONNECTION_SUCCEEDED = 4,
		MRTP_PEER_STATE_CONNECTED = 5,
		MRTP_PEER_STATE_DISCONNECT_LATER = 6,
		MRTP_PEER_STATE_DISCONNECTING = 7,
		MRTP_PEER_STATE_ACKNOWLEDGING_DISCONNECT = 8,
		MRTP_PEER_STATE_ZOMBIE = 9	//僵尸状态
	} MRtpPeerState;

#ifndef MRTP_BUFFER_MAXIMUM
#define MRTP_BUFFER_MAXIMUM (1 + 2 * MRTP_PROTOCOL_MAXIMUM_PACKET_COMMANDS)
#endif

	enum
	{
		MRTP_HOST_RECEIVE_BUFFER_SIZE = 1024 * 1024,
		MRTP_HOST_SEND_BUFFER_SIZE = 256 * 1024,
		MRTP_HOST_BANDWIDTH_THROTTLE_INTERVAL = 1000,
		MRTP_HOST_DEFAULT_MTU = 1400,
		MRTP_HOST_DEFAULT_MAXIMUM_PACKET_SIZE = 32 * 1024 * 1024,
		MRTP_HOST_DEFAULT_MAXIMUM_WAITING_DATA = 32 * 1024 * 1024,

		MRTP_PEER_DEFAULT_ROUND_TRIP_TIME = 500,
		MRTP_PEER_DEFAULT_PACKET_THROTTLE = 32,
		MRTP_PEER_PACKET_THROTTLE_SCALE = 32,
		MRTP_PEER_PACKET_THROTTLE_COUNTER = 7,
		MRTP_PEER_PACKET_THROTTLE_ACCELERATION = 2,
		MRTP_PEER_PACKET_THROTTLE_DECELERATION = 2,
		MRTP_PEER_PACKET_THROTTLE_INTERVAL = 5000,
		MRTP_PEER_PACKET_LOSS_SCALE = (1 << 16),
		MRTP_PEER_PACKET_LOSS_INTERVAL = 10000,
		MRTP_PEER_WINDOW_SIZE_SCALE = 64 * 1024,
		MRTP_PEER_TIMEOUT_LIMIT = 32,
		MRTP_PEER_TIMEOUT_MINIMUM = 5000,
		MRTP_PEER_TIMEOUT_MAXIMUM = 30000,
		MRTP_PEER_PING_INTERVAL = 1000,
		MRTP_PEER_RELIABLE_WINDOWS = 16,
		MRTP_PEER_RELIABLE_WINDOW_SIZE = 0x1000,
		MRTP_PEER_FREE_RELIABLE_WINDOWS = 8
	};

	typedef struct _MRtpChannel {
		mrtp_uint16 outgoingSequenceNumber;
		mrtp_uint16 usedReliableWindows;
		mrtp_uint16 commandWindows[MRTP_PEER_RELIABLE_WINDOWS];
		mrtp_uint16 incomingSequenceNumber;
		MRtpList incomingCommands;
	} MRtpChannel;


	typedef struct _MRtpRedundancyBuffer {
		MRtpList sentCommands;
		size_t buffercount;
		size_t packetSize;
		MRtpBuffer buffers[MRTP_BUFFER_MAXIMUM / 2];
	} MRtpRedundancyBuffer;

	typedef struct _MRtpPeer {
		MRtpListNode  dispatchList;
		struct _MRtpHost * host;
		mrtp_uint16 outgoingPeerID;
		mrtp_uint16 incomingPeerID;
		mrtp_uint32 connectID;
		mrtp_uint8 outgoingSessionID;
		mrtp_uint8 incomingSessionID;
		MRtpAddress address;            /**< Internet address of the peer */
		void * data;               /**< Application private data, may be freely modified */
		MRtpPeerState state;
		MRtpChannel * channels;
		size_t channelCount;       /**< Number of channels allocated for communication with peer */
		mrtp_uint32 incomingBandwidth;  /**< Downstream bandwidth of the client in bytes/second */
		mrtp_uint32 outgoingBandwidth;  /**< Upstream bandwidth of the client in bytes/second */
		mrtp_uint32 incomingBandwidthThrottleEpoch;
		mrtp_uint32 outgoingBandwidthThrottleEpoch;
		mrtp_uint32 incomingDataTotal;
		mrtp_uint32 outgoingDataTotal;
		mrtp_uint32 lastSendTime;
		mrtp_uint32 lastReceiveTime;
		mrtp_uint32 nextTimeout;
		mrtp_uint32 earliestTimeout;
		mrtp_uint32 packetLossEpoch;
		mrtp_uint32 packetsSent;
		mrtp_uint32 packetsLost;
		mrtp_uint32 packetLoss;          /**< mean packet loss of reliable packets as a ratio with respect to the constant MRTP_PEER_PACKET_LOSS_SCALE */
		mrtp_uint32 packetLossVariance;
		mrtp_uint32 packetThrottle;
		mrtp_uint32 packetThrottleLimit;
		mrtp_uint32 packetThrottleCounter;
		mrtp_uint32 packetThrottleEpoch;
		mrtp_uint32 packetThrottleAcceleration;
		mrtp_uint32 packetThrottleDeceleration;
		mrtp_uint32 packetThrottleInterval;
		mrtp_uint32 pingInterval;
		mrtp_uint32 timeoutLimit;
		mrtp_uint32 timeoutMinimum;
		mrtp_uint32 timeoutMaximum;
		mrtp_uint32 lastRoundTripTime;
		mrtp_uint32 lowestRoundTripTime;
		mrtp_uint32 lastRoundTripTimeVariance;
		mrtp_uint32 highestRoundTripTimeVariance;
		mrtp_uint32 roundTripTime;            /**< mean round trip time (RTT), in milliseconds, between sending a reliable packet and receiving its acknowledgement */
		mrtp_uint32 roundTripTimeVariance;
		mrtp_uint32 mtu;
		mrtp_uint32 windowSize;
		mrtp_uint32 reliableDataInTransit;
		mrtp_uint16 outgoingReliableSequenceNumber;
		MRtpList acknowledgements;
		MRtpList sentReliableCommands;
		MRtpList sentRedundancyNoAckCommands;
		MRtpList outgoingReliableCommands;
		MRtpList outgoingRedundancyCommands;
		MRtpList outgoingRedundancyNoAckCommands;
		MRtpList dispatchedCommands;
		int needsDispatch;
		mrtp_uint32 eventData;
		size_t totalWaitingData;
		size_t redundancyNum;
		size_t currentRedundancyNoAckBufferNum;
		MRtpRedundancyBuffer* redundancyNoAckBuffers;
		size_t currentRedundancyBufferNum;
		MRtpRedundancyBuffer* redundancyBuffers;
		mrtp_uint32 lastReceiveRedundancyNumber;
		mrtp_uint16 quickRetransmitNum;
	} MRtpPeer;


	typedef mrtp_uint32(MRTP_CALLBACK * MRtpChecksumCallback) (const MRtpBuffer * buffers, size_t bufferCount);

	typedef int (MRTP_CALLBACK * MRtpInterceptCallback) (struct _MRtpHost * host, struct _MRtpEvent * event);

	typedef struct _MRtpHost {
		MRtpSocket socket;
		MRtpAddress address;                     /**< Internet address of the host */
		mrtp_uint32 incomingBandwidth;           /**< downstream bandwidth of the host */
		mrtp_uint32 outgoingBandwidth;           /**< upstream bandwidth of the host */
		mrtp_uint32 bandwidthThrottleEpoch;
		mrtp_uint32  mtu;
		mrtp_uint32 randomSeed;
		int recalculateBandwidthLimits;
		MRtpPeer * peers;                       /**< array of peers allocated for this host */
		size_t peerCount;                   /**< number of peers allocated for this host */
		mrtp_uint32 serviceTime;
		MRtpList dispatchQueue;
		int continueSending;
		size_t packetSize;
		mrtp_uint16 headerFlags;
		MRtpProtocol commands[MRTP_PROTOCOL_MAXIMUM_PACKET_COMMANDS];
		size_t commandCount;
		MRtpBuffer buffers[MRTP_BUFFER_MAXIMUM];
		size_t bufferCount;
		mrtp_uint8 packetData[2][MRTP_PROTOCOL_MAXIMUM_MTU];
		MRtpAddress receivedAddress;
		mrtp_uint8 *receivedData;
		size_t receivedDataLength;
		mrtp_uint32 totalSentData;               /**< total data sent, user should reset to 0 as needed to prevent overflow */
		mrtp_uint32 totalSentPackets;            /**< total UDP packets sent, user should reset to 0 as needed to prevent overflow */
		mrtp_uint32 totalReceivedData;           /**< total data received, user should reset to 0 as needed to prevent overflow */
		mrtp_uint32 totalReceivedPackets;        /**< total UDP packets received, user should reset to 0 as needed to prevent overflow */
		size_t connectedPeers;
		size_t bandwidthLimitedPeers;
		size_t duplicatePeers;              /**< optional number of allowed peers from duplicate IPs, defaults to MRTP_PROTOCOL_MAXIMUM_PEER_ID */
		size_t maximumPacketSize;           /**< the maximum allowable packet size that may be sent or received on a peer */
		size_t maximumWaitingData;          /**< the maximum aggregate amount of buffer space a peer may use waiting for packets to be delivered */
		mrtp_uint8 redundancyNum;
	} MRtpHost;


	typedef enum _MRtpEventType {
		/** no event occurred within the specified time limit */
		MRTP_EVENT_TYPE_NONE = 0,
		MRTP_EVENT_TYPE_CONNECT = 1,
		MRTP_EVENT_TYPE_DISCONNECT = 2,
		MRTP_EVENT_TYPE_RECEIVE = 3
	} MRtpEventType;

	typedef struct _MRtpEvent {
		MRtpEventType        type;      /**< type of the event */
		MRtpPeer *           peer;      /**< peer that generated a connect, disconnect or receive event */
		mrtp_uint8           channelID; /**< channel on the peer that generated the event, if appropriate */
		mrtp_uint32          data;      /**< data associated with the event, if appropriate */
		MRtpPacket *         packet;    /**< packet associated with the event, if appropriate */
	} MRtpEvent;

	extern mrtp_uint8 channelIDs[];

	MRTP_API int mrtp_initialize(void);
	MRTP_API int mrtp_initialize_with_callbacks(const MRtpCallbacks * inits);
	MRTP_API void mrtp_deinitialize(void);

	MRTP_API mrtp_uint32 mrtp_time_get(void);
	MRTP_API void mrtp_time_set(mrtp_uint32);

	MRTP_API MRtpSocket mrtp_socket_create(MRtpSocketType);
	MRTP_API int mrtp_socket_bind(MRtpSocket, const MRtpAddress *);
	MRTP_API int mrtp_socket_get_address(MRtpSocket, MRtpAddress *);
	MRTP_API int mrtp_socket_listen(MRtpSocket, int);
	MRTP_API MRtpSocket mrtp_socket_accept(MRtpSocket, MRtpAddress *);
	MRTP_API int mrtp_socket_connect(MRtpSocket, const MRtpAddress *);
	MRTP_API int mrtp_socket_send(MRtpSocket, const MRtpAddress *, const MRtpBuffer *, size_t);
	MRTP_API int mrtp_socket_receive(MRtpSocket, MRtpAddress *, MRtpBuffer *, size_t);
	MRTP_API int mrtp_socket_wait(MRtpSocket, mrtp_uint32 *, mrtp_uint32);
	MRTP_API int mrtp_socket_set_option(MRtpSocket, MRtpSocketOption, int);
	MRTP_API int mrtp_socket_get_option(MRtpSocket, MRtpSocketOption, int *);
	MRTP_API int mrtp_socket_shutdown(MRtpSocket, MRtpSocketShutdown);
	MRTP_API void mrtp_socket_destroy(MRtpSocket);
	MRTP_API int mrtp_socketset_select(MRtpSocket, MRtpSocketSet *, MRtpSocketSet *, mrtp_uint32);

	MRTP_API int mrtp_address_set_host(MRtpAddress * address, const char * hostName);
	MRTP_API int mrtp_address_get_host_ip(const MRtpAddress * address, char * hostName, size_t nameLength);
	MRTP_API int mrtp_address_get_host(const MRtpAddress * address, char * hostName, size_t nameLength);

	MRTP_API MRtpPacket * mrtp_packet_create(const void *, size_t, mrtp_uint32);
	MRTP_API void mrtp_packet_destroy(MRtpPacket *);
	MRTP_API int mrtp_packet_resize(MRtpPacket *, size_t);
	MRTP_API mrtp_uint32 mrtp_crc32(const MRtpBuffer *, size_t);

	MRTP_API MRtpHost * mrtp_host_create(const MRtpAddress *, size_t, mrtp_uint32, mrtp_uint32);
	MRTP_API void mrtp_host_destroy(MRtpHost *);
	MRTP_API MRtpPeer * mrtp_host_connect(MRtpHost *, const MRtpAddress *);
	MRTP_API int mrtp_host_check_events(MRtpHost *, MRtpEvent *);
	MRTP_API int mrtp_host_service(MRtpHost *, MRtpEvent *, mrtp_uint32);
	MRTP_API void mrtp_host_flush(MRtpHost *);
	MRTP_API void mrtp_host_broadcast(MRtpHost *, mrtp_uint8, MRtpPacket *);
	MRTP_API void mrtp_host_channel_limit(MRtpHost *, size_t);
	MRTP_API void mrtp_host_bandwidth_limit(MRtpHost *, mrtp_uint32, mrtp_uint32);
	extern void mrtp_host_bandwidth_throttle(MRtpHost *);
	extern mrtp_uint32 mrtp_host_random_seed(void);
	MRTP_API void mrtp_host_set_redundancy_num(MRtpHost *host, mrtp_uint32 redundancy_num);

	MRTP_API int mrtp_peer_send_reliable(MRtpPeer * peer, MRtpPacket * packet);
	MRTP_API int mrtp_peer_send(MRtpPeer *peer, MRtpPacket *packet);
	MRTP_API MRtpPacket * mrtp_peer_receive(MRtpPeer *, mrtp_uint8 * channelID);
	MRTP_API void mrtp_peer_ping(MRtpPeer *);
	MRTP_API void mrtp_peer_ping_interval(MRtpPeer *, mrtp_uint32);
	MRTP_API void mrtp_peer_timeout(MRtpPeer *, mrtp_uint32, mrtp_uint32, mrtp_uint32);
	MRTP_API void mrtp_peer_reset(MRtpPeer *);
	MRTP_API void mrtp_peer_disconnect(MRtpPeer *, mrtp_uint32);
	MRTP_API void mrtp_peer_disconnect_now(MRtpPeer *, mrtp_uint32);
	MRTP_API void  mrtp_peer_disconnect_later(MRtpPeer *, mrtp_uint32);
	MRTP_API void mrtp_peer_throttle_configure(MRtpPeer *, mrtp_uint32, mrtp_uint32, mrtp_uint32);
	MRTP_API void mrtp_peer_quick_restransmit_configure(MRtpPeer * peer, mrtp_uint16 quickRetransmit);
	extern int mrtp_peer_throttle(MRtpPeer *, mrtp_uint32);
	extern void mrtp_peer_reset_queues(MRtpPeer *);
	extern void mrtp_peer_setup_outgoing_command(MRtpPeer *, MRtpOutgoingCommand *);
	extern MRtpOutgoingCommand * mrtp_peer_queue_outgoing_command(MRtpPeer *, const MRtpProtocol *, MRtpPacket *, mrtp_uint32, mrtp_uint16);
	extern MRtpIncomingCommand * mrtp_peer_queue_incoming_command(MRtpPeer *, const MRtpProtocol *, const void *, size_t, mrtp_uint32, mrtp_uint32);
	extern MRtpAcknowledgement * mrtp_peer_queue_acknowledgement(MRtpPeer *, const MRtpProtocol *, mrtp_uint16);
	extern void mrtp_peer_dispatch_incoming_reliable_commands(MRtpPeer *, MRtpChannel *);
	extern void mrtp_peer_dispatch_incoming_redundancy_noack_commands(MRtpPeer*, MRtpChannel *);
	extern void mrtp_peer_on_connect(MRtpPeer *);
	extern void mrtp_peer_on_disconnect(MRtpPeer *);
	extern void mrtp_peer_reset_redundancy_noack_buffer(MRtpPeer* peer, size_t redundancyNum);
	extern void mrtp_peer_reset_reduandancy_buffer(MRtpPeer* peer, size_t redundancyNum);


	extern size_t mrtp_protocol_command_size(mrtp_uint8);
	extern void mrtp_protocol_remove_redundancy_buffer_commands(MRtpRedundancyBuffer* mrtpRedundancyBuffer);


#ifdef __cplusplus
}
#endif // __cplusplus


#endif // !_MRTP_H_
