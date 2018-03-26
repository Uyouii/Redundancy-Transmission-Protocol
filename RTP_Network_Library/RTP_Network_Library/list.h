/**
@file  list.h
@brief MRtp list management
*/
#ifndef __MRTP_LIST_H__
#define __MRTP_LIST_H__

#include <stdlib.h>

typedef struct _MRtpListNode
{
	struct _MRtpListNode * next;
	struct _MRtpListNode * previous;
} MRtpListNode;

typedef MRtpListNode * MRtpListIterator;

typedef struct _MRtpList
{
	MRtpListNode sentinel;
} MRtpList;

extern void mrtp_list_clear(MRtpList *);

extern MRtpListIterator mrtp_list_insert(MRtpListIterator, void *);
extern void * mrtp_list_remove(MRtpListIterator);
extern MRtpListIterator mrtp_list_move(MRtpListIterator, void *, void *);

extern size_t mrtp_list_size(MRtpList *);

#define mrtp_list_begin(list) ((list) -> sentinel.next)
#define mrtp_list_end(list) (& (list) -> sentinel)

#define mrtp_list_empty(list) (mrtp_list_begin (list) == mrtp_list_end (list))

#define mrtp_list_next(iterator) ((iterator) -> next)
#define mrtp_list_previous(iterator) ((iterator) -> previous)

#define mrtp_list_front(list) ((void *) (list) -> sentinel.next)
#define mrtp_list_back(list) ((void *) (list) -> sentinel.previous)

#endif /* __MRTP_LIST_H__ */

