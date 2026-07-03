# Builds the ProcFS menu-bar app (ProcFS.app). The version comes from the repo
# VERSION file via ../Makefile.inc (KEXTVERSION).
include ../Makefile.inc

APP      := ProcFS
BUNDLE   := $(APP).app
CONTENTS := $(BUNDLE)/Contents
MACOS    := $(CONTENTS)/MacOS
BIN      := $(MACOS)/$(APP)

SWIFTFLAGS := -O -framework Cocoa
SIGNCERT   ?= -

all: $(BUNDLE)

$(BUNDLE): ProcFSMenu.swift Info.plist.in ../VERSION icon_light.png icon_dark.png
	mkdir -p $(MACOS) $(CONTENTS)/Resources
	swiftc $(SWIFTFLAGS) -o $(BIN) ProcFSMenu.swift
	cp icon_light.png icon_dark.png $(CONTENTS)/Resources/
	sed -e 's/__KEXTVERSION__/$(KEXTVERSION)/g' Info.plist.in > $(CONTENTS)/Info.plist
	@# Strip Finder info / quarantine xattrs; codesign rejects "resource fork,
	@# Finder information, or similar detritus" (the icon PNGs carry them).
	xattr -cr $(BUNDLE)
	codesign -s "$(SIGNCERT)" --force --timestamp=none $(BUNDLE)

clean:
	rm -rf $(BUNDLE)

.PHONY: all clean
