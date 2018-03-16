//=====================================================================
//
// test.cpp - rtp 测试用例
//
// 说明：
// gcc test.cpp -o test -lstdc++
//
//=====================================================================

#include <stdio.h>
#include <stdlib.h>

#include "test.h"

//#define TEST


// 模拟网络
LatencySimulator *vnet;

// 模拟网络：模拟发送一个 udp包
int udp_output(const char *buf, int len, irtpcb *rtp, void *user)
{
	union { int id; void *ptr; } parameter;
	parameter.ptr = user;
	vnet->send(parameter.id, buf, len);
	return 0;
}

// 测试用例
void test(int mode)
{
	// 创建模拟网络：丢包率10%，Rtt 60ms~125ms
	vnet = new LatencySimulator(10, 60, 125);

	// 创建两个端点的 rtp对象，第一个参数 conv是会话编号，同一个会话需要相同
	// 最后一个是 user参数，用来传递标识
	irtpcb *rtp1 = irtp_create(0x11223344, (void*)0);
	irtpcb *rtp2 = irtp_create(0x11223344, (void*)1);

	// 设置rtp的下层输出，这里为 udp_output，模拟udp网络输出函数
	rtp1->output = udp_output;
	rtp2->output = udp_output;

	IUINT32 current = iclock();
	IUINT32 slap = current + 20;
	IUINT32 index = 0;
	IUINT32 next = 0;
	IINT64 sumrtt = 0;
	int count = 0;
	int maxrtt = 0;

	// 配置窗口大小：平均延迟200ms，每20ms发送一个包，
	// 而考虑到丢包重发，设置最大收发窗口为128
	irtp_wndsize(rtp1, 128, 128);
	irtp_wndsize(rtp2, 128, 128);

	// 设置冗余个数
	irtp_set_redundancy(rtp1, 3);
	irtp_set_redundancy(rtp2, 3);

	// 判断测试用例的模式
	if (mode == 0) {
		// 默认模式
		irtp_nodelay(rtp1, 0, 10, 0, 0);
		irtp_nodelay(rtp2, 0, 10, 0, 0);
	}
	else if (mode == 1) {
		// 普通模式，关闭流控等
		irtp_nodelay(rtp1, 0, 10, 0, 1);
		irtp_nodelay(rtp2, 0, 10, 0, 1);
	}
	else {
		// 启动快速模式
		// 第二个参数 nodelay-启用以后若干常规加速将启动
		// 第三个参数 interval为内部处理时钟，默认设置为 10ms
		// 第四个参数 resend为快速重传指标，设置为2
		// 第五个参数 为是否禁用常规流控，这里禁止
		irtp_nodelay(rtp1, 1, 10, 2, 1);
		irtp_nodelay(rtp2, 1, 10, 2, 1);
		rtp1->rx_minrto = 10;
		rtp1->fastresend = 2;
	}


	char buffer[2000];
	int hr;

	IUINT32 ts1 = iclock();

	while (1) {
		isleep(1);
		current = iclock();
		irtp_update(rtp1, iclock());
		irtp_update(rtp2, iclock());

		// 每隔 20ms，rtp1发送数据
		for (; current >= slap; slap += 20) {
			((IUINT32*)buffer)[0] = index++;
			((IUINT32*)buffer)[1] = current;

			// 发送上层协议包
			irtp_send(rtp1, buffer, 8);
		}

		// 处理虚拟网络：检测是否有udp包从p1->p2
		while (1) {
			hr = vnet->recv(1, buffer, 2000);
			if (hr < 0) break;
			// 如果 p2收到udp，则作为下层协议输入到rtp2
			irtp_input(rtp2, buffer, hr);
		}

		// 处理虚拟网络：检测是否有udp包从p2->p1
		while (1) {
			hr = vnet->recv(0, buffer, 2000);
			if (hr < 0) break;
			// 如果 p1收到udp，则作为下层协议输入到rtp1
			irtp_input(rtp1, buffer, hr);
		}

		// rtp2接收到任何包都返回回去
		while (1) {
			hr = irtp_recv(rtp2, buffer, 10);
			// 没有收到包就退出
			if (hr < 0) break;
			// 如果收到包就回射
			irtp_send(rtp2, buffer, hr);
		}

		// rtp1收到rtp2的回射数据
		while (1) {
			hr = irtp_recv(rtp1, buffer, 10);
			// 没有收到包就退出
			if (hr < 0) break;
			IUINT32 sn = *(IUINT32*)(buffer + 0);
			IUINT32 ts = *(IUINT32*)(buffer + 4);
			IUINT32 rtt = current - ts;

			if (sn != next) {
				// 如果收到的包不连续
				printf("ERROR sn %d<->%d\n", (int)count, (int)next);
				return;
			}

			next++;
			sumrtt += rtt;
			count++;
			if (rtt > (IUINT32)maxrtt) maxrtt = rtt;

			printf("[RECV] mode=%d sn=%d rtt=%d\n", mode, (int)sn, (int)rtt);
			printf("rtp1 cwnd: %d rtp2 cwnd: %d\n", rtp1->congestion_wnd, rtp2->congestion_wnd);
			printf("rtp1 ssthresh: %d rtp2 ssthresh: %d \n", rtp1->ssthresh, rtp2->ssthresh);
#ifdef TEST

			printf("rtp1 cwnd: %d\n", rtp1->cwnd);
#endif // DEBUG

		}
		if (next > 1000) break;
	}

	ts1 = iclock() - ts1;

	irtp_release(rtp1);
	irtp_release(rtp2);

	const char *names[3] = { "default", "normal", "fast" };
	printf("%s mode result (%dms):\n", names[mode], (int)ts1);
	printf("avgrtt=%d maxrtt=%d tx=%d\n", (int)(sumrtt / count), (int)maxrtt, (int)vnet->tx1);
	printf("press enter to next ...\n");
	char ch; scanf("%c", &ch);
}

int main()
{
	//test(0);	// 默认模式，类似 TCP：正常模式，无快速重传，常规流控
	test(1);	// 普通模式，关闭流控等
	test(2);	// 快速模式，所有开关都打开，且关闭流控
	return 0;
}

/*
default mode result (20917ms):
avgrtt=740 maxrtt=1507

normal mode result (20131ms):
avgrtt=156 maxrtt=571

fast mode result (20207ms):
avgrtt=138 maxrtt=392
*/

