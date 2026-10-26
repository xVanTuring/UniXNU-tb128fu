#!/usr/bin/env python3
"""Bind an owned concrete backend to the unchanged original Legacy lifecycle.

build-native-legacy-display.py of UniXNU-qemu (itself sheng's, reduced to the display, the key store and the HID input)
without QEMU's VirtIO input, shared memory window and Metal accelerator: NativeLegacyDisplay, this board's
LegacyDisplayProbe.cpp, NativeGOPPresenter, sheng's KeyStoreProbe.cpp and NativeKeyStoreEvents, sheng's HidInputProbe.cpp
and NativeHidInput, and TouchState.cpp (the finger set; nothing writes it until the board has a touch driver). There is no
Metal device: QuartzCore's original CPU renderer draws, and the display copies its frames to the firmware frame buffer.
The original-class checks and bindings are unchanged. Their inputs, the reference candidate and the observation of the
original class, are UniXNU-qemu's (its build-display-abi.py observes the class under QEMU, which this board's kernel
cannot boot in): see the check below that makes them hold for this board's kernel. UniXNU's build-sheng-hid-input.py gets
the kernel step's kernel as its reference candidate, which carries the same original IOHIDFamily as every kernel of this
IPSW.
"""
import hashlib
import json
from pathlib import Path
import struct
import subprocess
import sys
ROOT=Path(__file__).resolve().parents[1];OUT=ROOT/'build/native-legacy-display';OUT.mkdir(parents=True,exist_ok=True)
sys.path.insert(0,str(ROOT/'kits/XnuPrelink'))
from native_prelink import macho,backed_offset,exports
def run(cmd,**kw):return subprocess.check_output([str(x) for x in cmd],timeout=90,**kw)
def require(v,why):
    if not v:raise SystemExit(why)
def sha(p):return hashlib.sha256(Path(p).read_bytes()).hexdigest()
# The audit's candidate and its runtime observation of the original IOMobileFramebufferLegacy, from UniXNU-qemu's tree
# (TB128FU_DISPLAY_REFERENCE overrides it). The addresses the audit and the bindings take from that candidate hold for this
# board's kernel only if the original IOMobileGraphicsFamily lies at the same addresses with the same bytes here, apart from
# the __DATA_CONST words that prelinking bound to kernel exports, which must name the same symbols in both kernels.
import os,shutil
FAMILY='com.apple.iokit.IOMobileGraphicsFamily'
reference_tree=Path(os.environ.get('TB128FU_DISPLAY_REFERENCE') or ROOT.parents[1]/'qemu/tree')
# A reference tree built before UniXNU renamed NativePrelink to XnuPrelink keeps its candidates under build/native-prelink.
reference_candidate=next((reference_tree/d/'native-owner-mobile-graphics-candidate.macho' for d in ('build/xnu-prelink','build/native-prelink')
                          if (reference_tree/d/'native-owner-mobile-graphics-candidate.macho').exists()),
                         reference_tree/'build/xnu-prelink/native-owner-mobile-graphics-candidate.macho')
reference_observation=reference_tree/'build/darwinboot-owner-hvf-mobile-graphics-legacy-class/gdb-stop.json'
board_kernel=ROOT.parent/'kernel/kernel.macho'
require(reference_candidate.exists() and reference_observation.exists() and board_kernel.exists(),
        f'Missing the display reference ({reference_tree}) or this board\'s kernel ({board_kernel})')
def segment_reader(path):
    data=Path(path).read_bytes();segs=[];at=32
    for _ in range(struct.unpack_from('<I',data,16)[0]):
        cmd,size=struct.unpack_from('<II',data,at)
        if cmd==0x19:segs.append(struct.unpack_from('<QQQQ',data,at+24))
        at+=size
    def read(va,n):
        for vm,vs,fo,fs in segs:
            if vm<=va and va+n<=vm+min(vs,fs):return data[fo+va-vm:fo+va-vm+n]
        raise SystemExit(f'{va:#x} is not file-backed in {path}')
    return read
def symbol_names(path):
    names={}
    for line in run([Path(run(['brew','--prefix','llvm'],text=True).strip())/'bin/llvm-nm',path],text=True).splitlines():
        parts=line.split()
        if len(parts)==3:names.setdefault(int(parts[0],16),parts[2])
    return names
