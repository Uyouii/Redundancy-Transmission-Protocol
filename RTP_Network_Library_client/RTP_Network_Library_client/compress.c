/**
@file compress.c
@brief An adaptive order-2 PPM range coder
*/
#define MRTP_BUILDING_LIB 1
#include <string.h>
#include "mrtp.h"

typedef struct _MRtpSymbol {
	/* binary indexed tree of symbols */
	mrtp_uint8 value;
	mrtp_uint8 count;
	mrtp_uint16 under;
	mrtp_uint16 left, right;

	/* context defined by this symbol */
	mrtp_uint16 symbols;
	mrtp_uint16 escapes;
	mrtp_uint16 total;
	mrtp_uint16 parent;
} MRtpSymbol;

/* adaptation constants tuned aggressively for small packet sizes rather than large file compression */
enum {
	MRTP_RANGE_CODER_TOP = 1 << 24,
	MRTP_RANGE_CODER_BOTTOM = 1 << 16,

	MRTP_CONTEXT_SYMBOL_DELTA = 3,
	MRTP_CONTEXT_SYMBOL_MINIMUM = 1,
	MRTP_CONTEXT_ESCAPE_MINIMUM = 1,

	MRTP_SUBCONTEXT_ORDER = 2,
	MRTP_SUBCONTEXT_SYMBOL_DELTA = 2,
	MRTP_SUBCONTEXT_ESCAPE_DELTA = 5
};

typedef struct _MRtpRangeCoder {
	/* only allocate enough symbols for reasonable MTUs, would need to be larger for large file compression */
	MRtpSymbol symbols[4096];
} MRtpRangeCoder;

void * mrtp_range_coder_create(void) {

	MRtpRangeCoder * rangeCoder = (MRtpRangeCoder *)mrtp_malloc(sizeof(MRtpRangeCoder));
	if (rangeCoder == NULL)
		return NULL;

	return rangeCoder;
}

void mrtp_range_coder_destroy(void * context) {

	MRtpRangeCoder * rangeCoder = (MRtpRangeCoder *)context;
	if (rangeCoder == NULL)
		return;

	mrtp_free(rangeCoder);
}

#define MRTP_SYMBOL_CREATE(symbol, value_, count_) \
{ \
    symbol = & rangeCoder -> symbols [nextSymbol ++]; \
    symbol -> value = value_; \
    symbol -> count = count_; \
    symbol -> under = count_; \
    symbol -> left = 0; \
    symbol -> right = 0; \
    symbol -> symbols = 0; \
    symbol -> escapes = 0; \
    symbol -> total = 0; \
    symbol -> parent = 0; \
}

#define MRTP_CONTEXT_CREATE(context, escapes_, minimum) \
{ \
    MRTP_SYMBOL_CREATE (context, 0, 0); \
    (context) -> escapes = escapes_; \
    (context) -> total = escapes_ + 256*minimum; \
    (context) -> symbols = 0; \
}

static mrtp_uint16 mrtp_symbol_rescale(MRtpSymbol * symbol) {

	mrtp_uint16 total = 0;
	for (;;)
	{
		symbol->count -= symbol->count >> 1;
		symbol->under = symbol->count;
		if (symbol->left)
			symbol->under += mrtp_symbol_rescale(symbol + symbol->left);
		total += symbol->under;
		if (!symbol->right) break;
		symbol += symbol->right;
	}
	return total;
}

#define MRTP_CONTEXT_RESCALE(context, minimum) \
{ \
    (context) -> total = (context) -> symbols ? mrtp_symbol_rescale ((context) + (context) -> symbols) : 0; \
    (context) -> escapes -= (context) -> escapes >> 1; \
    (context) -> total += (context) -> escapes + 256*minimum; \
}

#define MRTP_RANGE_CODER_OUTPUT(value) \
{ \
    if (outData >= outEnd) \
      return 0; \
    * outData ++ = value; \
}

