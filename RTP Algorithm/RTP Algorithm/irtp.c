
#include "irtp.h"

#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <stdio.h>

//=====================================================================
// RTP BASIC
//=====================================================================
const IUINT32 IRTP_RTO_NDL = 30;		// no delay min rto
const IUINT32 IRTP_RTO_MIN = 100;		// normal min rto
const IUINT32 IRTP_RTO_DEF = 200;
const IUINT32 IRTP_RTO_MAX = 60000;
const IUINT32 IRTP_CMD_PUSH = 81;		// cmd: push data
const IUINT32 IRTP_CMD_ACK = 82;		// cmd: ack
const IUINT32 IRTP_CMD_WASK = 83;		// cmd: window probe (ask)
const IUINT32 IRTP_CMD_WINS = 84;		// cmd: window size (tell)
const IUINT32 IRTP_ASK_SEND = 1;		// need to send IRTP_CMD_WASK
const IUINT32 IRTP_ASK_TELL = 2;		// need to send IRTP_CMD_WINS
const IUINT32 IRTP_WND_SND = 32;
const IUINT32 IRTP_WND_RCV = 32;
const IUINT32 IRTP_MTU_DEF = 1400;
const IUINT32 IRTP_ACK_FAST = 3;
const IUINT32 IRTP_INTERVAL = 100;
const IUINT32 IRTP_INTERVAL_MAX = 5000;
const IUINT32 IRTP_INTERVAL_MIN = 10;
const IUINT32 IRTP_OVERHEAD = 24;
const IUINT32 IRTP_DEADLINK = 20;
const IUINT32 IRTP_THRESH_INIT = 2;
const IUINT32 IRTP_THRESH_MIN = 2;
const IUINT32 IRTP_PROBE_INIT = 7000;		// 7 secs to probe window size
const IUINT32 IRTP_PROBE_LIMIT = 120000;	// up to 120 secs to probe window
const IUINT32 IRTP_REDUN_MAX = 4;

//---------------------------------------------------------------------
// encode / decode
//---------------------------------------------------------------------

/* encode 8 bits unsigned int */
static inline char *irtp_encode8u(char *p, unsigned char c) {
	*(unsigned char*)p++ = c;
	return p;
}

/* decode 8 bits unsigned int */
static inline const char *irtp_decode8u(const char *p, unsigned char *c) {
	*c = *(unsigned char*)p++;
	return p;
}

/* encode 16 bits unsigned int (lsb) */
static inline char *irtp_encode16u(char *p, unsigned short w) {
#if IWORDS_BIG_ENDIAN
	*(unsigned char*)(p + 0) = (w & 255);
	*(unsigned char*)(p + 1) = (w >> 8);
#else
	*(unsigned short*)(p) = w;
#endif
	p += 2;
	return p;
}

/* decode 16 bits unsigned int (lsb) */
static inline const char *irtp_decode16u(const char *p, unsigned short *w) {
#if IWORDS_BIG_ENDIAN
	*w = *(const unsigned char*)(p + 1);
	*w = *(const unsigned char*)(p + 0) + (*w << 8);
#else
	*w = *(const unsigned short*)p;
#endif
	p += 2;
	return p;
}

/* encode 32 bits unsigned int (lsb) */
static inline char *irtp_encode32u(char *p, IUINT32 l) {
#if IWORDS_BIG_ENDIAN
	*(unsigned char*)(p + 0) = (unsigned char)((l >> 0) & 0xff);
	*(unsigned char*)(p + 1) = (unsigned char)((l >> 8) & 0xff);
	*(unsigned char*)(p + 2) = (unsigned char)((l >> 16) & 0xff);
	*(unsigned char*)(p + 3) = (unsigned char)((l >> 24) & 0xff);
#else
	*(IUINT32*)p = l;
#endif
	p += 4;
	return p;
}

/* decode 32 bits unsigned int (lsb) */
static inline const char *irtp_decode32u(const char *p, IUINT32 *l) {
#if IWORDS_BIG_ENDIAN
	*l = *(const unsigned char*)(p + 3);
	*l = *(const unsigned char*)(p + 2) + (*l << 8);
	*l = *(const unsigned char*)(p + 1) + (*l << 8);
	*l = *(const unsigned char*)(p + 0) + (*l << 8);
#else 
	*l = *(const IUINT32*)p;
#endif
	p += 4;
	return p;
}

static inline long _itimediff(IUINT32 later, IUINT32 earlier) {
	return ((IINT32)(later - earlier));
}

static inline IUINT32 _imin_(IUINT32 a, IUINT32 b) {
	return a <= b ? a : b;
}

static inline IUINT32 _imax_(IUINT32 a, IUINT32 b) {
	return a >= b ? a : b;
}

static inline IUINT32 _ibound_(IUINT32 lower, IUINT32 middle, IUINT32 upper) {
	return _imin_(_imax_(lower, middle), upper);
}


//---------------------------------------------------------------------
// manage segment
//---------------------------------------------------------------------
typedef struct IRTPSEG IRTPSEG;

static void* (*irtp_malloc_hook)(size_t) = NULL;
static void(*irtp_free_hook)(void *) = NULL;

// redefine allocator
void irtp_allocator(void* (*new_malloc)(size_t), void(*new_free)(void*)) {
	irtp_malloc_hook = new_malloc;
	irtp_free_hook = new_free;
}

// internal malloc
static void* irtp_malloc(size_t size) {
	if (irtp_malloc_hook)
		return irtp_malloc_hook(size);
	return malloc(size);
}

// internal free
static void irtp_free(void *ptr) {
	if (irtp_free_hook) {
		irtp_free_hook(ptr);
	}
	else {
		free(ptr);
	}
}

// check log mask
static int irtp_canlog(const irtpcb *rtp, int mask) {
	if ((mask & rtp->logmask) == 0 || rtp->writelog == NULL) return 0;
	return 1;
}

// write log
void irtp_log(irtpcb *rtp, int mask, const char *fmt, ...) {
	char buffer[1024];
	va_list argptr;
	if ((mask & rtp->logmask) == 0 || rtp->writelog == 0) return;
	va_start(argptr, fmt);
	vsprintf(buffer, fmt, argptr);
	va_end(argptr);
	rtp->writelog(buffer, rtp, rtp->user);
}

