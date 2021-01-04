#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <ctype.h>
#include <stdint.h>
#include <zlib.h>
#include "png.h"

#ifdef TEST
void Dump(const void *data, long len);
#endif

typedef signed char boolean;

// the address can be unaligned
static uint16_t Get16(const void *mem, long off)
{
	const uint8_t *p = mem;
	p += off;
	return p[0] * 256 + p[1];
}
static uint32_t Get32(const void *mem, long off)
{
	const uint8_t *p = mem;
	p += off;
	return p[0] * 16777216 + p[1] * 65536 + p[2] * 256 + p[3];
}
static void Put8(void *mem, long off, uint8_t value)
{
	uint8_t *p = mem;
	p[off] = value;
}
static void Put16(void *mem, long off, uint16_t value)
{
	uint8_t *p = mem;
	p += off;
	p[0] = value >> 8;
	p[1] = value;
}
static void Put32(void *mem, long off, uint32_t value)
{
	uint8_t *p = mem;
	p += off;
	p[0] = value >> 24;
	p[1] = value >> 16;
	p[2] = value >> 8;
	p[3] = value;
}

uint32_t gCRCTable[256];
#define kCRCPoly	0xEDB88320

void MakeCRCTable(void)
{
	int v;
	uint32_t *table = gCRCTable;
	if (table[1] == 0) {
		// table is uninitialized
		table[0] = 0;
		for (v = 1; v < 256; v++) {
			uint32_t r = v;
			int i;
			for (i = 0; i < 8; i++) {
				if (r & 1) {
					r >>= 1;
					r ^= kCRCPoly;
				}
				else
					r >>= 1;
			}
			table[v] = r;
		}
	}
}

uint32_t UpdateCRC(uint32_t crc, const void *mem, long len)
{
	uint32_t r = crc;
	const uint8_t *p = mem;
	const uint32_t *table = gCRCTable;
	while (len-- != 0) {
		uint8_t v = *p++;
		r = (r >> 8) ^ table[v ^ (r & 255)];
	}
	return r;
}

static void * DeflateAllAtOnce(const void *data, unsigned long size, unsigned long *outsize)
{
	z_stream z;
	int zr;
	unsigned long zbound;
	unsigned long zchunksize = 16384;
	long zbufsize;
	uint8_t *zbuf;
	
	z.zalloc = Z_NULL;
	z.zfree = Z_NULL;
	z.opaque = Z_NULL;
	
	//zr = deflateInit2(&z, Z_BEST_COMPRESSION, Z_DEFLATED, 15, 8, Z_DEFAULT_STRATEGY);
	zr = deflateInit(&z, Z_BEST_COMPRESSION);
	if (zr != Z_OK) {
		fprintf(stderr, "zlib deflateInit error: %s\n", z.msg);
		return NULL;
	}
	zbound = deflateBound(&z, size);
	
	zbufsize = (zbound + zchunksize - 1) / zchunksize * zchunksize;
	zbuf = malloc(zbufsize);
	z.next_in = data;
	z.avail_in = size;
	z.next_out = zbuf;
	z.avail_out = zbufsize;
	
	do {
		zr = deflate(&z, Z_FINISH);
		if (zr == Z_STREAM_END)
			break;
		else if (zr == Z_OK) {
			// continue
			if (z.avail_out == 0) {
				uint8_t *p = realloc(zbuf, zbufsize + zchunksize);
				if (p) {
					zbuf = p;
					z.next_out = zbuf + zbufsize;
					zbufsize += zchunksize;
					z.avail_out = zchunksize;
				}
				else {
					fprintf(stderr, "DeflateAll: no memory\n");
					free(zbuf);
					zbuf = NULL;
					break;
				}
			}
		}
		else {
			// error
			fprintf(stderr, "zlib deflate error: %s\n", z.msg);
			free(zbuf);
			zbuf = NULL;
			break;
		}
	} while (1);
	
	if (zbuf && outsize)
		*outsize = z.total_out;
	
	deflateEnd(&z);
	
	return zbuf;
}