#define MRTP_RANGE_CODER_ENCODE(under, count, total) \
{ \
    encodeRange /= (total); \
    encodeLow += (under) * encodeRange; \
    encodeRange *= (count); \
    for (;;) \
    { \
        if((encodeLow ^ (encodeLow + encodeRange)) >= MRTP_RANGE_CODER_TOP) \
        { \
            if(encodeRange >= MRTP_RANGE_CODER_BOTTOM) break; \
            encodeRange = -encodeLow & (MRTP_RANGE_CODER_BOTTOM - 1); \
        } \
        MRTP_RANGE_CODER_OUTPUT (encodeLow >> 24); \
        encodeRange <<= 8; \
        encodeLow <<= 8; \
    } \
}

#define MRTP_RANGE_CODER_FLUSH \
{ \
    while (encodeLow) \
    { \
        MRTP_RANGE_CODER_OUTPUT (encodeLow >> 24); \
        encodeLow <<= 8; \
    } \
}

#define MRTP_RANGE_CODER_FREE_SYMBOLS \
{ \
    if (nextSymbol >= sizeof (rangeCoder -> symbols) / sizeof (MRtpSymbol) - MRTP_SUBCONTEXT_ORDER ) \
    { \
        nextSymbol = 0; \
        MRTP_CONTEXT_CREATE (root, MRTP_CONTEXT_ESCAPE_MINIMUM, MRTP_CONTEXT_SYMBOL_MINIMUM); \
        predicted = 0; \
        order = 0; \
    } \
}

#define MRTP_CONTEXT_ENCODE(context, symbol_, value_, under_, count_, update, minimum) \
{ \
    under_ = value*minimum; \
    count_ = minimum; \
    if (! (context) -> symbols) \
    { \
        MRTP_SYMBOL_CREATE (symbol_, value_, update); \
        (context) -> symbols = symbol_ - (context); \
    } \
    else \
    { \
        MRtpSymbol * node = (context) + (context) -> symbols; \
        for (;;) \
        { \
            if (value_ < node -> value) \
            { \
                node -> under += update; \
                if (node -> left) { node += node -> left; continue; } \
                MRTP_SYMBOL_CREATE (symbol_, value_, update); \
                node -> left = symbol_ - node; \
            } \
            else \
            if (value_ > node -> value) \
            { \
                under_ += node -> under; \
                if (node -> right) { node += node -> right; continue; } \
                MRTP_SYMBOL_CREATE (symbol_, value_, update); \
                node -> right = symbol_ - node; \
            } \
            else \
            { \
                count_ += node -> count; \
                under_ += node -> under - node -> count; \
                node -> under += update; \
                node -> count += update; \
                symbol_ = node; \
            } \
            break; \
        } \
    } \
}

