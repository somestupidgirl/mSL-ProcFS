#
# All-in-one Makefile
#
# Usage:
#   make                    # build kext + fs + tools + plists into $(OUT)
#   make ARCH=arm64e        # Apple Silicon kext + arm64 fs
#   make ARCH=x86_64        # Intel kext + x86_64 fs
#   make ARCH=universal     # fat kext + fs (arm64e + x86_64)
#   make tests              # also build the test programs
#   make check              # build tests + run tests/test_features.sh on /proc
#   make distcheck          # clean build of the .pkg/.dmg + verify the payload
#   sudo make install       # install everything into the system (run AFTER make)
#   sudo make uninstall     # remove everything from the system
#   make clean              # remove build artifacts (no sudo needed)
#
# Typical one-shot flow:
#   ./install.sh            # = make clean && make && sudo make install
#
# NOTE: never run the build as root. `make install` only COPIES the already-built
# artifacts from $(OUT) into place; it does not compile. This keeps every build
# artifact owned by the invoking user, so `make clean` never needs sudo.
#

MAKE=make
OUT=out

# Install locations.
EXT_DIR        := /Library/Extensions
FS_DIR         := /Library/Filesystems
SBIN_DIR       := /usr/local/sbin
DAEMON_DIR     := /Library/LaunchDaemons
APP_DIR        := /Applications
# ProcFS is one module of mSL/XNU, so its app is grouped under an mSL folder
# rather than sitting loose in /Applications alongside unrelated apps.
MSL_APP_DIR    := $(APP_DIR)/mSL
PREFPANE_DIR   := /Library/PreferencePanes
SYNTHETIC_CONF := /etc/synthetic.conf

# Identifiers / runtime files.
BUNDLE_ID      := com.beako.filesystems.procfs
DAEMON_PLIST   := com.beako.procfsd.plist
DAEMON_LABEL   := com.beako.procfsd
ARM_FLAG       := /var/db/procfs.enabled
KSYMS_FILE     := /var/db/procfs.ksyms
LINUX_CONF     := /var/db/procfs.linux
LINUX_VER_CONF := /var/db/procfs.linux_version

# Version (single source of truth: the repo VERSION file), used for the
# installer package / disk image names and metadata.
VERSION        := $(strip $(shell cat VERSION 2>/dev/null || echo 0.0.0))
PKG_ID         := com.beako.filesystems.procfs.pkg
PKG_COMP       := $(OUT)/procfs-component.pkg
PKG_OUT        := $(OUT)/ProcFS-$(VERSION).pkg
DMG_OUT        := $(OUT)/ProcFS-$(VERSION).dmg

# Detect native arch if ARCH not specified
NATIVE_ARCH := $(shell uname -m)
ifeq ($(NATIVE_ARCH),arm64)
    DEFAULT_ARCH := arm64e
else
    DEFAULT_ARCH := x86_64
endif
ARCH ?= $(DEFAULT_ARCH)
# Accept arm64 as alias for arm64e (kexts require arm64e ABI)
ifeq ($(ARCH),arm64)
    override ARCH := arm64e
endif

# Per-arch settings
ifeq ($(ARCH),arm64e)
    KEXT_ARCHFLAGS    := -arch arm64e
    KEXT_TRIPLE       := arm64e-apple-macos12.0
    FS_ARCHFLAGS      := -arch arm64
    FS_TRIPLE         := arm64-apple-macos12.0
    LIB_ARCHFLAGS     := -arch arm64e
    LIB_TRIPLE        := arm64e-apple-macos12.0
else ifeq ($(ARCH),x86_64)
    KEXT_ARCHFLAGS    := -arch x86_64
    KEXT_TRIPLE       := x86_64-apple-macos10.15
    FS_ARCHFLAGS      := -arch x86_64
    FS_TRIPLE         := x86_64-apple-macos10.15
    LIB_ARCHFLAGS     := -arch x86_64
    LIB_TRIPLE        := x86_64-apple-macos10.15
else ifeq ($(ARCH),universal)
    KEXT_ARCHFLAGS    := -arch arm64e
    KEXT_TRIPLE       := arm64e-apple-macos12.0
    FS_ARCHFLAGS      := -arch arm64
    FS_TRIPLE         := arm64-apple-macos12.0
    LIB_ARCHFLAGS     := -arch arm64e
    LIB_TRIPLE        := arm64e-apple-macos12.0
