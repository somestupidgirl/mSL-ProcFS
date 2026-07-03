include ../Makefile.inc

SDKROOT?=       $(shell xcrun --sdk macosx --show-sdk-path)

BUNDLEID?=           $(BUNDLEDOMAIN).$(FSNAME)
FSBUNDLE?=           $(FSNAME).fs
FSMACHO?=            $(FSNAME).out
ARCHFLAGS?=          -arch arm64
TARGET_TRIPLE?=      arm64-apple-macos12.0
PREFIX?=             /Library/Filesystems
DEVELOPER_DIR?=      /Applications/Xcode.app/Contents/Developer
MACOSX_VERSION_MIN?= 12.0

CC=       $(shell xcrun -find -sdk $(SDKROOT) cc)
CODESIGN= $(shell xcrun -find -sdk $(SDKROOT) codesign)

CFLAGS+= -target $(TARGET_TRIPLE) \
         -isysroot $(SDKROOT) \
         $(ARCHFLAGS) \
         -x c \
         -std=c99 \
         -bundle \
         -I../lib \
         -I../include \
         -idirafter ../xnu/bsd

CFLAGS+= -Wall -Wextra -Wno-vla -Wno-nullability-completeness

LDFLAGS+= -target $(TARGET_TRIPLE) \
          -isysroot $(SDKROOT) \
          $(ARCHFLAGS)

LIBS+= -lutil

FSLFLAGS+= -xml -c -undef-symbols -all-symbols -system-extensions

SRCS:= $(wildcard *.c)
OBJS:= $(SRCS:.c=.o)

all: debug

%.o: %.c $(HDRS)
	$(CC) $(CFLAGS) -c -o $@ $<

$(FSMACHO): $(OBJS)
	$(CC) $(LDFLAGS) $(LIBS) -o $@ $^
	otool -h $@
	otool -l $@ | grep uuid

Info.plist~: Info.plist.in
	sed \
		-e 's/__FSNAME__/$(FSNAME)/g' \
		-e 's/__FSMACHO__/$(FSNAME)/g' \
		-e 's/__FSVERSION__/$(FSVERSION)/g' \
		-e 's/__FSBUILD__/$(FSBUILD)/g' \
		-e 's/__BUNDLEID__/$(BUNDLEID)/g' \
		-e 's/__OSBUILD__/$(shell /usr/bin/sw_vers -buildVersion)/g' \
		-e 's/__CLANGVER__/$(shell $(CC) -v 2>&1 | grep version)/g' \
	$^ > $@

$(FSBUNDLE): $(FSMACHO) Info.plist~
	mkdir -p $@/Contents/Resources
	mv $< $@/Contents/Resources/mount_$(FSNAME)
	cp Info.plist~ $@/Contents/Info.plist
ifdef COMPATIBLE_VERSION
	/usr/libexec/PlistBuddy -c 'Add :OSBundleCompatibleVersion string "$(COMPATIBLE_VERSION)"' $@/Contents/Info.plist
endif
ifdef COPYRIGHT
	/usr/libexec/PlistBuddy -c 'Add :NSHumanReadableCopyright string "$(COPYRIGHT)"' $@/Contents/Info.plist
endif
ifdef SIGNCERT
	$(CODESIGN) --force --timestamp=none --sign $(SIGNCERT) $@
	/usr/libexec/PlistBuddy -c 'Add :CFBundleSignature string ????' $@/Contents/Info.plist
endif
ifdef MACOSX_VERSION_MIN
	/usr/libexec/PlistBuddy -c 'Add :LSMinimumSystemVersion string "$(MACOSX_VERSION_MIN)"' $@/Contents/Info.plist
endif
	/usr/libexec/PlistBuddy -c 'Print OSBundleLibraries' $@/Contents/Info.plist &> /dev/null || \
		/usr/libexec/PlistBuddy -c 'Add :OSBundleLibraries:com.apple.kpi.bsd string "8.0b1"' $@/Contents/Info.plist
	touch $@
	dsymutil $(ARCHFLAGS) -o $(FSNAME).fs.dSYM $@/Contents/Resources/mount_$(FSNAME)

debug: CFLAGS += -O0 -g
debug: $(FSBUNDLE)

release: CFLAGS += -O2
release: $(FSBUNDLE)

install: $(FSBUNDLE) uninstall
	test -d "$(PREFIX)"
	sudo cp -r $< "$(PREFIX)/$<"
	sudo chmod -R 755 "$(PREFIX)/$<"
	sudo chown -R root:wheel "$(PREFIX)/$<"

uninstall:
	test -d "$(PREFIX)"
	test -e "$(PREFIX)/$(FSBUNDLE)" && \
	sudo rm -rf "$(PREFIX)/$(FSBUNDLE)" || true

clean:
	rm -rf $(FSBUNDLE) $(FSBUNDLE).dSYM Info.plist~ $(OBJS) $(FSMACHO)

.PHONY: all debug release clean
