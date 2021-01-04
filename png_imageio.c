#include <ImageIO/ImageIO.h>
#include <CoreServices/CoreServices.h>
#include "png.h"

void * CompressToPNG(int width, int height, const void *rgb, const void *mask, long *outsize)
{
	CFMutableDataRef data = CFDataCreateMutable(kCFAllocatorDefault, 0);
	CGImageDestinationRef dest = CGImageDestinationCreateWithData(data, kUTTypePNG, 1, NULL);
	CGColorSpaceRef space = CGColorSpaceCreateDeviceRGB();
	CGDataProviderRef provider = CGDataProviderCreateWithData(NULL, rgb, 4 * width * height, NULL);
	const CGFloat decode[] = { 0, 1, 0, 1, 0, 1 };
	CGImageRef image;
	if (mask) {
		for (int i = 0; i < width * height; i++)
			((char *)rgb)[i*4] = ((char *)mask)[i];	// ignoring const; should allocate new memory...
		image = CGImageCreate(width, height, 8, 32, 4 * width, space, kCGImageAlphaFirst, provider, decode, true, kCGRenderingIntentDefault);
	}
	else {
		image = CGImageCreate(width, height, 8, 32, 4 * width, space, kCGImageAlphaNoneSkipFirst, provider, decode, true, kCGRenderingIntentDefault);
	}
	
	CGImageDestinationAddImage(dest, image, NULL);
	CGImageDestinationFinalize(dest);
	
	long size = CFDataGetLength(data);
	void *buf = malloc(size);
	CFDataGetBytes(data, CFRangeMake(0, size), buf);
	*outsize = size;
	
	CGDataProviderRelease(provider);
	CGColorSpaceRelease(space);
	CGImageRelease(image);
	CFRelease(dest);
	CFRelease(data);
	
	return buf;
}


void * ExpandPNG(const void *png, long pngsize, long *outwid, long *outhei)
{
	CGDataProviderRef provider = CGDataProviderCreateWithData(NULL, png, pngsize, NULL);
	//CGFloat decode[] = { 0, 1, 0, 1, 0, 1, 0, 1 };
	CGImageRef image = CGImageCreateWithPNGDataProvider(provider, NULL, true, kCGRenderingIntentDefault);
	int width = CGImageGetWidth(image);
	int height = CGImageGetHeight(image);
	char *buf = malloc(4 * width * height);
	char *buf2 = malloc(4 * width * height);
	// CGContext performs colour space conversion if png has specified some other colours...
	CGColorSpaceRef space = CGColorSpaceCreateDeviceRGB();
	//CGColorSpaceRef space = CGColorSpaceCreateWithName(kCGColorSpaceGenericRGB);	// 10.2 - 10.4
	//CGColorSpaceRef space = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);	// 10.5 -
	CGContextRef ctx;
	long i;
	
	{
		CGImageRef newimage = CGImageCreateCopyWithColorSpace(image, space);
		if (newimage) {
			CGImageRelease(image);
			image = newimage;
		}
	}
	
	// get alpha channel
	ctx = CGBitmapContextCreate(buf2, width, height, 8, 4 * width, space, kCGImageAlphaPremultipliedFirst);
	if (ctx == NULL) {
		fprintf(stderr, "can't create bitmap context!\n");
		free(buf);
		buf = NULL;
	}
	CGContextDrawImage(ctx, CGRectMake(0, 0, width, height), image);
	CGContextFlush(ctx);
	CGContextRelease(ctx);
	
	// get non-premultiplied pixels
	ctx = CGBitmapContextCreate(buf, width, height, 8, 4 * width, space, kCGImageAlphaNoneSkipFirst);
	if (ctx == NULL) {
		fprintf(stderr, "can't create bitmap context!\n");
		free(buf);
		buf = NULL;
	}
	CGContextDrawImage(ctx, CGRectMake(0, 0, width, height), image);
	CGContextFlush(ctx);
	CGContextRelease(ctx);
	
	// compose
	for (i = 0; i < width * height; i++) {
		buf[i*4+0] = buf2[i*4+0];
	}
	
	if (outwid)
		*outwid = width;
	if (outhei)
		*outhei = height;
	
	free(buf2);
	CGColorSpaceRelease(space);
	CGImageRelease(image);
	CGDataProviderRelease(provider);
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
#endif