else
    $(error Unknown ARCH=$(ARCH). Use arm64e, x86_64, or universal)
endif

KEXT_FLAGS := ARCHFLAGS="$(KEXT_ARCHFLAGS)" TARGET_TRIPLE="$(KEXT_TRIPLE)"
FS_FLAGS   := ARCHFLAGS="$(FS_ARCHFLAGS)"   TARGET_TRIPLE="$(FS_TRIPLE)"
LIB_FLAGS  := ARCHFLAGS="$(LIB_ARCHFLAGS)"  TARGET_TRIPLE="$(LIB_TRIPLE)"

# The build wipes and repopulates $(OUT) in a fixed order; never parallelise it.
.NOTPARALLEL:

# ---------------------------------------------------------------------------
# Build  ->  $(OUT)
# ---------------------------------------------------------------------------

# Default: everything needed to install (kext, fs, tools, plists, GUI). Not tests.
all: clean kextfs tools plists gui pkg dmg

ifeq ($(ARCH),universal)

# kext + fs as fat (arm64e + x86_64) binaries.
kextfs:
	rm -rf $(OUT)
	mkdir $(OUT)
	$(MAKE) -C lib  ARCHFLAGS="-arch arm64e" TARGET_TRIPLE="arm64e-apple-macos12.0"
	$(MAKE) debug -C kext ARCHFLAGS="-arch arm64e" TARGET_TRIPLE="arm64e-apple-macos12.0"
	$(MAKE) debug -C fs   ARCHFLAGS="-arch arm64"  TARGET_TRIPLE="arm64-apple-macos12.0"
	mv kext/procfs.kext kext/procfs.kext.dSYM fs/procfs.fs fs/procfs.fs.dSYM $(OUT)
	mv $(OUT)/procfs.kext $(OUT)/procfs.kext.arm64e
	mv $(OUT)/procfs.fs   $(OUT)/procfs.fs.arm64
	$(MAKE) -C kext clean
	$(MAKE) -C fs clean
	$(MAKE) -C lib all clean
	$(MAKE) -C lib  ARCHFLAGS="-arch x86_64" TARGET_TRIPLE="x86_64-apple-macos10.15"
	$(MAKE) debug -C kext ARCHFLAGS="-arch x86_64" TARGET_TRIPLE="x86_64-apple-macos10.15"
	$(MAKE) debug -C fs   ARCHFLAGS="-arch x86_64" TARGET_TRIPLE="x86_64-apple-macos10.15"
	rm -rf $(OUT)/procfs.kext.dSYM $(OUT)/procfs.fs.dSYM
	mv kext/procfs.kext kext/procfs.kext.dSYM fs/procfs.fs fs/procfs.fs.dSYM $(OUT)
	mv $(OUT)/procfs.kext $(OUT)/procfs.kext.x86_64
	mv $(OUT)/procfs.fs   $(OUT)/procfs.fs.x86_64
	cp -r $(OUT)/procfs.kext.arm64e $(OUT)/procfs.kext
	lipo -create $(OUT)/procfs.kext.arm64e/Contents/MacOS/procfs $(OUT)/procfs.kext.x86_64/Contents/MacOS/procfs -output $(OUT)/procfs.kext/Contents/MacOS/procfs
	cp -r $(OUT)/procfs.fs.arm64 $(OUT)/procfs.fs
	lipo -create $(OUT)/procfs.fs.arm64/Contents/Resources/mount_procfs $(OUT)/procfs.fs.x86_64/Contents/Resources/mount_procfs -output $(OUT)/procfs.fs/Contents/Resources/mount_procfs
	codesign --force --timestamp=none --sign - $(OUT)/procfs.kext
	codesign --force --timestamp=none --sign - $(OUT)/procfs.fs
	rm -rf $(OUT)/procfs.kext.arm64e $(OUT)/procfs.kext.x86_64
	rm -rf $(OUT)/procfs.fs.arm64 $(OUT)/procfs.fs.x86_64

else

# kext + fs for a single arch.
kextfs:
	rm -rf $(OUT)
	mkdir $(OUT)
	$(MAKE) -C lib $(LIB_FLAGS)
	$(MAKE) debug -C kext $(KEXT_FLAGS)
	mv kext/procfs.kext kext/procfs.kext.dSYM $(OUT)
	$(MAKE) debug -C fs $(FS_FLAGS)
	mv fs/procfs.fs fs/procfs.fs.dSYM $(OUT)

