#
# All-in-one Makefile
#

MAKE=make

all: debug

debug:
	$(MAKE) -C libkext
	$(MAKE) -C libklookup
	$(MAKE) -C libkprocfs
	$(MAKE) -C libreadfile

release: TARGET=release
release: debug

clean:
	$(MAKE) -C libkext clean
	$(MAKE) -C libklookup clean
	$(MAKE) -C libkprocfs clean
	$(MAKE) -C libreadfile clean

.PHONY: all debug release clean
