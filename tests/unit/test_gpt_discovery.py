"""Execute the patched upstream GPT selection code without disk mutation."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
SOURCE = Path("MdeModulePkg/Universal/Disk/PartitionDxe/Gpt.c")
PATCH = ROOT / "patches/firmware/mu-basecore/cfb9372/0004-enumerate-gpt-without-repair.patch"


class GptDiscoveryTests(unittest.TestCase):
    def test_validated_primary_or_backup_without_repairs(self):
        with tempfile.TemporaryDirectory(prefix="sunuefi-gpt-discovery-") as directory:
            work = Path(directory)
            target = work / SOURCE
            target.parent.mkdir(parents=True)
            shutil.copyfile(ROOT / "upstream/Mu-Silicium/Mu_Basecore" / SOURCE, target)
            subprocess.run(["git", "apply", "--check", str(PATCH)], cwd=work, check=True)
            subprocess.run(["git", "apply", str(PATCH)], cwd=work, check=True)
            text = target.read_text()
            start = text.index("  if (!PartitionValidGptTable")
            end = text.index("  // Read the EFI Partition Entries", start)
            selection = text[start:end]
            native = work / "selection.c"
            native.write_text(r"""
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#define DEBUG(x) ((void)0)
#define CopyMem memcpy
#define PRIMARY_PART_HEADER_LBA 1
typedef struct { uint64_t MyLBA, AlternateLBA, PartitionEntryLBA; } HEADER;
static unsigned valid, reads, writes;
static int PartitionValidGptTable(void *BlockIo, void *DiskIo, uint64_t Lba, HEADER *Out) {
  (void)BlockIo; (void)DiskIo; ++reads;
  if (Lba==1 && (valid&1)) { *Out=(HEADER){1, 99, 2}; return 1; }
  if (Lba==99 && (valid&2)) { *Out=(HEADER){99, 1, 90}; return 1; }
  return 0;
}
static int __attribute__((unused)) PartitionRestoreGptTable(void *BlockIo,void *DiskIo,HEADER *Header) {
  (void)BlockIo; (void)DiskIo; (void)Header; ++writes; return 1;
}
static uint64_t discover(void) {
  HEADER Primary={0},Backup={0},*PrimaryHeader=&Primary,*BackupHeader=&Backup;
  void *BlockIo=0,*DiskIo=0; uint64_t LastBlock=99;
""" + selection + r"""
  return PrimaryHeader->PartitionEntryLBA;
Done:
  return 0;
}
int main(void) {
  for (valid=0;valid<4;++valid) {
    reads=writes=0; uint64_t Entries=discover();
    assert(Entries==((valid&1)?2:(valid&2)?90:0));
    assert(reads==2 && writes==0);
  }
  puts("Patched upstream GPT discovery: primary, backup, both and neither; zero repair writes.");
}
""")
            binary = work / "selection"
            subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
                            "-fsanitize=address,undefined", "-fno-pie", "-no-pie",
                            str(native), "-o", str(binary)], check=True)
            subprocess.run([str(binary)], check=True)


if __name__ == "__main__":
    unittest.main()