endif

# Userspace tool: the daemon (procfsd).
tools:
	@mkdir -p $(OUT)
	$(MAKE) -C tools
	mv tools/procfsd $(OUT)/
	-mv tools/procfsd.dSYM $(OUT)/ 2>/dev/null || true

# LaunchDaemon plist(s).
plists:
	@mkdir -p $(OUT)
	cp tools/$(DAEMON_PLIST) $(OUT)/

# Menu-bar GUI app (ProcFS.app), the System Settings pane (ProcFS.prefPane) and
# the uninstaller (Uninstall-ProcFS.app). Versions are stamped from VERSION.
gui:
	@mkdir -p $(OUT)
	$(MAKE) -C gui
	rm -rf $(OUT)/ProcFS.app $(OUT)/ProcFS.prefPane $(OUT)/Uninstall-ProcFS.app
	mv gui/ProcFS.app gui/ProcFS.prefPane gui/Uninstall-ProcFS.app $(OUT)/

# ---------------------------------------------------------------------------
# Distribution: a double-clickable installer (.pkg) and disk image (.dmg).
# These build the artifacts and stage them; no root needed (nothing is installed
# on the build host). The .pkg's postinstall does the system setup at install
# time on the target machine.
# ---------------------------------------------------------------------------

# Installer package (.pkg) built from the artifacts in $(OUT).
pkg: all
	@echo "==> Staging installer payload"
	rm -rf $(OUT)/pkgroot $(OUT)/pkgres
	install -d $(OUT)/pkgroot/Library/Extensions $(OUT)/pkgroot/Library/Filesystems \
	           $(OUT)/pkgroot/usr/local/sbin $(OUT)/pkgroot/Library/LaunchDaemons \
	           $(OUT)/pkgroot/Applications/mSL $(OUT)/pkgroot/Library/PreferencePanes
	cp -R $(OUT)/procfs.kext          $(OUT)/pkgroot/Library/Extensions/
	cp -R $(OUT)/procfs.fs            $(OUT)/pkgroot/Library/Filesystems/
	cp    $(OUT)/procfsd $(OUT)/pkgroot/usr/local/sbin/
	cp    $(OUT)/$(DAEMON_PLIST)      $(OUT)/pkgroot/Library/LaunchDaemons/
	cp -R $(OUT)/ProcFS.app           $(OUT)/pkgroot/Applications/mSL/
	cp -R $(OUT)/Uninstall-ProcFS.app $(OUT)/pkgroot/Applications/mSL/
	cp -R $(OUT)/ProcFS.prefPane      $(OUT)/pkgroot/Library/PreferencePanes/
	@# codesign/pkgbuild reject Finder-info and similar xattrs.
	xattr -cr $(OUT)/pkgroot
	@echo "==> Building component package"
	pkgbuild --root $(OUT)/pkgroot --identifier $(PKG_ID) --version $(VERSION) \
	         --scripts installer/scripts --ownership recommended \
	         --component-plist installer/procfs-component.plist \
	         --install-location / $(PKG_COMP)
	@echo "==> Building product archive"
	mkdir -p $(OUT)/pkgres
	cp installer/resources/welcome.html installer/resources/conclusion.html $(OUT)/pkgres/
	cp LICENSE $(OUT)/pkgres/LICENSE
	sed -e 's/__KEXTVERSION__/$(VERSION)/g' installer/distribution.xml.in > $(OUT)/distribution.xml
	productbuild --distribution $(OUT)/distribution.xml --package-path $(OUT) \
	             --resources $(OUT)/pkgres $(PKG_OUT)
	rm -rf $(PKG_COMP) $(OUT)/distribution.xml $(OUT)/pkgroot $(OUT)/pkgres
	@echo "==> Built $(PKG_OUT)"

