#include <Carbon/Carbon.h>
#include <QuickTime/QuickTime.h>
#include "png.h"

#pragma GCC diagnostic ignored "-Wdeprecated-declarations"

#ifndef __LP64__

#if MAC_OS_X_VERSION_MAX_ALLOWED >= 1080
// QuickDraw declarations
extern pascal void SetRect(Rect *r, short left, short top, short right, short bottom);
extern pascal OSErr NewGWorld(GWorldPtr *newgw, short depth, Rect *bounds, CTabHandle ctab, GDHandle gdevice, GWorldFlags flags);
extern pascal OSErr NewGWorldFromPtr(GWorldPtr *newgw, UInt32 format, Rect *bounds, CTabHandle ctab, GDHandle gdevice, GWorldFlags flags, Ptr buffer, SInt32 bytesPerRow);
extern pascal void DisposeGWorld(GWorldPtr gw);
extern pascal PixMapHandle GetGWorldPixMap(GWorldPtr gw);
extern pascal Ptr GetPixBaseAddr(PixMapHandle pm);
#endif


void * CompressToPNG(int width, int height, const void *rgb, const void *mask, long *outsize)
{
	OSErr err;
	ComponentResult cr;
	ComponentInstance ci;
	Handle h;
	Rect r;
	UInt8 *buf = nil;
	GWorldPtr gw;
	unsigned long size;
	int i;
	
	err = OpenADefaultComponent(GraphicsExporterComponentType, kQTFileTypePNG, &ci);
	if (err != noErr) {
		fprintf(stderr, "can't load QuickTime PNG exporter (%d)\n", err);
		return nil;
	}
	
	if (mask) {
		for (i = 0; i < width * height; i++)
			((char *)rgb)[i*4] = ((char *)mask)[i];	// ignoring const; should allocate new memory...
	}
	else {
		// alpha?
	}
	
	SetRect(&r, 0, 0, width, height);
	err = NewGWorldFromPtr(&gw, k32ARGBPixelFormat, &r, nil, nil, 0, (Ptr)rgb, 4 * width);
	if (err == noErr) {
		h = NewHandle(0);
		cr = GraphicsExportSetInputGWorld(ci, gw);
		if (cr == noErr)
			cr = GraphicsExportSetOutputHandle(ci, h);
		if (cr == noErr)
			cr = GraphicsExportDoExport(ci, &size);
		if (cr == noErr) {
			//size = GetHandleSize(h);
			buf = malloc(size);
			BlockMoveData(*h, buf, size);
			if (outsize)
				*outsize = size;
		}
		else {
			fprintf(stderr, "export to PNG failed (%d)\n", (int)cr);
		}
	
		DisposeHandle(h);
		DisposeGWorld(gw);
	}
	else {
		fprintf(stderr, "NewGWorldFromPtr %d\n", err);
	}
	CloseComponent(ci);
	return buf;
}

void * ExpandPNG(const void *png, long pngsize, long *outwid, long *outhei)
{
	OSErr err;
	ComponentResult cr;
	ComponentInstance ci;
	Handle h;
	Rect r;
	int wid, hei;
	UInt8 *buf;
	GWorldPtr gw;
	int i;
	Boolean rotate = false;
	
	err = OpenADefaultComponent(GraphicsImporterComponentType, kQTFileTypePNG, &ci);
	if (err != noErr) {
		fprintf(stderr, "can't load QuickTime PNG importer (%d)\n", err);
		return nil;
	}
	
	h = NewHandle(0);
	err = PtrToXHand(png, h, pngsize);
	err = GraphicsImportSetDataHandle(ci, h);
	
	GraphicsImportGetNaturalBounds(ci, &r);
	//fprintf(stderr, "{%d %d %d %d}\n", r.top, r.left, r.bottom, r.right);
	
	//cr = GraphicsImportCreateCGImage(ci, &cgimg, kGraphicsImportCreateCGImageUsingCurrentSettings);	// needs 10.3 and QT 6.4
	wid = r.right - r.left;
	hei = r.bottom - r.top;
	buf = malloc(4 * wid * hei);
	err = QTNewGWorldFromPtr(&gw, k32ARGBPixelFormat, &r, nil, nil, 0, (Ptr)buf, 4 * wid);
	if (err == noErr) {
		cr = GraphicsImportSetGWorld(ci, gw, nil);
		if (cr == noErr)
			cr = GraphicsImportDraw(ci);
		if (cr == noErr) {
		}
		else {
			fprintf(stderr, "GraphicsImportSetGWorld/Draw %d\n", (int)cr);
			free(buf);
			buf = nil;
		}
	
		DisposeGWorld(gw);
	}
	else {
		free(buf);
		fprintf(stderr, "NewGWorldFromPtr %d\n", err);
		buf = nil;
	}
	
	if (outwid)
		*outwid = wid;
	if (outhei)
		*outhei = hei;
	
	if (rotate) {
		// ARGB -> RGBA
		for (i = 0; i < wid * hei; i++) {
			UInt8 a = buf[4*i];
			buf[4*i] = buf[4*i+1];
			buf[4*i+1] = buf[4*i+2];
			buf[4*i+2] = buf[4*i+3];
			buf[4*i+3] = a;
		}
	}
	
	DisposeHandle(h);
	CloseComponent(ci);
	return buf;
}

#ifdef TEST

void Dump(const void *data, long len)
{
	long i;
	const UInt8 *p = data;
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

#endif	// __LP64__