// output segment
static int irtp_output(irtpcb *rtp, const void *data, int size) {
	assert(rtp);
	assert(rtp->output);
	if (irtp_canlog(rtp, IRTP_LOG_OUTPUT)) {
		irtp_log(rtp, IRTP_LOG_OUTPUT, "[RO] %ld bytes", (long)size);
	}
	if (size == 0) return 0;
	return rtp->output((const char*)data, size, rtp, rtp->user);
}

// allocate a new rtp segment
static IRTPSEG* irtp_segment_new(irtpcb *rtp, int size) {
	return (IRTPSEG*)irtp_malloc(sizeof(IRTPSEG) + size);
}

// delete a segment
static void irtp_segment_delete(irtpcb *rtp, IRTPSEG *seg) {
	irtp_free(seg);
}

// output queue
void irtp_qprint(const char *name, const struct IQUEUEHEAD *head) {
#if 0
	const struct IQUEUEHEAD *p;
	printf("<%s>: [", name);
	for (p = head->next; p != head; p = p->next) {
		const IKCPSEG *seg = iqueue_entry(p, const IKCPSEG, node);
		printf("(%lu %d)", (unsigned long)seg->sn, (int)(seg->ts % 10000));
		if (p->next != head) printf(",");
	}
	printf("]\n");
#endif
}

//---------------------------------------------------------------------
// release a new rtpcb
//---------------------------------------------------------------------
void irtp_release(irtpcb *rtp) {
	assert(rtp);
	if (rtp) {
		IRTPSEG *seg;
		while (!iqueue_is_empty(&rtp->snd_buf)) {
			seg = iqueue_entry(rtp->snd_buf.next, IRTPSEG, node);
			iqueue_del(&seg->node);
			irtp_segment_delete(rtp, seg);
		}
		while (!iqueue_is_empty(&rtp->rcv_buf)) {
			seg = iqueue_entry(rtp->rcv_buf.next, IRTPSEG, node);
			iqueue_del(&seg->node);
			irtp_segment_delete(rtp, seg);
		}
		while (!iqueue_is_empty(&rtp->snd_queue)) {
			seg = iqueue_entry(rtp->snd_queue.next, IRTPSEG, node);
			iqueue_del(&seg->node);
			irtp_segment_delete(rtp, seg);
		}
		while (!iqueue_is_empty(&rtp->rcv_queue)) {
			seg = iqueue_entry(rtp->rcv_queue.next, IRTPSEG, node);
			iqueue_del(&seg->node);
			irtp_segment_delete(rtp, seg);
		}
		if (rtp->buffer) {
			irtp_free(rtp->buffer);
		}
		if (rtp->acklist) {
			irtp_free(rtp->acklist);
		}

		// release redundancy_buffer
		if (rtp->redundancy_num > 0) {
			for (int i = 0; i < rtp->redundancy_num; i++) {
				irtp_free(rtp->old_send_data[i].data);
			}
			irtp_free(rtp->old_send_data);
		}

		rtp->nrcv_buf = 0;
		rtp->nsnd_buf = 0;
		rtp->nrcv_que = 0;
		rtp->nsnd_que = 0;
		rtp->ackcount = 0;
		rtp->buffer = NULL;
		rtp->acklist = NULL;
		irtp_free(rtp);
	}
}

//---------------------------------------------------------------------
// create a new rtpcb
//---------------------------------------------------------------------
irtpcb* irtp_create(IUINT32 conv, void *user) {
	irtpcb *rtp = (irtpcb*)irtp_malloc(sizeof(struct IRTPCB));
	if (rtp == NULL) return NULL;
	rtp->conv = conv;
	rtp->user = user;
	rtp->snd_unack = 0;
	rtp->snd_next = 0;
	rtp->rcv_next = 0;
	rtp->recent_timestamp = 0;
	rtp->lastack_timestamp = 0;
	rtp->probe_timestamp = 0;
	rtp->probe_wait = 0;
	rtp->snd_wnd = IRTP_WND_SND;
	rtp->rcv_wnd = IRTP_WND_RCV;
	rtp->remote_wnd = IRTP_WND_RCV;
	rtp->congestion_wnd = 0;
	rtp->incr = 0;
	rtp->probe = 0;
	rtp->mtu = IRTP_MTU_DEF;
	rtp->mss = rtp->mtu - IRTP_OVERHEAD;
	rtp->stream = 0;

	rtp->buffer = (char*)irtp_malloc((rtp->mtu + IRTP_OVERHEAD) * 3);
	if (rtp->buffer == NULL) {
		irtp_free(rtp);
		return NULL;
	}

	iqueue_init(&rtp->snd_queue);
	iqueue_init(&rtp->rcv_queue);
	iqueue_init(&rtp->snd_buf);
	iqueue_init(&rtp->rcv_buf);
	rtp->nrcv_buf = 0;
	rtp->nsnd_buf = 0;
	rtp->nrcv_que = 0;
	rtp->nsnd_que = 0;
	rtp->state = 0;
	rtp->acklist = NULL;
	rtp->ackblock = 0;
	rtp->ackcount = 0;
	rtp->rx_srtt = 0;
	rtp->rx_rttval = 0;
	rtp->rx_rto = IRTP_RTO_DEF;
	rtp->rx_minrto = IRTP_RTO_MIN;
	rtp->current = 0;
	rtp->interval = IRTP_INTERVAL;
	rtp->flush_timestamp = IRTP_INTERVAL;
	rtp->nodelay = 0;
	rtp->updated = 0;
	rtp->logmask = 0;
	rtp->ssthresh = IRTP_THRESH_INIT;
	rtp->fastresend = 0;
	rtp->nocwnd = 0;
	rtp->xmit = 0;
	rtp->dead_link = IRTP_DEADLINK;
	rtp->output = NULL;
	rtp->writelog = NULL;

	rtp->redundancy_num = 0;
	rtp->old_send_data = NULL;
	rtp->r_buffer_size = 0;
	rtp->now_send_data_num = 0;
	rtp->old_send_data_used = 0;

	return rtp;
}

int irtp_wndsize(irtpcb *rtp, int sndwnd, int rcvwnd) {
	if (rtp) {
		if (sndwnd > 0) {
			rtp->snd_wnd = sndwnd;
		}
		if (rcvwnd > 0) {
			rtp->rcv_wnd = rcvwnd;
		}
	}
	return 0;
}