# Disk image (.dmg) wrapping the installer package and a README.
dmg: pkg
	@echo "==> Building disk image"
	rm -f $(DMG_OUT)
	rm -rf $(OUT)/dmg
	mkdir -p $(OUT)/dmg
	cp $(PKG_OUT) $(OUT)/dmg/
	cp installer/resources/DMG-README.txt $(OUT)/dmg/README.txt
	@# The .command is only a front-end; uninstall.sh must travel with it.
	cp installer/uninstall.command "$(OUT)/dmg/Uninstall ProcFS.command"
	cp installer/uninstall.sh      $(OUT)/dmg/uninstall.sh
	chmod +x "$(OUT)/dmg/Uninstall ProcFS.command" $(OUT)/dmg/uninstall.sh
	hdiutil create -volname "ProcFS $(VERSION)" -srcfolder $(OUT)/dmg \
	               -ov -format UDZO $(DMG_OUT)
	rm -rf $(OUT)/dmg
	@echo "==> Built $(DMG_OUT)"

# Test programs (not part of the default build).
tests:
	$(MAKE) -C tests

# Run the test suite: build the C test programs, then exercise the live mount
# with tests/test_features.sh (which reports PASS/FAIL per node). The feature
# tests need the filesystem mounted, so if $(PROC) (default /proc) is not a
# procfs mount they are skipped rather than failed - install the kext and reboot
# to run them. Override the mount with `make check PROC=/somewhere`.
PROC ?= /proc
check: tests
	@echo "==> procfs feature tests ($(PROC))"
	@if [ -e "$(PROC)/self" ]; then \
		PROC="$(PROC)" tests/test_features.sh; \
	else \
		echo "SKIP: $(PROC) is not a mounted procfs (install the kext and reboot, or set PROC=...)"; \
	fi

# Distribution sanity check: a clean build of the installer artifacts, then
# verify the .pkg and .dmg were produced and that the package payload carries
# every component (kext, filesystem, daemon, LaunchDaemon plist, app, prefpane).
#
# The clean build recompiles the kext libraries. Their compiler is pinned to
# Apple's cc in the lib Makefiles (libsbuf needs XNU-specific clang builtins
# behind kalloc_type that a non-Apple clang lacks), so CC is handled there; but
# the static-archive step defaults to PATH ar, which is GNU ar when Homebrew's
# binutils shadow it - so pin AR to Apple's for a robust clean build.
distcheck:
	@echo "==> Clean distribution build"
	$(MAKE) clean
	$(MAKE) dmg AR=/usr/bin/ar
	@echo "==> Checking distribution artifacts"
	@test -s "$(PKG_OUT)" || { echo "FAIL: $(PKG_OUT) missing or empty"; exit 1; }
	@test -s "$(DMG_OUT)" || { echo "FAIL: $(DMG_OUT) missing or empty"; exit 1; }
	@hdiutil imageinfo "$(DMG_OUT)" >/dev/null 2>&1 || { echo "FAIL: $(DMG_OUT) is not a valid disk image"; exit 1; }
	@echo "  ok  built $(notdir $(PKG_OUT)) and $(notdir $(DMG_OUT))"
	@rm -rf $(OUT)/distcheck; pkgutil --expand "$(PKG_OUT)" $(OUT)/distcheck 2>/dev/null || { echo "FAIL: cannot expand product archive"; exit 1; }
	@bom=`find $(OUT)/distcheck -name Bom | head -1`; \
	 test -n "$$bom" || { echo "FAIL: no component package (Bom) in archive"; exit 1; }; \
	 for f in procfs.kext procfs.fs procfsd $(DAEMON_PLIST) ProcFS.app \
	          Uninstall-ProcFS.app ProcFS.prefPane; do \
	   lsbom "$$bom" 2>/dev/null | grep -q "$$f" || { echo "FAIL: payload missing $$f"; rm -rf $(OUT)/distcheck; exit 1; }; \
	   echo "  ok  payload: $$f"; \
	 done
	@rm -rf $(OUT)/distcheck
	@echo "==> distcheck passed"

# Back-compat aliases.
debug: kextfs
release: TARGET=release
release: kextfs

# ---------------------------------------------------------------------------
# Install  (run as root, AFTER `make`; copies only, never compiles)
# ---------------------------------------------------------------------------

install: require-root require-built preinstall install-kext install-fs install-tools install-plists install-gui postinstall

require-root:
	@[ "$$(id -u)" -eq 0 ] || { echo "error: this target must be run as root (use: sudo make $(MAKECMDGOALS))"; exit 1; }