size_t mrtp_range_coder_compress(void * context, const MRtpBuffer * inBuffers, 
	size_t inBufferCount, size_t inLimit, mrtp_uint8 * outData, size_t outLimit)
{
	MRtpRangeCoder * rangeCoder = (MRtpRangeCoder *)context;
	mrtp_uint8 * outStart = outData, *outEnd = &outData[outLimit];
	const mrtp_uint8 * inData, *inEnd;
	mrtp_uint32 encodeLow = 0, encodeRange = ~0;
	MRtpSymbol * root;
	mrtp_uint16 predicted = 0;
	size_t order = 0, nextSymbol = 0;

	if (rangeCoder == NULL || inBufferCount <= 0 || inLimit <= 0)
		return 0;

	inData = (const mrtp_uint8 *)inBuffers->data;
	inEnd = &inData[inBuffers->dataLength];
	inBuffers++;
	inBufferCount--;

	MRTP_CONTEXT_CREATE(root, MRTP_CONTEXT_ESCAPE_MINIMUM, MRTP_CONTEXT_SYMBOL_MINIMUM);

	for (;;)
	{
		MRtpSymbol * subcontext, *symbol;
		mrtp_uint8 value;
		mrtp_uint16 count, under, *parent = &predicted, total;
		if (inData >= inEnd)
		{
			if (inBufferCount <= 0)
				break;
			inData = (const mrtp_uint8 *)inBuffers->data;
			inEnd = &inData[inBuffers->dataLength];
			inBuffers++;
			inBufferCount--;
		}
		value = *inData++;

		for (subcontext = &rangeCoder->symbols[predicted];
			subcontext != root;
			subcontext = &rangeCoder->symbols[subcontext->parent])
		{
			MRTP_CONTEXT_ENCODE(subcontext, symbol, value, under, count, MRTP_SUBCONTEXT_SYMBOL_DELTA, 0);
			*parent = symbol - rangeCoder->symbols;
			parent = &symbol->parent;
			total = subcontext->total;

			if (count > 0)
			{
				MRTP_RANGE_CODER_ENCODE(subcontext->escapes + under, count, total);
			}
			else
			{
				if (subcontext->escapes > 0 && subcontext->escapes < total)
					MRTP_RANGE_CODER_ENCODE(0, subcontext->escapes, total);
				subcontext->escapes += MRTP_SUBCONTEXT_ESCAPE_DELTA;
				subcontext->total += MRTP_SUBCONTEXT_ESCAPE_DELTA;
			}
			subcontext->total += MRTP_SUBCONTEXT_SYMBOL_DELTA;
			if (count > 0xFF - 2 * MRTP_SUBCONTEXT_SYMBOL_DELTA || subcontext->total > MRTP_RANGE_CODER_BOTTOM - 0x100)
				MRTP_CONTEXT_RESCALE(subcontext, 0);
			if (count > 0) goto nextInput;
		}

		MRTP_CONTEXT_ENCODE(root, symbol, value, under, count, MRTP_CONTEXT_SYMBOL_DELTA, MRTP_CONTEXT_SYMBOL_MINIMUM);
		*parent = symbol - rangeCoder->symbols;
		parent = &symbol->parent;
		total = root->total;

		MRTP_RANGE_CODER_ENCODE(root->escapes + under, count, total);
		root->total += MRTP_CONTEXT_SYMBOL_DELTA;
		if (count > 0xFF - 2 * MRTP_CONTEXT_SYMBOL_DELTA + MRTP_CONTEXT_SYMBOL_MINIMUM || root->total > MRTP_RANGE_CODER_BOTTOM - 0x100)
			MRTP_CONTEXT_RESCALE(root, MRTP_CONTEXT_SYMBOL_MINIMUM);

	nextInput:
		if (order >= MRTP_SUBCONTEXT_ORDER)
			predicted = rangeCoder->symbols[predicted].parent;
		else
			order++;
		MRTP_RANGE_CODER_FREE_SYMBOLS;
	}

	MRTP_RANGE_CODER_FLUSH;

	return (size_t)(outData - outStart);
}

#define MRTP_RANGE_CODER_SEED \
{ \
    if (inData < inEnd) decodeCode |= * inData ++ << 24; \
    if (inData < inEnd) decodeCode |= * inData ++ << 16; \
    if (inData < inEnd) decodeCode |= * inData ++ << 8; \
    if (inData < inEnd) decodeCode |= * inData ++; \
}

#define MRTP_RANGE_CODER_READ(total) ((decodeCode - decodeLow) / (decodeRange /= (total)))

#define MRTP_RANGE_CODER_DECODE(under, count, total) \
{ \
    decodeLow += (under) * decodeRange; \
    decodeRange *= (count); \
    for (;;) \
    { \
        if((decodeLow ^ (decodeLow + decodeRange)) >= MRTP_RANGE_CODER_TOP) \
        { \
            if(decodeRange >= MRTP_RANGE_CODER_BOTTOM) break; \
            decodeRange = -decodeLow & (MRTP_RANGE_CODER_BOTTOM - 1); \
        } \
        decodeCode <<= 8; \
        if (inData < inEnd) \
          decodeCode |= * inData ++; \
        decodeRange <<= 8; \
        decodeLow <<= 8; \
    } \
}