// fastest: irtp_nodelay(rtp, 1, 20, 2, 1)
// nodelay: 0:disable(default), 1:enable
// interval: internal update timer interval in millisec, default is 100ms 
// resend: 0:disable fast resend(default), 1:enable fast resend
// nc: 0:normal congestion control(default), 1:disable congestion control
int irtp_nodelay(irtpcb *rtp, int nodelay, int interval, int resend, int nc)
{
	if (nodelay >= 0) {
		rtp->nodelay = nodelay;
		if (nodelay) {
			rtp->rx_minrto = IRTP_RTO_NDL;
		}
		else {
			rtp->rx_minrto = IRTP_RTO_MIN;
		}
	}
	if (interval >= 0) {
		if (interval > IRTP_INTERVAL_MAX) interval = IRTP_INTERVAL_MAX;
		else if (interval < IRTP_INTERVAL_MIN) interval = IRTP_INTERVAL_MIN;
		rtp->interval = interval;
	}
	if (resend >= 0) {
		rtp->fastresend = resend;
	}
	if (nc >= 0) {
		rtp->nocwnd = nc;
	}
	return 0;
}

//---------------------------------------------------------------------
// update state (call it repeatedly, every 10ms-100ms), or you can ask 
// irtp_check when to call it again (without irtp_input/_send calling).
// 'current' - current timestamp in millisec. 
//---------------------------------------------------------------------
void irtp_update(irtpcb *rtp, IUINT32 current) {

	IINT32 slap;

	rtp->current = current;

	if (rtp->updated == 0) {
		rtp->updated = 1;
		rtp->flush_timestamp = rtp->current;
	}

	slap = _itimediff(rtp->current, rtp->flush_timestamp);

	if (slap >= 10000 || slap < -10000) {
		rtp->flush_timestamp = rtp->current;
		slap = 0;
	}

	if (slap >= 0) {
		rtp->flush_timestamp += rtp->interval;
		if (_itimediff(rtp->current, rtp->flush_timestamp) >= 0) {
			rtp->flush_timestamp = rtp->current + rtp->interval;
		}
		irtp_flush(rtp);
	}
}

static char *irtp_encode_seg(char *ptr, const IRTPSEG *seg) {
	ptr = irtp_encode32u(ptr, seg->conv);
	ptr = irtp_encode8u(ptr, (IUINT8)seg->command);
	ptr = irtp_encode8u(ptr, (IUINT8)seg->fragement);
	ptr = irtp_encode16u(ptr, (IUINT16)seg->wnd);
	ptr = irtp_encode32u(ptr, seg->timestamp);
	ptr = irtp_encode32u(ptr, seg->seq_num);
	ptr = irtp_encode32u(ptr, seg->unack);
	ptr = irtp_encode32u(ptr, seg->len);
	return ptr;
}

static void irtp_ack_get(const irtpcb *rtp, int p, IUINT32 *seq_num, IUINT32 *timestamp) {
	if (seq_num) seq_num[0] = rtp->acklist[p * 2 + 0];
	if (timestamp) timestamp[0] = rtp->acklist[p * 2 + 1];
}

static int irtp_wnd_unused(const irtpcb *rtp) {
	if (rtp->nrcv_que < rtp->rcv_wnd) {
		return rtp->rcv_wnd - rtp->nrcv_que;
	}
	return 0;
}

char *redundancy_send(irtpcb *rtp, size_t size) {
	char* send_ptr = rtp->buffer;
	rtp->old_send_data[rtp->now_send_data_num].len = size;
	for (int i = rtp->old_send_data_used - 1; i >= 0; i--) {
		int index = (rtp->now_send_data_num - i + rtp->redundancy_num) % rtp->redundancy_num;
		memcpy(send_ptr, rtp->old_send_data[index].data, rtp->old_send_data[index].len);
		send_ptr += rtp->old_send_data[index].len;
	}

	irtp_output(rtp, rtp->buffer, send_ptr - rtp->buffer);

	if (rtp->old_send_data_used < rtp->redundancy_num) {
		rtp->old_send_data_used++;
		rtp->now_send_data_num++;
	}
	else {
		rtp->now_send_data_num = (rtp->now_send_data_num + 1) % rtp->redundancy_num;
		
	}
	rtp->old_send_data[rtp->now_send_data_num].len = 0;

	return rtp->old_send_data[rtp->now_send_data_num].data;
}

