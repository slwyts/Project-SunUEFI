BOOTSELECT_CC ?= clang
BOOTSELECT_LD ?= ld.lld
BOOTSELECT_OBJCOPY ?= llvm-objcopy
OUT ?= out
# ABL enters with the MMU disabled. FDT properties are only 4-byte aligned,
# so byte-wise readers must not become unaligned 64-bit loads.
FLAGS = --target=aarch64-linux-gnu -O2 -std=c11 -ffreestanding -fno-builtin -fno-stack-protector -fno-unwind-tables -fno-asynchronous-unwind-tables -mgeneral-regs-only -mstrict-align -Wall -Wextra -Werror

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
$(OUT)/bootselect.elf: $(OUT)/Entry.o $(OUT)/BootSelect.o $(OUT)/BootRequest.o $(OUT)/EarlyTrace.o BootSelect.ld
	$(BOOTSELECT_LD) -T BootSelect.ld $(OUT)/Entry.o $(OUT)/BootSelect.o $(OUT)/BootRequest.o $(OUT)/EarlyTrace.o -o "$@"
$(OUT)/bootselect.bin: $(OUT)/bootselect.elf
	$(BOOTSELECT_OBJCOPY) -O binary "$<" "$@"
.PHONY: all