def family_extents(path):
    return sorted((e['segment'],e['va'],e['size']) for e in json.loads(path.with_suffix('.json').read_text())['extents'] if e['module']==FAMILY)
extents=family_extents(reference_candidate)
require(extents and extents==family_extents(board_kernel),'The original display family lies at other addresses in this kernel')
read_reference,read_board=segment_reader(reference_candidate),segment_reader(board_kernel)
reference_names=board_names=None
for segment,va,size in extents:
    a,b=read_reference(va,size),read_board(va,size)
    if segment!='__DATA_CONST':
        require(a==b,f'The original display family {segment} differs in this kernel')
        continue
    for at in range(0,size,8):
        if a[at:at+8]==b[at:at+8]:continue
        if reference_names is None:reference_names,board_names=symbol_names(reference_candidate),symbol_names(board_kernel)
        x,y=struct.unpack_from('<Q',a,at)[0],struct.unpack_from('<Q',b,at)[0]
        require(reference_names.get(x) and reference_names.get(x)==board_names.get(y),
                f'{va+at:#x}: the original display family binds another kernel symbol in this kernel')
staged=ROOT/'build/xnu-prelink/native-owner-mobile-graphics-candidate.macho';staged.parent.mkdir(parents=True,exist_ok=True)
shutil.copyfile(reference_candidate,staged);shutil.copyfile(reference_candidate.with_suffix('.json'),staged.with_suffix('.json'))
observed=ROOT/'build/darwinboot-owner-hvf-mobile-graphics-legacy-class/gdb-stop.json';observed.parent.mkdir(parents=True,exist_ok=True)
shutil.copyfile(reference_observation,observed)
(ROOT/'build/display-analysis').mkdir(parents=True,exist_ok=True)   # the audit writes its result there without creating it
print(run(['python3',ROOT/'scripts/audit-mobile-graphics-legacy.py'],text=True),end='')
audit=json.loads((ROOT/'build/display-analysis/mobile-graphics-legacy-abi.json').read_text())
pure=audit['abstract_slots'];methods=audit['methods']
header='''// SPDX-License-Identifier: BSD-2-Clause-Patent
// Opaque ABI storage for the exact original class, not source-level access
// to its private fields. The actual original metadata supplies its full
// Framebuffer/Service/IOService ancestry. Only qualified base prototypes may
// be called from source; opaque entries are original dispatch targets only.
#pragma once
#include <IOKit/IOService.h>
#include <libkern/libkern.h>
struct OriginalLegacySwapRequest;
class IOMobileFramebufferLegacy : public IOService {
 OSDeclareAbstractStructors(IOMobileFramebufferLegacy)
 unsigned char originalPrivate[0x18a0-sizeof(IOService)];
public:
 // Both fields are qualified by the complete original slot-249 getter.
 // This modifies this object's own mode state, never a vtable or executable.
 void installBootDimensions(uint32_t width,uint32_t height) {
  *reinterpret_cast<uint32_t *>(reinterpret_cast<unsigned char *>(this)+5808)=width;
  *reinterpret_cast<uint32_t *>(reinterpret_cast<unsigned char *>(this)+5812)=height;
 }
 const OriginalLegacySwapRequest *pendingSwapRequest() const {
  const OriginalLegacySwapRequest *request=nullptr;
  memcpy(&request,reinterpret_cast<const unsigned char *>(this)+5480,sizeof(request));return request;
 }
 // The real normal scanout backend removes its pending head before calling
 // slot 295. Slot 312 is a discard path (terminal state 8), not presentation.
 // The queue is a tail queue (head at +5480, tail pointer at +5488, each request's next at +2960 and previous-link at +2968): taking the head
 // moves the next request, if a client already queued one behind it (int-132: swap 3 arrived during swap 2's GPU copy and the old
 // single-entry form refused to retire swap 2, so every wait on it stayed busy), up to the head. The original queue's reference
 // moves to the current-swap slot for the original completion body.
 bool activateOnlyPendingSwap(const OriginalLegacySwapRequest *request) {
  auto self=reinterpret_cast<unsigned char *>(this);
  auto bytes=reinterpret_cast<const unsigned char *>(request);
  const void *current=nullptr,*head=nullptr,*tail=nullptr,*next=nullptr,*previous=nullptr;
  memcpy(&current,self+5472,8);memcpy(&head,self+5480,8);memcpy(&tail,self+5488,8);
  if(!request || current || head!=request)return false;
  memcpy(&next,bytes+2960,8);memcpy(&previous,bytes+2968,8);
  if(previous!=self+5480)return false;
  if(next) {
   const void *link=self+5480;
   memcpy(const_cast<unsigned char *>(reinterpret_cast<const unsigned char *>(next))+2968,&link,8);
  } else if(tail!=bytes+2960)return false;
  memcpy(self+5472,&request,8);
  if(next) {memcpy(self+5480,&next,8);}
  else {const void *empty=nullptr,*emptyTail=self+5480;memcpy(self+5480,&empty,8);memcpy(self+5488,&emptyTail,8);}
  return true;
 }
 void free() override;
 bool init(OSDictionary *properties=nullptr) override;
 IOReturn setProperties(OSObject *) override;
 void systemWillShutdown(IOOptionBits) override;
 IOService *probe(IOService *,SInt32 *) override;
 bool start(IOService *) override;
 void stop(IOService *) override;
 IOWorkLoop *getWorkLoop() const override;
 IOReturn newUserClient(task_t,void *,UInt32,IOUserClient **) override;
 IOReturn setPowerState(unsigned long,IOService *) override;
'''
for i in range(168,358):
    if i==184:header+=' virtual bool isMainDisplay();\n'
    elif i==188:header+=' virtual IOReturn getDotPitch(uint32_t *);\n'
    elif i==213:header+=' virtual IOReturn supportedFrameInfo(uint32_t,uint32_t *,char *,uint32_t *)=0;\n'
    elif i==249:header+=' virtual void getDisplayDimensions(uint32_t *,uint32_t *);\n'
    elif i==290:header+=' virtual void kickSwapQueue();\n'
    elif i==296:header+=' virtual bool validateSwap(const OriginalLegacySwapRequest *);\n'
    elif i==301:header+=' virtual bool getRefreshAttributes(uint64_t *,bool);\n'
    elif i==295:header+=' virtual void completeCurrentSwap();\n'
    elif i==337:header+=' virtual IOReturn collectCompletionReports(uint32_t,void *,uint32_t *,uint32_t)=0;\n'
    elif i==288:header+=' virtual bool startHardware();\n'
    elif i in pure:header+=f' virtual __attribute__((noreturn)) void opaque{i}()=0;\n'
    else:header+=f' virtual __attribute__((deprecated("Unclassified ABI: never call from source"))) void opaque{i}();\n'