//---------------------------------------------------------------------
// irtp_flush
//---------------------------------------------------------------------
void irtp_flush(irtpcb *rtp) {
	IUINT32 current = rtp->current;
	char *buffer = rtp->buffer;
	char *ptr = buffer;
	int count, size, i;
	IUINT32 resent, cwnd;
	IUINT32 rtomin;
	struct IQUEUEHEAD *p;
	int change = 0;
	int lost = 0;
	IRTPSEG seg;

	// 'irtp_update' haven't been called. 
	if (rtp->updated == 0) return;

	seg.conv = rtp->conv;
	seg.command = IRTP_CMD_ACK;
	seg.fragement = 0;
	seg.wnd = irtp_wnd_unused(rtp);	//接收窗口大小
	seg.unack = rtp->rcv_next;
	seg.len = 0;
	seg.seq_num = 0;
	seg.timestamp = 0;

	// flush acknowledges
	count = rtp->ackcount;
	for (i = 0; i < count; i++) {
		size = (int)(ptr - buffer);
		//如果buffer中的数据量大于mtu，则将buffer中的数据发送出去
		if (size + (int)IRTP_OVERHEAD >(int)rtp->mtu) {
			irtp_output(rtp, buffer, size);
			ptr = buffer;
		}
		irtp_ack_get(rtp, i, &seg.seq_num, &seg.timestamp);
		//将segment放到ptr下，如果是ack，则只发送包头的大小
		ptr = irtp_encode_seg(ptr, &seg);
	}

	rtp->ackcount = 0;

	// probe window size (if remote window size equals zero)
	if (rtp->remote_wnd == 0) {
		if (rtp->probe_wait == 0) {
			rtp->probe_wait = IRTP_PROBE_INIT;
			rtp->probe_timestamp = rtp->current + rtp->probe_wait;
		}
		else {
			if (_itimediff(rtp->current, rtp->probe_timestamp) >= 0) {
				if (rtp->probe_wait < IRTP_PROBE_INIT)
					rtp->probe_wait = IRTP_PROBE_INIT;
				rtp->probe_wait += rtp->probe_wait / 2;
				if (rtp->probe_wait > IRTP_PROBE_LIMIT)
					rtp->probe_wait = IRTP_PROBE_LIMIT;
				rtp->probe_timestamp = rtp->current + rtp->probe_wait;
				rtp->probe |= IRTP_ASK_SEND;
			}
		}
	}
	else {
		rtp->probe_timestamp = 0;
		rtp->probe_wait = 0;
	}

	// flush window probing commands
	if (rtp->probe & IRTP_ASK_SEND) {
		seg.command = IRTP_CMD_WASK;
		size = (int)(ptr - buffer);
		if (size + (int)IRTP_OVERHEAD > (int)rtp->mtu) {
			irtp_output(rtp, buffer, size);
			ptr = buffer;
		}
		ptr = irtp_encode_seg(ptr, &seg);
	}

	// flush window probing commands
	if (rtp->probe & IRTP_ASK_TELL) {
		seg.command = IRTP_CMD_WINS;
		size = (int)(ptr - buffer);
		if (size + (int)IRTP_OVERHEAD > (int)rtp->mtu) {
			irtp_output(rtp, buffer, size);
			ptr = buffer;
		}
		ptr = irtp_encode_seg(ptr, &seg);
	}

	rtp->probe = 0;
	// flush remaining data in the buffer
	size = (int)(ptr - buffer);
	if (size > 0) {
		irtp_output(rtp, buffer, size);
		ptr = buffer;
	}
	
	// calculate window size，取发送窗口和远端窗口的最小值
	cwnd = _imin_(rtp->snd_wnd, rtp->remote_wnd);
	// 如果不取消拥塞控制
	if (rtp->nocwnd == 0) cwnd = _imin_(rtp->congestion_wnd, cwnd);

	// move data from snd_queue to snd_buf
	while (_itimediff(rtp->snd_next, rtp->snd_unack + cwnd) < 0) {
		IRTPSEG *newseg;
		if (iqueue_is_empty(&rtp->snd_queue)) break;

		newseg = iqueue_entry(rtp->snd_queue.next, IRTPSEG, node);

		iqueue_del(&newseg->node);
		iqueue_add_tail(&newseg->node, &rtp->snd_buf);
		rtp->nsnd_que--;
		rtp->nsnd_buf++;

		newseg->conv = rtp->conv;
		newseg->command = IRTP_CMD_PUSH;
		newseg->wnd = seg.wnd;
		newseg->timestamp = current;
		newseg->seq_num = rtp->snd_next++;
		newseg->unack = rtp->rcv_next;
		newseg->resend_timestamp = current;
		newseg->rto = rtp->rx_rto;
		newseg->fastack = 0;
		newseg->xmit = 0;
	}

	// calculate resent
	if (rtp->fastresend > 0) {
		if (rtp->redundancy_num > 0) {
			resent = (IUINT32)(rtp->fastresend + rtp->redundancy_num);
		}
		else resent = (IUINT32)rtp->fastresend;
	}
	else resent = 0xffffffff;
	rtomin = (rtp->nodelay == 0) ? (rtp->rx_rto >> 3) : 0;
	
	// 如果设置了冗余传输，则将当前的buffer设置为冗余队列中的buffer
	if (rtp->redundancy_num > 0) {
		buffer = rtp->old_send_data[rtp->now_send_data_num].data;
		ptr = buffer;
	}

	// flush data segments
	for (p = rtp->snd_buf.next; p != &rtp->snd_buf; p = p->next) {
		IRTPSEG *segment = iqueue_entry(p, IRTPSEG, node);
		int needsend = 0;
		// 如果之前没有发送过，则直接发送
		if (segment->xmit == 0) {	
			needsend = 1;
			segment->xmit++;
			segment->rto = rtp->rx_rto;
			segment->resend_timestamp = current + segment->rto + rtomin;
		}
		// 超时进行重传
		else if (_itimediff(current, segment->resend_timestamp) >= 0) {
			needsend = 1;
			segment->xmit++;
			rtp->xmit++;
			if (rtp->nodelay == 0) {
				segment->rto += rtp->rx_rto;
			}
			else {
				segment->rto += rtp->rx_rto / 2;
			}
			segment->resend_timestamp = current + segment->rto;
			lost = 1;
		}
		//快速重传
		else if (segment->fastack >= resent) {
			needsend = 1;
			segment->xmit++;
			segment->fastack = 0;
			segment->resend_timestamp = current + segment->rto;
			change++;
		}

		if (needsend) {
			int size, need;
			segment->timestamp = current;
			segment->wnd = seg.wnd;
			segment->unack = rtp->rcv_next;

			size = (int)(ptr - buffer);
			need = IRTP_OVERHEAD + segment->len;

			// 如果开启冗余传输
			if (rtp->redundancy_num > 0) {
				if (size + need > (int)rtp->r_buffer_size) {
					buffer = redundancy_send(rtp, size);
					ptr = buffer;
				}
			}
			else  {
				if (size + need > (int)rtp->mtu) {
					irtp_output(rtp, buffer, size);
					ptr = buffer;
				}
			}

			ptr = irtp_encode_seg(ptr, segment);

			if (segment->len > 0) {
				memcpy(ptr, segment->data, segment->len);
				ptr += segment->len;
			}

			if (segment->xmit >= rtp->dead_link) {
				rtp->state = -1;
			}
		}
	}

	// flash remain segments
	size = (int)(ptr - buffer);
	if (size > 0) {
		// 如果开启冗余传输
		if (rtp->redundancy_num > 0) {
			redundancy_send(rtp, size);
		}
		else {
			irtp_output(rtp, buffer, size);
		}
	}

	// update ssthresh
	if (change) {
		IUINT32 inflight = rtp->snd_next - rtp->snd_unack;
		rtp->ssthresh = inflight / 2;
		if (rtp->ssthresh < IRTP_THRESH_MIN)
			rtp->ssthresh = IRTP_THRESH_MIN;
		rtp->congestion_wnd = rtp->ssthresh + resent;
		rtp->incr = rtp->congestion_wnd * rtp->mss;
	}

	//把ssthresh降低为cwnd的一半，将cwnd重置为1，重新进入慢起动过程
	if (lost) {
		rtp->ssthresh = cwnd / 2;
		if (rtp->ssthresh < IRTP_THRESH_MIN)
			rtp->ssthresh = IRTP_THRESH_MIN;
		rtp->congestion_wnd = 1;
		rtp->incr = rtp->mss;
	}

	if (rtp->congestion_wnd < 1) {
		rtp->congestion_wnd = 1;
		rtp->incr = rtp->mss;
	}
}

