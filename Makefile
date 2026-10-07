.PHONY: sources check uefi kernel-source

PYTHON ?= python3

sources:
	$(PYTHON) tools/prepare_sources.py

check:
	$(PYTHON) tools/check_host.py --group portable

uefi:
	$(PYTHON) tools/prepare_sources.py --check
	$(PYTHON) tools/piano_vendor_inputs.py
	bash tools/build_product.sh

kernel-source:
	$(PYTHON) tools/prepare_sources.py --check
	$(PYTHON) tools/prepare_release_kernel.py

.PHONY: bsp rootfs
bsp:
	./build.sh bsp inspect --target debian

rootfs:
	./build.sh rootfs --distro debian --desktop gnome --plan
