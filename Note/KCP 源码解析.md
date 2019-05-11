# KCP源码阅读

项目地址： [KCP - A Fast and Reliable ARQ Protocol](https://github.com/skywind3000/kcp)

# 简介

KCP是一个快速可靠协议，能以比 TCP浪费10%-20%的带宽的代价，换取平均延迟降低 30%-40%，且最大延迟降低三倍的传输效果。纯算法实现，并不负责底层协议（如UDP）的收发，需要使用者自己定义下层数据包的发送方式，以 callback的方式提供给 KCP。 连时钟都需要外部传递进来，内部不会有任何一次系统调用。


# 主要数据结构

## IKCPSEG

IKCPSEG是kcp的数据段结构，即储存需要发送的packet的内容。
```c
struct IKCPSEG
{
	struct IQUEUEHEAD node;	// 双向链表定义的队列
	IUINT32 conv;           // conversation, 会话序号: 接收到的数据包与发送的一致才接收此数据包
	IUINT32 cmd;            // command, 指令类型: 代表这个Segment的类型
	IUINT32 frg;            // fragment, 分段序号
	IUINT32 wnd;            // window, 窗口大小
	IUINT32 ts;             // timestamp, 发送的时间戳
	IUINT32 sn;             // sequence number, segment序号
	IUINT32 una;            // unacknowledged, 当前未收到的序号: 即代表这个序号之前的包均收到
	IUINT32 len;            // length, 数据长度
	IUINT32 resendts;		// 重发的时间戳
	IUINT32 rto;			// 超时重传的时间间隔
	IUINT32 fastack;		// ack跳过的次数，用于快速重传
	IUINT32 xmit;			// 发送的次数(即重传的次数)
	char data[1];
};

```

## IKCPCB

IKCPCB是kcp的控制块，用于控制packet的发送，接收，重传等操作。

buffer的发送和接收均调用外部传入的函数。IKCPCB只负责处理算法层面的逻辑。

```c
struct IKCPCB
{
	//conv:会话ID，mtu:最大传输单元，mss:最大分片大小，state:连接状态
	IUINT32 conv, mtu, mss, state;			
	//sun_una：第一个未确认的包，sen_nxt：待发送包的序号，rcv_nxt：待接收消息的序号
	IUINT32 snd_una, snd_nxt, rcv_nxt;		
	//ssthresh:拥塞窗口的阈值
	IUINT32 ts_recent, ts_lastack, ssthresh;
	//rx_rttval：ack接收rtt浮动值，rx_srtt：ack接收rtt平滑值(smoothed)，rx_rto：由ack接收延迟计算出来的复原时间，rx_minrto：最小复原时间
	IINT32 rx_rttval, rx_srtt, rx_rto, rx_minrto;	
	//sen_wnd：发送窗口大小，rcv_wnd：接收窗口大小，rmt_wnd：远端接收窗口大小，cwnd：拥塞窗口大小，probe	探查变量，IKCP_ASK_TELL表示告知远端窗口大小。IKCP_ASK_SEND表示请求远端告知窗口大小。
	IUINT32 snd_wnd, rcv_wnd, rmt_wnd, cwnd, probe;	
	//currunt：当前的时间戳，interval：内部flush刷新间隔，ts_flush：下次flush刷新时间戳
	IUINT32 current, interval, ts_flush, xmit;
	IUINT32 nrcv_buf, nsnd_buf;
	IUINT32 nrcv_que, nsnd_que;
	//nodelay：是否启动无延迟模式，update：是否调用过update函数的标识(kcp需要上层通过不断的ikcp_update和ikcp_check来驱动kcp的收发过程)
	IUINT32 nodelay, updated;
	//ts_probe：下次探查窗口的时间戳，probe_wait：探查窗口需要等待的时间
	IUINT32 ts_probe, probe_wait;
	//dead_link：最大重传次数，incr：可发送的最大数据量
	IUINT32 dead_link, incr;
	struct IQUEUEHEAD snd_queue;	//发送消息的队列
	struct IQUEUEHEAD rcv_queue;	//接收消息的队列
	struct IQUEUEHEAD snd_buf;		//发送消息的缓存
	struct IQUEUEHEAD rcv_buf;		//接收消息的缓存
	IUINT32 *acklist;				//待发送的ack的列表
	IUINT32 ackcount;				//ack数量
	IUINT32 ackblock;				//acklist的大小
	void *user;
	char *buffer;					//储存消息字节流的内存
	int fastresend;					//触发快速重传的重复ack个数
	int nocwnd, stream;				//nocwnd：取消拥塞控制，stream：是否采用流传输模式
	int logmask;
	int (*output)(const char *buf, int len, struct IKCPCB *kcp, void *user);	//发送消息的回调函数
	void (*writelog)(const char *log, struct IKCPCB *kcp, void *user);
};
```
# KCP网络包头结构

```c

|<------------ 4 bytes ------------>|
+--------+--------+--------+--------+
|               conv                | conv：Conversation, 会话序号，用于标识收发数据包是否一致
+--------+--------+--------+--------+ cmd: command，用于标识指令，例如：push，ack等
|  cmd   |  frg   |       wnd       | frg: Fragment, 分段序号，序号从大到小
+--------+--------+--------+--------+ wnd: 接收窗口大小
|                ts                 | ts: 发送的时间戳
+--------+--------+--------+--------+
|                sn                 | sn: Segment序号
+--------+--------+--------+--------+
|                una                | una: Unacknowledged, 当前未收到的序号，即代表这个序号之前的包均收到
+--------+--------+--------+--------+      
|                len                | len: data数据的长度
+--------+--------+--------+--------+

```
kcp包头总共占用了24个字节。给sn分配了4个字节，可以不用考虑序号越界的问题。

# KCP工作流程

## 发送数据

kcp首先将要发送的数据存到kcp->buffer中，如果需要发送的数据总量的大小大于kcp->mtu，则将buffer中的数据调用output函数发送出去，output函数由用户传入。

kcp数据包发送顺序：
1. `IKCP_CMD_ACK`(ack)
2. `IKCP_CMD_WASK`(请求远程窗口大小)
3. `IKCP_CMD_WINS`(发送本地窗口大小)
4. `IKCP_CMD_PUSH`(push data)

```
graph LR
ack--> |fill data| kcp_buffer[kcp->buffer] 
wnd_probe-->|fill data| kcp_buffer
wnd_tell-->|fill data| kcp_buffer
push --> |move_to| sen_buf
sen_buf[kcp ->snd_buf] --> |fill data| kcp_buffer
ack-->wnd_probe
wnd_probe[window probe]-->wnd_tell[window tell]
wnd_tell-->push[push data]
kcp_buffer--> check{check > mtu ?}
check -->|yes| send_data
```

在发送PUSH类型的数据时，首先需要将数据从sen_que移动到sen_buf中（在移动时会检测拥塞窗口的大小，sen_que可以理解为发送数的缓冲队列）。

kcp在发送sen_buf队列中的数据时会检测是否是第一次发送：
- 如果该segment的发送次数为0，则直接发送。
- 如果发送次数大于0并且已经超时，则再次发送并调整rto和下次超时时间
- 如果没有超时但是达到了快速重传的条件（被跳过了几个包），则发送并且更新下次超时时间。

## 接收数据

```
graph LR
A-->B
```


# 函数解析

## Input
```c
int ikcp_input(ikcpcb *kcp, const char *data, long size)
```

处理接收到的数据，data即用户传入的数据。kcp不负责网络数据的接收，需要用户将接收到的数据传入。

在接收到数据后，解析数据得到segment的sn，una等数据包头信息。首先根据una清除掉`kcp->snd_buf`中已经确认接收到的segment（una即表示该seg之前数据包均已收到），随后根据`kcp->sen_buf`更新`kcp->snd_una`。

1. ack: `IKCP_CMD_ACK`，首先根据该segment的rtt更新计算kcp的rtt和rto，删除掉`kcp->snd_buf`中相应的segment，更新kcp的sed_una(下一个未确认的segment)。
2. push:`IKCP_CMD_PUSH`，收到push segment后需要发送ack，将该segment的sn和ts放入`kcp->acklist`中。 如果该seg符合滑动窗口的范围，则将该segment放入`kcp->rev_buf`中。 如果`kcp->queue`的大小小于`kcp->rev_wnd`(滑动窗口的大小)，则将`kcp->rev_buf`符合条件的segment放入`kcp->rcv_queue`中(保证序号连续的seg)。
3. window ask: `IKCP_CMD_WASK`,将kcp->probe中相应位置为1，发送segment时向远端发送相应接收窗口大小。
4. `IKCP_CMD_WINS`，对应远端的窗口更新包，无需做额外的操作。

随后遍历`kcp->sed_buf`，更新每个segment的`seg->fastack`(被跳过的次数，用于判断是否需要快速重传)。

如果远端接收状态有更新，则更新本地拥塞窗口的大小。


## Receive 

```c
int ikcp_recv(ikcpcb *kcp, char *buffer, int len)
```

用户层面的数据读取，从`rcv_queue`中读取一个packet（如果该packet在发送前分段的话，则将fragement合并后放入buffer中）。input的操作保证了`rcv_queue`中的segment都是有序且连续的。

随后如果`rcv_queue`大小小于`rcv_wnd`（接收窗口）的大小，则将`rcv_buf`中合适的segment放入`rcv_queue`中。

如果需要告知远端主机窗口大小，则
```c
kcp->probe |= IKCP_ASK_TELL
```
将`ICKP_ASK_TELL`置为1。

## Send

```c
int ikcp_send(ikcpcb *kcp, const char *buffer, int len)
```

将buffer中需要发送的数据分片后放入`snd_queue`中。

在发送数据时，会首先检测是否开启了流模式。如果开启流模式，在发送数据时，如果上一个segment没有填满mss（最大分片大小），则将这次要发送的数据继续写入上一个segment。如果没有开启流模式，则创建一个新的segment发送。

如果需要发送的数据大小大于mss，则将其拆分为多个segment发送，如果不是流模式，则将其`frg`至为其相应的序号，序号从count-1开始递减至0，即count-1表示第一个segment，0表示最后一个segment。


## Flush

```c
void ikcp_flush(ikcpcb *kcp)
```

flush data，即发送需要发送的数据，ack，win probe，push data等，以及检测`snd_buf`中的数据是否需要重传。

将seg的wnd设置为接收窗口的剩余大小
```c
kcp->rcv_wnd - kcp->nrcv_que
```
`nrcv_que`指接收队列中的segment的数量。

在发送时将需要发送的数据填充到buffer中，如果buffer下次填入的数据量 > `mtu`，则调用output函数将buffer中的数据发送出去，ouotput函数由用户定义。

#### 发送segment

首先将`acklist`中需要发送的ack发送出去，kcp在发送时会优先发送ack。

如果远端接收窗口大小为0且当前时间超过下次发送窗口探测时间，则
```
kcp->probe |= IKCP_ASK_SEND;
```
并更新窗口探测时间间隔和下次窗口探测时间。

如果需要探测远端发送窗口大小或者需要告诉远端本机发送窗口大小，则发送相应的probe segment。

取拥塞窗口的大小为发送窗口和远端接收窗口的最小值。在已经发送的segment的数量不超过拥塞窗口大小时将送`sed_queue`中的segment放入`snd_buf`中。

遍历`snd_buf`中的数据：
1. 如果之前没有发送过，则设置其rto和超时重传时间，并将needsend置为1.
2. 如果`snd_buf`中的segment超过其超时重传时间，如果启动无延时模式，则将segment的rto*1.5，否则将rto * 2，并将needsend置为1。
3. 如果启动了快速重传并且该segment被跳过的次数大于resent，则将needsend置为1，并更新该segment的超时时间。

如果needsend被置为1，则将该segment发送出去。

如果segment的重传次数超过kcp的最大重传次数，则更新kcp的连接状态为-1。

发送之后更新ssthresh(拥塞窗口阈值)和cwnd(拥塞窗口大小)。