static void * InflateAllAtOnce(const void *data, unsigned long size, unsigned long expectedsize, unsigned long *outsize)
{
	z_stream z;
	int zr;
	unsigned long zchunksize = 16384;
	long zbufsize;
	uint8_t *zbuf;
	
	z.zalloc = Z_NULL;
	z.zfree = Z_NULL;
	z.opaque = Z_NULL;
	
	zr = inflateInit(&z);
	if (zr != Z_OK) {
		fprintf(stderr, "zlib inflateInit error: %s\n", z.msg);
		return NULL;
	}
	
	zbufsize = (expectedsize + zchunksize - 1) / zchunksize * zchunksize;
	zbuf = malloc(zbufsize);
	z.next_in = data;
	z.avail_in = size;
	z.next_out = zbuf;
	z.avail_out = zbufsize;
	
	do {
		zr = inflate(&z, Z_NO_FLUSH);
		if (zr == Z_STREAM_END)
			break;
		else if (zr == Z_OK) {
			// continue
			if (z.avail_out == 0) {
				uint8_t *p = realloc(zbuf, zbufsize + zchunksize);
				if (p) {
					zbuf = p;
					z.next_out = zbuf + zbufsize;
					zbufsize += zchunksize;
					z.avail_out = zchunksize;
				}
				else {
					fprintf(stderr, "InflateAll: no memory\n");
					free(zbuf);
					zbuf = NULL;
					break;
				}
			}
		}
		else {
			// error
			fprintf(stderr, "zlib inflate error: %s\n", z.msg);
			free(zbuf);
			zbuf = NULL;
			break;
		}
	} while (1);
	
	if (zbuf && outsize)
		*outsize = z.total_out;
	
	inflateEnd(&z);
	
	return zbuf;
}

/* make simple PNG with no interlace, zero filter */
void * CompressToPNG(int width, int height, const void *rgb, const void *mask, long *outsize)
{
	char pngsig[8] = "\x89PNG\15\12\32\12";
	char ihdr[25];
	char idathdr[8] = "\0\0\0\0IDAT";
	char iend[12] = "\0\0\0\0IEND\0\0\0\0";
	uint32_t crc;
	const uint8_t *rgbp = rgb;
	const uint8_t *maskp = mask;
	uint8_t *buf;
	long usize;
	uint8_t *zbuf;
	unsigned long zsize;
	char idatcrc[4];
	uint8_t *pngbuf;
	unsigned long pngsize;
	
	MakeCRCTable();
	
	// construct IHDR
	memmove(ihdr, "\0\0\0\15IHDR", 8);
	Put32(ihdr, 8, width);
	Put32(ihdr, 12, height);
	ihdr[16] = 8;	// depth
	if (mask)
		ihdr[17] = 6;	// colour type : Truecolour
	else
		ihdr[17] = 2;
	ihdr[18] = 0;	// compression method : deflate
	ihdr[19] = 0;	// filter method
	ihdr[20] = 0;	// interlace : none
	crc = UpdateCRC(-1, &ihdr[4], 17);
	Put32(ihdr, 21, ~ crc);
	
	// reorder pixel data
	if (mask) {
		int i, j;
		uint8_t *row;
		usize = (1 + 4 * width) * height;
		buf = malloc(usize);
		row = buf;
		for (i = 0; i < height; i++) {
			row[0] = 0;
			for (j = 0; j < width; j++) {
				row[j*4+1] = rgbp[(i*width+j)*4+1];
				row[j*4+2] = rgbp[(i*width+j)*4+2];
				row[j*4+3] = rgbp[(i*width+j)*4+3];
				row[j*4+4] = maskp[i*width+j];
			}
			row += 4 * width + 1;
		}
	}
	else {
		int i, j;
		uint8_t *row;
		usize = (1 + 3 * width) * height;
		buf = malloc(usize);
		row = buf;
		for (i = 0; i < height; i++) {
			row[0] = 0;
			for (j = 0; j < width; j++) {
				row[j*3+1] = rgbp[(i*width+j)*4+1];
				row[j*3+2] = rgbp[(i*width+j)*4+2];
				row[j*3+3] = rgbp[(i*width+j)*4+3];
			}
			row += 3 * width + 1;
		}
	}
	
	// construct IDAT
	zbuf = DeflateAllAtOnce(buf, usize, &zsize);
	if (zbuf == NULL) {
		free(buf);
		return NULL;
	}
	Put32(idathdr, 0, zsize);
	memmove(&idathdr[4], "IDAT", 4);
	crc = UpdateCRC(-1, &idathdr[4], 4);
	crc = UpdateCRC(crc, zbuf, zsize);
	Put32(idatcrc, 0, ~ crc);
	
	// IEND
	crc = UpdateCRC(-1, "IEND", 4);
	Put32(iend, 8, ~ crc);
	
	// make png stream
	pngsize = 8;	// signature
	pngsize += 25;	// IHDR
	pngsize += 12;	// IDAT header & crc
	pngsize += zsize;
	pngsize += 12;	// IEND
	pngbuf = malloc(pngsize);
	memmove(&pngbuf[0], pngsig, 8);
	memmove(&pngbuf[8], ihdr, 25);
	memmove(&pngbuf[33], idathdr, 8);
	memmove(&pngbuf[41], zbuf, zsize);
	memmove(&pngbuf[41+zsize], idatcrc, 4);
	memmove(&pngbuf[45+zsize], iend, 12);
	
	free(buf);
	free(zbuf);
	
	if (outsize)
		*outsize = pngsize;
	
	return pngbuf;
}