header+='};\nstatic_assert(sizeof(IOMobileFramebufferLegacy)==0x18a0,"Actual original instance extent");\n'
(OUT/'OriginalLegacyFramebufferABI.h').write_text(header)
print(run(['python3',ROOT/'scripts/build-arm-driver.py','NativeLegacyDisplay'],text=True),end='')
obj=OUT/'NativeLegacyDisplay.o';raw=obj.read_bytes();image=macho(raw)
st=next(p for c,p,_ in image['commands'] if c==2);sy,count,strings,_=struct.unpack_from('<4I',raw,st+8);symbols=[]
for i in range(count):
    ix,kind,section,_,va=struct.unpack_from('<IBBHQ',raw,sy+i*16)
    symbols.append((raw[strings+ix:raw.index(b'\0',strings+ix)].decode(),va,kind))
vtable=next(va for name,va,_ in symbols if name=='__ZTV19NativeLegacyDisplay');relocations={}
for segment in image['segments']:
    for sec in segment['sections']:
        if sec['va']<=vtable<sec['va']+sec['size']:
            ro,rc=struct.unpack_from('<2I',raw,sec['command_offset']+56)
            for i in range(rc):
                offset,bits=struct.unpack_from('<2I',raw,ro+i*8)
                if bits>>27&1 and bits>>28==0 and bits>>25&3==3:relocations[sec['va']+offset]=symbols[bits&0xffffff][0]
compiled=[relocations.get(vtable+16+i*8) for i in range(358)]
require(all(compiled) and vtable+16+358*8 not in relocations,'Exact owned 358-entry vtable')
candidate=ROOT/'build/xnu-prelink/native-owner-mobile-graphics-candidate.macho'
if not candidate.exists():  # pruned: same fallback as scripts/audit-mobile-graphics-legacy.py (identical original family executable)
    later=sorted((ROOT/'build/xnu-prelink').glob('native-owner-ufs-cpu-desktop-*-candidate.macho'),key=lambda f:f.stat().st_mtime)
    require(later,'No reference candidate available');candidate=later[-1]