#define MRTP_CONTEXT_DECODE(context, symbol_, code, value_, under_, count_, update, minimum, createRoot, visitNode, createRight, createLeft) \
{ \
    under_ = 0; \
    count_ = minimum; \
    if (! (context) -> symbols) \
    { \
        createRoot; \
    } \
    else \
    { \
        MRtpSymbol * node = (context) + (context) -> symbols; \
        for (;;) \
        { \
            mrtp_uint16 after = under_ + node -> under + (node -> value + 1)*minimum, before = node -> count + minimum; \
            visitNode; \
            if (code >= after) \
            { \
                under_ += node -> under; \
                if (node -> right) { node += node -> right; continue; } \
                createRight; \
            } \
            else \
            if (code < after - before) \
            { \
                node -> under += update; \
                if (node -> left) { node += node -> left; continue; } \
                createLeft; \
            } \
            else \
            { \
                value_ = node -> value; \
                count_ += node -> count; \
                under_ = after - before; \
                node -> under += update; \
                node -> count += update; \
                symbol_ = node; \
            } \
            break; \
        } \
    } \
}

#define MRTP_CONTEXT_TRY_DECODE(context, symbol_, code, value_, under_, count_, update, minimum, exclude) \
MRTP_CONTEXT_DECODE (context, symbol_, code, value_, under_, count_, update, minimum, return 0, exclude (node -> value, after, before), return 0, return 0)

#define MRTP_CONTEXT_ROOT_DECODE(context, symbol_, code, value_, under_, count_, update, minimum, exclude) \
MRTP_CONTEXT_DECODE (context, symbol_, code, value_, under_, count_, update, minimum, \
    { \
        value_ = code / minimum; \
        under_ = code - code%minimum; \
        MRTP_SYMBOL_CREATE (symbol_, value_, update); \
        (context) -> symbols = symbol_ - (context); \
    }, \
    exclude (node -> value, after, before), \
    { \
        value_ = node->value + 1 + (code - after)/minimum; \
        under_ = code - (code - after)%minimum; \
        MRTP_SYMBOL_CREATE (symbol_, value_, update); \
        node -> right = symbol_ - node; \
    }, \
    { \
        value_ = node->value - 1 - (after - before - code - 1)/minimum; \
        under_ = code - (after - before - code - 1)%minimum; \
        MRTP_SYMBOL_CREATE (symbol_, value_, update); \
        node -> left = symbol_ - node; \
    }) \

#define MRTP_CONTEXT_NOT_EXCLUDED(value_, after, before)