static boolean CheckCRC(const void *chunk)
{
	const uint8_t *p = chunk;
	long size = Get32(p, 0);
	uint32_t crc;
	crc = UpdateCRC(-1, p + 4, size + 4);
	if (Get32(p, 8 + size) == ~ crc) 
		return 1;
	else {
		fprintf(stderr, "CRC mismatch on %.4s (%08X): %08X calculated, %08X found\n", p + 4, Get32(p, 4), ~ crc, Get32(p, 8 + size));
		return 0;
	}
}

static const void * FindChunk(const void *afterthischunk, const void * limit, uint32_t chunk)
{
	const uint8_t *p = afterthischunk;
	const uint8_t *end = limit;
	uint32_t name;
	long size;
	do {
		size = Get32(p, 0);
		name = Get32(p, 4);
		if (name == chunk)
			return p;
		else if (name == 'IEND')
			return NULL;
		else
			p += size + 12;
	} while (p < end);
	return NULL;
}

static int PNGNComponents(int colourtype)
{
	int ncomp = 1;	// default value chosen for safety
	switch (colourtype) {
	case 0:
		ncomp = 1;
		break;
	case 2:
		ncomp = 3;
		break;
	case 3:
		ncomp = 1;
		break;
	case 4:
		ncomp = 2;
		break;
	case 6:
		ncomp = 4;
		break;
	}
	return ncomp;
}

static int Paeth(int a, int b, int c)
{
	int pa, pb, pc;
	pa = abs(b - c);
	pb = abs(a - c);
	pc = abs(a + b - 2 * c);
	if (pa <= pb && pa <= pc)
		return a;
	else if (pb <= pc)
		return b;
	else
		return c;
}

static void Unfilter(uint8_t *image, long width, long height, int depth, int ncomp)
{
	int i, j, k;
	long rowbytes = (ncomp * depth * width + 7) / 8;
	uint8_t *zero = calloc(1, rowbytes + 1);
	uint8_t *row = image;
	uint8_t *lastrow = zero;
	unsigned char filtertype;
	for (i = 0; i < height; i++) {
		filtertype = row[0];
		row[0] = 0;
		//fprintf(stderr, "%d: filter %d\n", i, filtertype);
		if (depth < 8) {
			// byte-by-byte filter
			// ok to access row[j-1] because row[0] is now 0
			for (j = 1; j <= rowbytes; j++) {
				switch (filtertype) {
				default:
				case 0:
					// do nothing
					break;
				case 1:	// sub
					row[j] += row[j-1];
					break;
				case 2:	// up
					row[j] += lastrow[j];
					break;
				case 3:	// average
					row[j] += ((int)row[j-1] + (int)lastrow[j]) / 2;
					break;
				case 4:	// paeth
					row[j] += Paeth(row[j-1], lastrow[j], lastrow[j-1]);
					break;
				}
			}
		}
		else {
			// byte-by-byte filter with pixel-size offset
			int nb = (depth + 7) / 8;
			int bpp = nb * ncomp;
			for (j = 1; j <= rowbytes; j++) {
				int a;
				int b;
				int c;
				a = j < bpp ? 0 : row[j-bpp];
				b = lastrow[j];
				c = j < bpp ? 0 : lastrow[j-bpp];
				switch (filtertype) {
				default:
				case 0:
					// do nothing
					break;
				case 1:	// sub
					row[j] += a;
					break;
				case 2:	// up
					row[j] += b;
					break;
				case 3:	// average
					row[j] += (a + b) / 2;
					break;
				case 4:	// paeth
					row[j] += Paeth(a, b, c);
					break;
				}
			}
		}
		lastrow = row;
		row += rowbytes + 1;
	}
	free(zero);
}