old=candidate.read_bytes();old_image=macho(old)
names={n:v for v,group in exports(old,old_image).items() for n in group}
def read(va,n):
    at=backed_offset(old_image['segments'],va,n,len(old));return old[at:at+n]
bindings={'__ZN25IOMobileFramebufferLegacyC2EPK11OSMetaClass':0xfffffff0057865cc,
          '__ZN25IOMobileFramebufferLegacyD2Ev':0xfffffff005786618,
          '__ZN25IOMobileFramebufferLegacy10gMetaClassE':0xfffffff006b35940,
          '__ZTV25IOMobileFramebufferLegacy':int(audit['legacy_vptr'],16)-16}
owned=[];native=[]
for i,name in enumerate(compiled):
    expected=int(methods[i]['address'],16)
    if name.startswith(('__ZN19NativeLegacyDisplay','__ZNK19NativeLegacyDisplay')):
        owned.append(i);require(i in (0,1,7,14,84,86,87,141,155,184,188,288,290,296,301) or i in pure,'Only qualified own methods and guarded leaves override')
    elif '25IOMobileFramebufferLegacy' in name:
        require(i not in pure,'No unresolved original abstract backend target');bindings[name]=expected
    else:
        require(i<168 and names.get(name)==expected,'Actual inherited native base symbol '+str(i));native.append(i)
