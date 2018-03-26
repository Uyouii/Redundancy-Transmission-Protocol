#ifndef __MRTP_TIME_H__
#define __MRTP_TIME_H__

#define MRTP_TIME_OVERFLOW 86400000 //一天的毫秒数

#define MRTP_TIME_LESS(a, b) ((a) - (b) >= MRTP_TIME_OVERFLOW)
#define MRTP_TIME_GREATER(a, b) ((b) - (a) >= MRTP_TIME_OVERFLOW)
#define MRTP_TIME_LESS_EQUAL(a, b) (! MRTP_TIME_GREATER (a, b))
#define MRTP_TIME_GREATER_EQUAL(a, b) (! MRTP_TIME_LESS (a, b))

#define MRTP_TIME_DIFFERENCE(a, b) ((a) - (b) >= MRTP_TIME_OVERFLOW ? (b) - (a) : (a) - (b))

#endif /* __MRTP_TIME_H__ */
