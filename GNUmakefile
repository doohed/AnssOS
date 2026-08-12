# Nuke built-in rules.
.SUFFIXES:

override IMAGE_NAME := AnssOS

.PHONY: all
all: $(IMAGE_NAME).iso

.PHONY: kernel
kernel:
	$(MAKE) -C kernel

# UEFI-only ISO: stage the kernel, Limine's boot config, and Limine's
# prebuilt UEFI El Torito image + BOOTX64.EFI, then let xorriso build the
# hybrid ISO 9660 image. No BIOS boot catalog entry / limine-bios-install
# step, since legacy BIOS boot is intentionally not supported.
$(IMAGE_NAME).iso: kernel limine.conf
	rm -rf iso_root
	mkdir -p iso_root/boot/limine
	cp kernel/bin/kernel iso_root/boot/
	cp limine.conf iso_root/boot/limine/
	cp limine/limine-uefi-cd.bin iso_root/boot/limine/
	mkdir -p iso_root/EFI/BOOT
	cp limine/BOOTX64.EFI iso_root/EFI/BOOT/
	xorriso -as mkisofs -R -r -J \
		-hfsplus -apm-block-size 2048 \
		--efi-boot boot/limine/limine-uefi-cd.bin \
		-efi-boot-part --efi-boot-image --protective-msdos-label \
		iso_root -o $(IMAGE_NAME).iso
	rm -rf iso_root

.PHONY: run
run: $(IMAGE_NAME).iso
	./run.sh

.PHONY: clean
clean:
	$(MAKE) -C kernel clean
	rm -rf iso_root $(IMAGE_NAME).iso

.PHONY: distclean
distclean: clean
