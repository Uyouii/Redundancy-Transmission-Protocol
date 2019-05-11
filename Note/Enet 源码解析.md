

# Enet源码解析


[ENet官方文档地址](http://enet.bespin.org/index.html)

首先看一下ENet官方给出的ENet的特性(Feature)。

# ENet 特性

ENet是一个具有TCP和UDP各自优点的基于UDP封装的一个网络库。

UDP缺少排序，连接管理，带宽管理，包大小限制等。TCP不能同时打开多个套接字故缺少多流的通信，并且由于其缓冲特性，其包管理机制过于复杂。

ENet则致力于将TCP和UDP的优点结合实现一个统一的网络协议库。

### 连接管理 (Connection Management)

ENet提供了一个简单的与外部主机通信的接口。连接的生命周期通过频繁的ping外部主机动态监管，同时通过主机与外部机器的包的往返时间和丢包情况来监管网络状况。

### 排序 (Sequencing)

ENet提供了多个的合理排序的网络包流而不是一个单一的比特流从而简化了不同类型数据的传输。

ENet通过为每个发送的网络包编号来实现对包的排序。这些序号会随着包的发送而增长。ENet保证序列号低的包优先发送，从而确保了所有的网络包都按次序发送。

对于不可靠的网络包，如果具有高序号的网络包已经到达，ENet则会简单的丢弃那些低序号的网络包。这样就保证了网络包到达后就可以立即被接收，从而减少了网络延时。对于可靠的网络包，如果一个高序号的包已经到达，但是之前的包确没有到达，ENet会推迟高序号包的接受直到其之前相应序号的网络包均已到达。

### 通道 (Channels)

既然ENet会推迟可靠网络包的提交来确保网络包的序号，但是无论到达的包是可靠的还是不可靠的，如果之前的网络包是可靠的包，他们都要被推迟提交，这样做可能会造成一些不需要严格保证次序的包同样被推迟提交，从而造成额外的延时。

为了解决上述问题并减少对包的次序的限制，ENet为一个连接提供了多个交流的通道(channel)。每个通道下的网络包可以独立排序，所以一个通道下的包的传送状态不会影响其他通道下的包的传送。

### 可靠性 (Reliability)

ENet为传送的包提供了可靠性选择，并确保外部主机会确认收到所有的可靠的网络包。如果外部主机在特定时间内没有确认收到网络包，ENet会尝试在合理的次数内重传这个网络包。 重传超时的时间会根据失败次数的增加变得更加宽松便于应对网络临时的混乱和拥塞。

### 拆分和重组 (Fragmentation and Reassembly)

ENet在发送包的时候不会考虑包的大小。大的网络包会被分成若干个大小合适的网络包，并在外部机器上重组为发送前的网络包便于远程机器接收。整个过程对于开发者来说都是透明的。

### 聚合 (Aggregation)

ENet包括了大部分协议指令，包括acknowledgement，packet transfer，确保连接的可用性，减少丢包的机会及其可能造成的延时等。

### 适应性 (Adaptability)

ENet为可靠包提供了一个动态适应的数据窗口以确保连接不会大量的网络包淹没掉。它也提供了一个静态的带宽分配机制以确保机器在发送和接受包时不会超过这个机器承载的范围。更近一步，ENet提供了一个动态的阀门来响应网络连接时带来的偏差，通过限制发送发送包的数量来应对各种类型的网络拥塞问题。

# ENet数据结构

## ENetHost

ENetHost即ENet的客户端。

```c
typedef struct _ENetHost
{
   ENetSocket           socket;
   ENetAddress          address;                    
   enet_uint32          incomingBandwidth;          
   enet_uint32          outgoingBandwidth;          
   enet_uint32          bandwidthThrottleEpoch;
   enet_uint32          mtu;
   enet_uint32          randomSeed;
   int                  recalculateBandwidthLimits;
   ENetPeer *           peers;                      
   size_t               peerCount;                   
   size_t               channelLimit;                
   enet_uint32          serviceTime;
   ENetList             dispatchQueue;
   int                  continueSending;
   size_t               packetSize;
   enet_uint16          headerFlags;
   ENetProtocol         commands [ENET_PROTOCOL_MAXIMUM_PACKET_COMMANDS];
   size_t               commandCount;
   ENetBuffer           buffers [ENET_BUFFER_MAXIMUM];
   size_t               bufferCount;
   ENetChecksumCallback checksum;                    
   ENetCompressor       compressor;
   enet_uint8           packetData [2][ENET_PROTOCOL_MAXIMUM_MTU];
   ENetAddress          receivedAddress;
   enet_uint8 *         receivedData;
   size_t               receivedDataLength;
   enet_uint32          totalSentData;               
   enet_uint32          totalSentPackets;            
   enet_uint32          totalReceivedData;           
   ENetInterceptCallback intercept;                  
   size_t               connectedPeers;
   size_t               bandwidthLimitedPeers;
   size_t               duplicatePeers;              
   size_t               maximumPacketSize;           
   size_t               maximumWaitingData;          
} ENetHost;
```



| 内部变量                       | 作用                                                         |
| ------------------------------ | ------------------------------------------------------------ |
| **socket**                     | 用于数据传输和连接的UDP套接字句柄                            |
| **address**                    | host的socket地址                                             |
| **incomingBandwidth**          | host接收数据的带宽，即download bandwidth                     |
| **outgoingBandwidth**          | host上传数据的带宽，即upstream bandwidth                     |
| **bandwidthThrottleEpoch**     | 记录host流量控制的时间戳，如果流量控制的时间间隔超过`bandwidthThrottleEpoch`，则进行流量控制。host流量控制时间间隔设置为1000ms。 |
| **mtu**                        | 即最大传输单元，当需要发送的单个packet的大小超过该值时会进行分片操作 |
| **randomSeed**                 | 用于生成connectID的随机数种子                                |
| **recalculateBandwidthLimits** | 用于记录是否需要重新计算带宽的标记变量，当有peer连接或者断开连接时会被置为1 |
| **peers**                      | host中用于储存peer的数组，在host初始化时设置，大小为`peerCount`。 |
| **peerCount**                  | 创建host时指定的peerCount，即最大的peer数。如果此时的peer数为`peerCount`，则在发起连接或者被动连接（收到connect command）时会失败，没有空间容纳新的peer。 |
| **channelLimit**               | 每个peer中可以容纳的channel数，最大为255，最小为1。          |
| **serviceTime**                | 标记host当前时间的时间戳                                     |
| **dispatchQueue**              | 待处理的peer队列。当peer中有event产生时，则将peer放入dispatchQueue中。 |
| **continueSending**            | 用于标记peer中的数据是否发送完的变量。在发送peer中的数据时为保证公平性，则对peer进行轮询发送，每次至多发送一个数据量小于mtu udp数据报，如果peer中数据没有发送完，则将该变量置为1，在下次循环继续发送。 |
| **packetSize**                 | 用于标记当前待发送的udp数据报中数据的大小，在添加数据前如果`packetSize`大小大于mtu，则将`continueSending`置为1，跳出循环，将当前数据发送出去，等待下次循环中发送剩余数据。 |
| **headerFlags**                | 标记发送特性的一些flag，例如是否发送当前时间，是否需要压缩等。`host->headerFlags`会携带到发送数据报的头部中发送到peer端。 |
| **commands**                   | 用于储存当前待发送udp数据报中的command                       |
| **commandCount**               | 记录当前待发送udp数据报中的command的数量，如果需要发送的command的数量大于`ENET_PROTOCOL_MAXIMUM_PACKET_COMMANDS`，则将当前数据报发送出去，剩余数据等待下次循环发送。 |
| **buffers**                    | 储存需要发送数据的buffer，用于传递给socket接口发送数据       |
| **bufferCount**                | 记录buffers中buffer的个数，同样传递给相应socke接口用于数据发送。最大值为`ENET_BUFFER_MAXIMUM`，如果buffers中buffer的个数大于该值，则先将当前数据报发送出去，剩余数据等待下次循环发送。 |
| **checksum**                   | 计算校验和的回调函数，需要用户自己设置                       |
| **compressor**                 | 用于压缩和解压的结构变量，需要用户自己设置                   |
| **packetData**                 | 一个二维数组，其中packetData[0]用于储存接收的数据，packetData[1]用于储存压缩后待发送的数据 |
| **receivedAddress**            | 接收数据的socket地址，host和port都设为0则意味接收任意地址的数据 |
| **receivedData**               | 用于标记当前读取到packetData[0]中位置的指针                  |
| **receivedDataLength**         | 当前接收的数据的长度                                         |
| **totalSentData**              | host发送出的总的数据量                                       |
| **totalSentPackets**           | host发送出的总的udp数据报数                                  |
| **totalReceivedData**          | host接收的总的数据量                                         |
| **totalReceivedPackets**       | host接收的总的udp数据报数                                    |
| **connectedPeers**             | 当前连接的peer的数量                                         |
| **bandwidthLimitedPeers**      | 需要进行流量控制的peer的数量                                 |
| **duplicatePeers**             | 允许重复的ip的最大的peer的数量，默认值为`ENET_PROTOCOL_MAXIMUM_PEER_ID` |
| **maximumPacketSize**          | 允许一个单个的`ENetPacket`发送和接收的最大的数据量           |
| **maximumWaitingData**         | 允许等待在buffer中的最大的数据量                             |


## ENetPeer

用于储存通信对端的信息的数据结构，同时控制数据的发送，重传等操作。

```c
typedef struct _ENetPeer
{ 
   ENetListNode  dispatchList;
   struct _ENetHost * host;
   enet_uint16   outgoingPeerID;
   enet_uint16   incomingPeerID;
   enet_uint32   connectID;
   enet_uint8    outgoingSessionID;
   enet_uint8    incomingSessionID;
   ENetAddress   address;           
   void *        data;               
   ENetPeerState state;
   ENetChannel * channels;
   size_t        channelCount;       
   enet_uint32   incomingBandwidth;  
   enet_uint32   outgoingBandwidth;  
   enet_uint32   incomingBandwidthThrottleEpoch;
   enet_uint32   outgoingBandwidthThrottleEpoch;
   enet_uint32   incomingDataTotal;
   enet_uint32   outgoingDataTotal;
   enet_uint32   lastSendTime;
   enet_uint32   lastReceiveTime;
   enet_uint32   nextTimeout;
   enet_uint32   earliestTimeout;
   enet_uint32   packetLossEpoch;
   enet_uint32   packetsSent;
   enet_uint32   packetsLost;
   enet_uint32   packetLoss;         
   enet_uint32   packetLossVariance;
   enet_uint32   packetThrottle;
   enet_uint32   packetThrottleLimit;
   enet_uint32   packetThrottleCounter;
   enet_uint32   packetThrottleEpoch;
   enet_uint32   packetThrottleAcceleration;
   enet_uint32   packetThrottleDeceleration;
   enet_uint32   packetThrottleInterval;
   enet_uint32   pingInterval;
   enet_uint32   timeoutLimit;
   enet_uint32   timeoutMinimum;
   enet_uint32   timeoutMaximum;
   enet_uint32   lastRoundTripTime;
   enet_uint32   lowestRoundTripTime;
   enet_uint32   lastRoundTripTimeVariance;
   enet_uint32   highestRoundTripTimeVariance;
   enet_uint32   roundTripTime;           
   enet_uint32   roundTripTimeVariance;
   enet_uint32   mtu;
   enet_uint32   windowSize;
   enet_uint32   reliableDataInTransit;
   enet_uint16   outgoingReliableSequenceNumber;
   ENetList      acknowledgements;
   ENetList      sentReliableCommands;
   ENetList      sentUnreliableCommands;
   ENetList      outgoingReliableCommands;
   ENetList      outgoingUnreliableCommands;
   ENetList      dispatchedCommands;	
   int           needsDispatch;
   enet_uint16   incomingUnsequencedGroup;
   enet_uint16   outgoingUnsequencedGroup;
   enet_uint32   unsequencedWindow [ENET_PEER_UNSEQUENCED_WINDOW_SIZE / 32]; 
   enet_uint32   eventData;
   size_t        totalWaitingData;
} ENetPeer;
```

| 内部变量                           | 作用                                                         |
| ---------------------------------- | :----------------------------------------------------------- |
| **dispatchList**                   | 在host中的`dispatchQueue`的链表节点。每当该peer产生一个event时，便将peer放入 `host->disptachQueue`中 |
| **host**                           | peer所在的host的指针                                         |
| **outgoingPeerID**                 | 对端`host->peers`中的peer的index                             |
| **incomingPeerID**                 | peer在本地`host->peers`中的index                             |
| **connectID**                      | 在创建host时会生成一个随机数种子，每次请求新的连接时，会将随机数种子递增1产生 connectID。用于匹配收到的 verify connect 是否对应之前connect请求。当需要计算校验和时，connectID也参与校验和的计算。 |
| **outgoingSessionID**              | 本地用于发送时封装protocol header的会话号，接收端在收到数据报时会将该 sessionid 取出与接收端的`peer->incomingSessionID`进行匹配。 |
| **incomingSessionID**              | 没到收到一个udp数据报时，会将protocol header中封装的session ID 取出与本地的`incomingSessionID`匹配，用于判断收到的数据报是否属于本次会话。 |
| **address**                        | 该peer的socket地址                                           |
| **state**                          | peer当前的状态，例如已经连接，断开连接等。                   |
| **channels**                       | peer用于发送数据的channel                                    |
| **channelCount**                   | `channels`的大小                                             |
| **incomingBandwidth**              | 该peer下载的带宽(字节/秒)                                    |
| **outgoingBandwidth**              | 该peer上传的带宽(字节/秒)                                    |
| **incomingBandwidthThrottleEpoch** | 在调节packetThrottle时用于记录调节该peer下载带宽的时间戳     |
| **outgoingBandwidthThrottleEpoch** | 在调节packetThrottle时用于记录调节该peer上传带宽的时间戳     |
| **incomingDataTotal**              | 记录在流量控制的间隔时间内从该peer接收的总的数据量的大小     |
| **outgoingDataTotal**              | 记录在流量控制的间隔时间内向该peer发送的总的数据量的大小     |
| **lastSendTime**                   | 记录上次发送数据包的时间戳                                   |
| **lastReceiveTime**                | 记录上次收到ack的时间，如果超过一定时间没有收到ack，则host会向该peer发送ping包用于判断该peer是否已经断开连接 |
| **nextTimeout**                    | 下次有数据包超时的时间戳。在发送command和接收ack时会根据数据包的rto设置peer下次超时的时间戳。如果检测到当前时间戳大于该值，则调用`enet_protocol_check_timeouts`进行超时检测。 |
| **earliestTimeout**                | 记录当前时间段内的最早的超时时间。每当收到ack时会将`earliestTimeout`重置为0 |
| **packetThrottle**                 | 流量控制的阀门，通过该值进行流量控制，最大值为32，最小为1    |
| **packetThrottleLimit**            | 在进行流量控制时会根据peer设置的带宽计算出该peer的`packetThrottle`的上限，确保数据的发送不会超过host的发送能力和peer的接收能力。`packetThrottle`的大小不会超过`packetThrottleLimit`的大小 |
| **packetThrottleCounter**          | 在发送不可靠包时会根据`packekThrottleCounter`的值和`packetThrottle`的值判断是否会在发送前丢掉该不可靠包 |
| **packetThrottleEpoch**            | 记录当前更新`lastRoundTripTime`,`lastRoundTripTimeVariance`,`lowestRoundTripTime`,`highestRoundTripTimeVariance`的时间戳，每隔`packetThrottleInterval`的时间间隔会刷新一次 |
| **packetThrottleAcceleration**     | 每当收到rtt时用于增加`packetThrottle`的增量                  |
| **packetThrottleDeceleration**     | 每当收到rtt试用于减少`packetThrottle`的增量                  |
| **packetThrottleInterval**         | 流量控制中相应变量更新周期的大小                             |
| **pingInterval**                   | 如果超过`pingInterval`的时间没有收到ack，则向peer发送一个ping包，用于探测是否断开连接 |
| **lastRoundTripTime**              | 记录上个流量控制周期中最小的rtt                              |
| **lowestRoundTripTime**            | 记录当前流量控制周期中最小的rtt                              |
| **lastRoundTripTimeVariance**      | 记录上个流量控制周期中最大的rtt的变化值                      |
| **highestRoundTripTimeVariance**   | 记录当前流量控制周期中最大的rtt的变化值                      |
| **roundTripTime**                  | 该peer当前平滑的rtt                                          |
| **roundTripTimeVariance**          | 该peer当前平滑的rtt的变化值                                  |
| **mtu**                            | 该peer的最大传输单元，当需要发送的单个packet的大小超过该值时会进行分片操作 |
| **reliableDataInTransit**          | 正在传输过程中的可靠包的大小（已经发送但没有收到ack的），如果其值超过由`packetThrottle`计算出的发送窗口的大小，则暂停发送 |
| **outgoingReliableSequenceNumber** | 由peer发送的ENet的系统指令的当前的包的序号。所谓系统指令指connect，disconnect，ping等指令。而reliable，unreliable数据包会在channel中发送，用的是channel的序号。 |
| **acknowledgements**               | 等待发送ack的队列                                            |
| **sentReliableCommands**           | 已经发送reliable但是没收到ack的队列                          |
| **sentUnreliableCommands**         | 已经发送的unreliable的队列                                   |
| **outgoingReliableCommands**       | 等待发送reliable的队列                                       |
| **outgoingUnreliableCommands**     | 等待发送unreliable的队列                                     |
| **dispatchedCommands**             | 已经收到的，等待用户处理的指令队列                           |
| **needsDispatch**                  | 是否需要用户处理，如果`dispatchedCommands`队列中有指令，则将该值置为1 |
| **incomingUnsequencedGroup**       | peer记录的当前到来的unsequenced数据包的该group的头部的序号   |
| **outgoingUnsequencedGroup**       | 发送的unsequenced数据包的序号                                |
| **unsequencedWindow**              | 用位图的方式记录当前unsequenced group的数据包有没有重复      |
| **totalWaitingData**               | ENet已经收到的但是用户还未处理的数据大小的总和               |


### SessionID的作用

ENet使用简单的SessionID的匹配防止两个具有相同ip地址和端口号的前后两次连接发送的数据发生混淆。（这种情况是在断开连接后如果发起的新的连接的端口号和之前的端口号相同，在连接时会被判定为相同的连接，这时如果之前连接发送的网络包在网络中没有消逝并发送到对端，会与本次的连接发送的数据产生混淆。TCP使主动断开连接的一方处于`TIME_WAIT`的状态来防止这种情况的发生。）

在每次请求连接时，接收请求连接的一方会更新相应的session并返回给该peer，在第二次握手时请求连接方会同步该Session的数值。在每次发送数据时会将SessionID包含在`ENetProtocolHeader.peerID`中，接收端在每次收到UDP数据报时会首先检测protocol header中的session ID，如果不匹配，则丢掉该数据包中的数据，表明该数据包不是本次连接中发送的数据。

因为仅仅是简单的ID匹配，所以并不能像TCP那样100%防止两次连接中数据包混淆这种情况的发生，但是大部分情况下仍是有效的。

## ENet Channel

每个peer中会有多个Channel用于数据的发送，每个Channel发送和接收数据的过程和对command的编号彼此是独立的。

```c
typedef struct _ENetChannel
{
   enet_uint16  outgoingReliableSequenceNumber;
   enet_uint16  outgoingUnreliableSequenceNumber;
   enet_uint16  usedReliableWindows;
   enet_uint16  reliableWindows [ENET_PEER_RELIABLE_WINDOWS];
   enet_uint16  incomingReliableSequenceNumber;
   enet_uint16  incomingUnreliableSequenceNumber;
   ENetList     incomingReliableCommands;
   ENetList     incomingUnreliableCommands;
} ENetChannel;
```

| 内部变量                             | 作用                                                         |
| :----------------------------------- | :----------------------------------------------------------- |
| **outgoingReliableSequenceNumber**   | channel中当前发送的可靠包的序号                              |
| **outgoingUnreliableSequenceNumber** | channel中当前发送的不可靠包的序号                            |
| **usedReliableWindows**              | 用位图的方式记录已经使用的发送窗口的序号                     |
| **reliableWindows**                  | 每个发送窗口中已经发送但是还没有收到ack的指令的个数          |
| **incomingReliableSequenceNumber**   | 已经收到的可靠包的序号                                       |
| **incomingUnreliableSequenceNumber** | 已经收到的不可靠包的序号                                     |
| **incomingReliableCommands**         | 已经收到的reliable数据包，等待有序排序后转到`peer->dispatchedCommands`队列中 |
| **incomingUnreliableCommands**       | 已经收到的unreliable的数据包，等待转到`peer->dispatchedCommands`队列中 |

### 关于reliableWindow

由于ENet采用的是选择重传的方式，为保证新窗口与老窗口的序号没有重叠，窗口的最大尺寸不应该超过序号空间的一半。ENet在发送新的数据包时会通过`usedReliableWindows`判断当前窗口占用是否与空闲窗口重叠，如果重叠则暂停数据包的发送。`reliableWindows`会记录各个窗口中目前在传输中的包的个数。

# ENet 协议

ENet发送数据时以一个udp数据报为单位，在发送时首先会在每个udp数据报的头部包含一个4个字节的`protocol header`表明当前数据报内的各个command由哪个peer发送和相应的发送时间。 在protocl header后会包含多个command。

```c
|<------------ command1 ----------->|<------------ command2 ----------->|  
-------------------------------------------------------------------------------------------------------
| protocol Header | command1 header | command1 data   | command2 header | command2 data   |     ...
-------------------------------------------------------------------------------------------------------
```

### 协议头部 (Protocol Header)

```c
typedef struct _ENetProtocolHeader
{
   enet_uint16 peerID;
   enet_uint16 sentTime;
} ENET_PACKED ENetProtocolHeader;
```
Protocol Header是ENet整个协议的头部，由于ENet底层由UDP封装而成，在发送UDP数据报时该字段会放在整个UDP数据报的头部，用于标记该数据报的peer和发送时间。

与TCP的端对端连接不同，ENet可以是多对多的连接，所以需要`peerID`字段标记相应的peer。peerID的同步会在三次握手时同步完成。

> protocol header
```
|<------------ 4 bytes ------------>|
+--------+--------+--------+--------+
|      peerID     |     sentTime    | 
+--------+--------+--------+--------+ 
|<---------protocol header--------->|
```



### 指令头部(Command Header)

ENet中共有12种command，其中每个command会有一个相同的4个字节的command heaer，包含command每个command必须的信息：command类型，所在的channelID和该command的序列号。除去command header，每个command剩余的内容根据command类型而不同，所以ENet不同类型的command的大小是不同的。

```c
typedef struct _ENetProtocolCommandHeader
{
   enet_uint8 command;
   enet_uint8 channelID;
   enet_uint16 reliableSequenceNumber;
} ENET_PACKED ENetProtocolCommandHeader;
```
在每次发送的UDP的数据报中可能会包含多个指令(Command)，所以由每个指令的头部来标记该指令的信息，包括:
- **command**: 指令类型
- **channelID**:该command的所在channel的序号
- **reliableSequenceNumber**:该指令在相应channel的序号

每个指令对应固定的格式，所以根据`command`对应的指令类型，便可以得到该指令对应的长度。

> command header
```
|<-------------------- 4 bytes -------------------->|
+------------+------------+------------+------------+
|   command  |  channelID | reliableSequenceNumber  | 
+------------+------------+------------+------------+ 
|<------------------command header----------------->|
```

## ENet 协议类型

ENet共有12种协议类型，每种协议类型会对应一个`Command Number`。

协议类型定义如下：
```c
typedef enum _ENetProtocolCommand
{
   ENET_PROTOCOL_COMMAND_NONE               = 0,
   ENET_PROTOCOL_COMMAND_ACKNOWLEDGE        = 1,
   ENET_PROTOCOL_COMMAND_CONNECT            = 2,
   ENET_PROTOCOL_COMMAND_VERIFY_CONNECT     = 3,
   ENET_PROTOCOL_COMMAND_DISCONNECT         = 4,
   ENET_PROTOCOL_COMMAND_PING               = 5,
   ENET_PROTOCOL_COMMAND_SEND_RELIABLE      = 6,
   ENET_PROTOCOL_COMMAND_SEND_UNRELIABLE    = 7,
   ENET_PROTOCOL_COMMAND_SEND_FRAGMENT      = 8,
   ENET_PROTOCOL_COMMAND_SEND_UNSEQUENCED   = 9,
   ENET_PROTOCOL_COMMAND_BANDWIDTH_LIMIT    = 10,
   ENET_PROTOCOL_COMMAND_THROTTLE_CONFIGURE = 11,
   ENET_PROTOCOL_COMMAND_SEND_UNRELIABLE_FRAGMENT = 12,
   ENET_PROTOCOL_COMMAND_COUNT              = 13,

   ENET_PROTOCOL_COMMAND_MASK               = 0x0F
} ENetProtocolCommand;
```

由于在协议中每个`Command Number`由一个字节储存，但是总共的协议号不超过16，所以4个bit便足够储存，剩下的4位bit ENet用于标记该Command的一些特性，例如是否需要排序，是否需要发送验证（Ack）等。



### Acknowledge指令

```c
typedef struct _ENetProtocolAcknowledge
{
   ENetProtocolCommandHeader header;
   enet_uint16 receivedReliableSequenceNumber;
   enet_uint16 receivedSentTime;
} ENET_PACKED ENetProtocolAcknowledge;
```
Acknowledge是接收确认指令，在收到可靠包后需要向发送端发送Acknowledge指令来表明已经收到了相应的数据包。其中包含了：
- **header**: 相应command的头部
- **receivedReliableSequenceNumber**: 需要返回Ack的相应指令的序号（不是该Acknowledge的序号，而是其对应发送端发送的指令的序号）
- **receivedSentTime**:其对应指令的发送时间，用于计算相应指令的rtt。

对于每个`CommandHeader`中commnd参数带有`ENET_PROTOCOL_COMMAND_FLAG_ACKNOWLEDGE`标记的command，接收端都会向发送端发送Ack指令。如果发送端在相应时间内没有收到Ack，则会重发该command，直到收到Ack。`ENET_PROTOCOL_COMMAND_FLAG_ACKNOWLEDGE`的值是`1 << 7`,占用commandNumber中空白位来标记该command是否需要重传。

### Connect指令

```c
typedef struct _ENetProtocolConnect
{
   ENetProtocolCommandHeader header;
   enet_uint16 outgoingPeerID;
   enet_uint8  incomingSessionID;
   enet_uint8  outgoingSessionID;
   enet_uint32 mtu;
   enet_uint32 windowSize;
   enet_uint32 channelCount;
   enet_uint32 incomingBandwidth;
   enet_uint32 outgoingBandwidth;
   enet_uint32 packetThrottleInterval;
   enet_uint32 packetThrottleAcceleration;
   enet_uint32 packetThrottleDeceleration;
   enet_uint32 connectID;
   enet_uint32 data;
} ENET_PACKED ENetProtocolConnect;
```

connect指令用于主动发起连接的一端进行主动连接操作，其中包含的参数较多：
- **header**: command的头部
- **outgoingPeerID**: 对应本地端的`ENetPeer.incomingPeerID`，后续通信通过该peerID访问`host->peers`中的相应的peer。
- **incomingSessionID**: 对应本地端的`ENetPeer.incomingSessionID`，本地用于匹配对端发送的Session ID，即每次收到该peer的UDP数据报时会与数据报中的Session ID的值进行匹配。
- **outgoingSessionID**: 对应本地端的`ENetPeer.outgoingSessionID`，本地用于发送时封装protocol header的会话号，接收端在收到数据报时会将该sessionid取出与接收端的`incomingSessionID`进行匹配。
- **mtu**:即最大传输单元。在发送数据大小大于mtu的packet时，ENet会在本地进行分片，并在接收端进行重组。
- **windowSize**: ENet发送窗口的大小，控制ENet在传输过程中的可靠包的数据量。每个peer会独立计算windowSize，在发送数据包时如果检测到正在传输的数据
- **channelCount**:channel的数量
- **incominBandwidth**: 对应本地的`host->incomingBandwidth`，即host的下载带宽。
- **outgoingBandwidth**: 对应本地的`host->outgoingBandwidth`，即host的上传带宽。
- **packetThrottleInterval**: 通过rtt对`packetThrottle`调节周期的设置。
- **packetThrottleAcceleration**：
- **packetThrottleDeceleration**：
- **connectID**：防止重复的连接（比如：connect 命令丢掉后重发）

### Verify Connet指令

verify Connect用于三次握手连接的第二次握手，同时用于主动连接方同步被动连接方的相关信息。

```c
typedef struct _ENetProtocolVerifyConnect
{
   ENetProtocolCommandHeader header;
   enet_uint16 outgoingPeerID;
   enet_uint8  incomingSessionID;
   enet_uint8  outgoingSessionID;
   enet_uint32 mtu;
   enet_uint32 windowSize;
   enet_uint32 channelCount;
   enet_uint32 incomingBandwidth;
   enet_uint32 outgoingBandwidth;
   enet_uint32 packetThrottleInterval;
   enet_uint32 packetThrottleAcceleration;
   enet_uint32 packetThrottleDeceleration;
   enet_uint32 connectID;
} ENET_PACKED ENetProtocolVerifyConnect;
```
其中参数基本和connect命令中的参数相同，不一一列举了。

### Protocol Bandwidth Limit指令

```c
typedef struct _ENetProtocolBandwidthLimit
{
   ENetProtocolCommandHeader header;
   enet_uint32 incomingBandwidth;
   enet_uint32 outgoingBandwidth;
} ENET_PACKED ENetProtocolBandwidthLimit;
```

Bandwidth Limit指令用于流量控制时调节对端对应本地的peer的带宽的相应的数值。

- **header**: command的头部
- **incomingBandwidth**: 对应本地端的`host->incomingBandwidth`，设置对端的peer对应本地的host的下载带宽的数值。
- **outgoingBandwidth**: 对应本地端的`host->outgoingBandwidth`，设置对端的peer对应本地host的上行带宽的数值。

### Throttle Configure 指令

```c
typedef struct _ENetProtocolThrottleConfigure
{
   ENetProtocolCommandHeader header;
   enet_uint32 packetThrottleInterval;
   enet_uint32 packetThrottleAcceleration;
   enet_uint32 packetThrottleDeceleration;
} ENET_PACKED ENetProtocolThrottleConfigure;
```

Throttle Configure用于调节由rtt控制 packetThrottle的相关设置，关于packetThrottle的作用请看ENet流量控制相关章节。

- **header**: command的头部
- **packetThrottleInterval**: peer调节rtt相关参数的周期
- **packetThrottleAcceleration**: 根据rtt调节packetThrottle的增加的速率
- **packetThrottleDeceleration**： 根据rtt调节packetThrottle的减小的速率

### Disconnect 指令

```c
typedef struct _ENetProtocolDisconnect
{
   ENetProtocolCommandHeader header;
   enet_uint32 data;
} ENET_PACKED ENetProtocolDisconnect;
```

用于断开连接。

### Ping指令

```c
typedef struct _ENetProtocolPing
{
   ENetProtocolCommandHeader header;
} ENET_PACKED ENetProtocolPing;
```
ENet当检测到超过一定的时间没有收到ack时便会发送ping指令以判断当前相连的peer是否已经断开连接。因为是探测指令，所以只发送指令头即可。

### Send Reliable 指令

```c
typedef struct _ENetProtocolSendReliable
{
   ENetProtocolCommandHeader header;
   enet_uint16 dataLength;
} ENET_PACKED ENetProtocolSendReliable;
```

用于发送不用分片的可靠包的指令，在发送时，相应的data数据会跟在指令的后面。

- **header**: 指令头部
- **dataLength**: 发送数据的长度

### Send Unreliable 指令
```c
typedef struct _ENetProtocolSendUnreliable
{
   ENetProtocolCommandHeader header;
   enet_uint16 unreliableSequenceNumber;
   enet_uint16 dataLength;
} ENET_PACKED ENetProtocolSendUnreliable;
```
发送不需要分片的Unreliable指令，与ENet unrealiable包的实现机制相关，不仅需要携带reliable sequence number，还需要携带unreliable sequence number。

- **unreliableSequeceNumber**: 不可靠包的序号
- **dataLength**: 发送数据的长度

### Send Unsequenced指令

```c
typedef struct _ENetProtocolSendUnsequenced
{
   ENetProtocolCommandHeader header;
   enet_uint16 unsequencedGroup;
   enet_uint16 dataLength;
} ENET_PACKED ENetProtocolSendUnsequenced;
```
发送不需要分片的Unsequenced指令，与reliable包和unreliable包的实现都不同，unsequenced不会用到header中的reliable sequence number，而是用相应的unsequencedGroup标记相应的序号。

- **unsequencedGroup**: 用于标记unsequenced包的序号
- **dataLength**: 需要发送的数据的长度

### Send Fragment指令

```c
typedef struct _ENetProtocolSendFragment
{
   ENetProtocolCommandHeader header;
   enet_uint16 startSequenceNumber;
   enet_uint16 dataLength;
   enet_uint32 fragmentCount;
   enet_uint32 fragmentNumber;
   enet_uint32 totalLength;
   enet_uint32 fragmentOffset;
} ENET_PACKED ENetProtocolSendFragment;
```

fragment用于发送所有需要分片的数据包，通过flag标记相应包的类型，例如reliable和unreliable等。

- **startSequenceNumber**: 被分片的数据包的开始的序号
- **dataLength**: 该分片携带的数据的长度
- **fragmentCount**: 总的分片的数量
- **fragmentNumber**: 该分片在所有分片中的序号，从0开始
- **totalLength**: 分片前的总的数据长度
- **fragmentOffset**: 该分片的起始位置在分片前的数据包中的偏移量

# ENet数据包类型

ENet中主要的数据包类型有：**Reliable**，**Unreliable**和**Unsequenced**和系统指令。

数据包的类型是通过commandheader中的command的携带的protocolFlag标记的。

由于command有8个字节，而ENet只有13种指令，只需要用到前4个bit，所以剩下的bit可以用来标记相应的数据包的类型，用于标记数据包的类型主要有两个flag：
```c
ENET_PROTOCOL_COMMAND_FLAG_ACKNOWLEDGE = (1 << 7),
ENET_PROTOCOL_COMMAND_FLAG_UNSEQUENCED = (1 << 6),
```
acknowledge和unsequenced。

如果标记unsequenced，则该数据包对应为unsequence数据包。
如果标记acknowledge，则对应reliable数据包，意味着需要对端返回ack指令。
如果都没有标记，则对应unreliable数据包。

系统指令一般是reliable的，如果标记了unsequenced，则会当成unsequenced处理，例如diconnectnow功能中的disconnect指令。

## 系统指令

系统指令包括有：`connect`,`verifyConnect`, `disconnect`, `ping`, `bandwidthLimit`, `throttleConfigure`。

系统指令所在的Channel是255，即不会占用peer中的任何一个channel。

系统指令一般都是需要对端发送ack的。

并且系统指令的发送不会受流量控制的限制，流量控制只是针对发送数据包的指令有效。

## Reliable数据包

如果相应命令标记了acknowledge，则会要求接收方收到后返回ack指令，如果没有收到ack，则会对数据包进行重传。如果重传次数超过一定的限制，则对该peer进行断开连接。

如果命令标记acknowledge的命令是`ENetProtocolSendReliable`和`ENetProtocolSendFragment`类型，接收方还会对指令进行排序。

对于`ENetProtocolSendReliable`则会对数据包进行排序，之后收到连续有序的数据包才会dispatch给用户。如果序号较大的数据包已经到达，而之前的数据包没有到达，ENet则会等待之前的数据包都到达以后，才会将相应的数据包发给用户。

对于`ENetProtocolSendFragment`类型的指令，接收方只有当分片中所有的数据包都收到后才会将数据dispatch给用户，如果有任何一个没有收到，则会等待至所有数据包都收到后。

reliable数据包的流量控制通过packetThrottle计算发送窗口大小实现，如果发现已经在传输中的reliable数据的量超过发送窗口大小，则会暂停对reliable数据包的传输，直到有空闲的发送窗口大小。

## Unreliable 数据包

对于Unreliable数据包，不会要求对端返回ack指令，同样会对数据包进行排序，但是与reliable数据包不同的是，如果后续的数据包已经到达，而之前的数据包则没有收到，则直接会将已经收到的数据包dispatch给用户，如果之前的数据包再次到达的话，则会直接丢弃。

并且Unreliable数据包的编号需要依赖reliable 序号，每次发送reliable数据包时，都会将unreliable数据包的编号重置为0开始传输。每个unreliable数据包会携带当前channel的reliable序号和unreliable序号。如果后续的reliable序号已经到达，则当前unreliable会被丢弃，如果后续的unreliable包到达，则当前unreliable序号同样会丢弃。

同时需要注意的一个细节是，ENet的每个数据包的sequence number的大小只有16位，也就是说最大序号只有65535，如果需要overflow的话，会从0开始重新统计。reliable包在进行接收的时候会判断当前的序号有没有已经overflow，但是对unreliable包确没有进行这种判断。

对于fragment的unreliable数据包，同样会等待所有分片都到达后才会将该数据包dispatch给用户，如果在处理分片的过程中，有该所有分片的后续序号的数据包到达，该分片中所有的数据包也会被丢弃。

unreliable数据包在进行流量控制的时候是通过packetThrottle计算一个数值在发送前进行概率性的随机丢弃的。如果packetThrottle是最大值，则不会丢弃，如果packetThrottle的值越小，则被丢弃的可能性越大。

## Unsequenced数据包

Unsequenced数据包同样是不会要求返回ack命令的，与unreliable不同的是，它不会依赖于reliable序号，也不会排序，对端只要收到，便会直接dispatch给用户。

unsequended数据包占用的队列其实是unreliable数据包的队列，所以流量控制也与unreliable数据包相同，会通过pakcetThrottle计算数值概率随机丢弃。

unsequenced数据包没有分片类型，如果需要分片，则会直接转成unreliable fragment发送。

# ENet整体流程框架

## 收发数据过程

ENet会在创建时建立一个`ENetHost`作为通信的客户端，host中包含与peer进行通信的socket，一个`ENetList dispatchQueue`：用于存放有事件产生的peer队列，和一个`ENetPeer* peers`数组用于存放与外部客户端通信的peer数据结构。

每个`ENetPeer`结构主要用于管理与外部的连接和数据发送，`ENetPeer`中用于数据发送的队列主要有5个：
```c
ENetList      acknowledgements;
ENetList      sentReliableCommands;
ENetList      sentUnreliableCommands;
ENetList      outgoingReliableCommands;
ENetList      outgoingUnreliableCommands;
```
- **acknowledgements**: 用于发送ack
- **sentReliableCommands**: 用于储存已经发送的可靠包
- **sentUnreliableCommands**: 用于储存已经发送的不可靠包
- **outgoingReliableCommands**: 准备发送的可靠包队列
- **outgpingUnreliableCommands**: 准备发送的不可靠包队列

其中需要ack的指令均由reliable队列管理，unreliable和unsequenced数据包均由unreliable队列管理。

在调用enet_peer_send函数时，会将需要发送的数据压入到outgoing队列中。而ack则是在收到可靠包时，将数据压入到`acknowledgements`队列中。

peer中还有一个队列：
```c
ENetList      dispatchedCommands;
```
用于储存已经接收好准备dispatch给用户的数据。

每个`ENetPeer`中还有一个`ENetChannel`数组，`ENetChannel`主要用于接收对端发送回来的数据。

`ENetChannel`中有两个队列：
```c
ENetList     incomingReliableCommands;
ENetList     incomingUnreliableCommands;
```
- **incomingRelibaleCommands**: 用于储存已经收到的可靠数据包。
- **incomingUnreliableCommands**: 用于储存已经收到的不可靠数据包

对数据的排序操作会在将其放进channel中的这两个incoming队列中进行，在channel中已经排序好的数据（reliable 数据包的话还需要保证序号连续）会放到peer的`dispatchedCommands`队列中，并且将peer放到host的`dispatchQueue`，当host下次处理时如果发现`dispatchQueue`中已经有peer存在，则会对其进行处理。如果没有，则会进入正常的收发包流程。

## host运行过程

host的主要运行通过一个`enet_host_service`函数，当有事件产生时，`enet_host_service`函数就会返回1，并将相应的event储存在传入的`ENetEvent`指针中，如果没有超过时间限制没有事件产生，则会返回0，出现错误时，`enet_host_service`会返回-1。

`enet_host_service`的整体流程如下：
```c
enet_protocol_dispatch_incoming_commands (host, event);

do {
    enet_host_bandwidth_throttle (host);

    enet_protocol_send_outgoing_commands (host, event, 1);

    enet_protocol_receive_incoming_commands (host, event);

    enet_protocol_send_outgoing_commands (host, event, 1);

    enet_protocol_dispatch_incoming_commands (host, event);

    do{
        enet_socket_wait();
    }while
}while  
```
在`enet_host_service`函数的每一个步骤中如果产生了Evnet，则直接返回。

首先会调用`protocol_dispatch_incoming_commands`函数查看当前是否存在待处理的事件，如果存在则直接返回。

否则进入循环，首先如果当前系统时间到达下次设置带宽限制的时间戳，则进行带宽限制。

然后发送相应的命令，`enet_protocol_send_outgoing_commands`会发送用户传入的数据和系统产生的指令。

随后调用`enet_protocol_receive_incoming_commands`从udp缓冲区中接收相应数据并处理。

随后再次调用`enet_protocol_send_outgoing_commands`发送相应数据

随后再次调用`enet_protocol_dispatch_incoming_commands`检测是否有事件产生，如果仍无事件产生，则进入`enet_socket_wait()`函数的循环

`enet_socket_wait()`内部由select函数实现，用于监听socket读是否有相应，如果udp缓冲区中有数据到达，则重新进入上述循环，如果无数据，则等待用户传入的等待时间后退出。

### 循环中两次调用send的原因

在调用`enet_protocol_receive_incoming_commands`函数后，如果接收到数据后会产生相应的ack指令，这是再次调用`enet_protocol_send_outgoing_commands`函数，则会立即将ack发送出去，不会像TCP那样捎带发送，减少了网络库对丢包的判断过程。


# ENet连接管理

## 连接建立流程

ENet在连接建立过程中同样需要三次握手，并且在建立连接的过程中改变peer的状态。

![ENet Connect](https://note.youdao.com/yws/api/personal/file/61D4208945A14AACBFAC8C8244D5D633?method=download&shareKey=a153c7cc67542b94fd4ca5db0a7a19f5)

首先两个host建立连接前需要保证peers数组内有空闲的peer(状态为disconnected)。

建立连接时，主动连接方首先找到一个状态为`disconnected`的peer，并向对端发送connect指令，并且将其状态变为`connecting`。

对端接收到connect指令后同样找到一个状态为`disconnected`的peer，并将其状态变为`acknowledgeing connect`，并返回一个verify connect指令。

主动连接方收到verify connect指令后，将其状态变为`connected`，意味着连接建立，并且向用户dispatch一个connect event事件，并返回ack指令。

对端接收到ack命令后，将其状态变为`connnected`，连接建立完成，并同样向用户dispatch一个connect event事件。

至此，双方连接建立完成。

## 断开连接流程

ENet提供了三种断开连接的方式：disconnect, disconneted now和disconnect later

### disconnect

![ENet Disonnect](https://note.youdao.com/yws/api/personal/file/628E534F2D974B4EA989C7A85C1AE7FC?method=download&shareKey=b23593745ca476e3fa5e6744bde76abf)

断开连接时，主动断开连接的一方向对端发送disconnect指令，并把状态由`connected`变为`disconnecting`。

对端收到disconnect指令之后，将状态由`connected`变为`acknowledging disconnect`，并且返回ack指令，在发送完ack指令之后，将状态变为`zombie`状态，并将断开连接事件dispatch给用户之后，将状态由`zombie`变为`disconnected`。

主动连接方收到ack后，将状态由`disconnecting`变为`zombie`，将断开连接事件dispatch给用户之后，将状态由`zombie`变为`disconnected`。

#### 为什么要有acknowledging disconnect状态

因为需要返回ack时，不能将peer状态设为`zombie`或者`disconnected`，因为在发送数据时，这两个状态的peer是被忽略的，所以需要设置一个状态，等待将ack发送出去之后，再将状态设置为`zombie`。

#### 为什么要有zombie状态

因为需要将断开连接的事件返回给用户，`zombie`状态就是已经准备断开连接，但是还没断开连接时的状态，被标记为`zombie`状态的peer不会分配给新的连接，除非用户已经处理该事件。当用户已经收到断开连接的事件后，才会真正将该peer的状态从`zombie`变为`disconnected`。

### disconnect later

disconnect later会首先将该peer的状态改为`disconnect later`。状态为`disconnect later`的peer不会再添加新的数据，也不会处理已经到来的数据，当检测到将现有queue中的数据发送完后，则会发送disconnect命令，进入之前的断开连接流程。

### disconnect now

![ENet DisConnectNow](https://note.youdao.com/yws/api/personal/file/D8987E62E0484750BE92D1423E27EF83?method=download&shareKey=bdea1ac300e791babc51ec62feb5c94e)

与disconnect不同的是，发送方在调用disconnect_now函数之后，会将peer现有队列中的数据和一个unsequenced的disconnect发送给对端，并直接将状态变为`disconnceted`，并且不会dispatch event给用户。

对端在收到unsequenced的disconnect指令之后，会将状态变为`zombie`，在将断开连接事件`dispatch`给用户之后，将状态变为`disconnected`。

# ENet 数据的发送

ENet数据发送主要在`enet_protocol_send_outgoing_commands`函数中进行，该函数会将在outgoing queue中的command和packet调用socket接口发送出去。

`enet_protocol_send_outgoing_commands`函数的大致流程为：
```c
while(host->continueSending) {

    for(peer in host->peers) {

        send_acknowldeges();

        check_timeouts();

        send_reliable_outgoing_commands();

        send_unreliable_outgoing_commands();

        enet_socket_send();
    }
}
```

ENet在发送时会遍历host中每个已经连接的peer，其中不同指令发送的顺序依次是：
1. 将ack放入host->buffers中
2. 检测当前send reliable queue中的命令是否超时，如果超时则将其加到outgoing command的队列头部
3. 将outgoing reliable queue中的命令放入host->buffers中
4. 将outgoing unreliable queue中的命令放入host->buffers中（包括unreliable 和 unsequenced命令）
5. 调用socket接口，将host->buffers中的数据用一个udp数据报发送出去

ENet在发送时会保证发送的udp数据报的大小不会超过peer的mtu的大小，如果一个peer内的数据没有发送完，则会将`host->continueSending`置为1，意味着还需要继续发送，在下次遍历peer时将数据发送出去。

为保证公平性，不会因为单个peer的需要发送的数据量过大而影响其余peer数据的发送，所以每次对单个peer发送的数据量至多为mtu，如果仍有数据没有发送，则会在下次循环中继续发送，直到`host->contingueSending`不再被置为1。

## reliable数据包的发送

在发送reliable数据时，会检测三个条件：
1. 发送的数据的范围是否在滑动窗口的有效范围内（选择重传的方法要求发送中的序号不超过序号空间的一半）。
2. 在传输中的数据总量的大小是否超过了发送窗口大小的限制。
3. host->buffers是否已经占满或者当前buffer中数据的大小是否超过mtu。

如果上述条件均不满足，意味着可以向buffer中继续添加数据，则从`peer -> outgoingReliableCommands`中将该command取出，放入buffer中，并将该command转移到`peer -> sentReliableCommands`队列中。

`peer -> sentReliableCommands`会缓存已经发送的但是还没有收到ack的可靠包的command，在每次超时检测时，会检测`peer -> sentReliableCommands`队列中的command是否已经超时，如果超时则会将该command重新放入到`peer -> outgoingReliableCommands`队列的头部，在发送数据时将其重新发送。如果一个command的rto超过最大限制或者重传次数超过最大限制，则判定当前peer已经断开连接，进入断开连接流程。

每当收到ack时，会从`peer -> sentReliableCommands`队列中将相应的command移除。

## unreliable数据报的发送

在发送unreliable数据时，会首先检测两个条件：
1. 发送的数据的范围是否在滑动窗口的有效范围内
2. 随机性丢弃一些数据。

与reliable数据不同，unreliable不会缓存已经发送的数据，因为它们不需要ack，但是为了避免发送的数据超过peer的带宽限制，则根据带宽控制阀门计算出的随机数在发送前为了保证在传输的数据量不超过peer的带宽，将不可靠包根据`packetThrottle`的值进行概率随机丢弃。

在发送时同样会将已经发送的command从`peer -> outgoingUnreliableCommands`放入`peer -> sentUnreliableCommands`，在每次调用`enet_socket_send`函数将数据发送出去后则会立即清空`peer -> sentUnreliableCommands`队列。

### 不立即清空`peer -> sentUnreliableCommands`队列的原因

host->buffers储存的内容不是真正意义的buffer，而是需要发送的数据的packet的指针。需要发送的数据的内容实际上仍在各个队列的command中存储，这样避免了数据拷贝的额外消耗。

如果直接将队列清空的话，在调用`enet_socket_send`发送时则会出现不可预知的错误，所以将`peer -> sentUnreliableCommands`队列在调用socket接口发送后清空。


# ENet 数据的接收

ENet在创建host时将socket设置为非阻塞模式。在每次接收数据时至多接收256次UDP数据报，如果udp缓冲区中没有数据或者接收次数达到256次，则跳出接收循环，先将接收到的数据dispatch给用户。但是如果收到连接，断开连接的事件，则不会继续接受数据包，而是直接跳出循环返回事件。

接收数据在`enet_protocol_receive_incoming_commands`函数中进行。

```c
for (packets = 0; packets < 256; ++ packets)
{
    receivedLength = enet_socket_receive ();

    if (receivedLength < 0)
        return -1;
    if (receivedLength == 0)
        return 0;

   enet_protocol_handle_incoming_commands ();
}
```
`enet_protocol_handle_incoming_commands`函数会对接收到的数据进行解析和处理。

在对数据进行解析和处理时，如果是系统指令，则直接对其进行相应的操作。如果是需要接收的数据类型的指令，例如send_reliable，send_unreliable指令，则需要对其进行排序，分片后的重组等操作。

## 接收send reliable指令

ENet在接收reliable指令时会首先将其按发送序号存放在`channel -> incomingReliableCommands`队列中。首先需要在`channel -> incomingReliableCommands`队列中查找到合适的位置，如果相应序号的指令已经存在，则将指令丢弃，如果不存在，则插入到队列中。

这里需要注意的时，由于ENet的序号只有16位，也就是最大只有65535，很容易发生越界。在进行插入的时候同样需要判断序号是否已经出现越界。

每次有新的数据包到来后，则会将`channel -> incomingReliableCommands`队列中连续不中断的commands移动到`peer -> dispatchedCommands`队列中，在下次调用`enet_host_service`函数时，该peer便会产生一个接收数据的事件，并将接收到的数据包返回给用户。

## 接收send unreliable指令

unreliable指令的接收与reliable大致相似，同样会对数据包进行排序，不同的是，在将数据包从`channel -> incomingUnreliableCommands`队列转移到`peer -> dispatchedCommands`队列中时，不会保证数据包的连续性，如果序号较大的已经到了，而序号较小的没有到达，则直接将已经收到的数据包放入`peer -> dispatchedCommands`队列中，如果后续较小序号的数据包到达的话，则直接丢弃。

还有一点不同的是，之前已经介绍到unreliable数据包的序号要依赖reliable数据包的序号，如果发送新的reliable数据包，则会将该peer中unreliable数据包的序号刷新。所以ENet默认unreliable序号不会超过65535的序号空间，也就没有对unreliable数据包的序号的越界情况进行考虑。

由于unsequenced指令同样放在unreliable队列中处理，如果有unsequenced的指令的话，则会直接放到`peer -> dispatchedCommands`队列中。

## 接收fragment指令

fragment分为reliable和unreliable两种类型。

在处理fragment指令时，会首先判断该分片组中的第一个分片是否已经在`channel -> incomingReliableCommands`或者`channel -> incomingUnreliableCommands`队列中。

如果已经到达，则将新到达的数据包的数据并入该第一个分片的command中，如果没有到达，则新建一个command，将其序号设置为分片组的start sequencenum，即第一个分片的序号，插入到`channel -> incomingReliableCommands`或者`channel -> incomingUnreliableCommands`队列中的合适位置，并将到达的command中的数据copy到该新建的command中。

在新建分片组第一个分片序号的command时，会同时建立一个位图，判断相应位置的command是否已经到达，如果相应分片已经全部到达，则调用dispatch函数将`channel -> incomingReliableCommands`或者`channel -> incomingUnreliableCommands`队列中的数据dispatch到`peer -> dispatchedCommands`队列中。

如果fragment是unreliable类型的话，操作与reliable类型的command基本相同，不同的是在调用dispatch函数时，如果后续序号的command已经到达，则会将没有重组完全的分片组全部丢弃，而不会进行等待。


# ENet RTT 和 RTO设置

## RTT运算过程

ENet在每次收到reliable包的acknowledge时，会对该peer的`roundTripTime`和`roundTripTimeVariance`进行更新。更新时并不是将该peer的`roundTripTime`设置为当前的rtt，而是根据当前的rtt和peer的`roundTripTime`的差值对`roundTripTime`和`roundTripTimeVariance`进行平滑的更新。

```c
rtt_var_thistime = rtt_thistime - peer->rtt
peer->rtt = peer->rtt + rtt_var_thistime / 8
peer->rtt_var = peer->rtt_var * 3/4 + rtt_var_thistime / 4
```

```c
if (peer -> roundTripTime < peer -> lowestRoundTripTime)
    peer -> lowestRoundTripTime = peer -> roundTripTime;
if (peer -> roundTripTimeVariance > peer -> highestRoundTripTimeVariance) 
    peer -> highestRoundTripTimeVariance = peer -> roundTripTimeVariance;
```
并根据`peer->rountTripTime`和`peer->roundTripTimeVariance`来更新`peer->lowestRoundTripTime`和`peer->highestRoundTripTimeVariance`这两个最值。

`peer->lowestRoundTripTime`和`peer->highestRoundTripTimeVariance`它们在每个`packetThrottleInterval`时间间隔中会被重置为该peer的`peer->rountTripTime`和`peer->roundTripTimeVariance`当前的值。

这两个最值用于`enet_peer_throttle`函数中对`peer->packetThrottle`的调控和更新

## RTO的设置

ENet在`enet_protocol_send_reliable_outgoing_commands`函数中每次发送相应可靠包时会为其设置rto，用于判断该包是否超时。

```c
command->rto = peer->rtt + 4 * peer->rtt_var
```

在`enet_protocol_check_timeouts`函数中每当检测到一个command超时后，会将其rto设置为原来的2倍，并重发该command。
```c
command->rto *= 2
```
当一个command的`roundTripTimeout`大于该command的`rounTripTimeout`,并且host在相应时间内没有收到相应peer发送的command，则视为该peer已经断开连接，并进入断开连接的流程。

# ENet 流量控制

ENet流量控制在创建host时，如果设置incomingBandWidth或者outgoingBandWidth为0，则将相应的流量控制关闭，否则则将其打开。

ENet的流量控制简单的通过一个packetThrottle变量实现，通过各个peer的带宽和相应时间内发送的数据量以及收发包的延迟对packetThrottle进行调控，从而达到控制发送数据量的目的。

对于可靠包的发送，ENet在发送数据前会根据padketThrottle计算一个发送窗口的大小，如果当前在传输过程中的数据量的大小超过了发送窗口的大小，则暂停数据的发送，直到发送窗口有足够的空间。

对于不可靠数据包，ENet在发送时会根据packetThrottle计算一个数值来随机丢弃相应的不可靠包，确保数据的发送不会超过peer的发送能力。

ENet流量控制分为两个部分：
1. 调节host端的发送能力，确保peer的download bandwidth足够承载host向其发送的数据。
2. 调节peer端的发送能力，确保peer端发送的数据不会超过host端download bandwitdh的承载能力。

## 调节host端的发送

调节host的发送能力其实是通过对`peer->packetThrottle`的调节完成的，ENet在发送数据时，会通过`peer->packetThrottle`计算出一个windowSize(发送窗口大小)，如果检测到在发送中的数据的量大于`peer->packetThrottle`的话，则暂停发送，直到发送窗口大小有足够的空间。

对于`peer->packetThrottle`的调节又分为两个步骤：
- 在每次收到ack时通过本次rtt对于之前rtt的变化对`peer->packetThrottle`进行调节
- 通过比较host的`outgoingBandwidth`和host在一定时间内发送的数据总量以及peer的`incomingBandwitdh`和相同时间内向该peer发送的数据量进行调节，确保host的发送不会超过peer的接收能力。

如果未打开流量控制，则步骤2的调节可以忽略。

### 通过rtt对packetThrottle进行调节

在ENet收到reliable包的ack时，会根据本次的rtt和lasttime_rtt对该peer的`packetThrottle`进行调节。

具体过程在`enet_peer_throttle`函数中：
```c
int enet_peer_throttle (ENetPeer * peer, enet_uint32 rtt) {
    if (peer -> lastRoundTripTime <= peer -> lastRoundTripTimeVariance)
    {
        peer -> packetThrottle = peer -> packetThrottleLimit;
    }
    else if (rtt < peer -> lastRoundTripTime)
    {
        peer -> packetThrottle += peer -> packetThrottleAcceleration;

        if (peer -> packetThrottle > peer -> packetThrottleLimit)
            peer -> packetThrottle = peer -> packetThrottleLimit;

        return 1;
    }
    else if (rtt > peer -> lastRoundTripTime + 2 * peer -> lastRoundTripTimeVariance)
    {
        if (peer -> packetThrottle > peer -> packetThrottleDeceleration)
            peer -> packetThrottle -= peer -> packetThrottleDeceleration;
        else
            peer -> packetThrottle = 0;

        return -1;
    }

    return 0;
}
```
关于`peer->lastRoundTripTime`和`peer->lastRoundTripTimeVariance`的更新过程在函数`enet_protocol_handle_acknowledge`中：
```c
if (peer -> packetThrottleEpoch == 0 ||
    ENET_TIME_DIFFERENCE (host -> serviceTime, peer -> packetThrottleEpoch) >= peer -> packetThrottleInterval)
{
    peer -> lastRoundTripTime = peer -> lowestRoundTripTime;
    peer -> lastRoundTripTimeVariance = peer -> highestRoundTripTimeVariance;
    peer -> lowestRoundTripTime = peer -> roundTripTime;
    peer -> highestRoundTripTimeVariance = peer -> roundTripTimeVariance;
    peer -> packetThrottleEpoch = host -> serviceTime;
}
```

可以看到`peer->lastRoundTripTime`取得是上一个时间段内peer统计的rtt的最小值。而`peer->lastRountRripTimeVariance`取得是上一个时间段内rtt_var的最大值。并且每次更新后将peer中相应的`lowestRoundTripTime`和`highestRoundTripTimeVariance`重置为当前的值。

在enet_peer_throttle函数中，
1. 如果上个时间段中rtt的最小值小于rtt的变化的最大幅度，可以理解为rtt至少在上个时间段内变为原来的一半，则直接将`peer->pakcetThrottle`设置为其上限`peer->packetThrottleLimit`。
2. 如果本次的rtt小于上个时间端内rtt的最小值，说明当前网络状况较好，则对该peer的`packetThrottle`进行相应的增加。
3. 如果本次的rtt大于上次的rtt的最小值加上2倍的rtt变化的最大值，说明当前网络延时有所增加，当前网络的拥塞状况较差，则对该peer的`packetThrottle`进行相应的减少。


### 通过带宽和数据发送对packetThrottle进行调节

首先统计出距离上次流量调节的时间间隔内的host发送的数据总量：
```c
if (host -> outgoingBandwidth != 0)
{
    dataTotal = 0;

    bandwidth = (host -> outgoingBandwidth * elapsedTime) / 1000; 

    for (peer = host -> peers; peer < & host -> peers [host -> peerCount]; ++ peer)
    {
        if (peer -> state != ENET_PEER_STATE_CONNECTED && peer -> state != ENET_PEER_STATE_DISCONNECT_LATER)
            continue;

        dataTotal += peer -> outgoingDataTotal;
    }
}
```
`elapsedTime`是距离上次流量控制的间隔时间。
`bandwidth`可以理解为在该间隔时间内host发送数据的能力。
`dataTotal`是在间隔时间内host向已连接的peer发送的数据的总量。

```c
while (peersRemaining > 0 && needsAdjustment != 0)
{
    needsAdjustment = 0;
    
    if (dataTotal <= bandwidth)
        throttle = ENET_PEER_PACKET_THROTTLE_SCALE;
    else
        throttle = (bandwidth * ENET_PEER_PACKET_THROTTLE_SCALE) / dataTotal;

    for (peer = host -> peers; peer < & host -> peers [host -> peerCount]; ++ peer)
    {
        enet_uint32 peerBandwidth;

        if ((peer -> state != ENET_PEER_STATE_CONNECTED && 
            peer -> state != ENET_PEER_STATE_DISCONNECT_LATER) ||	
            peer -> incomingBandwidth == 0 ||						
            peer -> outgoingBandwidthThrottleEpoch == timeCurrent)	
            continue;

        peerBandwidth = (peer -> incomingBandwidth * elapsedTime) / 1000;
        
        if ((throttle * peer -> outgoingDataTotal) / ENET_PEER_PACKET_THROTTLE_SCALE <= peerBandwidth)
            continue;

        peer -> packetThrottleLimit = (peerBandwidth *  ENET_PEER_PACKET_THROTTLE_SCALE) / peer -> outgoingDataTotal;
        
        if (peer -> packetThrottleLimit == 0)
            peer -> packetThrottleLimit = 1;
        
        if (peer -> packetThrottle > peer -> packetThrottleLimit)
            peer -> packetThrottle = peer -> packetThrottleLimit;

        peer -> outgoingBandwidthThrottleEpoch = timeCurrent;

        peer -> incomingDataTotal = 0;
        peer -> outgoingDataTotal = 0;

        needsAdjustment = 1;
        -- peersRemaining;
        bandwidth -= peerBandwidth;
        dataTotal -= peerBandwidth;
    }
}

if (peersRemaining > 0)
{
    if (dataTotal <= bandwidth)
        throttle = ENET_PEER_PACKET_THROTTLE_SCALE;
    else
        throttle = (bandwidth * ENET_PEER_PACKET_THROTTLE_SCALE) / dataTotal;

    for (peer = host -> peers; peer < & host -> peers [host -> peerCount]; ++ peer)
    {
        if ((peer -> state != ENET_PEER_STATE_CONNECTED && 
            peer -> state != ENET_PEER_STATE_DISCONNECT_LATER) ||
            peer -> outgoingBandwidthThrottleEpoch == timeCurrent)
            continue;

        peer -> packetThrottleLimit = throttle;

        if (peer -> packetThrottle > peer -> packetThrottleLimit)
            peer -> packetThrottle = peer -> packetThrottleLimit;

        peer -> incomingDataTotal = 0;
        peer -> outgoingDataTotal = 0;
    }
}
```

`throttle`是host的`bandwidth`和发送数据总量的比值的调节值。

```c
 if ((throttle * peer -> outgoingDataTotal) / ENET_PEER_PACKET_THROTTLE_SCALE <= peerBandwidth)
    continue;
```
如果`peer->incomingBandWidth`/`peer->outgoingDataTotal`的比值大于throttle，意味着此时peer的带宽足够承载host对peer发送数据的速度，则暂时不予处理，否则则对`peer->packetThrottleLimit`进行调节。设置相应peer的`packetThrottleLimit`：
```c
 peer -> packetThrottleLimit = (peerBandwidth * ENET_PEER_PACKET_THROTTLE_SCALE) / peer -> outgoingDataTotal;
```

ENet会保证`peer->packetThrottle`不超过`peer->packetThrottleLimit`。通过设置`packetThrottleLimit`保证发送数据时host向peer发送的数据量不会超过peer的`incomingBandwidth`。

对于之前没有设置的peer则统一将其`packetThrottle`设置为throttle。

#### 采用双层循环的原因

每次循环中会将 向peer发送数据量/peer的download bandwidth < throttle的peer的`packetThrottleLimit`设置为其当前的值，并在bandwidth和dataTotal中减去相应的peerBandwidth，再下一次循环中计算出的throttle会比上一次计算得出的throttle跟更高。如果只是单次循环并将剩下的peer的`packetThrottleLimit`设置为当前的throttle，会
对peer的incomingBandwidth造成浪费。

#### 最后统一设置为thtottle原因

为保证平均高效的利用host的带宽，虽然剩余的peer的`incomingBandwidth`足够承载host的发送的数据，但是host在发送数据时也需要考虑到host总的`outgoingBandwidth`，所以将剩下的peer的`pakcetThrottleLimit`设置为throttle，防止单个peer发送数据过多占满host的upload bandwidth。

## 调节peer端的发送

每当ENet中有peer连接或者断开连接时，会将`ENetHost->recalculateBandwidthLimits`结构变量置为1，在下次`enet_host_service`函数中调用`enet_host_bandwidth_throttle`时会进行该项的流量控制。

调节peer端发送能力，通过向该peer发送`ENetProtocolBandwidthLimit`指令实现。其中
- `ENetProtocolBandwidthLimit.outgoingBandwidth`简单的对应`host -> outgoingBandwidth`
- `ENetProtocolBandwidthLimit.incomingBandwidth`则是host端对该peer调节后的bandwidthLimit

host通过向peer发送`ENetProtocolBandwidthLimit`指令设置client端对应host的peer的`incomingBandwidth`，进而client端通过`incomingBandwidth`调节对应host的peer中的`packetThrottleLimit`和`packetThrottle`来控制对host流量传输。

具体调节流程可以看如下代码：
```c
host -> recalculateBandwidthLimits = 0;

peersRemaining = (enet_uint32) host -> connectedPeers;
bandwidth = host -> incomingBandwidth;
needsAdjustment = 1;

if (bandwidth == 0)
    bandwidthLimit = 0;
else
while (peersRemaining > 0 && needsAdjustment != 0)
{
    needsAdjustment = 0;
    bandwidthLimit = bandwidth / peersRemaining;

    for (peer = host -> peers; peer < & host -> peers [host -> peerCount]; ++ peer)
    {
        if ((peer -> state != ENET_PEER_STATE_CONNECTED && 
            peer -> state != ENET_PEER_STATE_DISCONNECT_LATER) ||	
            peer -> incomingBandwidthThrottleEpoch == timeCurrent)	
            continue;

        if (peer -> outgoingBandwidth > 0 &&
            peer -> outgoingBandwidth >= bandwidthLimit)
            continue;

        peer -> incomingBandwidthThrottleEpoch = timeCurrent;

        needsAdjustment = 1;
        -- peersRemaining;
        bandwidth -= peer -> outgoingBandwidth;
    }
}

for (peer = host -> peers; peer < & host -> peers [host -> peerCount];  ++ peer)
{
    if (peer -> state != ENET_PEER_STATE_CONNECTED && peer -> state != ENET_PEER_STATE_DISCONNECT_LATER)
        continue;

    command.header.command = ENET_PROTOCOL_COMMAND_BANDWIDTH_LIMIT | ENET_PROTOCOL_COMMAND_FLAG_ACKNOWLEDGE;
    command.header.channelID = 0xFF;	
    command.bandwidthLimit.outgoingBandwidth = ENET_HOST_TO_NET_32 (host -> outgoingBandwidth);

    if (peer -> incomingBandwidthThrottleEpoch == timeCurrent)
        command.bandwidthLimit.incomingBandwidth = ENET_HOST_TO_NET_32 (peer -> outgoingBandwidth);
    else
        command.bandwidthLimit.incomingBandwidth = ENET_HOST_TO_NET_32 (bandwidthLimit);

    enet_peer_queue_outgoing_command (peer, & command, NULL, 0, 0);
} 
```
`peersRemaining`指与host已经连接的peer的数量。
`bandwidth`指host的download bandwidth的大(incomingBandwidth)。

如果host端未开启流量控制，即`host -> incomingBandwidth`的值为0，则设置bandwidthLimit为0。

否则通过双层循环对带宽限制进行调节。

每次while循环会重新计算`bandwidthLimit`，并将不需要带宽限制的peer或者发送能力小于`bandwidthLimit`的peer标记出来。在接下来发送`ENetProtocolBandwidthLimit`指令时则将其中的`incomingBandwidth`设置为`peer->outgoingBandwidth`，即不用改变。对于发送能力大于`bandwidthLimit`的peer，则将相应的`ENetProtocolBandwidthLimit`指令中的`incomingBandwidth`设置为bandwidthlimit。

### 关于用双层循环的原因

如果第一次循环将所有发送能力大于`bandwidthLimit`的peer都设置为`bandwidthLimit`而对于那些发送能力小的则不改变的话，host的`incomingBandwidth`其实是没有使用完的，会造成浪费。

在每次for循环中将发送发送能力小于`bandwidthLimit`的peer剔除之后，重新计算的`bandwidthLimit`会增大，意味着host的`incomingBandwidth`可以承载更多的带宽，低于该值的peer都是不用调节的，这样可以更加充分的利用host的带宽，而不造成浪费。