require(owned==sorted([0,1,7,14,84,86,87,141,155,184,188,288,290,296,301]+pure),'Exact owned method set')
undefined={n for n,_,k in symbols if k==1 and 'IOMobileFramebufferLegacy' in n}
require(undefined==set(bindings),'All and only actual original-class imports qualified')
# Exact original IOSurface getters, independently observed through the real
# original swap request. These aliases call original code; no object layout
# is substituted and no original method/vtable is patched.
for name,address,field,wide in [
    ('descriptor',0xfffffff004b9ead0,48,True),('width',0xfffffff004b9ebbc,88,True),
    ('height',0xfffffff004b9ebc4,96,True),('stride',0xfffffff004b9ebd4,104,True),
    ('allocation',0xfffffff004b9ebfc,144,True),('format',0xfffffff004b9ebac,128,False),
    ('planes',0xfffffff004b9ebf4,152,False)]:
    expected=(0xf9400000 if wide else 0xb9400000)|((field//(8 if wide else 4))<<10)
    require(read(address,8)==struct.pack('<2I',expected,0xd65f03c0),'Exact original surface getter '+name)
    bindings['_native_legacy_surface_'+name]=address
# The panel refresh rate of NativeMobileDisplay c031d88 (sheng-vsync=<Hz>), as UniXNU-sheng's copy of this script qualifies it; the
# target kernel cache is the same. The original display-refresh notification sender (self, timestamp, continuous host time, frame
# interval): it sends the six-word vsync message (type 5) to every client entry queued at +5592, the tail queue that selector 72's
# registration (slot 208) fills at +5512 + type*16. The platform subclass that normally calls it on each panel vblank is not part of
# this port.
require(read(0xfffffff005796720,16)==bytes.fromhex('ff0302d1fd7b07a9fdc30191a0831ff8') and
        read(0xfffffff005796830,4)==bytes.fromhex('08ed4af9') and
        read(0xfffffff00579685c,16)==bytes.fromhex('e2230091c3008052240080521684ff97') and
        read(0xfffffff005795fbc,4)==bytes.fromhex('0ab182d2'),
        'Original vsync sender, its +5592 queue and six-word drop-allowed async send')
bindings['_native_legacy_vsync_notify']=0xfffffff005796720
# Slot 301 fills the six refresh words that setDisplayRefreshProperties publishes as IOMFBDisplayRefresh (minimum interval, its mach
# time, maximum interval, its mach time, step, its mach time; intervals in 32.32 seconds): the generic body fixes the minimum at
# 0x04444444 (1/60 s), and the render server paces its display timer by it.
require(int(methods[301]['address'],16)==0xfffffff00579ea5c and read(0xfffffff00579ea68,8)==bytes.fromhex('888888d28888a0f2') and
        read(0xfffffff00579ec54,4)==bytes.fromhex('08b544f9'),
        'Original slot-301 refresh words with the 1/60 s minimum, called by setDisplayRefreshProperties')
# setDisplayRefreshProperties itself (self): fills the six words through slot 301 and publishes them as IOMFBDisplayRefresh. Nothing in
# the family calls it (the platform subclass does), so without it the render server falls back to its own 1/60 s heartbeat.
require(read(0xfffffff00579ebb8,16)==bytes.fromhex('ff0302d1fd7b07a9fdc30191a0031ff8') and
        read(0xfffffff00579ed54,16)==bytes.fromhex('086540f90173ff90218c0c9100013fd6'),
        'Original setDisplayRefreshProperties publishing IOMFBDisplayRefresh through setProperty')
bindings['_native_legacy_set_refresh_properties']=0xfffffff00579ebb8
# Both protected destructors/constructors must follow the exact original chain.
def branch(pc):
    word=struct.unpack('<I',read(pc,4))[0];require(word&0xfc000000==0x94000000,'Actual protected base call')
    value=word&0x3ffffff;return pc+4*(value-(1<<26) if value&(1<<25) else value)
require(branch(0xfffffff0057865fc)==0xfffffff00577b510 and branch(0xfffffff005786630)==0xfffffff00577b55c,
        'Original protected Legacy C2/D2 really call their original Service bases')
# Actual caller passes only self at slot 288 and tests bit zero of w0.
require(read(0xfffffff005787ef8,16)==bytes.fromhex('080040f9088144f900013fd6e0000037'),
        'Original bool start_hardware() call and result check')
getter=int(methods[249]['address'],16)
for offset,field in [(0x30,5808),(0x58,5812)]:
    require(struct.unpack('<I',read(getter+offset,4))[0]==0xb9400000|((field//4)<<10)|(8<<5)|8,
            'Actual original width/height getter fields')
require(int(methods[295]['address'],16)==0xfffffff005795b88 and
        read(0xfffffff005795b98,16)==bytes.fromhex('e00740f9280080520101001272d3ff97'),
        'Actual original slot-295 passes true to the normal current completion body')
require(read(0xfffffff00578aa7c,24)==bytes.fromhex('080040f9084545f9030080d2e20303aa0400805200013fd6'),
        'Original completion calls optional report slot 337 with null outputs and zero limit')
require(read(0xfffffff005777e9c,16)==bytes.fromhex('080040f908e142f900013fd6c0000036'),
        'Original isMainDisplay dispatch uses only self and tests boolean bit zero')
asm=OUT/'original-bindings.S';asm.write_text(''.join(f'.globl {name}\n.set {name}, {va:#x}\n' for name,va in sorted(bindings.items())))
sdk=run(['xcrun','--sdk','iphoneos','--show-sdk-path'],text=True).strip()
run(['xcrun','clang','-target','arm64-apple-ios17.0','-isysroot',sdk,'-c',asm,'-o',OUT/'original-bindings.o'])
llvm=Path(run(['brew','--prefix','llvm'],text=True).strip())/'bin';late=OUT/'NativeLegacyDisplay.late.o'
run([llvm/'llvm-objcopy','--remove-section=__DATA,__mod_init_func','--globalize-symbol=__GLOBAL__sub_I_NativeLegacyDisplay.cpp',obj,late])
def code(data,info):
    sec=next(s for g in info['segments'] for s in g['sections'] if s['name']=='__text');return data[sec['fileoff']:sec['fileoff']+sec['size']]
require(code(raw,image)==code(late.read_bytes(),macho(late.read_bytes())),'Only owned initializer section registration changes')
r=json.loads((ROOT/'build/xnu/ios-obj/DEVELOPMENT_ARM64_BCM2837/iokit/DEVELOPMENT/IOPlatformExpert.cpo.json').read_text().strip().rstrip(','))
cmd=[];args=r['arguments'];i=0;source=ROOT/'kits/tb128fu/LegacyDisplayProbe.cpp'
while i<len(args):
    value=args[i]
    if value==r['file']:cmd.append(str(source))
    elif value=='-o':cmd+=['-o',str(OUT/'LegacyDisplayProbe.o')];i+=1
    elif value=='-Xclang' and args[i+1].startswith('-W'):i+=1
    elif not value.startswith('-W'):cmd.append(value)
    i+=1
cmd+=['-Wall','-Wextra','-Werror','-Wno-unused-parameter','-Wno-unnecessary-virtual-specifier','-Wno-c99-designator','-Wno-reorder-init-list',
      '-Wno-deprecated-enum-enum-conversion','-I',str(ROOT/'.cache/xnu-work/osfmk')]
run(cmd,cwd=r['directory'])
keystore_source=ROOT/'kits/tb128fu/KeyStoreProbe.cpp'
keystore_obj=OUT/'KeyStoreProbe.o'
keystore_cmd=[str(keystore_source) if str(arg)==str(source) else str(keystore_obj) if str(arg)==str(OUT/'LegacyDisplayProbe.o') else arg for arg in cmd]
run(keystore_cmd,cwd=r['directory'])
hid_source=ROOT/'kits/tb128fu/HidInputProbe.cpp'
hid_obj=OUT/'HidInputProbe.o'
run([str(hid_source) if str(arg)==str(source) else str(hid_obj) if str(arg)==str(OUT/'LegacyDisplayProbe.o') else arg for arg in cmd],cwd=r['directory'])
touch_source=ROOT/'kits/tb128fu/TouchState.cpp'
touch_obj=OUT/'TouchState.o'
run([str(touch_source) if str(arg)==str(source) else str(touch_obj) if str(arg)==str(OUT/'LegacyDisplayProbe.o') else arg for arg in cmd],cwd=r['directory'])
# The touch controller (TouchProbe.cpp, tb128fu-touch=<stage>): the board probe with the kernel's C++ flags; the generic GENI
# SPI library, the Himax protocol and the firmware object with the kernel's C flags. The firmware is the tablet's own
# Himax_firmware.bin from its vendor partition (package/linux-log/build-log-reader.py --vendor-file), kept outside Git in the
# board's .cache/firmware/touch (TB128FU_TOUCH_FIRMWARE overrides it).
touch_dir=OUT/'touch';touch_dir.mkdir(exist_ok=True)
touch_includes=['-I',str(ROOT/'kits/tb128fu'),'-I',str(ROOT/'drivers/QcomGeniSpi')]
probe_source=ROOT/'kits/tb128fu/TouchProbe.cpp';probe_obj=touch_dir/'TouchProbe.o'
run([str(probe_source) if str(arg)==str(source) else str(probe_obj) if str(arg)==str(OUT/'LegacyDisplayProbe.o') else arg for arg in cmd]+touch_includes,
    cwd=r['directory'])
firmware=Path(os.environ.get('TB128FU_TOUCH_FIRMWARE') or ROOT/'.cache/firmware/touch/Himax_firmware.bin')
require(firmware.is_file(),f'Missing the touch firmware {firmware} (package/linux-log/build-log-reader.py --vendor-file firmware/Himax_firmware.bin)')
blob=firmware.read_bytes();firmware_c=touch_dir/'HimaxFirmware.c'
firmware_c.write_text(f'// {firmware.name}, sha256 {hashlib.sha256(blob).hexdigest()}\n#include <stdint.h>\n'
                      'const uint8_t native_touch_firmware[] __attribute__((aligned(16))) = {'+','.join(map(str,blob))+'};\n'
                      f'const uint32_t native_touch_firmware_size = {len(blob)};\n')
c_record=json.loads((ROOT/'build/xnu/ios-obj/DEVELOPMENT_ARM64_BCM2837/osfmk/DEVELOPMENT/startup.o.json').read_text().strip().rstrip(','))
def c_compile(c_source,c_obj):
    out=[];a=c_record['arguments'];j=0
    while j<len(a):
        value=a[j]
        if value==c_record['file']:out.append(str(c_source))
        elif value in ('-o','-MF','-MT','-MJ'):
            if value=='-o':out+=['-o',str(c_obj)]
            j+=1
        elif value=='-Xclang' and a[j+1].startswith('-W'):j+=1
        elif not value.startswith('-W'):out.append(value)
        j+=1
    run(out+['-Wall','-Wextra','-Werror']+touch_includes,cwd=c_record['directory'])
touch_objs=[probe_obj]
for c_source in (ROOT/'drivers/QcomGeniSpi/GeniSpi.c',ROOT/'kits/tb128fu/HimaxProtocol.c',firmware_c):
    c_obj=touch_dir/(c_source.stem+'.o');c_compile(c_source,c_obj);touch_objs.append(c_obj)
# The Adreno 610 bring-up probe (GpuProbe.cpp, tb128fu-gpu=<stage>) with the kernel's C++ flags, and its zap shader: the
# tablet's a610_zap.elf (the single-file form of a610_zap.mdt/.b0x) from its vendor partition, kept outside Git in the board's
# .cache/firmware/gpu (TB128FU_GPU_ZAP overrides it). Stage 7 drives NativeAdreno, whose archive (build-adreno610.py,
# CONFIG_XNU_NATIVE_ADRENO) also carries the PM4 packet helpers of kits/AdrenoPM4.
gpu_source=ROOT/'kits/tb128fu/GpuProbe.cpp';gpu_obj=touch_dir/'GpuProbe.o'
run([str(gpu_source) if str(arg)==str(source) else str(gpu_obj) if str(arg)==str(OUT/'LegacyDisplayProbe.o') else arg for arg in cmd]
    +['-I',str(ROOT/'kits/AdrenoPM4'),'-I',str(ROOT/'kits/ArmSmmu4K'),'-I',str(ROOT/'drivers/Adreno610'),
      '-I',str(ROOT/'.cache/xnu-work/osfmk')],cwd=r['directory'])
# The RPM requests over GLINK (RpmGlink.cpp) through which stage 9 of the GPU probe votes the CX level and the DDR clock.
rpm_source=ROOT/'kits/tb128fu/RpmGlink.cpp';rpm_obj=touch_dir/'RpmGlink.o'
run([str(rpm_source) if str(arg)==str(source) else str(rpm_obj) if str(arg)==str(OUT/'LegacyDisplayProbe.o') else arg for arg in cmd],
    cwd=r['directory'])
# The USB log channel (UsbLog.cpp): the DWC3 controller and its femto PHY, brought up in stages from the late probe.
usb_source=ROOT/'kits/tb128fu/UsbLog.cpp';usb_obj=touch_dir/'UsbLog.o'
run([str(usb_source) if str(arg)==str(source) else str(usb_obj) if str(arg)==str(OUT/'LegacyDisplayProbe.o') else arg for arg in cmd],
    cwd=r['directory'])
zap=Path(os.environ.get('TB128FU_GPU_ZAP') or ROOT/'.cache/firmware/gpu/a610_zap.elf')
require(zap.is_file(),f'Missing the GPU zap shader {zap} (package/linux-log/build-log-reader.py --vendor-file firmware/a610_zap.elf)')
zap_blob=zap.read_bytes();zap_c=touch_dir/'GpuZap.c'
zap_c.write_text(f'// {zap.name}, sha256 {hashlib.sha256(zap_blob).hexdigest()}\n#include <stdint.h>\n'
                 'const uint8_t tb128fu_gpu_zap[] __attribute__((aligned(16))) = {'+','.join(map(str,zap_blob))+'};\n'
                 f'const uint32_t tb128fu_gpu_zap_size = {len(zap_blob)};\n')
zap_obj=touch_dir/'GpuZap.o';c_compile(zap_c,zap_obj)
# The CP microcode, a630_sqe.fw from the same place (TB128FU_GPU_SQE overrides it).
sqe=Path(os.environ.get('TB128FU_GPU_SQE') or ROOT/'.cache/firmware/gpu/a630_sqe.fw')
require(sqe.is_file(),f'Missing the GPU microcode {sqe} (package/linux-log/build-log-reader.py --vendor-file firmware/a630_sqe.fw)')
sqe_blob=sqe.read_bytes();sqe_c=touch_dir/'GpuSqe.c'
sqe_c.write_text(f'// {sqe.name}, sha256 {hashlib.sha256(sqe_blob).hexdigest()}\n#include <stdint.h>\n'
                 'const uint8_t tb128fu_gpu_sqe[] __attribute__((aligned(16))) = {'+','.join(map(str,sqe_blob))+'};\n'
                 f'const uint32_t tb128fu_gpu_sqe_size = {len(sqe_blob)};\n')
sqe_obj=touch_dir/'GpuSqe.o';c_compile(sqe_c,sqe_obj)
touch_objs+=[gpu_obj,rpm_obj,usb_obj,zap_obj,sqe_obj]
# The display backend, its probe, the key store, the HID input and the touch controller: sheng's touch panel, CPU frequency and
# GPU power objects and QEMU's VirtIO input, shared window and Metal accelerator stay out (they drive other hardware).
archive=OUT/'libNativeLegacyDisplay.late.a';archive.unlink(missing_ok=True)
kernel=ROOT.parent/'kernel/kernel.macho'
reference=ROOT/'build/xnu-prelink/native-owner-ufs-cpu-desktop-tb128fu-candidate.macho'
reference.parent.mkdir(parents=True,exist_ok=True)
reference.write_bytes(kernel.read_bytes());reference.with_suffix('.json').write_bytes(kernel.with_suffix('.json').read_bytes())
print(run(['python3',ROOT/'scripts/build-sheng-hid-input.py'],text=True),end='')
hid_dir=ROOT/'build/sheng-hid-input'
print(run(['python3',ROOT/'scripts/build-arm-driver.py','AppleKeyStoreState'],text=True),end='')
keyevents=ROOT/'build/apple-key-store-state'
keyevents_late=keyevents/'NativeKeyStoreEvents.late.o'
run([llvm/'llvm-objcopy','--remove-section=__DATA,__mod_init_func',
     '--globalize-symbol=__GLOBAL__sub_I_NativeKeyStoreEvents.cpp',
     keyevents/'NativeKeyStoreEvents.o',keyevents_late])
require(code((keyevents/'NativeKeyStoreEvents.o').read_bytes(),macho((keyevents/'NativeKeyStoreEvents.o').read_bytes()))==
        code(keyevents_late.read_bytes(),macho(keyevents_late.read_bytes())),'Owned event-service instructions unchanged by late init staging')
run(['xcrun','ar','rcs',archive,late,OUT/'original-bindings.o',OUT/'LegacyDisplayProbe.o',keystore_obj,keyevents_late,
     hid_obj,touch_obj,hid_dir/'NativeHidInput.late.o',hid_dir/'hid-original-bindings.o',*touch_objs])
sources=[ROOT/'drivers/GopFramebuffer/NativeLegacyDisplay.cpp',ROOT/'drivers/GopFramebuffer/NativeLegacyRequest.h',source,
         keystore_source,ROOT/'drivers/AppleKeyStoreState/NativeKeyStoreEvents.cpp',hid_source,touch_source,
         ROOT/'drivers/ShengHidInput/NativeHidInput.cpp',ROOT/'scripts/build-sheng-hid-input.py',probe_source,
         ROOT/'kits/tb128fu/TouchBoard.h',ROOT/'kits/tb128fu/HimaxProtocol.c',ROOT/'kits/tb128fu/HimaxProtocol.h',
         ROOT/'drivers/QcomGeniSpi/GeniSpi.c',ROOT/'drivers/QcomGeniSpi/GeniSpi.h',firmware,gpu_source,rpm_source,zap,sqe]
print(run(['python3',ROOT/'scripts/build-arm-driver.py','NativeGOPPresenter'],text=True),end='')
gop=ROOT/'build/native-gop-presenter';gop_late=gop/'NativeGOPPresenter.late.o'
run([llvm/'llvm-objcopy','--remove-section=__DATA,__mod_init_func','--globalize-symbol=__GLOBAL__sub_I_NativeGOPPresenter.cpp',
     gop/'NativeGOPPresenter.o',gop_late])
require(code((gop/'NativeGOPPresenter.o').read_bytes(),macho((gop/'NativeGOPPresenter.o').read_bytes()))==
        code(gop_late.read_bytes(),macho(gop_late.read_bytes())),'Owned presenter instructions unchanged by late init staging')
run(['xcrun','ar','rcs',archive,gop_late])
sources.extend([ROOT/'drivers/GopFramebuffer/NativeGOPPresenter.cpp',ROOT/'drivers/GopFramebuffer/NativeGOPWire.h'])
(OUT/'build-report.json').write_text(json.dumps(dict(
    source_sha256={str(p.relative_to(ROOT)):sha(p) for p in sources},generated_abi_sha256=sha(OUT/'OriginalLegacyFramebufferABI.h'),
    original_candidate_sha256=sha(candidate),original_audit_sha256=sha(ROOT/'build/display-analysis/mobile-graphics-legacy-abi.json'),
    compiled_object_sha256=sha(obj),late_object_sha256=sha(late),archive_sha256=sha(archive),
    bindings={n:hex(v) for n,v in bindings.items()},virtual_entries=358,owned_entries=owned,inherited_native_entries=native,
    guarded_backend_leaves=[i for i in pure if i not in (213,337)],original_instructions_modified=False,driver_initialized=False,display_presented=False,desktop=False),indent=2)+'\n')
print('Built genuine original Legacy inheritance/dispatch; seven backend leaves still guarded, runtime lifecycle unqualified')
