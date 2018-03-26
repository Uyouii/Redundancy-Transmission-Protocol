/**
@file  callbacks.h
@brief MRtp callbacks
*/
#ifndef __MRTP_CALLBACKS_H__
#define __MRTP_CALLBACKS_H__

#include <stdlib.h>

typedef struct _MRtpCallbacks
{
	void * (MRTP_CALLBACK * malloc) (size_t size);
	void (MRTP_CALLBACK * free) (void * memory);
	void (MRTP_CALLBACK * no_memory) (void);
} MRtpCallbacks;

/** @defgroup callbacks MRtp internal callbacks
@{
@ingroup private
*/
extern void * mrtp_malloc(size_t);
extern void   mrtp_free(void *);

/** @} */

#endif /* __MRTP_CALLBACKS_H__ */

