"""Generic execution model of linked actual AArch64 SEC batch instructions."""
import re
class Fatal(Exception):pass
def run_batch(raw,dis,symbols,words=64,fault=None,el=4,spsel=1,sctlr=0x1000,bytes_override=None):
 codebase=0x100000
 ins={int(m[1],16):(m[2],m[3].split('//')[0].split('<')[0].strip())for line in dis.splitlines()if(m:=re.match(r'\s*([0-9a-f]+): [0-9a-f]{8}\s+(\w+(?:\.\w+)?)\s*(.*)',line))}
 memory=[(codebase,bytearray(raw)),(symbols['PianoColdReadActiveState'],bytearray(8)),(0x200000,bytearray(bytes(range(256)))),(0x210000,bytearray(b'\xa5'*256)),(0x220000,bytearray(96)),(0x230000,bytearray(4096))]
 def region(a,n):
  for b,p in memory:
   if b<=a and a+n<=b+len(p):return p,a-b
  raise AssertionError(('unmapped modeled access',hex(a),n))
 def read(a,n):p,o=region(a,n);return int.from_bytes(p[o:o+n],'little')
 def write(a,n,v):p,o=region(a,n);p[o:o+n]=(v&((1<<(n*8))-1)).to_bytes(n,'little')
 r={f'x{i}':0 for i in range(31)};r.update(x0=0x200000,x1=0x210000,x2=0x220000,x3=words*4 if bytes_override is None else bytes_override,sp=0x230800,x30=0xdeadbeef)
 sys={'CurrentEL':el,'SPSel':spsel,'SCTLR_EL1':sctlr,'VBAR_EL1':0x81200000,'DAIF':0x3c0,'ELR_EL1':0,'ESR_EL1':0,'FAR_EL1':0,'SPSR_EL1':5}
 def get(x):
  if x.startswith('#'):return int(x[1:],0)
  if x in ('xzr','wzr'):return 0
  return r['x'+x[1:]if x.startswith('w')else x]&((1<<32)-1 if x.startswith('w')else (1<<64)-1)
 def put(x,v):
  if x not in ('xzr','wzr'):r['x'+x[1:]if x.startswith('w')else x]=v&((1<<32)-1 if x.startswith('w')else (1<<64)-1)
 def address(s):
  m=re.fullmatch(r'\[(\w+)(?:, (#[^\]]+))?\](?:(!)|, (#[^ ]+))?',s);assert m,s
  off=get(m[2])if m[2]else 0
  if m[3]:put(m[1],get(m[1])+off);off=0
  return get(m[1])+off,m[1],get(m[4])if m[4]else 0
 pc=symbols['PianoColdSecRead256'];zero=False;carry=False;attempts=0;injected=False;installs=0;steps=0;fatal=False
 try:
  while True:
   steps+=1;assert steps<20000
   if pc==symbols['PianoSecReadFatal']:raise Fatal()
   op,s=ins[pc];nxt=pc+4;p=s.split(', ')
   if op=='adr':put(p[0],int(p[1],0))
   elif op=='ret':break
   elif op=='b':nxt=int(s,0)
   elif op=='bl':put('x30',pc+4);nxt=int(s,0)
   elif op in ('b.eq','b.ne','b.hi','b.hs','b.cs'):
    yes=zero if op=='b.eq'else not zero if op=='b.ne'else carry and not zero if op=='b.hi'else carry
    if yes:nxt=int(s,0)
   elif op in ('cbz','cbnz'):
    if (get(p[0])==0)==(op=='cbz'):nxt=int(p[1],0)
   elif op in ('tbz','tbnz'):
    if bool(get(p[0])&(1<<get(p[1])))==(op=='tbnz'):nxt=int(p[2],0)
   elif op=='mrs':put(p[0],sys[p[1]])
   elif op=='msr':
    if p[0]=='DAIFSet':sys['DAIF']|=get(p[1])<<6
    elif p[0]=='DAIFClr':sys['DAIF']&=~(get(p[1])<<6)
    else:
     sys[p[0]]=get(p[1])
     if p[0]=='VBAR_EL1'and get(p[1])==symbols['PianoColdReadVectors']:installs+=1
   elif op=='mov':put(p[0],get(p[1]))
   elif op in ('add','adds','sub','subs'):
    a,b=get(p[1]),get(p[2]);v=a+b if op.startswith('add')else a-b;put(p[0],v)
    if op.endswith('s'):zero=get(p[0])==0;carry=v>>64!=0 if op.startswith('add')else a>=b
   elif op=='cmp':zero=get(p[0])==get(p[1]);carry=get(p[0])>=get(p[1])
   elif op=='tst':zero=not(get(p[0])&get(p[1]))
   elif op=='and':put(p[0],get(p[1])&get(p[2]))
   elif op=='lsr':put(p[0],get(p[1])>>get(p[2]))
   elif op=='ldr':
    reg,operand=s.split(', ',1);a,b,post=address(operand);n=4 if reg.startswith('w')else 8
    if operand=='[x0]'and reg=='w9':
     index=attempts;attempts+=1
     if fault and not injected and index==fault.get('word',0):
      injected=True;sys.update(ELR_EL1=pc+fault.get('pc_delta',0),ESR_EL1=fault.get('esr',0x96000010),FAR_EL1=a+fault.get('far_delta',0),SPSR_EL1=fault.get('spsr',5));nxt=sys['VBAR_EL1']+(0x380 if fault.get('serror')else 0x200);pc=nxt;continue
    put(reg,read(a,n));put(b,get(b)+post)
   elif op=='str':reg,operand=s.split(', ',1);a,b,post=address(operand);write(a,4 if reg.startswith('w')else 8,get(reg));put(b,get(b)+post)
   elif op in ('ldp','stp'):
    a1,a2,operand=s.split(', ',2);a,b,post=address(operand)
    if op=='ldp':put(a1,read(a,8));put(a2,read(a+8,8))
    else:write(a,8,get(a1));write(a+8,8,get(a2))
    put(b,get(b)+post)
   elif op in ('dsb','isb'):pass
   elif op=='eret':nxt=sys['ELR_EL1']
   elif op in ('smc','brk'):raise Fatal()
   else:raise AssertionError(('unsupported actual instruction',op,s))
   pc=nxt
 except Fatal:fatal=True
 return {'status':r['x0'],'words':read(0x220000+88,8),'attempts':attempts,'installs':installs,'fatal':fatal,'vbar':sys['VBAR_EL1'],'daif':sys['DAIF'],'armed':read(0x220000,8),'active':read(symbols['PianoColdReadActiveState'],8),'scratch':bytes(region(0x210000,256)[0]),'sp':r['sp'],'steps':steps}
