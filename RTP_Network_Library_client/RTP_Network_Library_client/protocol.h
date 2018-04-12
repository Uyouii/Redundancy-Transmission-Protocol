#ifndef _MRTP_PROTOCOL_H_
#define _MRTP_PROTOCOL_H_

#include "types.h"

enum {
	MRTP_PROTOCOL_MINIMUM_MTU = 576,
	MRTP_PROTOCOL_MAXIMUM_MTU = 4096,
	MRTP_PROTOCOL_MAXIMUM_PACKET_COMMANDS = 32,
	MRTP_PROTOCOL_MINIMUM_WINDOW_SIZE = 4096,
	MRTP_PROTOCOL_MAXIMUM_WINDOW_SIZE = 65536,
	MRTP_PROTOCOL_MAXIMUM_PEER_ID = 0xFFF,
	MRTP_PROTOCOL_MAXIMUM_FRAGMENT_COUNT = 1024 * 1024,

	MRTP_PROTOCOL_RELIABLE_CHANNEL_NUM = 0,
	MRTP_PROTOCOL_REDUNDANCY_CHANNEL_NUM = 1,
	MRTP_PROTOCOL_REDUNDANCY_NOACK_CHANNEL_NUM = 2,
	MRTP_PROTOCOL_CHANNEL_COUNT = 3,
	MRTP_PROTOCOL_DEFAULT_REDUNDANCY_NUM = 3,
	MRTP_PROTOCOL_MAXIMUM_REDUNDANCY_NUM = 5,
	MRTP_PROTOCOL_MINIMUM_REDUNDANCY_NUM = 2,

	MRTP_PROTOCOL_MAXIMUM_REDUNDNACY_BUFFER_SIZE = 600,

	MRTP_PROTOCOL_DEFAULT_QUICK_RETRANSMIT = 5,
	MRTP_PROTOCOL_MAXIMUM_QUICK_RETRANSMIT = 10,
	MRTP_PROTOCOL_MINIMUM_QUICK_RETRANSMIT = 3,
};

typedef enum _MRtpProtocolCommand {
	MRTP_PROTOCOL_COMMAND_NONE = 0,
	MRTP_PROTOCOL_COMMAND_ACKNOWLEDGE = 1,
	MRTP_PROTOCOL_COMMAND_CONNECT = 2,
	MRTP_PROTOCOL_COMMAND_VERIFY_CONNECT = 3,
	MRTP_PROTOCOL_COMMAND_DISCONNECT = 4,
	MRTP_PROTOCOL_COMMAND_PING = 5,
	MRTP_PROTOCOL_COMMAND_SEND_RELIABLE = 6,
	MRTP_PROTOCOL_COMMAND_SEND_FRAGMENT = 7,
	MRTP_PROTOCOL_COMMAND_BANDWIDTH_LIMIT = 8,
	MRTP_PROTOCOL_COMMAND_THROTTLE_CONFIGURE = 9,
	MRTP_PROTOCOL_COMMAND_SEND_REDUNDANCY_NO_ACK = 10,
	MRTP_PROTOCOL_COMMAND_SEND_REDUNDANCY_FRAGEMENT_NO_ACK = 11,
	MRTP_PROTOCOL_COMMAND_SEND_REDUNDANCY = 12,
	MRTP_PROTOCOL_COMMAND_SEND_REDUNDANCY_FRAGMENT = 13,
	MRTP_PROTOCOL_COMMAND_SET_QUICK_RETRANSMIT = 14,
	MRTP_PROTOCOL_COMMAND_REDUNDANCY_ACKNOWLEDGE = 15,
	MRTP_PROTOCOL_COMMAND_RETRANSMIT_REDUNDANCY = 16,
	MRTP_PROTOCOL_COMMAND_COUNT = 17,

	MRTP_PROTOCOL_COMMAND_MASK = 0x1F
} MRtpProtocolCommand;

