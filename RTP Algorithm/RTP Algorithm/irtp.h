#ifndef _IRTP_H_
#define _IRTP_H_

#include <stddef.h>
#include <stdlib.h>
#include <assert.h>
#include "iqueue.h"
#include "struct_define.h"


//=====================================================================
// SEGMENT
//=====================================================================
struct IRTPSEG
{
	struct IQUEUEHEAD node;		// 双向链表定义的队列
	IUINT32 conv;				// conversation, 会话序号: 接收到的数据包与发送的一致才接收此数据包
	IUINT32 command;			// command, 指令类型: 代表这个Segment的类型
	IUINT32 fragement;			// fragment, 分段序号
	IUINT32 wnd;				// window, 窗口大小
	IUINT32 timestamp;			// timestamp, 发送的时间戳
	IUINT32 seq_num;			// sequence number, segment序号
	IUINT32 unack;				// unacknowledged, 当前未收到的序号: 即代表这个序号之前的包均收到
	IUINT32 len;				// length, 数据长度
	IUINT32 resend_timestamp;	// 重发的时间戳
	IUINT32 rto;				// 超时重传的时间间隔
	IUINT32 fastack;			// ack跳过的次数，用于快速重传
	IUINT32 xmit;				// 发送的次数
	char data[1];				// 用于标记segment数据的起始位置
};

typedef struct PACKETCACHE {
	char* data;
	IUINT32 len;
} PacketCache;


//---------------------------------------------------------------------
// IRTPCB
//---------------------------------------------------------------------
struct IRTPCB {

	//conv:会话ID，mtu:最大传输单元，mss:最大分片大小，state:连接状态
	IUINT32 conv, mtu, mss, state;

	//snd_unack：第一个未确认的包，snd_next：待发送包的序号，rcv_next：待接收消息的序号
	IUINT32 snd_unack, snd_next, rcv_next;

	//ssthresh:拥塞窗口的阈值，慢起动阈值
	IUINT32 recent_timestamp, lastack_timestamp, ssthresh;

	//rx_rttval：ack接收rtt浮动值，rx_srtt：ack接收rtt平滑值(smoothed)，
	//rx_rto：由ack接收延迟计算出来的复原时间，rx_minrto：最小复原时间
	IINT32 rx_rttval, rx_srtt, rx_rto, rx_minrto;

	//sen_wnd：发送窗口大小，rcv_wnd：接收窗口大小，
	//remote_wnd：远端接收窗口大小，congestion_wnd：拥塞窗口大小，
	//probe	探查变量，IRTP_ASK_TELL表示告知远端窗口大小。IRTP_ASK_SEND表示请求远端告知窗口大小。
	IUINT32 snd_wnd, rcv_wnd, remote_wnd, congestion_wnd, probe;

	//currunt：当前的时间戳，interval：内部flush刷新间隔，flush_timestamp：下次flush刷新时间戳
	IUINT32 current, interval, flush_timestamp, xmit;
	IUINT32 nrcv_buf, nsnd_buf;
	IUINT32 nrcv_que, nsnd_que;

	//nodelay：是否启动无延迟模式，
	//update：是否调用过update函数的标识(rtp需要上层通过不断的irtp_update和irtp_check来驱动rtp的收发过程)
	IUINT32 nodelay, updated;

	//probe_timestamp：下次探查窗口的时间戳，probe_wait：探查窗口需要等待的时间
	IUINT32 probe_timestamp, probe_wait;

	//dead_link：最大重传次数，incr：可发送的最大数据量
	IUINT32 dead_link, incr;

	struct IQUEUEHEAD snd_queue;	//发送消息的队列
	struct IQUEUEHEAD rcv_queue;	//接收消息的队列
	struct IQUEUEHEAD snd_buf;		//发送消息的缓存
	struct IQUEUEHEAD rcv_buf;		//接收消息的缓存
	
	IUINT32 *acklist;				//待发送的ack的列表
	IUINT32 ackcount;				//ack数量
	IUINT32 ackblock;				//acklist的大小

