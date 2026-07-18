#
# libbsdmalloc - header-only library.
#
# There is nothing to compile: the whole library is the <sys/malloc.h> wrapper
# under include/. Consumers simply add
#
#     -Ipath/to/libbsdmalloc/include
#
# ahead of their kernel-header search path so that <sys/malloc.h> resolves to
# the wrapper (which then #include_next's the real kernel header).
#

PREFIX ?=   /usr/local
INCDIR  =   $(PREFIX)/include

all:
	@echo "libbsdmalloc: header-only, nothing to build"

clean:
	@:

install:
	install -d $(INCDIR)/sys
	install -m 0644 include/sys/malloc.h $(INCDIR)/sys/malloc.h

.PHONY: all clean install
