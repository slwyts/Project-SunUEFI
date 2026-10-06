"""Small generic interpreter of llvm-disassembled candidate AArch64 instructions.

This is a host instruction model, not ARM execution. It consumes assembled
addresses/operands; no handoff/copy/CRC algorithm is separately implemented.
"""
import re
def run(raw,dis,base,el=4,fd_bytes=0x300000,fd_base=0xa7100000):
 ins={int(m[1],16):(m[2],m[3].split('//')[0].split('<')[0].strip()) for line in dis.splitlines() if(m:=re.match(r'\s*([0-9a-f]+): [0-9a-f]{8}\s+(\w+(?:\.\w+)?)\s*(.*)',line))}
 payload=bytes((i*17+3)&255 for i in range(fd_bytes))
 memory=[(base,bytearray(raw+payload)),(0xa7fff000,bytearray(4096))]
 if not (base<=fd_base and fd_base+fd_bytes<=base+len(raw)+fd_bytes):memory.append((fd_base,bytearray(fd_bytes)))
 def region(a,n):
  for b,p in memory:
   if b<=a and a+n<=b+len(p):return p,a-b
  raise AssertionError(('unmapped modeled access',hex(a),n))
 def read(a,n):p,o=region(a,n);return int.from_bytes(p[o:o+n],'little')
 writes=[]
 def write(a,n,v):p,o=region(a,n);p[o:o+n]=(v&((1<<(n*8))-1)).to_bytes(n,'little');writes.append((a,n))
 regs={f'x{i}':0 for i in range(31)};regs['sp']=0xa9000000;regs['x0']=0xa8500000
 def get(x):
  if x.startswith('#'):return int(x[1:],0)
  if x in ('xzr','wzr'):return 0
  return regs['x'+x[1:] if x.startswith('w') else x]&((1<<32)-1 if x.startswith('w') else (1<<64)-1)
 def put(x,v):
  if x not in ('xzr','wzr'):regs['x'+x[1:] if x.startswith('w') else x]=v&((1<<32)-1 if x.startswith('w') else (1<<64)-1)
 def address(s):
  m=re.fullmatch(r'\[(\w+)(?:, (#[^\]]+))?\](?:, (#[^ ]+))?',s);assert m,s
  return get(m[1])+(get(m[2])if m[2]else 0),m[1],get(m[3])if m[3]else 0
 pc=0;zero=False;mrs=[];steps=0
 while True:
  steps+=1;assert steps<2000000
  op,s=ins[pc];nextpc=pc+4;p=s.split(', ')
  if op=='adr':put(p[0],base+int(p[1],0))
  elif op=='b':nextpc=int(s,0)
  elif op in ('b.eq','b.ne'):
   if zero==(op=='b.eq'):nextpc=int(s,0)
  elif op=='br':assert get(s)==fd_base;break
  elif op=='mrs':
   mrs.append(p[1]);assert p[1]=='CurrentEL' or el==4,'EL-specific MRS on unknown EL'
   put(p[0],{'CurrentEL':el,'CNTVCT_EL0':123,'CNTFRQ_EL0':1000000000,'VBAR_EL1':0x81200000,'SCTLR_EL1':0x30d01808}[p[1]])
  elif op=='mov':put(p[0],get(p[1]))
  elif op=='movk':
   shift=int(p[2].split('#')[1],0);mask=65535<<shift;put(p[0],(get(p[0])&~mask)|(get(p[1])<<shift))
  elif op in ('sub','subs'):v=get(p[1])-get(p[2]);put(p[0],v);zero=(get(p[0])==0) if op=='subs' else zero
  elif op=='cmp':zero=get(p[0])==get(p[1])
  elif op=='tst':zero=not(get(p[0])&get(p[1]))
  elif op in ('eor','orr'):put(p[0],get(p[1])^get(p[2]) if op=='eor' else get(p[1])|get(p[2]))
  elif op=='lsr':put(p[0],get(p[1])>>get(p[2]))
  elif op=='mvn':put(p[0],~get(p[1]))
  elif op in ('ldr','ldrb'):
   reg,operand=s.split(', ',1);n=1 if op=='ldrb' else 4 if reg.startswith('w') else 8
   if operand.startswith('['):a,r,post=address(operand);put(reg,read(a,n));put(r,get(r)+post)
   else:put(reg,read(base+int(operand,0),n))
  elif op=='str':reg,operand=s.split(', ',1);a,r,post=address(operand);write(a,4 if reg.startswith('w')else 8,get(reg));put(r,get(r)+post)
  elif op in ('ldp','stp'):
   r1,r2,operand=s.split(', ',2);a,r,post=address(operand)
   if op=='ldp':put(r1,read(a,8));put(r2,read(a+8,8))
   else:write(a,8,get(r1));write(a+8,8,get(r2))
   put(r,get(r)+post)
  elif op=='dsb':pass
  else:raise AssertionError(('unsupported actual instruction',op,s))
  pc=nextpc
 handoff=bytes(region(0xa7fff000,144)[0][:144]);copied=bytes(region(fd_base,fd_bytes)[0][region(fd_base,fd_bytes)[1]:region(fd_base,fd_bytes)[1]+fd_bytes])
 assert copied==payload
 for a,n in writes:assert 0xa7fff000<=a and a+n<=0xa7fff090 or fd_base<=a and a+n<=fd_base+fd_bytes
 return handoff,mrs,steps