static long ToARGB(void *pngimage, long width, long height, int pngdepth, int pngcolourtype, const void *pltechunk, const void *bkgdchunk, void *dest, long destwid, int hstart, int vstart, int hshift, int vshift)
{
	long i, j;
	uint8_t *stream = pngimage;
	uint8_t *argb = dest;
	const uint8_t *bkgd = bkgdchunk;
	const uint8_t *plte = pltechunk;
	int ncomp = PNGNComponents(pngcolourtype);
	long rowbytes = (pngdepth * ncomp * width + 7) / 8;
	uint8_t *row;
	// for depth <= 8
	int cpb = 8 / pngdepth;	// components per byte
	int mask = (1 << pngdepth) - 1;
	int mult = 255 / mask;	// 255 is divisible with all 1, 3, 15, 255
	// for depth > 8
	int nb = (pngdepth + 7) / 8;	// something like 12-bit format is padded
	double div = ((1 << pngdepth) - 1) / 255.0;
	int bgpix = -1;
	
	Unfilter(stream, width, height, pngdepth, ncomp);
	if (bkgd) {
		switch (pngcolourtype) {
		case 0:
		case 4:
			// unsupported
			bgpix = Get16(bkgd, 8);
			break;
		case 3:
			bgpix = bkgd[8];
			break;
		case 2:
		case 6:
			// unsupported
			break;
		}
	}
	row = stream;
	for (i = 0; i < height; i++) {
		for (j = 0; j < width; j++) {
			int pix;
			int alpha;
			int xb;
			//long pindex = (i*destwid<<vshift)+(j<<hshift);
			long pindex = ((i<<vshift)+vstart)*destwid+(j<<hshift)+hstart;
			//fprintf(stderr, "%d %d\n", i << vshift, j << hshift);
			//fprintf(stderr, "%ld %ld\n", (i << vshift) + vstart, (j << hshift) + hstart);
			if (pngdepth <= 8) {
				switch (pngcolourtype) {
				case 0:
					// grey
					pix = ((row[1 + j / cpb]) >> (cpb - 1 - (j % cpb)) * pngdepth) & mask;
					xb = pix * mult;	
					argb[4*pindex+0] = 255;
					argb[4*pindex+1] = xb;
					argb[4*pindex+2] = xb;
					argb[4*pindex+3] = xb;
					break;
				case 3:
					// indexed
					pix = ((row[1 + j / cpb]) >> (cpb - 1 - (j % cpb)) * pngdepth) & mask;
					argb[4*pindex+0] = bkgd && pix == bgpix ? 0 : 255;
					argb[4*pindex+1] = plte[8+3*pix];
					argb[4*pindex+2] = plte[8+3*pix+1];
					argb[4*pindex+3] = plte[8+3*pix+2];
					break;
				case 4:
					// greyalpha
					// assume pngdepth = 8
					pix = row[1 + 2 * j];
					xb = pix;
					alpha = row[1 + 2 * j + 1];
					argb[4*pindex+0] = alpha;
					argb[4*pindex+1] = xb;
					argb[4*pindex+2] = xb;
					argb[4*pindex+3] = xb;
					break;
				case 2:
					// RGB
					// assume pngdepth = 8
					argb[4*pindex+0] = 255;
					argb[4*pindex+1] = row[1+3*j];
					argb[4*pindex+2] = row[1+3*j+1];
					argb[4*pindex+3] = row[1+3*j+2];
					break;
				case 6:
					// RGBA
					// assume pngdepth = 8
					argb[4*pindex+0] = row[1+4*j+3];
					argb[4*pindex+1] = row[1+4*j];
					argb[4*pindex+2] = row[1+4*j+1];
					argb[4*pindex+3] = row[1+4*j+2];
					break;
				}
			}
			else {
				// 16 bpc
				switch (pngcolourtype) {
				case 0:
					// grey
					pix = Get16(row, 1+nb*j);
					xb = round(pix / div);	
					argb[4*pindex+0] = 255;
					argb[4*pindex+1] = xb;
					argb[4*pindex+2] = xb;
					argb[4*pindex+3] = xb;
					break;
				case 4:
					// greyalpha
					pix = Get16(row, 1+nb*2*j);
					xb = round(pix / div);
					alpha = Get16(row, 1+nb*(2*j+1));
					argb[4*pindex+0] = round(alpha / div);
					argb[4*pindex+1] = xb;
					argb[4*pindex+2] = xb;
					argb[4*pindex+3] = xb;
					break;
				case 2:
					// RGB
					argb[4*pindex+0] = 255;
					argb[4*pindex+1] = round(Get16(row, 1+nb*3*j) / div);
					argb[4*pindex+2] = round(Get16(row, 1+nb*(3*j+1)) / div);
					argb[4*pindex+3] = round(Get16(row, 1+nb*(3*j+2)) / div);
					break;
				case 6:
					// RGBA
					argb[4*pindex+0] = round(Get16(row, 1+nb*(4*j+3)) / div);
					argb[4*pindex+1] = round(Get16(row, 1+nb*4*j) / div);
					argb[4*pindex+2] = round(Get16(row, 1+nb*(4*j+1)) / div);
					argb[4*pindex+3] = round(Get16(row, 1+nb*(4*j+2)) / div);
					break;
				}
			}
		}
		row += rowbytes + 1;
	}
	return (rowbytes+1) * height;
}

