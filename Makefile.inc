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
SIGNCERT?= -
