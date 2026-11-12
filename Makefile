.PHONY: sources check uefi kernel-source bsp rootfs

PYTHON ?= python3

# Thin aliases kept for muscle memory. ./build.sh remains the only entry point.
sources:
	./build.sh sources

check:
	./build.sh check

uefi:
	$(PYTHON) tools/prepare_sources.py --check
	$(PYTHON) tools/piano_vendor_inputs.py
	bash tools/build_product.sh

kernel-source:
	$(PYTHON) tools/prepare_sources.py --check
	$(PYTHON) tools/prepare_release_kernel.py

bsp:
	./build.sh bsp inspect --target debian

rootfs:
	./build.sh rootfs --distro debian --desktop gnome --plan