//---------------------------------------------------------------------
// user/upper level send, returns below zero for error
// 把需要发送的buffer中的数据分片后放到queue中
//---------------------------------------------------------------------
int irtp_send(irtpcb *rtp, const char *buffer, int len) {

	IRTPSEG *seg;
	int count, i;

	assert(rtp->mss > 0);
	if (len < 0) return -1;

	// 如果开启冗余传输，
	IUINT32 send_mss = (rtp->redundancy_num > 0) ? (rtp->mtu) / rtp->redundancy_num - IRTP_OVERHEAD : rtp->mss;
	if (send_mss <= 0)
		return -2;	//冗余个数太多

	// append to previous segment in streaming mode (if possible)
	// 是否开启流模式
	if (rtp->stream != 0) {
		if (!iqueue_is_empty(&rtp->snd_queue)) {
			IRTPSEG *old = iqueue_entry(rtp->snd_queue.prev, IRTPSEG, node);
			if (old->len < send_mss) {
				int capacity = send_mss - old->len;
				int extend = (len < capacity) ? len : capacity;
				seg = irtp_segment_new(rtp, old->len + extend);
				assert(seg);
				if (seg == NULL) {
					return -2;
				}
				iqueue_add_tail(&seg->node, &rtp->snd_queue);
				memcpy(seg->data, old->data, old->len);
				if (buffer) {
					memcpy(seg->data + old->len, buffer, extend);
					buffer += extend;
				}
				seg->len = old->len + extend;
				seg->fragement = 0;
				len -= extend;
				iqueue_del_init(&old->node);
				irtp_segment_delete(rtp, old);
			}
		}
		if (len <= 0) {
			return 0;
		}
	}

	if (len <= (int)send_mss) count = 1;
	else count = (len + send_mss - 1) /send_mss;

	if (count > 255) return -2;

	if (count == 0) count = 1;

	// fragment
	for (i = 0; i < count; i++) {
		int size = len >(int)send_mss ? (int)send_mss : len;
		seg = irtp_segment_new(rtp, size);
		assert(seg);
		if (seg == NULL) {
			return -2;
		}
		if (buffer && len > 0) {
			memcpy(seg->data, buffer, size);
		}
		seg->len = size;
		seg->fragement = (rtp->stream == 0) ? (count - i - 1) : 0;
		iqueue_init(&seg->node);
		iqueue_add_tail(&seg->node, &rtp->snd_queue);
		rtp->nsnd_que++;
		if (buffer) {
			buffer += size;
		}
		len -= size;
	}

	return 0;
}


//---------------------------------------------------------------------
// parse ack
// 更新该rtp block的rtt和rto
//---------------------------------------------------------------------
static void irtp_update_ack(irtpcb *rtp, IINT32 rtt) {
	IINT32 rto = 0;
	if (rtp->rx_srtt == 0) {
		rtp->rx_srtt = rtt;
		rtp->rx_rttval = rtt / 2;
	}
	else {
		long delta = rtt - rtp->rx_srtt;
		if (delta < 0) delta = -delta;
		rtp->rx_rttval = (3 * rtp->rx_rttval + delta) / 4;
		rtp->rx_srtt = (7 * rtp->rx_srtt + rtt) / 8;
		if (rtp->rx_srtt < 1) rtp->rx_srtt = 1;
	}
	//rto的计算？
	rto = rtp->rx_srtt + _imax_(rtp->interval, 4 * rtp->rx_rttval);
	rtp->rx_rto = _ibound_(rtp->rx_minrto, rto, IRTP_RTO_MAX);
}

//接收到una后将发送缓冲区中una之前的seg清空，表示之前的segment都已经收到了
static void irtp_parse_una(irtpcb *rtp, IUINT32 una) {
	struct IQUEUEHEAD *p, *next;
	for (p = rtp->snd_buf.next; p != &rtp->snd_buf; p = next) {
		IRTPSEG *seg = iqueue_entry(p, IRTPSEG, node);
		next = p->next;
		if (_itimediff(una, seg->seq_num) > 0) {
			iqueue_del(p);
			irtp_segment_delete(rtp, seg);
			rtp->nsnd_buf--;
		}
		else {
			break;
		}
	}
}

//更新sed_unack（下一个未确认的segment）
static void irtp_shrink_buf(irtpcb *rtp) {
	struct IQUEUEHEAD *p = rtp->snd_buf.next;
	if (p != &rtp->snd_buf) {
		IRTPSEG *seg = iqueue_entry(p, IRTPSEG, node);
		rtp->snd_unack = seg->seq_num;
	}
	else {
		rtp->snd_unack = rtp->snd_next;
	}
}

//将找到sed_buf中的相应的seg并删掉，表明该segment已经收到了ack
static void irtp_parse_ack(irtpcb *rtp, IUINT32 sn) {
	struct IQUEUEHEAD *p, *next;
	//如果是已经确认过的或者是还没发送的，直接丢掉
	if (_itimediff(sn, rtp->snd_unack) < 0 || _itimediff(sn, rtp->snd_next) >= 0)
		return;

	for (p = rtp->snd_buf.next; p != &rtp->snd_buf; p = next) {
		IRTPSEG *seg = iqueue_entry(p, IRTPSEG, node);
		next = p->next;
		if (sn == seg->seq_num) {
			iqueue_del(p);
			irtp_segment_delete(rtp, seg);
			rtp->nsnd_buf--;
			break;
		}
		if (_itimediff(sn, seg->seq_num) < 0) {
			break;
		}
	}
}

