#ifndef ICNSBUILDER_H
#define ICNSBUILDER_H 1

#include <stdint.h>

struct ICNSBuilder_ {
	char *data;
	long capacity;
	long length;
};
typedef struct ICNSBuilder_ ICNSBuilder;

// it32 seems to need 4-byte have pad before compressed data
long ICNSCompressImage(uint32_t tag, const void *imgdata, long datasize, void *destbuf);
#define ICNSCompressedPadSizeForTag(tag) ((tag) == 'it32' ? 4 : 0)

void ICNSBuilderInit(ICNSBuilder *builder);
void ICNSBuilderTerminate(ICNSBuilder *builder);

int ICNSAddData(ICNSBuilder *builder, uint32_t tag, const void *data, long size);

long ICNSBuilderGetSize(ICNSBuilder *builder);
void * ICNSBuilderGetDataPtr(ICNSBuilder *builder);

#endif

