# Builds the ProcFS menu-bar app (ProcFS.app). The version comes from the repo
# VERSION file via ../Makefile.inc (KEXTVERSION).
include ../Makefile.inc

APP      := ProcFS
BUNDLE   := $(APP).app
CONTENTS := $(BUNDLE)/Contents
MACOS    := $(CONTENTS)/MacOS
BIN      := $(MACOS)/$(APP)
ICONSET  := AppIcon.iconset

PREFPANE := ProcFS.prefPane
PPCONT   := $(PREFPANE)/Contents

# Uninstaller app. It carries no removal logic: installer/uninstall.sh is
# bundled as a resource and does the work, so the app and the Terminal
# front-end cannot drift apart.
UNAPP    := Uninstall-ProcFS.app
UNCONT   := $(UNAPP)/Contents
UNSCRIPT := ../installer/uninstall.sh

ICNS     := AppIcon.icns

SWIFTFLAGS := -O -framework Cocoa
SIGNCERT   ?= -

all: $(BUNDLE) $(PREFPANE) $(UNAPP)

# Finder/Dock app icon, shared by both bundles (referenced by CFBundleIconFile):
# render the standard iconset sizes from appicon.png and pack them into an icns.
$(ICNS): appicon.png
	rm -rf $(ICONSET)
	mkdir -p $(ICONSET)
	@for spec in 16:16x16 32:16x16@2x 32:32x32 64:32x32@2x 128:128x128 \
	             256:128x128@2x 256:256x256 512:256x256@2x 512:512x512 1024:512x512@2x; do \
	    px=$${spec%%:*}; nm=$${spec#*:}; \
	    sips -z $$px $$px appicon.png --out $(ICONSET)/icon_$$nm.png >/dev/null; \
	done
	iconutil -c icns $(ICONSET) -o $@
	rm -rf $(ICONSET)

$(BUNDLE): ProcFSMenu.swift main.swift Updater.swift Info.plist.in ../VERSION icon_menu.png $(ICNS)
	mkdir -p $(MACOS) $(CONTENTS)/Resources
	swiftc $(SWIFTFLAGS) -o $(BIN) ProcFSMenu.swift main.swift Updater.swift
	cp icon_menu.png $(ICNS) $(CONTENTS)/Resources/
	sed -e 's/__KEXTVERSION__/$(KEXTVERSION)/g' Info.plist.in > $(CONTENTS)/Info.plist
	@# Strip Finder info / quarantine xattrs; codesign rejects "resource fork,
	@# Finder information, or similar detritus" (the icon PNGs carry them).
	xattr -cr $(BUNDLE)
	codesign -s "$(SIGNCERT)" --force --timestamp=none $(BUNDLE)

# System Settings preference pane (ProcFS.prefPane). Its principal binary is a
# loadable Mach-O bundle (-emit-library + -bundle) loaded by System Settings.
$(PREFPANE): ProcFSPref.swift Updater.swift prefpane-Info.plist.in ../VERSION appicon.png
	mkdir -p $(PPCONT)/MacOS $(PPCONT)/Resources
	swiftc -O -emit-library -Xlinker -bundle \
	    -framework AppKit -framework PreferencePanes -framework ServiceManagement \
	    -module-name ProcFSPref \
	    -o $(PPCONT)/MacOS/$(APP) ProcFSPref.swift Updater.swift
	cp appicon.png $(PPCONT)/Resources/
	sed -e 's/__KEXTVERSION__/$(KEXTVERSION)/g' prefpane-Info.plist.in > $(PPCONT)/Info.plist
	xattr -cr $(PREFPANE)
	codesign -s "$(SIGNCERT)" --force --timestamp=none $(PREFPANE)

$(UNAPP): Uninstaller.swift uninstaller-Info.plist.in ../VERSION $(UNSCRIPT) $(ICNS)
	mkdir -p $(UNCONT)/MacOS $(UNCONT)/Resources
	@# -parse-as-library: a single-file module is otherwise treated as a script,
	@# and @main cannot coexist with top-level code.
	swiftc $(SWIFTFLAGS) -parse-as-library \
	    -o $(UNCONT)/MacOS/Uninstall-ProcFS Uninstaller.swift
	cp $(UNSCRIPT) $(ICNS) $(UNCONT)/Resources/
	chmod +x $(UNCONT)/Resources/uninstall.sh
	sed -e 's/__KEXTVERSION__/$(KEXTVERSION)/g' uninstaller-Info.plist.in > $(UNCONT)/Info.plist
	xattr -cr $(UNAPP)
	codesign -s "$(SIGNCERT)" --force --timestamp=none $(UNAPP)

clean:
	rm -rf $(BUNDLE) $(PREFPANE) $(UNAPP) $(ICONSET) $(ICNS)

.PHONY: all clean
