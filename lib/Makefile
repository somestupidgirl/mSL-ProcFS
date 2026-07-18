#
# All-in-one Makefile
#

MAKE=make

all: debug

debug:
	$(MAKE) -C libbsdmalloc
	$(MAKE) -C libsbuf
	$(MAKE) -C libkext
	$(MAKE) -C libcpuft
	$(MAKE) -C libkern
	$(MAKE) -C libreadfile

release: TARGET=release
release: debug

clean:
	$(MAKE) -C libbsdmalloc clean
	$(MAKE) -C libsbuf clean
	$(MAKE) -C libkext clean
	$(MAKE) -C libcpuft clean
	$(MAKE) -C libkern clean
	$(MAKE) -C libreadfile clean

.PHONY: all debug release clean