size_t mrtp_range_coder_decompress(void * context, const mrtp_uint8 * inData, 
	size_t inLimit, mrtp_uint8 * outData, size_t outLimit)
{
	MRtpRangeCoder * rangeCoder = (MRtpRangeCoder *)context;
	mrtp_uint8 * outStart = outData, *outEnd = &outData[outLimit];
	const mrtp_uint8 * inEnd = &inData[inLimit];
	mrtp_uint32 decodeLow = 0, decodeCode = 0, decodeRange = ~0;
	MRtpSymbol * root;
	mrtp_uint16 predicted = 0;
	size_t order = 0, nextSymbol = 0;

	if (rangeCoder == NULL || inLimit <= 0)
		return 0;

	MRTP_CONTEXT_CREATE(root, MRTP_CONTEXT_ESCAPE_MINIMUM, MRTP_CONTEXT_SYMBOL_MINIMUM);

	MRTP_RANGE_CODER_SEED;

	for (;;)
	{
		MRtpSymbol * subcontext, *symbol, *patch;
		mrtp_uint8 value = 0;
		mrtp_uint16 code, under, count, bottom, *parent = &predicted, total;

		for (subcontext = &rangeCoder->symbols[predicted];
			subcontext != root;
			subcontext = &rangeCoder->symbols[subcontext->parent])
		{
			if (subcontext->escapes <= 0)
				continue;
			total = subcontext->total;
			if (subcontext->escapes >= total)
				continue;
			code = MRTP_RANGE_CODER_READ(total);
			if (code < subcontext->escapes)
			{
				MRTP_RANGE_CODER_DECODE(0, subcontext->escapes, total);
				continue;
			}
			code -= subcontext->escapes;
			
			MRTP_CONTEXT_TRY_DECODE(subcontext, symbol, code, value, under, count, MRTP_SUBCONTEXT_SYMBOL_DELTA, 0, MRTP_CONTEXT_NOT_EXCLUDED);
			
			bottom = symbol - rangeCoder->symbols;
			MRTP_RANGE_CODER_DECODE(subcontext->escapes + under, count, total);
			subcontext->total += MRTP_SUBCONTEXT_SYMBOL_DELTA;
			if (count > 0xFF - 2 * MRTP_SUBCONTEXT_SYMBOL_DELTA || subcontext->total > MRTP_RANGE_CODER_BOTTOM - 0x100)
				MRTP_CONTEXT_RESCALE(subcontext, 0);
			goto patchContexts;
		}

		total = root->total;

		code = MRTP_RANGE_CODER_READ(total);
		if (code < root->escapes)
		{
			MRTP_RANGE_CODER_DECODE(0, root->escapes, total);
			break;
		}
		code -= root->escapes;

		MRTP_CONTEXT_ROOT_DECODE(root, symbol, code, value, under, count, MRTP_CONTEXT_SYMBOL_DELTA, MRTP_CONTEXT_SYMBOL_MINIMUM, MRTP_CONTEXT_NOT_EXCLUDED);
		
		bottom = symbol - rangeCoder->symbols;
		MRTP_RANGE_CODER_DECODE(root->escapes + under, count, total);
		root->total += MRTP_CONTEXT_SYMBOL_DELTA;
		if (count > 0xFF - 2 * MRTP_CONTEXT_SYMBOL_DELTA + MRTP_CONTEXT_SYMBOL_MINIMUM || root->total > MRTP_RANGE_CODER_BOTTOM - 0x100)
			MRTP_CONTEXT_RESCALE(root, MRTP_CONTEXT_SYMBOL_MINIMUM);

	patchContexts:
		for (patch = &rangeCoder->symbols[predicted];
			patch != subcontext;
			patch = &rangeCoder->symbols[patch->parent])
		{
			MRTP_CONTEXT_ENCODE(patch, symbol, value, under, count, MRTP_SUBCONTEXT_SYMBOL_DELTA, 0);
			*parent = symbol - rangeCoder->symbols;
			parent = &symbol->parent;
			if (count <= 0)
			{
				patch->escapes += MRTP_SUBCONTEXT_ESCAPE_DELTA;
				patch->total += MRTP_SUBCONTEXT_ESCAPE_DELTA;
			}
			patch->total += MRTP_SUBCONTEXT_SYMBOL_DELTA;
			if (count > 0xFF - 2 * MRTP_SUBCONTEXT_SYMBOL_DELTA || patch->total > MRTP_RANGE_CODER_BOTTOM - 0x100)
				MRTP_CONTEXT_RESCALE(patch, 0);
		}
		*parent = bottom;

		MRTP_RANGE_CODER_OUTPUT(value);

		if (order >= MRTP_SUBCONTEXT_ORDER)
			predicted = rangeCoder->symbols[predicted].parent;
		else
			order++;
		MRTP_RANGE_CODER_FREE_SYMBOLS;
	}

	return (size_t)(outData - outStart);
}

/** @defgroup host MRtp host functions
@{
*/

/** Sets the packet compressor the host should use to the default range coder.
@param host host to enable the range coder for
@returns 0 on success, < 0 on failure
*/
int mrtp_host_compress_with_range_coder(MRtpHost * host) {

	MRtpCompressor compressor;
	memset(&compressor, 0, sizeof(compressor));
	compressor.context = mrtp_range_coder_create();
	if (compressor.context == NULL)
		return -1;
	compressor.compress = mrtp_range_coder_compress;
	compressor.decompress = mrtp_range_coder_decompress;
	compressor.destroy = mrtp_range_coder_destroy;
	mrtp_host_compress(host, &compressor);
	return 0;
}

/** @} */