typedef enum _MRtpProtocolFlag {
	MRTP_PROTOCOL_COMMAND_FLAG_ACKNOWLEDGE = (1 << 7),
	MRTP_PROTOCOL_COMMAND_FLAG_REDUNDANCY_ACKNOWLEDGE = (1 << 6),

	MRTP_PROTOCOL_HEADER_SESSION_MASK = (3 << 12),
	MRTP_PROTOCOL_HEADER_SESSION_SHIFT = 12,

	MRTP_PROTOCOL_HEADER_FLAG_COMPRESSED = (1 << 14),
	MRTP_PROTOCOL_HEADER_FLAG_SENT_TIME = (1 << 15),
	MRTP_PROTOCOL_HEADER_FLAG_MASK = MRTP_PROTOCOL_HEADER_FLAG_COMPRESSED | MRTP_PROTOCOL_HEADER_FLAG_SENT_TIME,

} MRtpProtocolFlag;


// 取消msvc中的优化对齐
#ifdef _MSC_VER
#pragma pack(push, 1)
#define MRTP_PACKED
#elif defined(__GNUC__) || defined(__clang__)
#define MRTP_PACKED __attribute__ ((packed)) 
#else
#define MRTP_PACKED
#endif

typedef struct _MRtpProtocolHeader {
	mrtp_uint16 peerID;
	mrtp_uint16 sentTime;
} MRTP_PACKED MRtpProtocolHeader;

typedef struct _MRtpProtocolCommandHeader {
	mrtp_uint8 command;
	//mrtp_uint8 channelID;
	mrtp_uint16 sequenceNumber;
} MRTP_PACKED MRtpProtocolCommandHeader;

typedef struct _MRtpProtocolAcknowledge {
	MRtpProtocolCommandHeader header;
	mrtp_uint16 receivedReliableSequenceNumber;
	mrtp_uint16 receivedSentTime;
} MRTP_PACKED MRtpProtocolAcknowledge;

typedef struct _MRtpProtocolConnect {
	MRtpProtocolCommandHeader header;
	mrtp_uint16 outgoingPeerID;
	mrtp_uint8 incomingSessionID;
	mrtp_uint8 outgoingSessionID;
	mrtp_uint32 mtu;
	mrtp_uint32 windowSize;
	mrtp_uint32 incomingBandwidth;
	mrtp_uint32 outgoingBandwidth;
	mrtp_uint32 packetThrottleInterval;
	mrtp_uint32 packetThrottleAcceleration;
	mrtp_uint32 packetThrottleDeceleration;
	mrtp_uint32 connectID;
	mrtp_uint32 data;
} MRTP_PACKED MRtpProtocolConnect;

typedef struct _MRtpProtocolVerifyConnect {
	MRtpProtocolCommandHeader header;
	mrtp_uint16 outgoingPeerID;
	mrtp_uint8 incomingSessionID;
	mrtp_uint8 outgoingSessionID;
	mrtp_uint32 mtu;
	mrtp_uint32 windowSize;
	mrtp_uint32 incomingBandwidth;
	mrtp_uint32 outgoingBandwidth;
	mrtp_uint32 packetThrottleInterval;
	mrtp_uint32 packetThrottleAcceleration;
	mrtp_uint32 packetThrottleDeceleration;
	mrtp_uint32 connectID;
} MRTP_PACKED MRtpProtocolVerifyConnect;

typedef struct _MRtpProtocolBandwidthLimit {
	MRtpProtocolCommandHeader header;
	mrtp_uint32 incomingBandwidth;
	mrtp_uint32 outgoingBandwidth;
} MRTP_PACKED MRtpProtocolBandwidthLimit;

typedef struct _MRtpProtocolThrottleConfigure {
	MRtpProtocolCommandHeader header;
	mrtp_uint32 packetThrottleInterval;
	mrtp_uint32 packetThrottleAcceleration;
	mrtp_uint32 packetThrottleDeceleration;
} MRTP_PACKED MRtpProtocolThrottleConfigure;

typedef struct _MRtpProtocolDisconnect {
	MRtpProtocolCommandHeader header;
	mrtp_uint32 data;
} MRTP_PACKED MRtpProtocolDisconnect;

typedef struct _MRtpProtocolPing {
	MRtpProtocolCommandHeader header;
} MRTP_PACKED MRtpProtocolPing;