require-built:
	@[ -d "$(OUT)/procfs.kext" ] && [ -d "$(OUT)/procfs.fs" ] && \
	 [ -x "$(OUT)/procfsd" ] && \
	 [ -f "$(OUT)/$(DAEMON_PLIST)" ] && [ -d "$(OUT)/ProcFS.app" ] || \
		{ echo "error: build artifacts missing in $(OUT)/. Run 'make' first."; exit 1; }

# Tear down any previously installed/loaded build first. macOS caches third-party
# kexts in the Auxiliary Kernel Collection, so a stale staged copy otherwise
# shadows the freshly installed build and the new version is never detected.
preinstall:
	@echo "==> Removing any previously installed procfs (unmount, unload, clear staging)"
	-@mount | awk '/\(procfs[ ,]/ { print $$3 }' | while read -r mp; do \
		echo "    umount $$mp"; umount "$$mp" 2>/dev/null || true; done
	-@kmutil unload -b $(BUNDLE_ID) 2>/dev/null || true
	-@kmutil clear-staging 2>/dev/null || true

install-kext:
	rm -rf $(EXT_DIR)/procfs.kext
	cp -R $(OUT)/procfs.kext $(EXT_DIR)/procfs.kext
	chown -R root:wheel $(EXT_DIR)/procfs.kext
	chmod -R 755 $(EXT_DIR)/procfs.kext

install-fs:
	rm -rf $(FS_DIR)/procfs.fs
	cp -R $(OUT)/procfs.fs $(FS_DIR)/procfs.fs
	chown -R root:wheel $(FS_DIR)/procfs.fs
	chmod -R 755 $(FS_DIR)/procfs.fs

# procfsd serves proc_pidinfo data to the kext over the kernel-control bridge,
# optionally loads the kext, and mounts /proc - all as root.
install-tools:
	install -d -m 755 -o root -g wheel $(SBIN_DIR)
	install -m 755 -o root -g wheel $(OUT)/procfsd      $(SBIN_DIR)/procfsd

# The LaunchDaemon is RunAtLoad, so procfsd starts on the next boot. Auto-load
# of the kext and auto-mount of /proc stay DISARMED until the operator creates
# $(ARM_FLAG), so a kext panic during development cannot boot-loop the machine.
install-gui:
	install -d -m 755 -o root -g wheel $(MSL_APP_DIR)
	rm -rf $(MSL_APP_DIR)/ProcFS.app
	cp -R $(OUT)/ProcFS.app $(MSL_APP_DIR)/ProcFS.app
	chown -R root:wheel $(MSL_APP_DIR)/ProcFS.app
	chmod -R 755 $(MSL_APP_DIR)/ProcFS.app
	@# Strip com.apple.quarantine so Gatekeeper doesn't flag the app as
	@# "damaged"/"unverified" when the payload arrives via an internet download.
	xattr -dr com.apple.quarantine $(MSL_APP_DIR)/ProcFS.app 2>/dev/null || true
	rm -rf $(MSL_APP_DIR)/Uninstall-ProcFS.app
	cp -R $(OUT)/Uninstall-ProcFS.app $(MSL_APP_DIR)/Uninstall-ProcFS.app
	chown -R root:wheel $(MSL_APP_DIR)/Uninstall-ProcFS.app
	chmod -R 755 $(MSL_APP_DIR)/Uninstall-ProcFS.app
	xattr -dr com.apple.quarantine $(MSL_APP_DIR)/Uninstall-ProcFS.app 2>/dev/null || true
	rm -rf $(PREFPANE_DIR)/ProcFS.prefPane
	cp -R $(OUT)/ProcFS.prefPane $(PREFPANE_DIR)/ProcFS.prefPane
	chown -R root:wheel $(PREFPANE_DIR)/ProcFS.prefPane
	chmod -R 755 $(PREFPANE_DIR)/ProcFS.prefPane
	@# Launch the menu-bar app in the console user's GUI session so its icon
	@# shows immediately. Best-effort (root install -> hop to the logged-in user).
	-@u=$$(stat -f '%Su' /dev/console 2>/dev/null); \
	  uid=$$(id -u "$$u" 2>/dev/null); \
	  if [ -n "$$uid" ] && [ "$$u" != "root" ] && [ "$$u" != "loginwindow" ]; then \
	      launchctl asuser "$$uid" sudo -u "$$u" open "$(MSL_APP_DIR)/ProcFS.app" >/dev/null 2>&1 || true; \
	  fi

