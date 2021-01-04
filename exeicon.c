/*
	exe2icns [-f|-n] [-o output.icns] exefile.exe 
*/

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <math.h>
#include <ctype.h>
#include <unistd.h>
#include "icnsbuilder.h"
#include "png.h"

#define DO_GAMMA_CORRECTION	1

enum {
	kSuccess = 0,
	kInvalidFile = 101,
	kExeHasNoIcon = 102,
	kI
};

enum {
	kIconResourceType = 3,
	kIconGroupResourceType = 14,
};

enum {
	kLCIDJapanese = 1041,
};

typedef signed char bool;

struct Parameters_ {
	char *infilename;
	char *outfilename;
	bool synth128;
	bool forceoverwrite;
};
typedef struct Parameters_ Parameters;

// exe file field accessors
// we can assume mem to be aligned
static uint16_t Get16(const void *mem, long off)
{
	const uint8_t *p = mem;
	p += off;
	return p[0] + 256 * p[1];
}
static uint32_t Get32(const void *mem, long off)
{
	const uint8_t *p = mem;
	p += off;
	return p[0] + 256 * p[1] + 65536 * p[2] + 16777216 * p[3];
}

void * LoadFile(FILE *fp, long *outlenp)
{
	const long kChunkSize = 16384;
	char *buf = NULL;
	char *p;
	long datalen = 0;
	long bufsize = kChunkSize;
	long c;
	do {
		p = realloc(buf, bufsize + kChunkSize);
		if (p == NULL)
			break;
		buf = p;
		bufsize += kChunkSize;
		c = fread(&buf[datalen], 1, bufsize - datalen, fp);
		if (c <= 0)
			break;
		datalen += c;
	} while (1);
	if (outlenp)
		*outlenp = datalen;
	return buf;
}

static const char * TagName(uint32_t tag)
{
	static char s[5];
	s[0] = tag >> 24;
	s[1] = tag >> 16;
	s[2] = tag >> 8;
	s[3] = tag;
	s[4] = 0;
	return s;
}

/*
	https://docs.microsoft.com/en-us/windows/win32/debug/pe-format
	https://docs.microsoft.com/en-us/previous-versions/ms809762(v=msdn.10)#pe-file-resources
*/

/*
	Windows Resource Structure
	
	Root dir -> Type dir -> Name dir -> Language dir
	e.g.
	Root -> Icon Group (14) -> 101 -> Japanese (1041)
	
	Resource Directory Table:
	0/4	reserved
	4/4	timestamp
	8/2+2	major/minor version
	12/2	# of named entries (1)
	14/2	# of ID entries (2)
	[followed by (1) + (2) resource directory entries]
	
	Resource Directory Entry:
	0/4	name offset/ID
	4/4	data entry offset (high bit 0)/subdirectory offset (high bit 1)
	
	Resource Directory String:
	0/2	length
	2/x	unicode characters
	
	Resource Data Entry
	0/4	payload addr (addr when .rsrc is loaded into the virtual address)
	4/4	payload size
	8/4	codepage of resource
	12/4	reserved
*/

long ResourceFindIDEntry(const void *rsrcData, long diroffset, int rsrcid)
{
	const char *p = rsrcData;
	int namedcount, idcount;
	int i;
	const long kTableSize = 16;
	const long kEntrySize = 8;
	p += diroffset;
	namedcount = Get16(p, 12);
	idcount = Get16(p, 14);
	p += kTableSize + kEntrySize * namedcount;
	for (i = 0; i < idcount; i++) {
		uint32_t entryid = Get32(p, 0);
		if (entryid & 0x80000000)	// this is a named resource
			;
		else if (entryid == rsrcid) {
			// found
			return Get32(p, 4);
			break;
		}
		p += kEntrySize;
	}
	return 0;	// address 0 is regarded invalid
}


