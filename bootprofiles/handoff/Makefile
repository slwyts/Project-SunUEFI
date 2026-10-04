CC = clang
OBJCOPY = llvm-objcopy
all: BootShim.bin
BootShim.elf: BootShim.S
	$(CC) --target=aarch64-linux-gnu -c $< -o $@ -DFD_BASE=$(FD_BASE) -DFD_SIZE=$(FD_SIZE)
BootShim.bin: BootShim.elf
	$(OBJCOPY) -O binary $< $@