//根据max_ack更新sed_buf中seg被跳过的次数(fastack++)
//用于快速重传
static void irtp_parse_fastack(irtpcb *rtp, IUINT32 sn) {

	struct IQUEUEHEAD *p, *next;

	if (_itimediff(sn, rtp->snd_unack) < 0 || _itimediff(sn, rtp->snd_next) >= 0)
		return;

	for (p = rtp->snd_buf.next; p != &rtp->snd_buf; p = next) {
		IRTPSEG *seg = iqueue_entry(p, IRTPSEG, node);
		next = p->next;
		if (_itimediff(sn, seg->seq_num) < 0) {
			break;
		}
		else if (sn != seg->seq_num) {
			seg->fastack++;
		}
	}
}

//---------------------------------------------------------------------
// parse data
// 将收到的segment放到rev_buf中
// 如果rev_queue的大小小于rec_wnd的大小，则将rev_buf中连续的segment push到rev_que中
//---------------------------------------------------------------------
void irtp_parse_data(irtpcb *rtp, IRTPSEG *newseg) {
	struct IQUEUEHEAD *p, *prev;
	IUINT32 sn = newseg->seq_num;
	int repeat = 0;
	//超出滑动窗口的范围
	if (_itimediff(sn, rtp->rcv_next + rtp->rcv_wnd) >= 0 ||
		_itimediff(sn, rtp->rcv_next) < 0) {
		irtp_segment_delete(rtp, newseg);
		return;
	}

	for (p = rtp->rcv_buf.prev; p != &rtp->rcv_buf; p = prev) {
		IRTPSEG *seg = iqueue_entry(p, IRTPSEG, node);
		prev = p->prev;
		if (seg->seq_num == sn) {	//重复了
			repeat = 1;
			break;
		}
		if (_itimediff(sn, seg->seq_num) > 0) {	//找到相应插入的位置
			break;
		}
	}

	if (repeat == 0) {
		iqueue_init(&newseg->node);
		iqueue_add(&newseg->node, p);
		rtp->nrcv_buf++;
	}
	else {
		irtp_segment_delete(rtp, newseg);
	}

#if 0
	irtp_qprint("rcvbuf", &rtp->rcv_buf);
	printf("rcv_nxt=%lu\n", rtp->rcv_nxt);
#endif

	// move available data from rcv_buf -> rcv_queue
	while (!iqueue_is_empty(&rtp->rcv_buf)) {
		IRTPSEG *seg = iqueue_entry(rtp->rcv_buf.next, IRTPSEG, node);
		if (seg->seq_num == rtp->rcv_next && rtp->nrcv_que < rtp->rcv_wnd) {
			iqueue_del(&seg->node);
			rtp->nrcv_buf--;
			iqueue_add_tail(&seg->node, &rtp->rcv_queue);
			rtp->nrcv_que++;
			rtp->rcv_next++;
		}
		else {
			break;
		}
	}

#if 0
	irtp_qprint("queue", &rtp->rcv_queue);
	printf("rcv_nxt=%lu\n", rtp->rcv_nxt);
#endif

#if 1
	//	printf("snd(buf=%d, queue=%d)\n", rtp->nsnd_buf, rtp->nsnd_que);
	//	printf("rcv(buf=%d, queue=%d)\n", rtp->nrcv_buf, rtp->nrcv_que);
#endif
}

//---------------------------------------------------------------------
// ack append
// 把sn（sequence number）和ts（time stamp）放到ack list中，flush时发送ack
//---------------------------------------------------------------------
static void irtp_ack_push(irtpcb *rtp, IUINT32 sn, IUINT32 ts) {
	size_t newsize = rtp->ackcount + 1;
	IUINT32 *ptr;

	//更新ack list的大小
	if (newsize > rtp->ackblock) {
		IUINT32 *acklist;
		size_t newblock;

		for (newblock = 8; newblock < newsize; newblock <<= 1);
		acklist = (IUINT32*)irtp_malloc(newblock * sizeof(IUINT32) * 2);

		if (acklist == NULL) {
			assert(acklist != NULL);
			abort();
		}

		if (rtp->acklist != NULL) {
			size_t x;
			for (x = 0; x < rtp->ackcount; x++) {
				acklist[x * 2 + 0] = rtp->acklist[x * 2 + 0];
				acklist[x * 2 + 1] = rtp->acklist[x * 2 + 1];
			}
			irtp_free(rtp->acklist);
		}

		rtp->acklist = acklist;
		rtp->ackblock = newblock;
	}

	ptr = &rtp->acklist[rtp->ackcount * 2];
	ptr[0] = sn;
	ptr[1] = ts;
	rtp->ackcount++;
}

