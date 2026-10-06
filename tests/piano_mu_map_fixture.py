"""Compile actual Mu capability conversion and CoreGetMemoryMap emission lines."""
import hashlib,re
def actual_function(text,name):
 match=re.search(r'(?:static\s+)?(?:UINT64|VOID)\s+\n?'+re.escape(name)+r'\s*\([^)]*\)\s*\{',text)
 if not match:raise AssertionError('missing actual Mu function '+name)
 pos=match.end();depth=1
 while depth:
  depth+=(text[pos]=='{')-(text[pos]=='}');pos+=1
 return text[match.start():pos]
def write_mu_map_fixture(root,path):
 base=root/'upstream/Mu-Silicium/Mu_Basecore/MdeModulePkg/Core/Dxe'
 gcd=(base/'Gcd/Gcd.c').read_text();page=(base/'Mem/Page.c').read_text();main=(base/'DxeMain.h').read_text()
 table=re.search(r'GCD_ATTRIBUTE_CONVERSION_ENTRY\s+mAttributeConversionTable\[\]\s*=\s*\{.*?\n\};',gcd,re.S)
 output=re.search(r'MemoryMapEnd->PhysicalStart\s*= Entry->Start;\s*MemoryMapEnd->VirtualStart\s*= 0;\s*MemoryMapEnd->NumberOfPages\s*= EFI_SIZE_TO_PAGES \(\(UINTN\)\(Entry->End - Entry->Start \+ 1\)\);\s*MemoryMapEnd->Attribute\s*= Entry->Attribute;\s*SetEfiMemoryDescriptorType \(MemoryMapEnd, Entry->Type\);',page)
 clear=re.search(r'MemoryMap->Attribute &= ~\(UINT64\)EFI_MEMORY_ACCESS_MASK;',page)
 if not table or not output or not clear:raise AssertionError('actual Mu capability/map emission drift')
 pins='// Actual Mu Gcd.c SHA256 '+hashlib.sha256((base/'Gcd/Gcd.c').read_bytes()).hexdigest()+'\n// Actual Mu Page.c SHA256 '+hashlib.sha256((base/'Mem/Page.c').read_bytes()).hexdigest()+'\n'
 definitions='\n'.join(re.findall(r'(?m)^#define EFI_MEMORY_(?:PRESENT|INITIALIZED|TESTED)\s+[^\n]+',main))
 text=pins+'#ifndef ASSERT\n#define ASSERT(x) assert(x)\n#endif\n'+definitions+'\ntypedef struct {UINT64 Attribute,Capability;BOOLEAN Memory;} GCD_ATTRIBUTE_CONVERSION_ENTRY;\n'+table.group()+'\n'+actual_function(gcd,'CoreConvertResourceDescriptorHobAttributesToCapabilities')+'\n'
 text+='STATIC struct {BOOLEAN Runtime;} mMemoryTypeStatistics[EfiMaxMemoryType];\n'+actual_function(page,'SetEfiMemoryDescriptorType')+'\n'
 # Only EFI/list storage boundaries are supplied. The emission and permission
 # cleanup lines below are exact actual CoreGetMemoryMap code, not a rewrite.
 text+='STATIC VOID ActualMuEfiDescriptor(EFI_MEMORY_DESCRIPTOR *MemoryMapEnd,UINT64 Base,UINT64 Pages,EFI_MEMORY_TYPE Type,UINT64 Attribute){\nstruct {UINT64 Start,End,Attribute;EFI_MEMORY_TYPE Type;} Storage={Base,Base+Pages*4096-1,Attribute,Type},*Entry=&Storage;\nZeroMem(MemoryMapEnd,sizeof(*MemoryMapEnd));\n'+output.group()+'\nEFI_MEMORY_DESCRIPTOR *MemoryMap=MemoryMapEnd;\n'+clear.group()+'\n}\n'
 path.write_text(text)
