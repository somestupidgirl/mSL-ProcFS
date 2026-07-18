#
# Put user-defined variables here
#

KEXTNAME?=				procfs
# Single source of truth: the repo VERSION file (next to this Makefile.inc).
# Bump the version by editing VERSION; the kext defines, Info.plist and the
# procfs.fs bundle plist all pick it up from here. Overridable from the command
# line / environment (KEXTVERSION=...).
KEXTVERSION?=			$(strip $(shell cat $(dir $(lastword $(MAKEFILE_LIST)))VERSION 2>/dev/null || echo 1.0.0))
KEXTBUILD?=				1
FSNAME?=				$(KEXTNAME)
FSVERSION?=				$(KEXTVERSION)
FSBUILD?=				$(KEXTBUILD)
BUNDLEDOMAIN?=			com.beako.filesystems
COPYRIGHT?= 			"Copyright © 2022 - 2026 Sunneva N. Mariu, All rights reserved."
#
# Code-signing identity. "-" means ad-hoc, which is what a development build
# wants: loading a kext with a real certificate needs a Developer ID with
# Apple's kext-signing entitlement, which an ordinary "Apple Development" cert
# is not.
#
# Overridable from the environment, so verify the identity actually exists
# before handing it to codesign. A stale SIGNCERT left exported in a shell -
# pointing at a certificate that has since been removed - would otherwise fail
# the build late, at the codesign step, with "no identity found".
#
SIGNCERT?= -
ifneq ($(SIGNCERT),-)
ifeq ($(shell security find-identity -v 2>/dev/null | grep -c $(SIGNCERT)),0)
$(warning SIGNCERT '$(SIGNCERT)' is not a valid signing identity in the keychain; falling back to ad-hoc signing)
SIGNCERT:= -
endif
endif
