# NX-Tether: top level build.
#
# Requirements: devkitA64 + libnx + switch-libjpeg-turbo (devkitPro), and the git submodules
# in lib/ (git submodule update --init). libnx must be recent enough for Atmosphere-libs
# (switchbrew/libnx commit feebd02 or later). See README.md.

TID     := 4200000000000E7B
VERSION := 0.1.3
SYSMODULE_NSP := sysmodule/out/nintendo_nx_arm64_armv8a/release/sysmodule.nsp

.PHONY: all sysmodule overlay dist test clean

all: sysmodule overlay

sysmodule:
	$(MAKE) -C sysmodule

overlay:
	$(MAKE) -C overlay

dist: all
	rm -rf dist
	mkdir -p dist/atmosphere/contents/$(TID)/flags
	mkdir -p dist/switch/.overlays
	cp $(SYSMODULE_NSP) dist/atmosphere/contents/$(TID)/exefs.nsp
	touch dist/atmosphere/contents/$(TID)/flags/boot2.flag
	printf 'usb:hs\n' > dist/atmosphere/contents/$(TID)/mitm.lst
	cp sysmodule/toolbox.json dist/atmosphere/contents/$(TID)/toolbox.json
	cp overlay/usb-tether.ovl dist/switch/.overlays/usb-tether.ovl
	cd dist && zip -r ../nx-tether-$(VERSION).zip .

test:
	$(MAKE) -C tests check

clean:
	$(MAKE) -C sysmodule clean
	$(MAKE) -C overlay clean
	$(MAKE) -C tests clean
	rm -rf dist nx-tether-*.zip
