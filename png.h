#ifndef PNG_H
#define PNG_H 1

// rgb = 32-bit RGB (skipping the 1st byte), mask = 8-bit alpha channel
// free() the returned pointer by yourself
void * CompressToPNG(int width, int height, const void *rgb, const void *mask, long *outsize);

// png -> 32-bit ARGB
// free() the returned pointer by yourself
void * ExpandPNG(const void *png, long pngsize, long *outwid, long *outhei);

#endif
