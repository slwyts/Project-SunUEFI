BOOTSELECT_CC ?= clang
BOOTSELECT_LD ?= ld.lld
BOOTSELECT_OBJCOPY ?= llvm-objcopy
OUT ?= out
# ABL enters with the MMU disabled. FDT properties are only 4-byte aligned,
# so byte-wise readers must not become unaligned 64-bit loads.
MDE_INCLUDE = ../../../upstream/Mu-Silicium/Mu_Basecore/MdePkg/Include
SPLASH_SOURCE = ../../components/product-support/Library/ProductBootManagerLib
FLAGS = --target=aarch64-linux-gnu -O2 -std=c11 -ffreestanding -fno-builtin -fno-stack-protector -fno-unwind-tables -fno-asynchronous-unwind-tables -mgeneral-regs-only -mstrict-align -fshort-wchar -Wall -Wextra -Werror -Wno-misleading-indentation -DPIANO_EARLY_SPLASH -DPIANO_EARLY_CHOOSER -I$(MDE_INCLUDE) -I$(MDE_INCLUDE)/AArch64
OBJECTS = $(addprefix $(OUT)/,Entry.o BootSelect.o BootRequest.o EarlyTrace.o EarlySplash.o EarlyKeys.o EarlyMmio.o ProductSplash.o)

all: $(OUT)/bootselect.bin

$(OUT):
	mkdir -p "$@"
$(OUT)/BootSelect.o: BootSelect.c BootSelect.h | $(OUT)
	$(BOOTSELECT_CC) $(FLAGS) -c "$<" -o "$@"
$(OUT)/BootRequest.o: BootRequest.c BootRequest.h | $(OUT)
	$(BOOTSELECT_CC) $(FLAGS) -c "$<" -o "$@"
$(OUT)/EarlyTrace.o: EarlyTrace.c EarlyTrace.h | $(OUT)
	$(BOOTSELECT_CC) $(FLAGS) -c "$<" -o "$@"
$(OUT)/Entry.o: Entry.S | $(OUT)
	$(BOOTSELECT_CC) --target=aarch64-linux-gnu -c "$<" -o "$@"
$(OUT)/EarlySplash.o: EarlySplash.c EarlyKeys.h $(SPLASH_SOURCE)/ProductSplashAssets.h | $(OUT)
	$(BOOTSELECT_CC) $(FLAGS) -c "$<" -o "$@"
$(OUT)/EarlyKeys.o: EarlyKeys.c EarlyKeys.h BootSelect.h | $(OUT)
	$(BOOTSELECT_CC) $(FLAGS) -c "$<" -o "$@"
$(OUT)/EarlyMmio.o: EarlyMmio.S | $(OUT)
	$(BOOTSELECT_CC) --target=aarch64-linux-gnu -c "$<" -o "$@"
$(OUT)/ProductSplash.o: $(SPLASH_SOURCE)/ProductSplash.c $(SPLASH_SOURCE)/ProductSplashAssets.h | $(OUT)
	$(BOOTSELECT_CC) $(FLAGS) -c "$<" -o "$@"
$(OUT)/bootselect.elf: $(OBJECTS) BootSelect.ld
	$(BOOTSELECT_LD) -T BootSelect.ld $(OBJECTS) -o "$@"
$(OUT)/bootselect.bin: $(OUT)/bootselect.elf
	$(BOOTSELECT_OBJCOPY) -O binary "$<" "$@"
.PHONY: all
