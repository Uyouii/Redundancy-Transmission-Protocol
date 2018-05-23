#include "list.h"

void mrtp_list_clear(MRtpList * list) {
	list->sentinel.next = &list->sentinel;
	list->sentinel.previous = &list->sentinel;
}

MRtpListIterator mrtp_list_insert(MRtpListIterator position, void * data) {

	MRtpListIterator result = (MRtpListIterator)data;

	result->previous = position->previous;
	result->next = position;

	result->previous->next = result;
	position->previous = result;

	return result;
}

void * mrtp_list_remove(MRtpListIterator position) {
	position->previous->next = position->next;
	position->next->previous = position->previous;

	return position;
}

MRtpListIterator mrtp_list_move(MRtpListIterator position, void * dataFirst, void * dataLast) {

	MRtpListIterator first = (MRtpListIterator)dataFirst,
		last = (MRtpListIterator)dataLast;

	first->previous->next = last->next;
	last->next->previous = first->previous;

	first->previous = position->previous;
	last->next = position;

	first->previous->next = first;
	position->previous = last;

	return first;
}

size_t mrtp_list_size(MRtpList * list) {

	size_t size = 0;
	MRtpListIterator position;

	for (position = mrtp_list_begin(list);
		position != mrtp_list_end(list);
		position = mrtp_list_next(position))
		++size;

	return size;
}

