#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdio.h>
#include "icnsbuilder.h"

enum {
	kFileHeaderSize	= 8,
	kIconHeaderSize = 8
};

struct RGB_ {
	uint8_t r;
	uint8_t g;
	uint8_t b;
};
typedef struct RGB_ RGB;

// these must be able to handle unaligned addresses
static void Put16(void *mem, long offset, int16_t value)
{
	uint8_t *p = mem;
	p += offset;
	p[0] = value >> 8;
	p[1] = value;
}
static void Put32(void *mem, long offset, int32_t value)
{
	uint8_t *p = mem;
	p += offset;
	p[0] = value >> 24;
	p[1] = value >> 16;
	p[2] = value >> 8;
	p[3] = value;
}


RGB palette16[16];
RGB palette256[256];

static void MakePalette16(void)
{
	// RGBColor from clut id 4
	static const uint16_t clut4[] = {
		0xFFFF, 0xFFFF, 0xFFFF,
		0xFC00, 0xF37D, 0x052F,	// yellow
		0xFFFF, 0x648A, 0x028C,	// orange
		0xDD6B, 0x08C2, 0x06A2,	// red
		0xF2D7, 0x0856, 0x84EC,	// magenta
		0x46E3, 0x0000, 0xA53E,	// purple
		0x0000, 0x0000, 0xD400,	// blue
		0x0241, 0xAB54, 0xEAFF,	// cyan
		0x1F21, 0xB793, 0x1431,	// light green
		0x0000, 0x64AF, 0x11B0,	// dark green
		0x5600, 0x2C9D, 0x0524,	// brown
		0x90D7, 0x7160, 0x3A34,	// light brown
		0xC000, 0xC000, 0xC000,	// light grey
		0x8000, 0x8000, 0x8000,	// grey
		0x4000, 0x4000, 0x4000,	// dark grey
		0x0000, 0x0000, 0x0000,	// black
	};
	int i;
	for (i = 0; i < 16; i++) {
		palette16[i].r = clut4[i*3+0] >> 8;
		palette16[i].g = clut4[i*3+1] >> 8;
		palette16[i].b = clut4[i*3+2] >> 8;
	}
}
static void MakePalette256(void)
{
	int i;
	uint8_t grad[] = {
		0xEE, 0xDD, 0xBB, 0xAA, 0x88, 0x77, 0x55, 0x44, 0x22, 0x11
	};
	for (i = 0; i < 215; i++) {
		palette256[i].r = 0xFF - 0x33 * (i / 36);
		palette256[i].g = 0xFF - 0x33 * ((i / 6) % 6);
		palette256[i].b = 0xFF - 0x33 * (i % 6);
	}
	for (i = 0; i < 10; i++) {
		palette256[215+i].r = grad[i];
		palette256[225+i].g = grad[i];
		palette256[235+i].b = grad[i];
		palette256[245+i].r = grad[i];
		palette256[245+i].g = grad[i];
		palette256[245+i].b = grad[i];
	}
	palette256[255].r = 0;
	palette256[255].g = 0;
	palette256[255].b = 0;
}

long ICNSCompressChannel(const void *imgdata, int channeloff, long npixels, void *dest)
{
	const int8_t *base = imgdata;
	int8_t *q = dest;
	long pos = 0;
	long runstart = 0;
	long i;
	const int step = 4;	// bytes per pixel
	base += channeloff;
	while (pos < npixels) {
		int8_t byte = base[pos*step];
		// count # of same bytes
		for (i = 1; i < 130; i++)
			if (base[pos*step] != base[(pos+i)*step])
				break;
		if (i > npixels - pos)
			i = npixels - pos;
		if (i >= 3) {
			while (runstart < pos) {
				long run = pos - runstart;
				long j;
				if (run > 128)
					run = 128;
				*q++ = run - 1;
				for (j = 0; j < run; j++)
					q[j] = base[(runstart+j) * step];
				q += run;
				runstart += run;
			}
			*q++ = i - 3 - 128;
			*q++ = byte;
			pos = pos + i;
			runstart = pos;
		}
		else {
			pos += i;
		}
	}
	while (runstart < pos) {
		long run = pos - runstart;
		long j;
		if (run > 128)
			run = 128;
		*q++ = run - 1;
		for (j = 0; j < run; j++)
			q[j] = base[(runstart+j) * step];
		q += run;
		runstart += run;
	}
	return q - (int8_t *)dest;
}

long ICNSCompressImage(uint32_t tag, const void *imgdata, long datasize, void *dest)
{
	int8_t *q = dest;
	long len;
	long padbytes = ICNSCompressedPadSizeForTag(tag);
	memset(dest, 0, padbytes);
	q += padbytes;
	len = ICNSCompressChannel(imgdata, 1, datasize / 4, q);
	q += len;
	len = ICNSCompressChannel(imgdata, 2, datasize / 4, q);
	q += len;
	len = ICNSCompressChannel(imgdata, 3, datasize / 4, q);
	q += len;
	return q - (int8_t *)dest;
}

static int Grow(ICNSBuilder *builder, long size)
{
	char *p = realloc(builder->data, size);
	if (p) {
		builder->data = p;
		builder->capacity = size;
		builder->length = size;
		Put32(builder->data, 4, builder->length);
		return 1;
	}
	return 0;
}

void ICNSBuilderInit(ICNSBuilder *builder)
{
	builder->data = NULL;
	builder->capacity = 0;
	builder->length = 0;
	
	Grow(builder, kFileHeaderSize);
	Put32(builder->data, 0, 'icns');
}

void ICNSBuilderTerminate(ICNSBuilder *builder)
{
	free(builder->data);
	builder->data = NULL;
	builder->capacity = 0;
	builder->length = 0;
}

int ICNSAddData(ICNSBuilder *builder, uint32_t tag, const void *data, long size)
{
	long off = builder->length;
	Grow(builder, off + kIconHeaderSize + size);
	Put32(builder->data, off + 0, tag);
	Put32(builder->data, off + 4, size + kIconHeaderSize);
	memmove(builder->data + off + kIconHeaderSize, data, size);
	return 0;
}

long ICNSBuilderGetSize(ICNSBuilder *builder)
{
	return builder->length;
}

void * ICNSBuilderGetDataPtr(ICNSBuilder *builder)
{
	return builder->data;
}