/* simple expansion without colour profile / gamma conversion */
void * ExpandPNG(const void *png, long pngsize, long *outwid, long *outhei)
{
	char pngsig[8] = "\x89PNG\15\12\32\12";
	const uint8_t *pngp = png;
	const uint8_t *pngend = pngp + pngsize;
	uint32_t crc;
	long ihdroff = 8;
	// ihdr data
	const uint8_t *ihdr = pngp + 8;
	long pngwid, pnghei;
	unsigned char pngdepth, pngcolourtype, pngcompression, pngfilter, pnginterlace;
	const uint8_t *plte;
	const uint8_t *bkgd;
	const uint8_t *idat;
	long payloadsize;
	uint8_t *payload;
	uint8_t *stream;
	unsigned long streamsize;
	uint8_t *argb;
	
	if (memcmp(pngp, pngsig, 8) != 0) {
		fprintf(stderr, "ExpandPNG: not a png data\n");
		return NULL;
	}
	
	MakeCRCTable();
	
	if (memcmp(ihdr, "\0\0\0\15IHDR", 8) != 0) {
		fprintf(stderr, "ExpandPNG: can't find IHDR\n");
		return NULL;
	}
	crc = UpdateCRC(-1, ihdr + 4, 17);
	if (Get32(ihdr, 21) != ~ crc) {
		fprintf(stderr, "CRC mismatch on IHDR (expected %08X, found %08X)\n", (int)~ crc, (int)Get32(ihdr, 21));
	}
	
	CheckCRC(ihdr);
	
	pngwid = Get32(ihdr, 8);
	pnghei = Get32(ihdr, 12);
	pngdepth = ihdr[16];
	pngcolourtype = ihdr[17];
	pngcompression = ihdr[18];
	pngfilter = ihdr[19];
	pnginterlace = ihdr[20];
	
	switch (pngdepth) {
	case 1:
	case 2:
	case 4:
		if (pngcolourtype == 0 || pngcolourtype == 3)
			;
		else {
			fprintf(stderr, "unsupported colour depth/type (%d/%d)\n", pngdepth, pngcolourtype);
			return NULL;
		}
		break;
	case 16:
		if (pngcolourtype == 3) {
			fprintf(stderr, "unsupported colour depth/type (%d/%d)\n", pngdepth, pngcolourtype);
			return NULL;
		}
		break;
	case 8:
		break;
	default:
		fprintf(stderr, "unsupported colour depth (%d)\n", pngdepth);
		return NULL;
	}
	switch (pngcolourtype) {
	case 0:	// grey
	case 2:	// truecolour
	case 3:	// indexed
	case 4:	// greyalpha
	case 6:	// rgbalpha
		break;
	default:
		fprintf(stderr, "unsupported colour type (%d)\n", pngcolourtype);
		return NULL;
	}
	if (pngcompression != 0) {
		fprintf(stderr, "unsupported compression method (%d)\n", pngcompression);
		return NULL;
	}
	if (pngfilter != 0) {
		fprintf(stderr, "unsupported filter method (%d)\n", pngfilter);
		return NULL;
	}
	if (pnginterlace == 0 || pnginterlace == 1)
		;
	else {
		fprintf(stderr, "unsupported interlace method (%d)\n", pnginterlace);
		return NULL;
	}
	
	plte = FindChunk(ihdr, pngend, 'PLTE');
	if (plte)
		CheckCRC(plte);
	
	if (pngcolourtype == 3 && plte == NULL) {
		fprintf(stderr, "indexed colour png but palette is not found\n");
		return NULL;
	}
	
	bkgd = FindChunk(ihdr, pngend, 'bKGD');
	if (bkgd)
		CheckCRC(bkgd);
	
	// concatenate all IDAT
	idat = FindChunk(ihdr, pngend, 'IDAT');
	payloadsize = 0;
	payload = malloc(0);
	while (idat && Get32(idat, 4) == 'IDAT') {
		long size = Get32(idat, 0);
		uint8_t *p;
		CheckCRC(idat);
		p = realloc(payload, payloadsize + size);
		if (p) {
			payload = p;
		}
		else {	
			free(payload);
			fprintf(stderr, "ExpandPNG: no memory\n");
			return NULL;
		}
		memmove(payload + payloadsize, idat + 8, size);
		payloadsize += size;
		idat += 12 + size;
	}
	
	stream = InflateAllAtOnce(payload, payloadsize, (pngwid*PNGNComponents(pngcolourtype)*pngdepth + 7)/8 * pnghei, &streamsize);
	
	free(payload);
	
	if (stream == NULL) {
		return NULL;
	}
	
	argb = malloc(4 * pngwid * pnghei);
	
	if (pnginterlace == 1) {
		int pass;
		const int hshifts[7] = { 3, 3, 2, 2, 1, 1, 0 };
		const int vshifts[7] = { 3, 3, 3, 2, 2, 1, 1 };
		const int hstarts[7] = { 0, 4, 0, 2, 0, 1, 0 };
		const int vstarts[7] = { 0, 0, 4, 0, 2, 0, 1 };
		int hshift, vshift;
		int hstart, vstart;
		long subwid;
		long subhei;
		uint8_t *substream = stream;
		long subimglen;
		// adam7
		for (pass = 0; pass < 7; pass++) {
			hshift = hshifts[pass];
			vshift = vshifts[pass];
			hstart = hstarts[pass];
			vstart = vstarts[pass];
			subwid = (pngwid + (1 << hshift) - 1 - hstart) >> hshift;
			subhei = (pngwid + (1 << vshift) - 1 - vstart) >> vshift;
		
			subimglen = ToARGB(substream, subwid, subhei, pngdepth, pngcolourtype, plte, bkgd, &argb[0/*4*(vstart*pngwid+hstart)*/], pngwid, hstart, vstart, hshift, vshift);
			substream += subimglen;
		}
	}
	else if (pnginterlace == 0) {
		ToARGB(stream, pngwid, pnghei, pngdepth, pngcolourtype, plte, bkgd, argb, pngwid, 0, 0, 0, 0);
	}
	else {
		fprintf(stderr, "ExpandPNG: unknown interlace method %d\n", pnginterlace);
		free(stream);
		free(argb);
		return NULL;
	}
	
	free(stream);
	
	if (outwid)
		*outwid = pngwid;
	if (outhei)
		*outhei = pnghei;
	return argb;
}

