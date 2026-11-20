PREFIX  = /Library/Printers/HP-LJ1020
QUEUE   = HP_LaserJet_1020
CFLAGS  = -O2 -Wall -Wextra -Wno-deprecated-declarations -arch arm64 -arch x86_64 -mmacosx-version-min=11.0 -Ivendor/jbigkit
JBIG    = vendor/jbigkit/jbig.c vendor/jbigkit/jbig_ar.c
PLUGIN  = hplip-3.25.8-plugin.run

all: build/rastertozjs build/hplj1020-hotplug firmware/hp_laserjet_1020.fw

# HP's firmware is not redistributed here; extract it from HP's HPLIP plugin, pinned by checksum.
firmware/hp_laserjet_1020.fw:
	@mkdir -p build firmware
	curl -fsSL -o build/$(PLUGIN) https://www.openprinting.org/download/printdriver/auxfiles/HP/plugins/$(PLUGIN)
	echo "69151006c59a4dbdd5469df5eb97c253b5fcfeafe297360850c5a03c1ce68f95  build/$(PLUGIN)" | shasum -a 256 -c
	tail -c +$$(($$(head -n 591 build/$(PLUGIN) | wc -c) + 1)) build/$(PLUGIN) | tar -xzOf - ./hp_laserjet_1020.fw.gz | gunzip > $@.tmp
	echo "9a6d03c858d9cf64ba86fdbe6cf0beec1297d2e45e606162b19f3857efae4ff4  $@.tmp" | shasum -a 256 -c
	mv $@.tmp $@

build/rastertozjs: src/rastertozjs.c $(JBIG)
	@mkdir -p build
	cc $(CFLAGS) -o $@ src/rastertozjs.c $(JBIG) -lcups

build/hplj1020-hotplug: src/hplj1020-hotplug.c
	@mkdir -p build
	cc $(CFLAGS) -o $@ $<

install: all
	install -d -o root -g wheel $(PREFIX)/filter $(PREFIX)/bin $(PREFIX)/firmware
	install -m 755 -o root -g wheel build/rastertozjs $(PREFIX)/filter/
	install -m 755 -o root -g wheel build/hplj1020-hotplug scripts/hplj1020-firmware $(PREFIX)/bin/
	install -m 644 -o root -g wheel firmware/hp_laserjet_1020.fw $(PREFIX)/firmware/
	install -m 644 -o root -g wheel ppd/HP-LaserJet-1020.ppd "/Library/Printers/PPDs/Contents/Resources/HP LaserJet 1020.ppd"
	install -m 644 -o root -g wheel launchd/local.hplj1020.firmware.plist /Library/LaunchDaemons/
	-launchctl bootout system/local.hplj1020.firmware 2>/dev/null
	launchctl bootstrap system /Library/LaunchDaemons/local.hplj1020.firmware.plist
	uri=$$(/usr/libexec/cups/backend/usb 2>/dev/null | awk '/MDL:HP LaserJet 1020;/{print $$2; exit}'); \
	  [ -n "$$uri" ] && lpadmin -p $(QUEUE) -E -v "$$uri" -P ppd/HP-LaserJet-1020.ppd -D "HP LaserJet 1020" -o printer-is-shared=false || echo "Printer not connected: add it later in System Settings"

uninstall:
	-lpadmin -x $(QUEUE)
	-launchctl bootout system/local.hplj1020.firmware
	rm -rf $(PREFIX) /Library/LaunchDaemons/local.hplj1020.firmware.plist "/Library/Printers/PPDs/Contents/Resources/HP LaserJet 1020.ppd"

clean:
	rm -rf build

.PHONY: all install uninstall clean
