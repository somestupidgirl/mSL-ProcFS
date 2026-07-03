#
# All-in-one Makefile
#
# Usage:
#   make                    # build kext + fs + tools + plists into $(OUT)
#   make ARCH=arm64e        # Apple Silicon kext + arm64 fs
#   make ARCH=x86_64        # Intel kext + x86_64 fs
#   make ARCH=universal     # fat kext + fs (arm64e + x86_64)
#   make tests              # also build the test programs
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
SYNTHETIC_CONF := /etc/synthetic.conf

# Identifiers / runtime files.
BUNDLE_ID      := com.beako.filesystems.procfs
DAEMON_PLIST   := com.beako.procfsd.plist
DAEMON_LABEL   := com.beako.procfsd
ARM_FLAG       := /var/db/procfs.enabled
KSYMS_FILE     := /var/db/procfs.ksyms

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
all: kextfs tools plists gui pkg dmg

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

# Userspace tools: the daemon (procfsd) and the symbol stager (procfs_ksyms).
tools:
	@mkdir -p $(OUT)
	$(MAKE) -C tools
	mv tools/procfsd tools/procfs_ksyms $(OUT)/
	-mv tools/procfsd.dSYM tools/procfs_ksyms.dSYM $(OUT)/ 2>/dev/null || true

# LaunchDaemon plist(s).
plists:
	@mkdir -p $(OUT)
	cp tools/$(DAEMON_PLIST) $(OUT)/

# Menu-bar GUI app (ProcFS.app). Its version is stamped from the VERSION file.
gui:
	@mkdir -p $(OUT)
	$(MAKE) -C gui
	rm -rf $(OUT)/ProcFS.app
	mv gui/ProcFS.app $(OUT)/

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
	           $(OUT)/pkgroot/Applications
	cp -R $(OUT)/procfs.kext          $(OUT)/pkgroot/Library/Extensions/
	cp -R $(OUT)/procfs.fs            $(OUT)/pkgroot/Library/Filesystems/
	cp    $(OUT)/procfsd $(OUT)/procfs_ksyms $(OUT)/pkgroot/usr/local/sbin/
	cp    $(OUT)/$(DAEMON_PLIST)      $(OUT)/pkgroot/Library/LaunchDaemons/
	cp -R $(OUT)/ProcFS.app           $(OUT)/pkgroot/Applications/
	@# codesign/pkgbuild reject Finder-info and similar xattrs.
	xattr -cr $(OUT)/pkgroot
	@echo "==> Building component package"
	pkgbuild --root $(OUT)/pkgroot --identifier $(PKG_ID) --version $(VERSION) \
	         --scripts installer/scripts --ownership recommended \
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
	cp installer/uninstall.command "$(OUT)/dmg/Uninstall ProcFS.command"
	chmod +x "$(OUT)/dmg/Uninstall ProcFS.command"
	hdiutil create -volname "ProcFS $(VERSION)" -srcfolder $(OUT)/dmg \
	               -ov -format UDZO $(DMG_OUT)
	rm -rf $(OUT)/dmg
	@echo "==> Built $(DMG_OUT)"

# Test programs (not part of the default build).
tests:
	$(MAKE) -C tests

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
	 [ -x "$(OUT)/procfsd" ] && [ -x "$(OUT)/procfs_ksyms" ] && \
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
# stages kernel symbols, optionally loads the kext, and mounts /proc - all as
# root. procfs_ksyms decompresses the booted kernelcache and writes
# $(KSYMS_FILE), from which the kext resolves private symbols. They are DISTINCT
# binaries: procfsd execs procfs_ksyms as its stager, so installing procfsd over
# procfs_ksyms makes procfsd recursively re-exec itself and fork-bombs the host.
install-tools:
	install -d -m 755 -o root -g wheel $(SBIN_DIR)
	install -m 755 -o root -g wheel $(OUT)/procfsd      $(SBIN_DIR)/procfsd
	install -m 755 -o root -g wheel $(OUT)/procfs_ksyms $(SBIN_DIR)/procfs_ksyms

# The LaunchDaemon is RunAtLoad, so procfsd starts on the next boot. Auto-load
# of the kext and auto-mount of /proc stay DISARMED until the operator creates
# $(ARM_FLAG), so a kext panic during development cannot boot-loop the machine.
install-gui:
	rm -rf $(APP_DIR)/ProcFS.app
	cp -R $(OUT)/ProcFS.app $(APP_DIR)/ProcFS.app
	chown -R root:wheel $(APP_DIR)/ProcFS.app
	chmod -R 755 $(APP_DIR)/ProcFS.app

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
	@echo "procfs: installed kext, fs, daemon, stager and LaunchDaemon."
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
	rm -rf $(APP_DIR)/ProcFS.app
	rm -f  $(SBIN_DIR)/procfsd $(SBIN_DIR)/procfs_ksyms
	rm -f  $(ARM_FLAG) $(KSYMS_FILE)
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

.PHONY: all kextfs tools plists gui tests debug release pkg dmg \
        install require-root require-built preinstall \
        install-kext install-fs install-tools install-plists install-gui postinstall \
        tools-install uninstall clean
