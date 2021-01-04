CFLAGS = -g -Wno-shift-op-parentheses
LDFLAGS = -g

# ImageeIO: for 32/64-bit Mac OS X >= 10.4
#PNG_O = png_imageio.o
#LIBS = -framework Carbon

# QuickTime: for 32-bit Mac OS X (ppc/intel)
#PNG_O = png_qt.o
#LIBS = -framework Carbon -framework QuickTime

# zlib: the most generic one
PNG_O = png_zlib.o
LIBS = -lz


exe2icns: exeicon.o icnsbuilder.o $(PNG_O)
	$(CC) $(LDFLAGS) $^ $(LIBS) -o $@

# palette requires OS X Carbon
palette: palette.o
	$(CC) $(LDFLAGS) $^ -framework Carbon -o $@

clean:
	-rm *.o exe2icns

.c.o:
	$(CC) -c $(CFLAGS) $< -o $@
