#!/usr/bin/env python3
"""Execute production v143 timing patches, including their real emitted veneer."""
import argparse,hashlib,json,os,shlex,struct,subprocess,sys
from pathlib import Path
from unicorn import Uc,UC_ARCH_ARM64,UC_MODE_ARM
from unicorn.arm64_const import (UC_ARM64_REG_PC,UC_ARM64_REG_SP,UC_ARM64_REG_X0,
 UC_ARM64_REG_X8,UC_ARM64_REG_X9,UC_ARM64_REG_X19,UC_ARM64_REG_X22,
 UC_ARM64_REG_S0,UC_ARM64_REG_S1,UC_ARM64_REG_S2,UC_ARM64_REG_S3,
 UC_ARM64_REG_S8,UC_ARM64_REG_S9,UC_ARM64_REG_S10,UC_ARM64_REG_LR,
 UC_ARM64_REG_NZCV,UC_ARM64_REG_X25)
ROOT=Path(__file__).resolve().parents[1]
def main():
 parser=argparse.ArgumentParser(description=__doc__)
 parser.add_argument('--scratch',type=Path,default=ROOT/'analysis/v143-port')
 parser.add_argument('--source',type=Path,default=ROOT/'rewritten/v143/main.patched.bin')
 parser.add_argument('--layout',type=Path,default=ROOT/'analysis/v143-port/runtime-layout.json')
 parser.add_argument('--cc',default=os.environ.get('CC','xcrun clang' if sys.platform=='darwin' else 'clang'))
 args=parser.parse_args()
 out=args.scratch.resolve();out.mkdir(parents=True,exist_ok=True)
 source=args.source.resolve();layout_path=args.layout.resolve();os.chdir(ROOT)
 expected=json.loads((ROOT/'assets/expected-v1.4.3.json').read_text());raw=source.read_bytes()
 assert hashlib.sha256(raw).hexdigest()==expected['rewritten_sha256']['main.patched.bin']
 layout=json.loads(layout_path.read_text())[0]['layout']
 executable=str(out/'v143-timing-fixture')
 subprocess.run(shlex.split(args.cc)+['-D_DEFAULT_SOURCE','-std=c11','-O1','-Wall','-Wextra','-Werror','-Iruntime','scripts/v143_timing_fixture.c','runtime/game_patches_v143.c','android/gfx/host_test_support.c','-lm','-o',executable],check=True)
 target=out/'main-timing-fixture.bin'
 fixture_env={k:v for k,v in os.environ.items() if not k.startswith('TOTK_')}|{'TOTK_REALTIME':'1','TOTK_FPS60':'1','TOTK_OUTPUT_RES':'0','TOTK_ASPECT':'0','TOTK_CLOTH_REALTIME':'1','TOTK_HIGHFPS_FIX':'1'}
 # Steady high refresh and jitter must preserve elapsed time. Hitches retain
 # the per-interval 1/15 s cap; each pattern runs through the five-sample mean.
 patterns=[(1/120,),(1/144,),(1/160,),(1/240,),(1/240,1/80),(.004,.0127),(1/288,1/96),(.003,.0045,.006),(1/240,1/240,1/60),
           (1/120,)*15+(.2,),(.1,)]
 run=subprocess.run([executable,str(source),str(target),str(layout['TEXT_END'])]+[','.join(str(round(x*1e9)) for x in p) for p in patterns],capture_output=True,text=True,check=True,env=fixture_env)
 (out/'timing-fixture.log').write_text(run.stdout+run.stderr);samples=json.loads(run.stdout)
 for name,seconds in [('pin',0.01),('sample',0.025)]:
  assert all(abs(x-y)<1e-6 for x,y in zip(samples[name],[seconds*30,seconds,seconds*60]))
 realtime_cases=[]
 assert len(samples['realtime'])==len(patterns)
 for pattern,(real,game) in zip(patterns,samples['realtime']):
  # Startup/end lag of the five-sample average is bounded by a few presents.
  capped=[min(x,1/15) for x in pattern];expect=real*sum(capped)/sum(pattern)
  assert abs(game-expect)<=3*max(capped),('game time is not real time (hitches cut at 1/15 s)',pattern,real,game,expect)
  realtime_cases.append(dict(intervals_ms=[round(x*1e3,4) for x in pattern],real=real,game=game,ratio=game/real))
 # Same placement as game_patches_v143.c: the producer veneer (4 words) sits in
 # the zero padding after the text segment, so iOS can embed it in the signed RX
 # segment. It must end before read-only data starts, which is stricter than the
 # runtime's 16 KiB page bound.
 patched=target.read_bytes();data=(len(raw)+0x3fff)&~0x3fff;stub=(layout['TEXT_END']+15)&~15
 assert stub+16<=layout['RO_OFF'],'frame-step veneer would overlap read-only data'
 assert raw[stub:stub+16]==bytes(16),'frame-step veneer padding not empty in the rewritten image'
 changes=[o for o in range(0,len(raw),4) if raw[o:o+4]!=patched[o:o+4]]
 allowed={0x2db204,0x2db208,0x2db22c,0x2db360,0x2db36c,0x2db340,0x2db34c,0x3e5658,0x2daa54,0x2daa58,0x2daa6c,0x2daa70,0x2daa74,0x2daa78,
          0x246eac,0x345904,0x24944c,0x28bc98,0x628c74,0xb43460,0x1ff04f4,0x253fbb0,0x562a64}|{stub+4*i for i in range(4)}|{stub+48+4*i for i in range(32)}|{stub+176+4*i for i in range(19)}|{stub+256+4*i for i in range(5)}
 # FPS60 hardening (game_patches_v143.c, default on with TOTK_FPS60=1): its
 # 125-word veneer at +416 is always written; the four fade clamps install by
 # default and the six cutscene-floor sites only without framegen.
 fade_sites={0x0a7ac4,0x95dd24,0x6155e4,0x615660}
 floor_sites={0x45c700,0x2b23c7c,0x42c3bc,0xa5a188,0x2db238,0xa5a670}
 allowed|={stub+416+4*i for i in range(125)}|fade_sites|floor_sites
 assert set(changes)==allowed,[hex(o) for o in sorted(set(changes)^allowed)]
 def bits(value):return struct.unpack('<I',struct.pack('<f',value))[0]
 def scalar(word):return struct.unpack('<f',struct.pack('<I',word))[0]
 def consume_fixture(name,env,words=(),input_image=source,binary=executable):
  image=out/f'main-timing-{name}.bin'
  result=subprocess.run([binary,str(input_image),str(image),str(layout['TEXT_END']),'--consume']+[f'{word:08x}' for word in words],capture_output=True,text=True,check=True,env=env)
  (out/f'timing-{name}.log').write_text(result.stdout+result.stderr)
  observed=json.loads(result.stdout)
  assert observed['initial']==0 and observed['initialized_capture']==0,'published nominal seconds must not count as a consumed producer step'
  return image.read_bytes(),observed
 adaptive_env=fixture_env|{'TOTK_FRAMEGEN':'adaptive'}
 adaptive_patched,adaptive_getter=consume_fixture('adaptive',adaptive_env)
 assert stub+20<=stub+48<=layout['RO_OFF'],'adaptive producer overlaps reserved veneer/read-only data'
 assert raw[stub:stub+20]==bytes(20),'adaptive producer padding is not empty'
 adaptive_changes={o for o in range(0,len(raw),4) if raw[o:o+4]!=adaptive_patched[o:o+4]}
 adaptive_allowed=(allowed-floor_sites)|{stub+16}
 assert adaptive_changes==adaptive_allowed,[hex(o) for o in sorted(adaptive_changes^adaptive_allowed)]
 assert {o for o in range(0,len(raw),4) if patched[o:o+4]!=adaptive_patched[o:o+4]}=={stub+12,stub+16}|floor_sites,'adaptive changed a legacy instruction outside its extra store/resume and the framegen-gated cutscene floor'
 assert adaptive_patched[stub:stub+12]==patched[stub:stub+12]
 assert struct.unpack_from('<I',adaptive_patched,stub+12)[0]==0xbd000d08,'adaptive must store selected s8 ticks at page+12'
 assert struct.unpack_from('<I',adaptive_patched,stub+16)[0]==0x14000000|(((0x3e565c-stub-16)>>2)&0x3ffffff)
 assert adaptive_patched[stub+20:stub+48]==patched[stub+20:stub+48]
 def check_getter(observed,selected,words):
  assert len(observed['samples'])==len(words)
  for word,(taken,again,left) in zip(words,observed['samples']):
   ticks=scalar(word)
   expected=scalar(bits(ticks/60)) if selected and 0<ticks<float('inf') else 0
   assert abs(taken-expected)<1e-9,(hex(word),taken,expected)
   assert again==0,'a second guest present reused the last producer step'
   if selected:assert left==0,'getter did not consume the captured word'
  for reason,(taken,again,left) in observed['reset'].items():
   expected=scalar(bits(1.5/60)) if selected and reason=='fresh' else 0
   assert abs(taken-expected)<1e-9 and again==0,(reason,taken,again,expected)
   if selected:assert left==0,('stale capture after reset/pin',reason,left)
 check_getter(adaptive_getter,True,())
 # Every old/non-exact selection emits the exact original four-word veneer. A
 # non-empty, non-0 TOTK_FRAMEGEN also leaves the six cutscene-floor sites original.
 def without_floor(image):
  kept=bytearray(image[:len(raw)])
  for o in floor_sites:kept[o:o+4]=raw[o:o+4]
  return bytes(kept)
 legacy_modes=(None,'','0','1','2','locked','auto','Adaptive','adaptive ',' adaptive')
 legacy_selection_cases=[]
 for index,mode in enumerate(legacy_modes):
  env=fixture_env if mode is None else fixture_env|{'TOTK_FRAMEGEN':mode}
  image,observed=consume_fixture(f'legacy-{index}',env,(bits(1.5),))
  reference=patched[:len(raw)] if mode in (None,'','0') else without_floor(patched)
  assert image[:len(raw)]==reference,('legacy emitted code changed',mode)
  check_getter(observed,False,(bits(1.5),))
  legacy_selection_cases.append(mode)
 exported,export_getter=consume_fixture('ios-export',adaptive_env|{'TOTK_EXPORT_IOS':'fixture'},(bits(1.5),))
 assert exported[:len(raw)]==without_floor(patched),'iOS export acquired adaptive instrumentation'
 check_getter(export_getter,False,(bits(1.5),))
 # On Linux the same source can also exercise the iOS compile-time branch
 # without an Apple SDK; it checks an already-embedded legacy image read-only.
 ios_build_checked=False
 if sys.platform.startswith('linux'):
  ios_executable=str(out/'v143-timing-ios-fixture')
  subprocess.run(shlex.split(args.cc)+['-D_DEFAULT_SOURCE','-DTARGET_OS_IPHONE=1','-DTARGET_OS_OSX=0','-std=c11','-O1','-Wall','-Wextra','-Werror','-Iruntime','scripts/v143_timing_fixture.c','runtime/game_patches_v143.c','android/gfx/host_test_support.c','-lm','-o',ios_executable],check=True)
  embedded_source=out/'main-timing-embedded-fixture.bin';embedded_source.write_bytes(without_floor(patched))
  embedded,embedded_getter=consume_fixture('ios-build',adaptive_env,(bits(1.5),),embedded_source,ios_executable)
  assert embedded[:len(raw)]==without_floor(patched),'iOS compile-time path changed embedded legacy code'
  check_getter(embedded_getter,False,(bits(1.5),));ios_build_checked=True
 def execute(seconds,apply,half=False,call=0x3e5720,nominal_fps=60,image=patched,captures=None):
  mu=Uc(UC_ARCH_ARM64,UC_MODE_ARM);mu.mem_map(0,len(image));mu.mem_write(0,image if apply else raw)
  mu.mem_map(0x10000000,0x20000);mu.mem_write(data,struct.pack('<ffff',seconds*30,seconds,seconds*60,0))
  mu.reg_write(UC_ARM64_REG_SP,0x10013000)
  # Execute the actual selector and producer, not a synthesized callee input.
  # x8 points to the FrameRateMgr singleton, x9 is a LIVE task flag after this
  # producer. A veneer that uses x9 as scratch would fail this check.
  mu.reg_write(UC_ARM64_REG_X8,0x1001f000);mu.mem_write(0x1001f000,struct.pack('<Q',0x10000000))
  mu.reg_write(UC_ARM64_REG_X19,0x10003000);mu.mem_write(0x100034f8,struct.pack('<I',0x13572468))
  mu.reg_write(UC_ARM64_REG_X22,0x10002000);mu.mem_write(0x10002250,struct.pack('<I',nominal_fps))
  mu.mem_write(0x100021da,bytes([int(half)]));mu.reg_write(UC_ARM64_REG_S9,bits(1.0))
  mu.emu_start(0x3e562c,0x3e565c,count=100);assert mu.reg_read(UC_ARM64_REG_PC)==0x3e565c
  assert mu.reg_read(UC_ARM64_REG_X9)==0x13572468,'live producer flags clobbered'
  interval=struct.unpack('<f',struct.pack('<I',mu.reg_read(UC_ARM64_REG_S8)))[0]
  captured=struct.unpack('<I',mu.mem_read(data+12,4))[0]
  if apply and image is adaptive_patched:
   assert captured==mu.reg_read(UC_ARM64_REG_S8),'captured interval differs from actual post-selector s8'
  else:assert captured==0,'legacy producer wrote an adaptive capture'
  want_interval=(seconds*60 if apply else 60/nominal_fps)*(.5 if half else 1)
  assert abs(interval-want_interval)<1e-6,(interval,want_interval)
  # Both original call paths copy s8 and call the untouched FrameRateMgr.
  mu.emu_start(call,0x3e684c,count=100);assert mu.reg_read(UC_ARM64_REG_PC)==0x3e684c
  ratio,inverse,dt,fps=struct.unpack('<fffi',mu.mem_read(0x10000010,16))
  mu.reg_write(UC_ARM64_REG_X19,0x10000000);mu.mem_write(0x10000250,struct.pack('<I',60))
  mu.emu_start(0x2daa54,0x2daa80,count=40);assert mu.reg_read(UC_ARM64_REG_PC)==0x2daa80
  global_ratio=struct.unpack('<f',mu.mem_read(0x3a22128,4))[0]
  if captures is not None:captures.append(dict(ticks_bits=captured,seconds=dt))
  return ratio,inverse,dt,fps,global_ratio
 cases=[]
 for hz in (15,20,25,30,50,60,90,100,120):
  for half in (False,True):
   scale=.5 if half else 1
   for call in (0x3e5720,0x3e583c):
    for nominal in (30,60,100):
     result=execute(1/hz,True,half,call,nominal);want=(30/hz*scale,hz/30/scale,scale/hz,int(hz/scale),30/hz)
     assert all(abs(x-y)<1e-5 for x,y in zip(result,want)),(hz,half,call,nominal,result,want)
     cases.append(dict(hz=hz,half_step=half,call=hex(call),nominal_fps=nominal,ratio=result[0],inverse=result[1],seconds=result[2],rounded_fps=result[3],global_ratio=result[4]))
 negative=[execute(1/50,False,half) for half in (False,True)]
 assert abs(negative[0][2]-1/60)<1e-6 and abs(negative[1][2]-1/120)<1e-6
 assert abs(negative[0][2]-1/50)>0.001 and abs(negative[1][2]-1/100)>0.001
 # Exercise the adaptive-only extra STR through the original selector, both
 # original caller paths and unchanged FrameRateMgr arithmetic. Include awkward
 # rates that distinguish effective consumed float time from nominal targets.
 adaptive_cases=[];captures=[]
 for hz in (25,30,43,59,60,120):
  for half in (False,True):
   for call in (0x3e5720,0x3e583c):
    for nominal in (30,60,100):
     result=execute(1/hz,True,half,call,nominal,adaptive_patched,captures)
     legacy_result=execute(1/hz,True,half,call,nominal)
     assert result==legacy_result,'adaptive capture changed FrameRateMgr arithmetic'
     ticks=scalar(captures[-1]['ticks_bits']);consumed=scalar(bits(ticks/60))
     assert abs(bits(consumed)-bits(result[2]))<=1,('capture does not equal effective consumed seconds',hz,half,consumed,result[2])
     adaptive_cases.append(dict(hz=hz,half_step=half,call=hex(call),nominal_fps=nominal,captured_ticks=ticks,consumed_seconds=result[2]))
 invalid_words=(0,0x80000000,bits(-1),0x7f800000,0xff800000,0x7fc00000)
 capture_words=[sample['ticks_bits'] for sample in captures]
 _,consumed_getter=consume_fixture('adaptive-consumed',adaptive_env,tuple(capture_words)+invalid_words)
 check_getter(consumed_getter,True,tuple(capture_words)+invalid_words)
 # Advance a nanosecond timestamp from the real native getter values, retaining
 # fractional ns like the adaptive runtime. It must track FrameRateMgr's actual
 # float step (including half steps), not the next published nominal seconds.
 timestamp=0;fraction=0.0;effective_ns=0.0;rounding_bound=0.0
 for sample,(taken,_,_) in zip(captures,consumed_getter['samples']):
  exact=scalar(bits(scalar(sample['ticks_bits'])/60))
  ns=taken*1e9+fraction;step=int(ns+.5);fraction=ns-step;timestamp+=step
  effective_ns+=sample['seconds']*1e9
  rounding_bound+=abs(exact-sample['seconds'])*1e9+abs(exact-taken)*1e9
  assert abs(timestamp-effective_ns)<=rounding_bound+.501,'captured timestamp interval differs from consumed simulation dt'
 captured_timestamp=dict(ns=timestamp,consumed_ns=effective_ns,float_rounding_bound_ns=rounding_bound)
 # Execute the original callers, including their fixed 1/30 constructors and
 # the production veneers. Checking only FrameRateMgr missed this second clock.
 assert stub+176+19*4<=layout['RO_OFF'],'cloth veneers overlap read-only data'
 def cloth_clock(hz,factor,site,apply=True):
  mu=Uc(UC_ARCH_ARM64,UC_MODE_ARM);mu.mem_map(0,len(patched));mu.mem_write(0,patched if apply else raw)
  mu.mem_map(0x10000000,0x20000);sp=0x10010000;mu.reg_write(UC_ARM64_REG_SP,sp)
  mu.mem_write(data,struct.pack('<fff',30/hz,1/hz,60/hz))
  def put(reg,value):mu.reg_write(reg,bits(value))
  def get(reg):return struct.unpack('<f',struct.pack('<I',mu.reg_read(reg)))[0]
  # Ordinary animation has already been scaled: it must not be scaled twice.
  ordinary=(.7/hz,.7*30/hz)
  mu.reg_write(UC_ARM64_REG_X9,0x12345678)
  mu.reg_write(UC_ARM64_REG_NZCV,0xa0000000)
  if site==0x246eac:
   start,end=0x246e78,0x246ebc
   put(UC_ARM64_REG_S10,factor);mu.mem_write(sp+0x138,struct.pack('<ff',ordinary[1],ordinary[0]))
   mu.emu_start(start,end,count=100)
   result=struct.unpack('<ff',mu.mem_read(sp+0x360,8))
   normal=struct.unpack('<ff',mu.mem_read(sp+0x2e0,8))
   assert abs(get(UC_ARM64_REG_S10)-factor)<1e-6,'callee-saved time factor changed'
  elif site==0x345904:
   start,end=0x3458d8,0x345910
   put(UC_ARM64_REG_S0,factor);put(UC_ARM64_REG_S9,ordinary[0]);put(UC_ARM64_REG_S8,ordinary[1])
   mu.emu_start(start,end,count=100)
   result=struct.unpack('<ff',mu.mem_read(sp+0x110,8))
   normal=struct.unpack('<ff',mu.mem_read(sp+0x90,8))
   assert abs(get(UC_ARM64_REG_S0)-factor)<1e-6,'original factor changed'
  else:
   start,end=site,0x2a0e40
   put(UC_ARM64_REG_S0,ordinary[0]);put(UC_ARM64_REG_S1,ordinary[1])
   put(UC_ARM64_REG_S2,factor/30);put(UC_ARM64_REG_S3,factor)
   mu.emu_start(start,end,count=100)
   result=(get(UC_ARM64_REG_S2),get(UC_ARM64_REG_S3))
   normal=(get(UC_ARM64_REG_S0),get(UC_ARM64_REG_S1))
   assert mu.reg_read(UC_ARM64_REG_LR)==site+4,'original callee return address changed'
  assert mu.reg_read(UC_ARM64_REG_PC)==end,'veneer did not return to original code'
  assert mu.reg_read(UC_ARM64_REG_SP)==sp,'guest stack changed'
  assert mu.reg_read(UC_ARM64_REG_NZCV)==0xa0000000,'live condition flags changed'
  if site!=0x345904:assert mu.reg_read(UC_ARM64_REG_X9)==0x12345678,'live integer register changed'
  assert all(abs(a-b)<1e-6 for a,b in zip(normal,ordinary)),'ordinary clock was scaled twice'
  return result
 cloth_cases=[]
 for hz in (15,20,30,40,50,60,90,100,120):
  for factor in (0,.25,1,2):
   for site in (0x246eac,0x345904,0x24944c,0x28bc98):
    result=cloth_clock(hz,factor,site);want=(factor/hz,factor*30/hz)
    assert all(abs(a-b)<1e-6 for a,b in zip(result,want)),(hz,factor,hex(site),result,want)
    cloth_cases.append(dict(hz=hz,factor=factor,site=hex(site),seconds=result[0],ratio=result[1]))
 cloth_negative=[cloth_clock(120,1,site,False) for site in (0x246eac,0x345904,0x24944c,0x28bc98)]
 assert all(abs(dt*120-4)<1e-5 for dt,ratio in cloth_negative),'negative control must reproduce 4x wind clock at 120 FPS'
 # The third out-of-line caller already uses measured time. Leave it alone.
 assert patched[0x2afac18:0x2afac1c]==raw[0x2afac18:0x2afac1c]
 # A mismatched game instruction must fail closed, for each new patch site.
 bad_source=out/'invalid-cloth-fixture.bin'
 try:
  for site in (0x246eac,0x345904,0x24944c,0x28bc98):
   bad=bytearray(raw);bad[site]^=1;bad_source.write_bytes(bad)
   rejected=subprocess.run([executable,str(bad_source),str(out/'rejected-cloth-fixture.bin'),str(layout['TEXT_END'])],capture_output=True,text=True,env=fixture_env)
   assert rejected.returncode!=0 and 'secondary animation instruction mismatch' in rejected.stderr,(hex(site),rejected.stderr)
 finally:bad_source.unlink(missing_ok=True)
 # Trampoline slow-motion check (#61): the contact code compares the time object's FPS (X+0x28) with 100. The veneer
 # must compare FPS * 30 Hz ratio (X+0x38) = 30 / time scale, keep the original 100.0 in s1 and return to 0x562a6c.
 def bounce_check(hz,scale,apply=True):
  mu=Uc(UC_ARCH_ARM64,UC_MODE_ARM);mu.mem_map(0,len(patched));mu.mem_write(0,patched if apply else raw)
  mu.mem_map(0x10000000,0x10000);holder,obj,x=0x10000000,0x10001000,0x10002000
  ratio=30/hz;mu.mem_write(holder,struct.pack('<Q',obj));mu.mem_write(obj+0xc8,struct.pack('<Q',x))
  mu.mem_write(x+0x28,struct.pack('<f',30/(ratio*scale)));mu.mem_write(x+0x38,struct.pack('<f',ratio))
  mu.reg_write(UC_ARM64_REG_X25,holder);mu.emu_start(0x562a5c,0x562a70,count=20)
  assert mu.reg_read(UC_ARM64_REG_PC)==0x562a70,'bounce veneer did not return to original code'
  assert mu.reg_read(UC_ARM64_REG_S1)==0x42c80000,'bounce check threshold changed'
  return struct.unpack('<f',struct.pack('<I',mu.reg_read(UC_ARM64_REG_S0)&0xffffffff))[0]
 bounce_cases=[]
 for hz in (30,60,100,120):
  for scale in (1,.5,.25,.1):
   got=bounce_check(hz,scale);assert abs(got-30/scale)<1e-3,(hz,scale,got)
   bounce_cases.append(dict(hz=hz,scale=scale,compared=got))
 assert bounce_check(120,1,False)>100,'negative control: the original check fires at 120 FPS with time running normally'
 assert abs(bounce_check(30,.25,False)-bounce_check(30,.25))<1e-3,'at 30 Hz the patched check must equal the original'
 report=dict(status='pass',version='v143',scope='production ARM64 timing and secondary cloth/hair clock veneers; original clock arithmetic, half-step selector, adaptive consumed-step capture/getter/reset, legacy and iOS byte preservation, slow motion, zero factor, both inline/delegated actor paths, ordinary clock and ABI preservation',cases=cases,realtime_cases=realtime_cases,negative_control=negative,adaptive_cases=adaptive_cases,captured_timestamp=captured_timestamp,consumed_getter=consumed_getter,legacy_selection_cases=legacy_selection_cases,ios_export_checked=True,ios_build_checked=ios_build_checked,cloth_cases=cloth_cases,cloth_negative_control=cloth_negative,cloth_instruction_guard_negative_controls=4,bounce_cases=bounce_cases,veneer_offset=hex(stub),changed_original_words=len(changes),adaptive_changed_original_words=len(adaptive_changes),sources={p:hashlib.sha256((ROOT/p).read_bytes()).hexdigest() for p in ('runtime/game_patches_v143.c','scripts/v143_timing_fixture.c','scripts/verify_v143_timing.py')})
 (out/'realtime-validation.json').write_text(json.dumps(report,indent=2)+'\n');print(json.dumps(dict(status='pass',cases=len(cases),adaptive_cases=len(adaptive_cases),legacy_selection_cases=len(legacy_selection_cases),ios_build_checked=ios_build_checked,realtime_ratios=[round(c['ratio'],6) for c in realtime_cases],cloth_cases=len(cloth_cases),cloth_negative_control=cloth_negative,bounce_cases=len(bounce_cases),changed_original_words=len(changes))))
if __name__=='__main__':main()