install-plists:
	install -m 644 -o root -g wheel $(OUT)/$(DAEMON_PLIST) $(DAEMON_DIR)/$(DAEMON_PLIST)
	@# Ensure the LaunchDaemon is enabled. A prior `launchctl disable` (e.g. during
	@# development/debugging) persists across boots in the launchd override store
	@# and would otherwise keep procfsd from starting at login - so /proc would
	@# never auto-mount even though the plist is RunAtLoad.
	-@launchctl enable system/$(DAEMON_LABEL) 2>/dev/null || true
	@# Create the /proc mount point on the read-only system volume via synthetic.conf.
	@grep -qxF 'proc' $(SYNTHETIC_CONF) 2>/dev/null || printf 'proc\n' >> $(SYNTHETIC_CONF)
	@echo "procfs: ensured 'proc' in $(SYNTHETIC_CONF) -> /proc is created at boot."

postinstall:
	@echo "procfs: installed kext, fs, daemon, app and LaunchDaemon."
	@echo "procfs: auto-load + auto-mount stay DISARMED. To arm them:"
	@echo "          sudo touch $(ARM_FLAG)"
	@echo "procfs: then REBOOT (creates /proc, loads kext, procfsd mounts /proc)."

# Convenience: (re)install only the userspace tools + plist, no kext/fs rebuild.
tools-install: require-root require-built install-tools install-plists
	@echo "procfs: tools + plist installed."

# ---------------------------------------------------------------------------
# Uninstall  (run as root): tear everything out of the system, then clean.
# ---------------------------------------------------------------------------

uninstall: require-root
	@echo "==> Unmounting procfs"
	-@mount | awk '/\(procfs[ ,]/ { print $$3 }' | while read -r mp; do \
		echo "    umount $$mp"; umount "$$mp" 2>/dev/null || true; done
	@echo "==> Stopping and removing the LaunchDaemon"
	-@launchctl bootout system/$(DAEMON_LABEL) 2>/dev/null || true
	-@launchctl disable system/$(DAEMON_LABEL) 2>/dev/null || true
	rm -f $(DAEMON_DIR)/$(DAEMON_PLIST)
	@echo "==> Unloading kext and clearing the staging cache"
	-@kmutil unload -b $(BUNDLE_ID) 2>/dev/null || true
	-@kmutil clear-staging 2>/dev/null || true
	@echo "==> Removing installed files"
	rm -rf $(EXT_DIR)/procfs.kext
	rm -rf $(FS_DIR)/procfs.fs
	rm -rf $(MSL_APP_DIR)/ProcFS.app
	rm -rf $(MSL_APP_DIR)/Uninstall-ProcFS.app
	@# Take the mSL folder with it when nothing else is left in it.
	-rmdir $(MSL_APP_DIR) 2>/dev/null || true
	rm -rf $(PREFPANE_DIR)/ProcFS.prefPane
	rm -f  $(SBIN_DIR)/procfsd $(SBIN_DIR)/procfs_ksyms
	rm -f  $(ARM_FLAG) $(KSYMS_FILE) $(LINUX_CONF) $(LINUX_VER_CONF)
	@echo "==> Removing 'proc' from $(SYNTHETIC_CONF)"
	-@if [ -f $(SYNTHETIC_CONF) ]; then \
		grep -vxF 'proc' $(SYNTHETIC_CONF) > $(SYNTHETIC_CONF).tmp 2>/dev/null && \
		mv $(SYNTHETIC_CONF).tmp $(SYNTHETIC_CONF) || rm -f $(SYNTHETIC_CONF).tmp; \
	fi
	$(MAKE) clean
	@echo "procfs: uninstalled. The now-empty /proc mount point persists until the next reboot."

# ---------------------------------------------------------------------------
# Clean  (never needs sudo: the build never produces root-owned files)
# ---------------------------------------------------------------------------

clean:
	rm -rf $(OUT)
	$(MAKE) -C kext clean
	$(MAKE) -C fs clean
	$(MAKE) -C lib clean
	$(MAKE) -C tests clean
	$(MAKE) -C tools clean
	$(MAKE) -C gui clean

.PHONY: all kextfs tools plists gui tests check distcheck debug release pkg dmg \
        install require-root require-built preinstall \
        install-kext install-fs install-tools install-plists install-gui postinstall \
        tools-install uninstall clean
