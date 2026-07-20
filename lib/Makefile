#
# All-in-one Makefile
#
# libreadfile is intentionally not built: nothing in procfs uses read_file()
# yet. The submodule is kept in lib/ for future use; add it back here (and
# link -lreadfile with -I../lib/libreadfile/include in kext/Makefile) when a
# consumer appears.
#

MAKE=make

all: debug

debug:
	$(MAKE) -C libbsdmalloc
	$(MAKE) -C libsbuf
	$(MAKE) -C libkext
	$(MAKE) -C libcpuft

release: TARGET=release
release: debug

clean:
	$(MAKE) -C libbsdmalloc clean
	$(MAKE) -C libsbuf clean
	$(MAKE) -C libkext clean
	$(MAKE) -C libcpuft clean

.PHONY: all debug release clean