// index is 0-based
long ResourceFindIndEntry(const void *rsrcData, long diroffset, int index)
{
	const char *p = rsrcData;
	int namedcount, idcount;
	const long kTableSize = 16;
	const long kEntrySize = 8;
	p += diroffset;
	namedcount = Get16(p, 12);
	idcount = Get16(p, 14);
	if (index < namedcount + idcount) {
		return Get32(p, kTableSize + kEntrySize * index + 4);
	}
	return 0;	// address 0 is regarded invalid
}

/*
	Icon Group Resource
	
	https://devblogs.microsoft.com/oldnewthing/20120720-00/?p=7083
	
	GRPICONDIR:
	0/2	reserved
	2/2	type (1 = icon)
	4/2	# of icons (n)
	6/x	n * GRPICONDIRENTRY
	
	GRPICONDIRENTRY:
	0/1	width
	1/1	height
	2/1	# of colours (0 if 256 colours or more)
	3/1	reserved
	4/2	# of planes (must be 1)
	6/2	bit count
	8/4	data size
	12/2	id
*/

long FindIcon(const void *rsrcData, int id, int langcode, long *outaddr, long *outsize)
{
	const char *p = rsrcData;
	long icondir;
	long theicon;
	long icondata;
	long iconoff, iconsize;
	
	icondir = ResourceFindIDEntry(p, 0, kIconResourceType);
	if (icondir == 0) {
		// no icon
		return 0;
	}
	icondir &= 0x7FFFFFFF;	// assuming another directory
	
	theicon = ResourceFindIDEntry(p, icondir, id);
	if (theicon == 0) {
		// can't find icon
		return 0;
	}
	theicon &= 0x7FFFFFFF;		// assuming once again
	
	icondata = ResourceFindIDEntry(p, theicon, langcode);
	if (icondata == 0)
		icondata = ResourceFindIndEntry(p, theicon, 0);
	if (icondata == 0) {
		// can't find icon
		return 0;
	}
	
	iconoff = Get32(p, icondata + 0);
	iconsize = Get32(p, icondata + 4);
	
	if (outaddr)
		*outaddr = iconoff;
	if (outsize)
		*outsize = iconsize;
	
	return icondata;
}

long FindIndIconGroup(const void *rsrcData, int idx, int langcode, long *outaddr, long *outsize)
{
	const char *p = rsrcData;
	long icongroupdir;
	long thegroup;
	long groupdata;
	long groupoff, groupsize;
	
	icongroupdir = ResourceFindIDEntry(p, 0, kIconGroupResourceType);
	if (icongroupdir == 0) {
		// no icon
		return 0;
	}
	icongroupdir &= 0x7FFFFFFF;	// assuming another directory
	
	thegroup = ResourceFindIndEntry(p, icongroupdir, idx);
	if (thegroup == 0) {
		// can't find icon group
		return 0;
	}
	thegroup &= 0x7FFFFFFF;		// assuming once again
	
	groupdata = ResourceFindIDEntry(p, thegroup, langcode);
	if (groupdata == 0)
		groupdata = ResourceFindIndEntry(p, thegroup, 0);
	if (groupdata == 0) {
		// can't find icon group
		return 0;
	}
	
	groupoff = Get32(p, groupdata + 0);
	groupsize = Get32(p, groupdata + 4);
	
	if (outaddr)
		*outaddr = groupoff;
	if (outsize)
		*outsize = groupsize;
	
	return groupdata;
}

bool IsPNGTag(uint32_t tag)
{
	return tag == 'ic08'	// 256x256
		|| tag == 'ic07'	// 128x256
		;
}

double GammaCorrectedAverage(double ingamma, double outgamma, int n, ...)
{
	int i;
	va_list ap;
	double sum = 0;
	double outrgamma = 1.0 / outgamma;
	
	va_start(ap, n);
	for (i = 0; i < n; i++) {
		double v = va_arg(ap, double);
		sum += pow(v, ingamma);
	}
	sum /= n;
	va_end(ap);
	return pow(sum, outrgamma);
}