	size_t redundancy_num;
	PacketCache *old_send_data;		//用于缓存之前发送内容的buffer
	size_t old_send_data_used;
	size_t r_buffer_size;
	size_t now_send_data_num;		// 当前是冗余缓冲的哪个buffer

	void *user;
	char *buffer;					//储存消息字节流的内存
	int fastresend;					//触发快速重传的重复ack个数
	int nocwnd, stream;				//nocwnd：取消拥塞控制，stream：是否采用流传输模式
	int(*output)(const char *buf, int len, struct IRTPCB *rtp, void *user);	//发送消息的回调函数
};

typedef struct IRTPCB irtpcb;

#define IRTP_LOG_OUTPUT			1
#define IRTP_LOG_INPUT			2
#define IRTP_LOG_SEND			4
#define IRTP_LOG_RECV			8
#define IRTP_LOG_IN_DATA		16
#define IRTP_LOG_IN_ACK			32
#define IRTP_LOG_IN_PROBE		64
#define IRTP_LOG_IN_WINS		128
#define IRTP_LOG_OUT_DATA		256
#define IRTP_LOG_OUT_ACK		512
#define IRTP_LOG_OUT_PROBE		1024
#define IRTP_LOG_OUT_WINS		2048

#ifdef __cplusplus	//if compiled by cpp
extern "C" {
#endif

// create a new rtp control object, 'conv' must equal in two endpoint
// from the same connection. 'user' will be passed to the output callback
// output callback can be setup like this: 'rtp->output = my_udp_output'
irtpcb* irtp_create(IUINT32 conv, void *user);

// set maximum window size: sndwnd=32, rcvwnd=32 by default
int irtp_wndsize(irtpcb *rtp, int sndwnd, int rcvwnd);

// fastest: irtp_nodelay(rtp, 1, 20, 2, 1)
// nodelay: 0:disable(default), 1:enable
// interval: internal update timer interval in millisec, default is 100ms 
// resend: 0:disable fast resend(default), 1:enable fast resend
// nc: 0:normal congestion control(default), 1:disable congestion control
int irtp_nodelay(irtpcb *rtp, int nodelay, int interval, int resend, int nc);

// update state (call it repeatedly, every 10ms-100ms), or you can ask 
// irtp_check when to call it again (without irtp_input/_send calling).
// 'current' - current timestamp in millisec. 
void irtp_update(irtpcb *rtp, IUINT32 current);

// flush pending data
void irtp_flush(irtpcb *rtp);

// user/upper level send, returns below zero for error
int irtp_send(irtpcb *rtp, const char *buffer, int len);

// when you received a low level packet (eg. UDP packet), call it
int irtp_input(irtpcb *rtp, const char *data, long size);

// user/upper level recv: returns size, returns below zero for EAGAIN
int irtp_recv(irtpcb *rtp, char *buffer, int len);

// release rtp control object
void irtp_release(irtpcb *rtp);

// set output callback, which will be invoked by rtp
void irtp_setoutput(irtpcb *rtp, int(*output)(const char *buf, int len,
	irtpcb *rtp, void *user));


// Determine when should you invoke irtp_update:
// returns when you should invoke irtp_update in millisec, if there 
// is no irtp_input/_send calling. you can call irtp_update in that
// time, instead of call update repeatly.
// Important to reduce unnacessary irtp_update invoking. use it to 
// schedule irtp_update (eg. implementing an epoll-like mechanism, 
// or optimize irtp_update when handling massive rtp connections)
IUINT32 irtp_check(const irtpcb *rtp, IUINT32 current);

// check the size of next message in the recv queue
int irtp_peeksize(const irtpcb *rtp);

// change MTU size, default is 1400
int irtp_setmtu(irtpcb *rtp, int mtu);

// read conv
IUINT32 irtp_getconv(const void *ptr);

// setup allocator
void irtp_allocator(void* (*new_malloc)(size_t), void(*new_free)(void*));

// get how many packet is waiting to be sent
int irtp_waitsnd(const irtpcb *rtp);

int irtp_set_redundancy(irtpcb *rtp, int redun);


#ifdef __cplusplus
}
#endif

#endif // !_IRTP_H_
