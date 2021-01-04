/*
	palette.c - dump a Mac OS 'clut' resource
*/
#ifdef __MACH__
#include <Carbon/Carbon.h>
#else
#include <Quickdraw.h>
#include <Resources.h>
#endif
#include <stdio.h>
#include <stdlib.h>

int main(int argc, char *argv[])
{
	CTabHandle h;
	long size;
	long i;
	short clutid = 0;

	if (argc > 1)
		clutid = atoi(argv[1]);

#ifndef __LP64__
	if (clutid == 0)
		clutid = 1;
	h = GetCTable(clutid);
#else
	if (clutid == 0) {
		long n = CountResources('clut');
		SetResLoad(false);
		printf("%ld cluts found\n", n);
		for (i = 1; i <= n; i++) {
			h = (Handle)GetIndResource('clut', i);
			if (h) {
				short resid;
				ResType restype;
				Str255 name;
				GetResInfo((Handle)h, &resid, &restype, name);
				printf("%d\n", resid);
			}
		}
	}
	h = (CTabHandle)GetResource('clut', clutid);
#endif
	if (h) {
		printf("id %d\n", clutid);
		size = (**h).ctSize;
		if (size >= 256) {
			printf("ctSize seems too large: %ld\n", size);
			size = 0;
		}
		for (i = 0; i <= size; i++) {
			RGBColor rgb = (**h).ctTable[i].rgb;
			printf("%3ld %04hX %04hX %04hX\n", i, rgb.red, rgb.green, rgb.blue);
		}
	}
	return 0;
}