//---------------------------------------------------------------------
// input data
// 处理接收到的包
//---------------------------------------------------------------------
int irtp_input(irtpcb *rtp, const char *data, long size) {

	IUINT32 una = rtp->snd_unack;
	IUINT32 maxack = 0;
	int flag = 0;

	if (irtp_canlog(rtp, IRTP_LOG_INPUT)) {
		irtp_log(rtp, IRTP_LOG_INPUT, "[RI] %d bytes", size);
	}

	if (data == NULL || (int)size < (int)IRTP_OVERHEAD) return -1;

	while (1) {
		IUINT32 ts, sn, len, una, conv;
		IUINT16 wnd;
		IUINT8 cmd, frg;
		IRTPSEG *seg;

		if (size < (int)IRTP_OVERHEAD) break;

		//解析rtp的数据包头
		data = irtp_decode32u(data, &conv);
		if (conv != rtp->conv) return -1;		//-1 会话序号不一致

		data = irtp_decode8u(data, &cmd);
		data = irtp_decode8u(data, &frg);
		data = irtp_decode16u(data, &wnd);
		data = irtp_decode32u(data, &ts);
		data = irtp_decode32u(data, &sn);
		data = irtp_decode32u(data, &una);
		data = irtp_decode32u(data, &len);

		size -= IRTP_OVERHEAD;

		if ((long)size < (long)len) return -2;	//-2 数据长度不一致

		if (cmd != IRTP_CMD_PUSH && cmd != IRTP_CMD_ACK &&
			cmd != IRTP_CMD_WASK && cmd != IRTP_CMD_WINS)
			return -3;							//-3 指令错误

		rtp->remote_wnd = wnd;
		irtp_parse_una(rtp, una);	//将snd_buf中的una之前的seg清空
		irtp_shrink_buf(rtp);		//更新sed_unack（下一个未确认的segment）

		if (cmd == IRTP_CMD_ACK) {
			// 如果收到包的时间比预计的晚
			if (_itimediff(rtp->current, ts) >= 0) {
				irtp_update_ack(rtp, _itimediff(rtp->current, ts));
			}
			irtp_parse_ack(rtp, sn);
			irtp_shrink_buf(rtp);
			if (flag == 0) {
				flag = 1;
				maxack = sn;
			}
			else {
				if (_itimediff(sn, maxack) > 0) {
					maxack = sn;
				}
			}
			if (irtp_canlog(rtp, IRTP_LOG_IN_ACK)) {
				irtp_log(rtp, IRTP_LOG_IN_DATA,
					"input ack: sn=%lu rtt=%ld rto=%ld", sn,
					(long)_itimediff(rtp->current, ts),
					(long)rtp->rx_rto);
			}
		}
		else if (cmd == IRTP_CMD_PUSH) {
			if (irtp_canlog(rtp, IRTP_LOG_IN_DATA)) {
				irtp_log(rtp, IRTP_LOG_IN_DATA,
					"input psh: sn=%lu ts=%lu", sn, ts);
			}
			if (_itimediff(sn, rtp->rcv_next + rtp->rcv_wnd) < 0) {
				irtp_ack_push(rtp, sn, ts);
				// 如果数据报之前没有收到过
				if (_itimediff(sn, rtp->rcv_next) >= 0) {
					seg = irtp_segment_new(rtp, len);
					seg->conv = conv;
					seg->command = cmd;
					seg->fragement = frg;
					seg->wnd = wnd;
					seg->timestamp = ts;
					seg->seq_num = sn;
					seg->unack = una;
					seg->len = len;

					if (len > 0) {
						memcpy(seg->data, data, len);
					}
					irtp_parse_data(rtp, seg);
				}
			}
		}
		else if (cmd == IRTP_CMD_WASK) {
			// ready to send back IRTP_CMD_WINS in irtp_flush
			// tell remote my window size
			rtp->probe |= IRTP_ASK_TELL;
			if (irtp_canlog(rtp, IRTP_LOG_IN_PROBE)) {
				irtp_log(rtp, IRTP_LOG_IN_PROBE, "input probe");
			}
		}
		else if (cmd == IRTP_CMD_WINS) {
			// do nothing
			if (irtp_canlog(rtp, IRTP_LOG_IN_WINS)) {
				irtp_log(rtp, IRTP_LOG_IN_WINS,
					"input wins: %lu", (IUINT32)(wnd));
			}
		}
		else {
			return -3;	//wrong instruction
		}

		data += len;
		size -= len;
	}

	if (flag != 0) {
		irtp_parse_fastack(rtp, maxack);
	}

	//如果远端接收状态有更新
	//rtp->snd_unack 是sendbuf中未确认的seg
	//rcv_next是rcvbuf中的下一个待接收的seg
	if (_itimediff(rtp->snd_unack, una) > 0) {
		if (rtp->congestion_wnd < rtp->remote_wnd) {
			IUINT32 mss = rtp->mss;
			if (rtp->congestion_wnd < rtp->ssthresh) {
				rtp->congestion_wnd++;
				rtp->incr += mss;
			}
			else {
				if (rtp->incr < mss) rtp->incr = mss;
				rtp->incr += (mss * mss) / rtp->incr + (mss / 16);
				if ((rtp->congestion_wnd + 1) * mss <= rtp->incr) {
					rtp->congestion_wnd++;
				}
			}
			if (rtp->congestion_wnd > rtp->remote_wnd) {
				rtp->congestion_wnd = rtp->remote_wnd;
				rtp->incr = rtp->remote_wnd * mss;
			}
		}
	}
	return 0;
}

//---------------------------------------------------------------------
// peek data size
//计算得到rcv_queue中的第一个packet分段前的长度
//---------------------------------------------------------------------
int irtp_peeksize(const irtpcb *rtp) {
	struct IQUEUEHEAD *p;
	IRTPSEG *seg;
	int length = 0;

	assert(rtp);

	if (iqueue_is_empty(&rtp->rcv_queue)) return -1;

	seg = iqueue_entry(rtp->rcv_queue.next, IRTPSEG, node);
	if (seg->fragement == 0) return seg->len;

	if (rtp->nrcv_que < seg->fragement + 1) return -1;	//分段中的所有frg没有接受完

	for (p = rtp->rcv_queue.next; p != &rtp->rcv_queue; p = p->next) {
		seg = iqueue_entry(p, IRTPSEG, node);
		length += seg->len;
		if (seg->fragement == 0) break;
	}

	return length;
}

//---------------------------------------------------------------------
// user/upper level recv: returns size, returns below zero for EAGAIN
//---------------------------------------------------------------------
int irtp_recv(irtpcb *rtp, char *buffer, int len) {
	struct IQUEUEHEAD *p;
	int ispeek = (len < 0) ? 1 : 0;
	int peeksize;
	int recover = 0;
	IRTPSEG *seg;
	assert(rtp);

	if (iqueue_is_empty(&rtp->rcv_queue))
		return -1;

	if (len < 0) len = -len;

	peeksize = irtp_peeksize(rtp);

#ifdef TEST
	printf("peeksize %d len %d\n", peeksize, len);
#endif // DEBUG

	if (peeksize < 0)
		return -2;		// 所有的分段没有接收完

	if (peeksize > len)
		return -3;		//buffer不够存这个packet了

	if (rtp->nrcv_que >= rtp->rcv_wnd)
		recover = 1;

	// merge fragment
	for (len = 0, p = rtp->rcv_queue.next; p != &rtp->rcv_queue; ) {
		int fragment;
		seg = iqueue_entry(p, IRTPSEG, node);
		p = p->next;

		if (buffer) {
			memcpy(buffer, seg->data, seg->len);
			buffer += seg->len;
		}

		len += seg->len;
		fragment = seg->fragement;

		if (irtp_canlog(rtp, IRTP_LOG_RECV)) {
			irtp_log(rtp, IRTP_LOG_RECV, "recv sn=%lu", seg->seq_num);
		}

		if (ispeek == 0) {
			iqueue_del(&seg->node);
			irtp_segment_delete(rtp, seg);
			rtp->nrcv_que--;
		}

		if (fragment == 0)
			break;
	}

	assert(len == peeksize);

	// move available data from rcv_buf -> rcv_queue
	while (!iqueue_is_empty(&rtp->rcv_buf)) {
		IRTPSEG *seg = iqueue_entry(rtp->rcv_buf.next, IRTPSEG, node);
		if (seg->seq_num == rtp->rcv_next && rtp->nrcv_que < rtp->rcv_wnd) {
			iqueue_del(&seg->node);
			rtp->nrcv_buf--;
			iqueue_add_tail(&seg->node, &rtp->rcv_queue);
			rtp->nrcv_que++;
			rtp->rcv_next++;
		}
		else {
			break;
		}
	}

	// fast recover
	if (rtp->nrcv_que < rtp->rcv_wnd && recover) {
		// ready to send back IRTP_CMD_WINS in irtp_flush
		// tell remote my window size
		rtp->probe |= IRTP_ASK_TELL;
	}

	return len;
}

