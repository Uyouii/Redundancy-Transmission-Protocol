#include "mrtp.h"

static MRtpCallbacks callbacks = { malloc, free, abort };

int mrtp_initialize_with_callbacks(const MRtpCallbacks * inits) {

	if (inits->malloc != NULL || inits->free != NULL)
	{
		if (inits->malloc == NULL || inits->free == NULL)
			return -1;

		callbacks.malloc = inits->malloc;
		callbacks.free = inits->free;
	}

	if (inits->no_memory != NULL)
		callbacks.no_memory = inits->no_memory;

	return mrtp_initialize();
}


void * mrtp_malloc(size_t size) {
	void * memory = callbacks.malloc(size);

	if (memory == NULL)
		callbacks.no_memory();

	return memory;
}

void mrtp_free(void * memory) {
	callbacks.free(memory);
}

