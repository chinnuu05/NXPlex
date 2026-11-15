# NXPlex: build everything and package an SD card zip.
#
#   make            sysmodule + overlay
#   make dist       ...and dist/NXPlex-<version>.zip (copy its contents to the SD root)
#   make host       PC tools and tests (Linux/WSL)
#   make clean
#
# Uses $(DEVKITPRO). Set DEVKITA64 to use another copy of the compiler
# (see tools/setup-toolchain.sh for Ubuntu 22.04).

VERSION := $(shell cat VERSION)
TID     := 4200000000504C58
SD      := dist/sdcard

all: sysmodule overlay

sysmodule:
	$(MAKE) -C sysmodule

overlay:
	$(MAKE) -C overlay

host:
	$(MAKE) -C host

dist: all
	rm -rf $(SD)
	mkdir -p $(SD)/atmosphere/contents/$(TID) $(SD)/switch/.overlays
	cp sysmodule/nxplex-sys.nsp $(SD)/atmosphere/contents/$(TID)/exefs.nsp
	printf '{\n    "name": "NXPlex",\n    "tid": "$(TID)",\n    "requires_reboot": false\n}\n' \
		> $(SD)/atmosphere/contents/$(TID)/toolbox.json
	cp overlay/nxplex.ovl $(SD)/switch/.overlays/nxplex.ovl
	rm -f dist/NXPlex-$(VERSION).zip
	cd $(SD) && zip -qr ../NXPlex-$(VERSION).zip atmosphere switch
	@echo "dist/NXPlex-$(VERSION).zip"

clean:
	$(MAKE) -C sysmodule clean
	$(MAKE) -C overlay clean
	$(MAKE) -C host clean
	rm -rf $(SD) dist/NXPlex-*.zip

.PHONY: all sysmodule overlay host dist clean