//---------------------------------------------------------------------
// set output callback, which will be invoked by rtp
//---------------------------------------------------------------------
void irtp_setoutput(irtpcb *rtp, int(*output)(const char *buf, int len,
	irtpcb *rtp, void *user)) {
	rtp->output = output;
}

//---------------------------------------------------------------------
// Determine when should you invoke irtp_update:
// returns when you should invoke irtp_update in millisec, if there 
// is no irtp_input/_send calling. you can call irtp_update in that
// time, instead of call update repeatly.
// Important to reduce unnacessary irtp_update invoking. use it to 
// schedule irtp_update (eg. implementing an epoll-like mechanism, 
// or optimize irtp_update when handling massive rtp connections)
//---------------------------------------------------------------------
IUINT32 irtp_check(const irtpcb *rtp, IUINT32 current) {
	IUINT32 ts_flush = rtp->flush_timestamp;
	IINT32 tm_flush = 0x7fffffff;
	IINT32 tm_packet = 0x7fffffff;
	IUINT32 minimal = 0;
	struct IQUEUEHEAD *p;

	if (rtp->updated == 0) {
		return current;
	}

	if (_itimediff(current, ts_flush) >= 10000 ||
		_itimediff(current, ts_flush) < -10000) {
		ts_flush = current;
	}

	if (_itimediff(current, ts_flush) >= 0) {
		return current;
	}

	tm_flush = _itimediff(ts_flush, current);

	for (p = rtp->snd_buf.next; p != &rtp->snd_buf; p = p->next) {
		const IRTPSEG *seg = iqueue_entry(p, const IRTPSEG, node);
		IINT32 diff = _itimediff(seg->resend_timestamp, current);
		if (diff <= 0) {
			return current;
		}
		if (diff < tm_packet) tm_packet = diff;
	}

	minimal = (IUINT32)(tm_packet < tm_flush ? tm_packet : tm_flush);
	if (minimal >= rtp->interval) minimal = rtp->interval;

	return current + minimal;
}

int irtp_setmtu(irtpcb *rtp, int mtu) {
	char *buffer;
	if (mtu < 50 || mtu < (int)IRTP_OVERHEAD)
		return -1;
	buffer = (char*)irtp_malloc((mtu + IRTP_OVERHEAD) * 3);
	if (buffer == NULL)
		return -2;
	rtp->mtu = mtu;
	rtp->mss = rtp->mtu - IRTP_OVERHEAD;
	irtp_free(rtp->buffer);
	rtp->buffer = buffer;

	// 初始化冗余buffer设置
	if (rtp->redundancy_num > 0) {
		for (int i = 0; i < rtp->redundancy_num; i++) {
			irtp_free(rtp->old_send_data[i].data);
		}
		rtp->r_buffer_size = rtp->mtu / rtp->redundancy_num;
		for (int i = 0; i < rtp->redundancy_num; i++) {
			rtp->old_send_data[i].data = irtp_malloc(rtp->r_buffer_size);
			rtp->old_send_data[i].len = 0;
		}
		rtp->now_send_data_num = 0;
		rtp->old_send_data_used = 1; // 设置初始使用的窗口的个数
	}

	return 0;
}

int irtp_interval(irtpcb *rtp, int interval) {
	if (interval > 5000) interval = 5000;
	else if (interval < 10) interval = 10;
	rtp->interval = interval;
	return 0;
}


int irtp_waitsnd(const irtpcb *rtp) {
	return rtp->nsnd_buf + rtp->nsnd_que;
}

// read conv
IUINT32 irtp_getconv(const void *ptr) {
	IUINT32 conv;
	irtp_decode32u((const char*)ptr, &conv);
	return conv;
}

// update the size of the old_send_data buffer
int irtp_set_redundancy(irtpcb *rtp, int redun) {
	if (redun < 0)
		return -1;	// wrong number of redundancy number
	if (redun <= 1) redun = 0;
	if (redun >= IRTP_REDUN_MAX) redun = IRTP_REDUN_MAX;
	if (redun != rtp->redundancy_num) {
		if (redun > 0) {
			for (int i = 0; i < rtp->redundancy_num; i++) {
				irtp_free(rtp->old_send_data[i].data);
			}
			irtp_free(rtp->old_send_data);

			rtp->redundancy_num = redun;
			rtp->r_buffer_size = rtp->mtu / rtp->redundancy_num;

			rtp->old_send_data = irtp_malloc(sizeof(PacketCache) * rtp->redundancy_num);
			for (int i = 0; i < rtp->redundancy_num; i++) {
				rtp->old_send_data[i].data = irtp_malloc(rtp->r_buffer_size);
				rtp->old_send_data[i].len = 0;
			}
			rtp->now_send_data_num = 0;
			rtp->old_send_data_used = 1; // 设置初始使用的窗口的个数
		}
		else if (redun == 0) {
			for (int i = 0; i < rtp->redundancy_num; i++) {
				irtp_free(rtp->old_send_data[i].data);
			}
			irtp_free(rtp->old_send_data);
			rtp->old_send_data = NULL;
			rtp->redundancy_num = 0;
			rtp->now_send_data_num = 0;
			rtp->r_buffer_size = 0;
			rtp->old_send_data_used = 0;
		}
	}
	return 0;
}