void * ExtractMainIconAsICNSFromResource(const void *rsrcData, long rsrclen, long virtualaddr, bool synth128, long *outicnssize)
{
	const uint8_t *p = rsrcData;
	int langcode = kLCIDJapanese;
	long groupdata;	// offset to icon group resource data entry
	long groupoff;	// offset to the actual payload
	long groupsize;	// size of the payload
	void *icnsdata = NULL;
	
	groupdata = FindIndIconGroup(rsrcData, 0, langcode, &groupoff, &groupsize);
	
	if (groupdata == 0) {
		return NULL;
	}
	
	groupoff -= virtualaddr;
	
	// icon group found
	// parse icon group resource
	{
		const uint8_t *q = p + groupoff;
		int count = Get16(q, 4);
		int i;
		bool done256 = 0, done128 = 0, done48 = 0, done32 = 0, done16 = 0, done12 = 0;
		uint8_t *rgb256 = malloc(256 * 256 * 4);
		uint8_t *mask256 = malloc(256 * 256);
		int bpp256 = 0;
		ICNSBuilder builder;
		
		ICNSBuilderInit(&builder);
		q += 6;
		for (i = 0; i < count; i++) {
			int id = Get16(q, 12);
			int width = q[0] == 0 ? 256 : (uint8_t)q[0];
			int height = q[1] == 0 ? 256 : (uint8_t)q[1];
			int bpp = Get16(q, 6);
			long iconoff;
			long iconsize;
			long icondata;
			uint32_t tag = 0;
			uint32_t masktag = 0;
			
			// the largest icon size we can get here is 256 x 256
			if (width == 256 && height == 256 && bpp >= 8) {
				if (! done256) {
					tag = 'ic08';
					done256 = 1;
				}
			}
			else if (width == 128 && height == 128 && bpp >= 8) {
				if (! done128) {
					tag = 'it32';
					masktag = 't8mk';
					done128 = 1;
				}
			}
			else if (width == 48 && height == 48 && bpp >= 8) {
				if (! done48) {
					tag = 'ih32';
					masktag = 'h8mk';
					done48 = 1;
				}
			}
			else if (width == 32 && height == 32 && bpp >= 8) {
				if (! done32) {
					tag = 'il32';
					masktag = 'l8mk';
					done32 = 1;
				}
			}
			else if (width == 16 && height == 16 && bpp >= 8) {
				if (! done16) {
					tag = 'is32';
					masktag = 's8mk';
					done16 = 1;
				}
			}
			//else if (width == 16 && height == 12) {
			//	tag = 'icm8';
			//}
			
			if (tag != 0) {
				fprintf(stderr, "processing icon: %d x %d, %d bit(s) > '%s'\n", width, height, bpp, TagName(tag));
				icondata = FindIcon(rsrcData, id, langcode, &iconoff, &iconsize);
				if (icondata) {
					bool ispng = 0;
					uint8_t *pngrgba = NULL;
					uint8_t *rgb = NULL;
					uint8_t *mask = NULL;
					uint8_t *png = NULL;
					long pngsize;
					iconoff -= virtualaddr;
					fprintf(stderr, "icon data at %08lX, length %08lX\n", iconoff, iconsize);
					// do extraction
					if (memcmp(p + iconoff, "\x89PNG", 4) == 0) {
						ispng = 1;
						if (IsPNGTag(tag)) {
							png = malloc(iconsize);
							memmove(png, p + iconoff, iconsize);
							pngsize = iconsize;
						}
						else {
							int i, j;
							long pngwid, pnghei;
							pngrgba = ExpandPNG(p + iconoff, iconsize, &pngwid, &pnghei);
							rgb = malloc(4 * width * height);
							mask = malloc(width * height);
							for (i = 0; i < height; i++) {
								for (j = 0; j < width; j++) {
									rgb[4*(i*width + j) + 0] = 0;
									rgb[4*(i*width+j)+1] = pngrgba[4*(i*width+j)+1];
									rgb[4*(i*width+j)+2] = pngrgba[4*(i*width+j)+2];
									rgb[4*(i*width+j)+3] = pngrgba[4*(i*width+j)+3];
									mask[i*width+j] = pngrgba[4*(i*width+j)+0];
								}
							}
						}
					}
					else {
						long infosize = Get32(p, iconoff + 0);
						width = Get32(p, iconoff + 4);
						height = Get32(p, iconoff + 8) / 2;	// icon dib height must be divided by 2
						bpp = Get16(p, iconoff + 14);
						// 
						if (bpp == 32 || bpp == 24) {
							int i, j;
							const uint8_t *dib = p + iconoff + infosize;
							rgb = malloc(4 * width * height);
							mask = malloc(1 * width * height);
							for (i = 0; i < height; i++) {
								for (j = 0; j < width; j++) {
									// DIB image holds components in BGRA order, bottom to top
									rgb[4*(i*width + j) + 0] = 0;
									rgb[4*(i*width + j) + 1] = dib[4*((height-i-1)*width + j) + 2];
									rgb[4*(i*width + j) + 2] = dib[4*((height-i-1)*width + j) + 1];
									rgb[4*(i*width + j) + 3] = dib[4*((height-i-1)*width + j) + 0];
									mask[i*width + j] = dib[4*((height-i-1)*width + j) + 3];
								}
							}
							if (infosize + 4 * width * height < iconsize) {
								// has mask data?
								fprintf(stderr, "this icon seems to have a mask (%ld bytes), which is unsupported by this program\n", iconsize - infosize - 4 * width * height);
							}
						}
						else if (bpp == 8) {
							int i, j;
							int dibrow = ((width + 3) / 4) * 4;		// align to 32-bit boundary
							int maskrow = ((width + 31) / 32) * 4;
							const uint8_t *palette = p + iconoff + infosize;
							const uint8_t *dib = palette + 256 * 4;
							const uint8_t *dibmask = dib + width * height;
							rgb = malloc(4 * width * height);
							mask = malloc(1 * width * height);
							for (i = 0; i < height; i++) {
								for (j = 0; j < width; j++) {
									uint8_t idx = dib[(height-i-1)*dibrow + j];
									uint8_t maskbit;
									rgb[4*(i*width + j) + 0] = 0;
									rgb[4*(i*width + j) + 1] = palette[4*idx + 2];
									rgb[4*(i*width + j) + 2] = palette[4*idx + 1];
									rgb[4*(i*width + j) + 3] = palette[4*idx + 0];
									maskbit = (dibmask[(height-i-1)*maskrow + j/8] >> 7-j%8) & 1;
									mask[i*width + j] = maskbit ? 0 : 255;
								}
							}
						}
						else if (bpp == 4) {
							int i, j;
							int dibrow = ((width + 7) / 8) * 4;		// align to 32-bit boundary
							int maskrow = ((width + 31) / 32) * 4;
							const uint8_t *palette = p + iconoff + infosize;
							const uint8_t *dib = palette + 16 * 4;
							const uint8_t *dibmask = dib + width * height;
							rgb = malloc(4 * width * height);
							mask = malloc(1 * width * height);
							for (i = 0; i < height; i++) {
								for (j = 0; j < width; j++) {
									uint8_t idx = (dib[(height-i-1)*dibrow + j/2] >> ((j & 1) ? 4 : 0)) & 15;
									uint8_t maskbit;
									rgb[4*(i*width + j) + 0] = 0;
									rgb[4*(i*width + j) + 1] = palette[4*idx + 2];
									rgb[4*(i*width + j) + 2] = palette[4*idx + 1];
									rgb[4*(i*width + j) + 3] = palette[4*idx + 0];
									maskbit = (dibmask[(height-i-1)*maskrow + j/8] >> 7-j%8) & 1;
									mask[i*width + j] = maskbit ? 0 : 255;
								}
							}
						}
						
					}
					if (IsPNGTag(tag)) {
						if (png)
							fprintf(stderr, "passing through the png data for %s\n", TagName(tag));
						if (png == NULL)
							png = CompressToPNG(width, height, rgb, mask, &pngsize);
						ICNSAddData(&builder, tag, png, pngsize);
					}
					else {
						uint8_t *compressed = malloc(4 * width * height * 2);
						long compsize = ICNSCompressImage(tag, rgb, 4 * width * height, compressed);
						//ICNSAddData(&builder, tag, rgb, 4 * width * height);
						ICNSAddData(&builder, tag, compressed, compsize);
						ICNSAddData(&builder, masktag, mask, width * height);
						free(compressed);
					}
					
					if (width == 256 && height == 256) {
						if (bpp > bpp256) {
							memmove(rgb256, rgb, 256 * 256 * 4);
							memmove(mask256, mask, 256 * 256);
						}
					}
					
					free(pngrgba);	// freeing NULL is ok
					free(png);
					free(rgb);
					free(mask);
				}
			}
			else {
				fprintf(stderr, "skipping icon: %d x %d, %d bit(s)\n", width, height, bpp);
			}
			q += 14;
		}
		
		if (done256 && ! done128 && synth128) {
			// synthesize osx-standard 128x128 pixel icon
			int i, j;
			uint8_t *rgb = malloc(128 * 128 * 4);
			uint8_t *mask = malloc(128 * 128);
			uint8_t *compressed = malloc(128 * 128 * 4 * 2);
			long compsize;
			fprintf(stderr, "synthesizing 128 x 128 icon [it32/t8mk]...\n");
			for (i = 0; i < 128; i++) {
				for (j = 0; j < 128; j++) {
					uint8_t r1 = rgb256[((i*2)*256+(j*2))*4 + 1];
					uint8_t g1 = rgb256[((i*2)*256+(j*2))*4 + 2];
					uint8_t b1 = rgb256[((i*2)*256+(j*2))*4 + 3];
					uint8_t r2 = rgb256[((i*2)*256+(j*2+1))*4 + 1];
					uint8_t g2 = rgb256[((i*2)*256+(j*2+1))*4 + 2];
					uint8_t b2 = rgb256[((i*2)*256+(j*2+1))*4 + 3];
					uint8_t r3 = rgb256[((i*2+1)*256+(j*2))*4 + 1];
					uint8_t g3 = rgb256[((i*2+1)*256+(j*2))*4 + 2];
					uint8_t b3 = rgb256[((i*2+1)*256+(j*2))*4 + 3];
					uint8_t r4 = rgb256[((i*2+1)*256+(j*2+1))*4 + 1];
					uint8_t g4 = rgb256[((i*2+1)*256+(j*2+1))*4 + 2];
					uint8_t b4 = rgb256[((i*2+1)*256+(j*2+1))*4 + 3];
					rgb[(i*128+j)*4] = 255;
#if DO_GAMMA_CORRECTION
					// outgamma should actually be 1.8, but other images aren't doing gamma correction
					rgb[(i*128+j)*4+1] = (255 * GammaCorrectedAverage(2.2, 2.2, 4, r1/255.0, r2/255.0, r3/255.0, r4/255.0) + 0.5);
					rgb[(i*128+j)*4+2] = (255 * GammaCorrectedAverage(2.2, 2.2, 4, g1/255.0, g2/255.0, g3/255.0, g4/255.0) + 0.5);
					rgb[(i*128+j)*4+3] = (255 * GammaCorrectedAverage(2.2, 2.2, 4, b1/255.0, b2/255.0, b3/255.0, b4/255.0) + 0.5);
#else
					rgb[(i*128+j)*4+1] = (r1 + r2 + r3 + r4 + 2) / 4;
					rgb[(i*128+j)*4+2] = (g1 + g2 + g3 + g4 + 2) / 4;
					rgb[(i*128+j)*4+3] = (b1 + b2 + b3 + b4 + 2) / 4;
#endif
				}
			}
			for (i = 0; i < 128; i++) {
				for (j = 0; j < 128; j++) {
					uint8_t m1 = mask256[(i*2)*256+(j*2)];
					uint8_t m2 = mask256[(i*2)*256+(j*2+1)];
					uint8_t m3 = mask256[(i*2+1)*256+(j*2)];
					uint8_t m4 = mask256[(i*2+1)*256+(j*2+1)];
					// no mask gamma
					mask[i*128+j] = (m1 + m2 + m3 + m4 + 2) / 4;
				}
			}
			compsize = ICNSCompressImage('it32', rgb, 4 * 128 * 128, compressed);
			ICNSAddData(&builder, 'it32', compressed, compsize);
			//ICNSAddData(&builder, 'it32', rgb, 128 * 128 * 4);
			ICNSAddData(&builder, 't8mk', mask, 128 * 128);
#if 0
			{
				FILE *fp = fopen("test.tiff", "wb");
				long l;
				unsigned char tiffhdr[] = {
					'M', 'M', 0, 42, 
					0, 0, 0, 8,
					0, 13,
					// 10
					1, 0,	// ImageWidth
					0, 3, 0, 0, 0, 1, 0, 128, 0, 0,
					1, 1,	// ImageLength
					0, 3, 0, 0, 0, 1, 0, 128, 0, 0,
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
					0, 3, 0, 0, 0, 1, 0, 128, 0, 0,
					1, 23,	// StripByteCounts
					0, 4, 0, 0, 0, 1, 0, 1, 0, 0,
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
				//if (fp) for (l = 0; l < 128 * 128; l++) fputc(rgb[l*4+1], fp);
				fwrite(tiffhdr, 1, sizeof(tiffhdr), fp);
				for (l = 0; l < 128 * 128; l++) {
					rgb[l*4] = rgb[l*4+1];
					rgb[l*4+1] = rgb[l*4+2];
					rgb[l*4+2] = rgb[l*4+3];
					rgb[l*4+3] = mask[l];
				}
				fwrite(rgb, 1, 128 * 128 * 4, fp);
				fclose(fp);
			}
#endif
			free(rgb);
			free(mask);
			free(compressed);
		}
		
		if (1) {
			uint8_t *p = ICNSBuilderGetDataPtr(&builder);
			long size = ICNSBuilderGetSize(&builder);
			if (size > 0) {
				icnsdata = malloc(size);
				memmove(icnsdata, p, size);
				if (outicnssize)
					*outicnssize = size;
			}
		}
		ICNSBuilderTerminate(&builder);
		free(rgb256);
		free(mask256);
	}
	return icnsdata;
}


int DoFile(FILE *ifp, FILE *ofp)
{
	long exesize;
	char *exe = LoadFile(ifp, &exesize);
	int result = 0;
	long pos;
	long peoff;
	int nsecs;
	int opthdrsize;
	int optmagic;
	long alignment;
	long sectableoff;
	int i;
	char *rsrcdata;
	long rsrcsize;
	long rawoff = 0;
	long rawsize = 0;
	long virtualaddr = 0;
	void *icnsdata = NULL;
	long icnssize;
	
	if (Get16(exe, 0) != 0x5A4D) {	// 'MZ'
		result = kInvalidFile;
		fprintf(stderr, "no MZ signature\n");
		goto FreeExit;
	}
	peoff = Get32(exe, 60);
	if (Get32(exe, peoff) != 0x00004550) {	// 'PE\0\0'
		result = kInvalidFile;
		fprintf(stderr, "no PE signature at %lX\n", peoff);
		goto FreeExit;
	}
	
	nsecs = Get16(exe, peoff + 4 + 2);
	opthdrsize = Get16(exe, peoff + 4 + 16);
	optmagic = Get16(exe, peoff + 4 + 20);
	if (optmagic == 0x20B) {
		// 64-bit optional header
		alignment = Get32(exe, peoff + 4 + 20 + 36);
	}
	else /*if (optmagic == 0x10B)*/ {
		// 32-bit optional header
		// actually there's not much difference
		alignment = Get32(exe, peoff + 4 + 20 + 36);
	}
	sectableoff = peoff + 4 + 20 + opthdrsize;
	
	// find .rsrc section
	for (i = 0; i < nsecs; i++) {
		const int sechdrsize = 40;
		long sechdroff = sectableoff + i * sechdrsize;
		const char *sechdr = exe + sechdroff;
		fprintf(stderr, "[%.8s section header at %08lX]\n", sechdr, sechdroff);
		if (strcmp(sechdr, ".rsrc") == 0) {
			// found
			virtualaddr = Get32(sechdr, 12);
			rawoff = Get32(sechdr, 20);
			rawsize = Get32(sechdr, 16);
			fprintf(stderr, "[.rsrc offset %08lX / size %08lX / virtualaddr %08lX]\n", rawoff, rawsize, virtualaddr);
			/*{
				FILE *fp = fopen("test.rsrc", "wb");
				if (fp)
					fwrite(exe + rawoff, 1, rawsize, fp);
				fclose(fp);
			}*/
			icnsdata = ExtractMainIconAsICNSFromResource(exe + rawoff, rawsize, virtualaddr, 1, &icnssize);
			break;
		}
	}
	
	if (icnsdata) {
		fwrite(icnsdata, 1, icnssize, ofp);
		free(icnsdata);
	}
	else {
		fprintf(stderr, "no icon data in executable\n");
		result = kExeHasNoIcon;
	}
	
FreeExit:
	free(exe);
	return result;
}

void Usage(FILE *fp)
{
	fputs("usage: exe2icns [-f|-n] [-o outicon.icns] exefile.exe\n", fp);
	fputs("usage: exe2icns -h\n", fp);
}

void Help(FILE *fp)
{
	Usage(fp);
	fputs("  -f              # force overwriting the output file\n", fp);
	fputs("  -h              # show this help\n", fp);
	fputs("  -n              # suppress auto-synthesis of 128 x 128 icon\n", fp);
	fputs("                  # from 256 x 256 icon\n", fp);
	fputs("  -o <icon.icns>  # specify the output file name (default: <exefile>.icns)\n", fp);
}

bool ParseArgs(int argc, char *argv[], Parameters *pp)
{
	// set default params
	pp->synth128 = 1;
	pp->forceoverwrite = 0;
	pp->infilename = NULL;
	pp->outfilename = NULL;
	// parse
	do {
		int op = getopt(argc, argv, "fhno:");
		if (op == -1)
			break;
		switch (op) {
		case 'f':
			pp->forceoverwrite = 1;
			break;
		case 'n':
			pp->synth128 = 0;
			break;
		case 'o':
			pp->outfilename = optarg;
			break;
		case 'h':
			Help(stdout);
			exit(0);
			//break;
		default:
			Usage(stderr);
			exit(1);
			//break;
		}
	} while (1);
	if (optind < argc) {
		pp->infilename = argv[optind];
		return 1;
	}
	else {
		fprintf(stderr, "no input file\n");
		Usage(stderr);
		return 0;
	}
}

int main(int argc, char *argv[])
{
	Parameters pr;
	
	if (ParseArgs(argc, argv, &pr)) {
		FILE *fp = fopen(pr.infilename, "rb");
		int r;
		if (fp) {
			char *icnsname = NULL;
			FILE *ofp;
			bool ov;
			if (pr.outfilename == NULL) {
				long l = strlen(pr.infilename);
				char *p = strrchr(pr.infilename, '.');
				icnsname = malloc(l + 5 + 1);
				if (p && strcasecmp(p, ".exe") == 0) {
					l = p - pr.infilename;
				}
				memmove(icnsname, pr.infilename, l);
				strcpy(icnsname + l, ".icns");
				pr.outfilename = icnsname;
			}
			// file exists?
			if (pr.forceoverwrite)
				ov = 1;
			else {
				ofp = fopen(pr.outfilename, "rb");
				if (ofp) {
					int ch;
					fprintf(stderr, "overwrite %s? [y/n]\n", pr.outfilename);
					ch = fgetc(stdin);
					ov = tolower(ch) == 'y';
				}
				fclose(ofp);
			}
			//
			if (ov) {
				ofp = fopen(pr.outfilename, "wb");
				if (ofp) {
					r = DoFile(fp, ofp);
				}
				else {
					fprintf(stderr, "can't open %s for writing\n", pr.outfilename);
					r = 1;
				}
				fclose(ofp);
			}
			fclose(fp);
			free(icnsname);
			return r;
		}
		else {
			fprintf(stderr, "can't open %s\n", pr.infilename);
			return 1;
		}
	}
	else {
		return 1;
	}
}