#ifdef TEST

void Dump(const void *data, long len)
{
	long i;
	const uint8_t *p = data;
	while (len > 0) {
		long slen = 16;
		if (len < slen)
			slen = len;
		for (i = 0; i < slen; i++) {
			fprintf(stderr, "%02hhX ", p[i]);
		}
		for ( ; i < 16; i++) {
			fprintf(stderr, "   ");
		}
		for (i = 0; i < slen; i++) {
			fprintf(stderr, "%c", isprint(p[i]) ? p[i] : '.');
		}
		for ( ; i < 16; i++) {
			fprintf(stderr, " ");
		}
		fputs("\n", stderr);
		len -= slen;
		p += 16;
	}
}

void MakeRGBATIFF(const char *filename, void *data, int width, int height)
{
	if (data == NULL) {
		fprintf(stderr, "NULL tiff?\n");
	}
	else {
		FILE *fp = fopen(filename, "wb");
		long l = 4 * width * height;
		unsigned char tiffhdr[] = {
			'M', 'M', 0, 42, 
			0, 0, 0, 8,
			0, 13,
			// 10
			1, 0,	// ImageWidth
			0, 3, 0, 0, 0, 1, width >> 8, width, 0, 0,
			1, 1,	// ImageLength
			0, 3, 0, 0, 0, 1, height >> 8, height, 0, 0,
			1, 2,	// BitsPerSample
			0, 3, 0, 0, 0, 4, 0, 0, 0, 170,
			1, 3,	// Compression
			0, 3, 0, 0, 0, 1, 0, 1, 0, 0,
			1, 6,	// PhotometricInterpretation
			0, 3, 0, 0, 0, 1, 0, 2, 0, 0,
			1, 17,	// StripOffsets
			0, 4, 0, 0, 0, 1, 0, 0, 0, 194,
			1, 21,	// SamplesPerPixel
			0, 3, 0, 0, 0, 1, 0, 4, 0, 0,
			1, 22,	// RowsPerStrip
			0, 3, 0, 0, 0, 1, height >> 8, height, 0, 0,
			1, 23,	// StripByteCounts
			0, 4, 0, 0, 0, 1, l >> 24, l >> 16, l >> 8, l,
			1, 26,	// XResolution
			0, 5, 0, 0, 0, 1, 0, 0, 0, 178,
			1, 27,	// YResolution
			0, 5, 0, 0, 0, 1, 0, 0, 0, 186,
			1, 40,	// ResolutionUnit
			0, 3, 0, 0, 0, 1, 0, 2, 0, 0,
			1, 82,	// ExtraSamples
			0, 3, 0, 0, 0, 1, 0, 2, 0, 0,
			// 166
			0, 0, 0, 0,
			// 170
			0, 8, 0, 8, 0, 8, 0, 8,
			// 178
			0, 0, 0, 72, 0, 0, 0, 1,
			// 186
			0, 0, 0, 72, 0, 0, 0, 1,
			// 194
		};
		fwrite(tiffhdr, 1, sizeof(tiffhdr), fp);
		fwrite(data, 1, l, fp);
		fclose(fp);
	}
}