typedef struct _MRtpProtocolSendReliable {
	MRtpProtocolCommandHeader header;
	mrtp_uint16 dataLength;
} MRTP_PACKED MRtpProtocolSendReliable;

typedef struct _MRtpProtocolSendFragment {
	MRtpProtocolCommandHeader header;
	mrtp_uint16 startSequenceNumber;
	mrtp_uint16 dataLength;
	mrtp_uint32 fragmentCount;
	mrtp_uint32 fragmentNumber;
	mrtp_uint32 totalLength;
	mrtp_uint32 fragmentOffset;
} MRTP_PACKED MRtpProtocolSendFragment;

typedef struct _MRtpProtocolSendRedundancy {
	MRtpProtocolCommandHeader header;
	mrtp_uint16 dataLength;
} MRTP_PACKED MRtpProtocolSendRedundancy;

typedef struct _MRtpProtocolRetransmitRedundancy {
	MRtpProtocolCommandHeader header;
	mrtp_uint16 dataLength;
	mrtp_uint16 retransmitSequenceNumber;
} MRTP_PACKED MRtpProtocolRetransmitRedundancy;

typedef struct _MRtpProtocolSendRedundancyFragment {
	MRtpProtocolCommandHeader header;
	mrtp_uint16 startSequenceNumber;
	mrtp_uint16 dataLength;
	mrtp_uint32 fragmentCount;
	mrtp_uint32 fragmentNumber;
	mrtp_uint32 totalLength;
	mrtp_uint32 fragmentOffset;
} MRTP_PACKED MRtpProtocolSendRedundancyFragment;

typedef struct _MRtpProtocolSendRedundancyNoAck {
	MRtpProtocolCommandHeader header;
	mrtp_uint16 datalength;
} MRTP_PACKED MRtpProtocolSendRedundancyNoAck;

typedef struct _MRtpProtocolSendRedundancyFragementNoAck {
	MRtpProtocolCommandHeader header;
	mrtp_uint16 startSequenceNumber;
	mrtp_uint16 dataLength;
	mrtp_uint32 fragmentCount;
	mrtp_uint32 fragmentNumber;
	mrtp_uint32 totalLength;
	mrtp_uint32 fragmentOffset;
} MRTP_PACKED MRtpProtocolSendRedundancyFragementNoAck;

typedef struct _MRtpProtocolSetQuickRetransmit {
	MRtpProtocolCommandHeader header;
	mrtp_uint16 quickRetransmit;
} MRTP_PACKED MRtpProtocolSetQuickRetransmit;

typedef struct _MRtpProtocolRedundancyAcknowledge {
	MRtpProtocolCommandHeader header;
	mrtp_uint16 receivedSequenceNumber;
	mrtp_uint16 receivedSentTime;
	mrtp_uint16 nextUnackSequenceNumber;	//the packet before this seq number has already received
} MRTP_PACKED MRtpProtocolRedundancyAcknowledge;

typedef union _MRtpProtocol {
	MRtpProtocolCommandHeader header;
	MRtpProtocolAcknowledge acknowledge;
	MRtpProtocolConnect connect;
	MRtpProtocolVerifyConnect verifyConnect;
	MRtpProtocolDisconnect disconnect;
	MRtpProtocolPing ping;
	MRtpProtocolSendReliable sendReliable;
	MRtpProtocolSendFragment sendFragment;
	MRtpProtocolBandwidthLimit bandwidthLimit;
	MRtpProtocolThrottleConfigure throttleConfigure;
	MRtpProtocolSendRedundancyNoAck sendRedundancyNoAck;
	MRtpProtocolSendRedundancyFragementNoAck sendRedundancyFragementNoAck;
	MRtpProtocolSendRedundancy sendRedundancy;
	MRtpProtocolSendRedundancyFragment sendRedundancyFragment;
	MRtpProtocolSetQuickRetransmit setQuickRestrnsmit;
	MRtpProtocolRedundancyAcknowledge redundancyAcknowledge;
	MRtpProtocolRetransmitRedundancy retransmitRedundancy;
} MRTP_PACKED MRtpProtocol;

#ifdef _MSC_VER
#pragma pack(pop)
#endif

#endif // 
