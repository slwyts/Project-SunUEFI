# Canonical components for the single product's eventual EARLY NV phase.
# Not a test profile. Integrator must resolve HALIOMMU<->VariableArch cycle,
# publish the restored runtime FVB/PIANO_NV_READY before VariableRuntimeDxe
# entry, and register/retire the actual storage owner. Do not include as a blind
# replacement for current EmuVariableNvModeEnable=TRUE.
[Components]
 MdeModulePkg/Universal/FaultTolerantWriteDxe/FaultTolerantWriteDxe.inf {
  <LibraryClasses>
   VariableFlashInfoLib|pianoProductNvPkg/Library/PianoVariableFlashInfoLib/PianoVariableFlashInfoLib.inf
 }
 MdeModulePkg/Universal/Variable/RuntimeDxe/VariableRuntimeDxe.inf {
  <PcdsFixedAtBuild>
   gEfiMdeModulePkgTokenSpaceGuid.PcdEmuVariableNvModeEnable|FALSE
   gEfiMdeModulePkgTokenSpaceGuid.PcdVariableStoreSize|0x40000
  <LibraryClasses>
   VariableFlashInfoLib|pianoProductNvPkg/Library/PianoVariableFlashInfoLib/PianoVariableFlashInfoLib.inf
  <BuildOptions>
   GCC:*_*_*_CC_FLAGS = -DPIANO_NV_BOOT_ONLY=1
 }