int main(int argc, char *argv[])
{
	if (argc >= 2) {
		FILE *fp = fopen(argv[1], "rb");
		setenv("CGBITMAP_CONTEXT_LOG_ERRORS", "1", 1);
		if (fp) {
			long sz;
			char *buf;
			char *buf2;
			char *buf3;
			char *mask;
			long size;
			long wid, hei;
			fseek(fp, 0, SEEK_END);
			sz = ftell(fp);
			buf = malloc(sz);
			rewind(fp);
			fread(buf, 1, sz, fp);
			Dump(buf, 16);
			buf2 = ExpandPNG(buf, sz, &wid, &hei);
			mask = malloc(wid * hei);
			if (buf2) {
				// ARGB->RGBA
				int i;
				for (i = 0; i < wid * hei; i++) {
					mask[i] = buf2[i*4+0];
					buf2[i*4+0] = buf2[i*4+1];
					buf2[i*4+1] = buf2[i*4+2];
					buf2[i*4+2] = buf2[i*4+3];
					buf2[i*4+3] = mask[i];
				}
			}
			else {
				fprintf(stderr, "ExpandPNG returned NULL\n");
				free(buf);
				free(mask);
				return 1;
			}
			MakeRGBATIFF("test.tiff", buf2, wid, hei);
			{
				// back to ARGB
				int i;
				for (i = 0; i < wid * hei; i++) {
					//mask[i] = buf2[i*4+3];
					buf2[i*4+3] = buf2[i*4+2];
					buf2[i*4+2] = buf2[i*4+1];
					buf2[i*4+1] = buf2[i*4+0];
				}
			}
			buf3 = CompressToPNG(wid, hei, buf2, mask, &size);
			fprintf(stderr, "%ld bytes PNG\n", size);
			{
				FILE *fp = fopen("test.png", "wb");
				if (fp)
					fwrite(buf3, 1, size, fp);
				fclose(fp);
			}
			free(buf);
			free(buf2);
			free(buf3);
			free(mask);
		}
		fclose(fp);
		return 0;
	}
	else {
		fputs("./a.out <pngfile.png> => test.tiff, test.png\n", stderr);
		fputs("png file?\n", stderr);
		return 1;
	}
}
#endif	// TEST
