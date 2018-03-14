
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

//---------------------------------------------------------------------
// encode / decode
//---------------------------------------------------------------------

static inline long _itimediff(IUINT32 later, IUINT32 earlier) {
	return ((IINT32)(later - earlier));
}



//---------------------------------------------------------------------
// manage segment
//---------------------------------------------------------------------
typedef struct IRTPSEG IRTPSEG;

static void* (*irtp_malloc_hook)(size_t) = NULL;
static void(*irtp_free_hook)(void *) = NULL;

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


static int irtp_wnd_unused(const irtpcb *rtp) {
	if (rtp->nrcv_que < rtp->rcv_wnd) {
		return rtp->rcv_wnd - rtp->nrcv_que;
	}
	return 0;
}

//---------------------------------------------------------------------
// irtp_flush
//---------------------------------------------------------------------
void irtp_flush(irtpcb *rtp)
{
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

	// calculate window size，取发送窗口和远端窗口的最小值
	cwnd = _imin_(rtp->snd_wnd, rtp->remote_wnd);
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
	resent = (rtp->fastresend > 0) ? (IUINT32)rtp->fastresend : 0xffffffff;
	rtomin = (rtp->nodelay == 0) ? (rtp->rx_rto >> 3) : 0;

	// flush data segments
	for (p = rtp->snd_buf.next; p != &rtp->snd_buf; p = p->next) {
		IRTPSEG *segment = iqueue_entry(p, IRTPSEG, node);
		int needsend = 0;
		if (segment->xmit == 0) {
			needsend = 1;
			segment->xmit++;
			segment->rto = rtp->rx_rto;
			segment->resend_timestamp = current + segment->rto + rtomin;
		}
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

			if (size + need > (int)rtp->mtu) {
				irtp_output(rtp, buffer, size);
				ptr = buffer;
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
		irtp_output(rtp, buffer, size);
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