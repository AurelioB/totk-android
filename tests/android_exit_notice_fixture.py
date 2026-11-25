"""Host fixture for real exit policy/store/adapter; only Android and UI dispatch are doubles.

The compatibility probe can exercise 253aca1's original interfaces as well as R2.
This lets the R2 regression assertions fail behaviorally on R1, not merely fail to compile.
"""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]

STUBS = {
    'android/os/Build.java': 'package android.os; public class Build { public static class VERSION { public static int SDK_INT=33; } }',
    'android/os/Process.java': 'package android.os; public class Process { public static int myPid(){return (int)ProcessHandle.current().pid();}public static int myUid(){return 100;} }',
    'android/os/SystemClock.java': 'package android.os; public class SystemClock { public static long elapsedRealtime(){return System.nanoTime()/1000000;} }',
    'android/os/Looper.java': '''package android.os;
public class Looper {private static final Looper main=new Looper();public static Looper getMainLooper(){return main;}
 public static Looper myLooper(){return Thread.currentThread().getName().equals("FakeUI")?main:null;}}''',
    'android/content/pm/PackageInfo.java': '''package android.content.pm;
public class PackageInfo {public long firstInstallTime=1,lastUpdateTime=9000;public long getLongVersionCode(){return 7;}}''',
    'android/content/pm/PackageManager.java': '''package android.content.pm;
public class PackageManager {public final PackageInfo info=new PackageInfo();
 public PackageInfo getPackageInfo(String name,int flags){com.aure.totk.ProbeUi.notUi("package metadata");return info;}}''',
    'android/content/pm/ApplicationInfo.java': 'package android.content.pm; public class ApplicationInfo {public String sourceDir="/installed/base.apk";}',
    'android/content/Context.java': '''package android.content;
import java.io.*;import android.app.*;import android.content.pm.*;
public class Context {
 public File files;public ActivityManager manager=new ActivityManager();public PackageManager packages=new PackageManager();public ApplicationInfo appInfo=new ApplicationInfo();
 public Context getApplicationContext(){return this;}public File getFilesDir(){return files;}public String getPackageName(){return "com.aure.totk";}
 public <T>T getSystemService(Class<T> type){return type.cast(manager);}public PackageManager getPackageManager(){return packages;}
 public ApplicationInfo getApplicationInfo(){return appInfo;}
}''',
    'android/app/Activity.java': '''package android.app;
import android.content.Context;import com.aure.totk.ProbeUi;
public class Activity extends Context {
 public volatile boolean destroyed,finishing;public volatile Runnable beforeResult;
 public Activity(Context app){files=app.files;manager=app.manager;packages=app.packages;appInfo=app.appInfo;}
 public boolean isDestroyed(){return destroyed;}public boolean isFinishing(){return finishing;}
 public void runOnUiThread(Runnable action){ProbeUi.post(()->{Runnable before=beforeResult;beforeResult=null;if(before!=null)before.run();action.run();});}
}''',
    'android/app/ApplicationExitInfo.java': '''package android.app;
public class ApplicationExitInfo {
 public String name="com.aure.totk",token="";public int pid=999999,reason=3,status,uid=100;public long stamp=20000;
 public String getProcessName(){return name;}public int getPid(){return pid;}public int getRealUid(){return uid;}
 public int getReason(){return reason;}public int getStatus(){return status;}public int getImportance(){return 400;}
 public long getTimestamp(){return stamp;}public long getPss(){return 1234;}public long getRss(){return 2345;}
 public byte[] getProcessStateSummary(){return token.getBytes(java.nio.charset.StandardCharsets.UTF_8);}
}''',
    'android/app/ActivityManager.java': '''package android.app;
import java.util.*;import java.util.concurrent.*;import java.util.concurrent.atomic.*;
public class ActivityManager {
 public final List<ApplicationExitInfo> exits=new CopyOnWriteArrayList<>();public final AtomicInteger queries=new AtomicInteger();
 public final CountDownLatch queried=new CountDownLatch(1);public volatile byte[] summary;public volatile boolean unavailable;
 public List<ApplicationExitInfo> getHistoricalProcessExitReasons(String name,int pid,int limit){
  com.aure.totk.ProbeUi.notUi("history");queries.incrementAndGet();queried.countDown();
  if(unavailable)throw new SecurityException("history unavailable");return exits;
 }
 public void setProcessStateSummary(byte[] value){com.aure.totk.ProbeUi.notUi("process summary");summary=value;}
 public List<RunningAppProcessInfo> getRunningAppProcesses(){return Collections.emptyList();}
 public static class RunningAppProcessInfo{public int pid,uid;public String processName;}
}''',
    'android/view/KeyEvent.java': '''package android.view;
public class KeyEvent {public static final int KEYCODE_BUTTON_A=96,KEYCODE_BUTTON_B=97,ACTION_UP=1;
 public int getAction(){return ACTION_UP;}public boolean isCanceled(){return false;}}''',
    'android/app/AlertDialog.java': '''package android.app;
import android.view.KeyEvent;
public class AlertDialog {
 public static final int BUTTON_POSITIVE=-1;public static volatile int shown;public static volatile AlertDialog last;
 public String message;public boolean focused;public Activity owner;private Dismiss dismiss;private Key key;
 public interface Click{void run(AlertDialog d,int which);}public interface Dismiss{void run(AlertDialog d);}
 public interface Key{boolean run(AlertDialog d,int code,KeyEvent event);}
 public static class Builder{
  final AlertDialog d=new AlertDialog();public Builder(Activity a){d.owner=a;}
  public Builder setTitle(String s){return this;}public Builder setMessage(String s){d.message=s;return this;}
  public Builder setPositiveButton(String s,Click c){return this;}public AlertDialog create(){return d;}
 }
 public void setCanceledOnTouchOutside(boolean b){}public void setOnDismissListener(Dismiss d){dismiss=d;}
 public void setOnKeyListener(Key k){key=k;}public void show(){if(owner.destroyed)throw new IllegalStateException("destroyed window");shown++;last=this;}
 public class Button{public void requestFocus(){focused=true;}}public Button getButton(int n){return new Button();}
 public void dismiss(){if(dismiss!=null){Dismiss d=dismiss;dismiss=null;d.run(this);}}
 public void controller(){if(!key.run(this,KeyEvent.KEYCODE_BUTTON_A,new KeyEvent()))throw new AssertionError("controller not consumed");}
}''',
    'android/util/Log.java': '''package android.util;
public class Log {public static final java.util.concurrent.atomic.AtomicInteger lines=new java.util.concurrent.atomic.AtomicInteger();public static volatile String last;
 public static int i(String t,String m){com.aure.totk.ProbeUi.notUi("diagnostic append/logcat");lines.incrementAndGet();last=m;return 0;}
 public static int w(String t,String m,Throwable e){com.aure.totk.ProbeUi.notUi("diagnostic error");return 0;}}''',
    'com/aure/totk/ProbeUi.java': '''package com.aure.totk;
import java.util.concurrent.*;import java.util.concurrent.atomic.*;import java.security.Permission;
public class ProbeUi {
 public static volatile boolean guard;public static volatile String files;
 public static final AtomicReference<Throwable> failure=new AtomicReference<>();
 public static final ExecutorService ui=Executors.newSingleThreadExecutor(r->{Thread t=new Thread(r,"FakeUI");t.setDaemon(true);return t;});
 public static void post(Runnable r){ui.execute(()->{try{r.run();}catch(Throwable t){failure.compareAndSet(null,t);}});}
 public static void run(Runnable r)throws Exception{ui.submit(r).get(5,TimeUnit.SECONDS);checkFailure();}
 public static void checkFailure(){Throwable t=failure.get();if(t!=null)throw new AssertionError("UI dispatch failed",t);}
 public static void notUi(String operation){if(guard&&Thread.currentThread().getName().equals("FakeUI"))throw new AssertionError(operation+" on UI thread");}
 @SuppressWarnings("removal") public static void installIoGuard(String directory){files=directory;System.setSecurityManager(new SecurityManager(){
  public void checkPermission(Permission p){}public void checkRead(String path){check(path);}public void checkWrite(String path){check(path);}
  private void check(String path){if(path!=null&&(path.startsWith(files)||path.startsWith("/proc/")))notUi("receipt/proc/log IO: "+path);}
 });guard=true;}
}''',
}

MODERN = r'''

    static void contractStore(Context app)throws Exception{
        ApplicationExitInfo e=exit();Object first=receipt(app,90000,e.pid,e.token);
        check(claim(app,first,e)&&!claim(app,first,e),"persistent notice-once failed");
        ExitNoticeStore store=new ExitNoticeStore(app.files);
        Object second=receipt(app,1000,e.pid,TOKEN+"-next");e.token=TOKEN+"-next";
        ExitNoticeStore.Receipt r=(ExitNoticeStore.Receipt)second;
        ExitNoticeStore.Owner owner=new ExitNoticeStore.Owner("boot",42,123);
        ExitNoticeStore.Reservation claim=store.reserve(r,fact(e),"reserved",owner,o->true);
        check(claim.status==ExitNoticeStore.Status.READY,"reservation missing");
        check(store.reserve(r,fact(e),"other",owner,o->true).status==ExitNoticeStore.Status.BUSY,"concurrent delivery not excluded");
        store.release(claim.claim);
        ExitNoticeStore.Reservation retry=store.reserve(r,fact(e),"recreated",owner,o->true);
        check(retry.status==ExitNoticeStore.Status.READY&&store.shown(retry.claim),"unshown recreation lost notice");
        Object third=receipt(app,500,e.pid,TOKEN+"-orphan");e.token=TOKEN+"-orphan";
        store.reserve((ExitNoticeStore.Receipt)third,fact(e),"dead-process",owner,o->true);
        check(store.reserve((ExitNoticeStore.Receipt)third,fact(e),"reboot",owner,o->false).status==ExitNoticeStore.Status.GONE,"ambiguous dead-owner delivery repeated");
        Object fourth=receipt(app,400,e.pid,TOKEN+"-new-record");e.token=TOKEN+"-new-record";
        check(claim(app,fourth,e),"orphan/PID reuse hid a different record");
        for(int i=0;i<25;i++)receipt(app,0,999999,TOKEN+"-bounded-"+i);
        check(store.receipts().size()==16,"receipt count not bounded");
        File env=new File(app.files,"totk.env");check(ExitNoticeStore.enabled(env),"notice not default on");
        for(String value:new String[]{"0","'0'","\"0\""}){
            Files.writeString(env.toPath(),"export TOTK_EXIT_NOTICE="+value+"\n");check(!ExitNoticeStore.enabled(env),"exact zero not silent");
        }
        for(String value:new String[]{"1","00","false"," 0","0 # comment"}){
            Files.writeString(env.toPath(),"TOTK_EXIT_NOTICE="+value+"\n");check(ExitNoticeStore.enabled(env),"invalid value silenced");
        }
        Files.writeString(env.toPath(),"TOTK_EXIT_NOTICE=0\nTOTK_EXIT_NOTICE=1\n");check(ExitNoticeStore.enabled(env),"last assignment lost");
        ExitNoticePolicy.Session session=((ExitNoticeStore.Receipt)fourth).session;ApplicationExitInfo wrong=exit();
        wrong.uid=101;check(ExitNoticePolicy.classify(session,app.getPackageName(),100,install(app),false,new ExitNoticePolicy.Exit[]{fact(wrong)}).exit==null,"wrong UID matched");
        wrong.uid=100;wrong.token=session.token;wrong.pid=12345;
        check(ExitNoticePolicy.classify(session,app.getPackageName(),100,install(app),false,new ExitNoticePolicy.Exit[]{fact(wrong)}).exit==null,"reused PID matched without session identity");
        wrong.pid=session.pid;wrong.name="com.aure.totk:setup";
        check(ExitNoticePolicy.classify(session,app.getPackageName(),100,install(app),false,new ExitNoticePolicy.Exit[]{fact(wrong)}).exit==null,"setup record matched game");
    }
    static void claimChild(Context app)throws Exception{
        ExitNoticeStore store=new ExitNoticeStore(app.files);List<ExitNoticeStore.Receipt> rows=store.receipts();
        System.out.println(rows.isEmpty()?false:claim(app,rows.get(0),exit()));
    }
    static final boolean MODERN=true;
    static final String TOKEN="totk-session-v2:00000000-0000-0000-0000-000000000001";
    static String install(Context a){return a.packages.info.firstInstallTime+":"+a.packages.info.lastUpdateTime+":"+a.packages.info.getLongVersionCode()+":"+a.appInfo.sourceDir;}
    static Object receipt(Context a,long wall,int pid,String token)throws Exception{
        return new ExitNoticeStore(a.files).begin(token,pid,123,"old-boot",install(a),new File(a.files,"totk.log").getPath(),new File(a.files,"totk.env").getPath());
    }
    static ExitNoticePolicy.Exit fact(ApplicationExitInfo e){return new ExitNoticePolicy.Exit(e.name,e.uid,e.pid,e.reason,e.status,400,e.stamp,1234,2345,e.token);}
    static boolean claim(Context a,Object value,ApplicationExitInfo e)throws Exception{
        ExitNoticeStore store=new ExitNoticeStore(a.files);ExitNoticeStore.Receipt r=(ExitNoticeStore.Receipt)value;
        ExitNoticeStore.Reservation reserved=store.reserve(r,fact(e),UUID.randomUUID().toString(),new ExitNoticeStore.Owner("boot",42,123),owner->true);
        return reserved.status==ExitNoticeStore.Status.READY&&store.shown(reserved.claim);
    }
    static String classify(Context a,long start,ApplicationExitInfo e,boolean orderly,boolean tracked)throws Exception{
        ExitNoticePolicy.Session session=tracked?new ExitNoticePolicy.Session(1,TOKEN,e.pid,123,"boot",install(a)):null;
        return ExitNoticePolicy.classify(session,a.getPackageName(),100,install(a),orderly,new ExitNoticePolicy.Exit[]{fact(e)}).message;
    }
    static void closeReceipt(Context a,Object value)throws Exception{new ExitNoticeStore(a.files).close(((ExitNoticeStore.Receipt)value).session.token);}
    static void clear(Context a)throws Exception{Files.deleteIfExists(new File(a.files,"state/exit-notice/state-v2").toPath());}
    static native void nativeStart(Object bridge,byte[] log,byte[] data);
    static native void nativeBackground();static native void nativeClose();
    static final class Bridge {public static void gameSessionEvent(int event,byte[] log,byte[] data){ExitNotice.nativeEvent(event,log,data);}}
    static void nativeEvents(Context a,String library)throws Exception{
        System.load(library);ExitNotice.attach(a);
        ProbeUi.run(()->{nativeStart(new Bridge(),new File(a.files,"totk.log").toString().getBytes(StandardCharsets.UTF_8),a.files.toString().getBytes(StandardCharsets.UTF_8));nativeBackground();});
        Thread close=new Thread(ExitR2Probe::nativeClose,"close-poller");close.start();close.join(5000);check(!close.isAlive(),"close did not complete");
        List<ExitNoticeStore.Receipt> rows=new ExitNoticeStore(a.files).receipts();
        check(rows.size()==1&&rows.get(0).closed&&rows.get(0).backgroundElapsedMs>0,"start/background/close ordering lost");
        check(a.manager.summary!=null&&rows.get(0).session.token.equals(new String(a.manager.summary,StandardCharsets.UTF_8)),"platform summary not bound to receipt");
        try{ProbeUi.run(()->ExitNotice.nativeEvent(2,null,null));throw new AssertionError("UI close wait accepted");}
        catch(ExecutionException expected){check(expected.getCause() instanceof IllegalStateException,"unexpected UI close rejection");}
    }
'''
LEGACY = r'''
    static void contractStore(Context app)throws Exception{throw new AssertionError("R2 store contract required");}
    static void claimChild(Context app)throws Exception{throw new AssertionError("R2 child contract required");}
    static final boolean MODERN=false;
    static final String TOKEN="totk-session-v2:00000000-0000-0000-0000-000000000001";
    static Object receipt(Context a,long wall,int pid,String token)throws Exception{
        File dir=new File(a.files,"state/exit-notice");dir.mkdirs();File file=new File(dir,"session-"+wall+"-"+pid+"-123");
        Files.writeString(file.toPath(),"1\n"+pid+"\n123\n"+wall+"\n"+new File(a.files,"totk.log")+"\n"+new File(a.files,"totk.env")+"\n");
        for(ExitNoticeStore.Receipt r:new ExitNoticeStore(a.files).receipts())if(r.file.equals(file))return r;
        throw new AssertionError("R1 receipt unreadable");
    }
    static ExitNoticePolicy.Exit fact(ApplicationExitInfo e){return new ExitNoticePolicy.Exit(e.name,e.pid,e.reason,e.status,400,e.stamp,1234,2345);}
    static boolean claim(Context a,Object value,ApplicationExitInfo e)throws Exception{return new ExitNoticeStore(a.files).claim((ExitNoticeStore.Receipt)value);}
    static String classify(Context a,long start,ApplicationExitInfo e,boolean orderly,boolean tracked)throws Exception{
        return ExitNoticePolicy.classify(tracked?new ExitNoticePolicy.Session(e.pid,123,start):null,a.getPackageName(),0,Long.MAX_VALUE,false,orderly,false,new ExitNoticePolicy.Exit[]{fact(e)}).message;
    }
    static void closeReceipt(Context a,Object value)throws Exception{Files.writeString(new File(((ExitNoticeStore.Receipt)value).file+".closed").toPath(),"closed\n");}
    static void clear(Context a)throws Exception{File[] files=new File(a.files,"state/exit-notice").listFiles();if(files!=null)for(File f:files)if(!f.getName().equals("lock"))f.delete();}
    static void nativeEvents(Context a,String library)throws Exception{throw new AssertionError("R1 native receipt lifecycle performs synchronous file IO");}
'''

PROBE = r'''
    static void check(boolean ok,String message){if(!ok)throw new AssertionError(message);}
    static Context context(String directory){Context a=new Context();a.files=new File(directory);return a;}
    static ApplicationExitInfo exit(){ApplicationExitInfo e=new ApplicationExitInfo();e.token=TOKEN;return e;}
    static boolean busy(ExitNotice notice)throws Exception{
        java.lang.reflect.Field field;
        try{field=ExitNotice.class.getDeclaredField("current");field.setAccessible(true);return field.get(notice)!=null;}
        catch(NoSuchFieldException old){field=ExitNotice.class.getDeclaredField("checking");field.setAccessible(true);return field.getBoolean(notice);}
    }
    static void settle(ExitNotice... notices)throws Exception{
        long limit=System.nanoTime()+TimeUnit.SECONDS.toNanos(8);
        while(System.nanoTime()<limit){
            final boolean[] active={false};ProbeUi.run(()->{try{for(ExitNotice n:notices)active[0]|=busy(n);}catch(Exception e){throw new RuntimeException(e);}});
            if(!active[0]){ProbeUi.run(()->{});ProbeUi.checkFailure();return;}Thread.sleep(10);
        }throw new AssertionError("notice requests did not settle");
    }
    static void showOnce(Context app,String mode)throws Exception{
        ApplicationExitInfo e=exit();Object prior=receipt(app,10000,e.pid,e.token);app.manager.exits.add(e);
        boolean expected=true;
        if(mode.equals("normal")){e.reason=1;expected=false;}
        if(mode.equals("user")){e.reason=10;expected=false;}
        if(mode.equals("crash"))e.reason=4;
        if(mode.equals("anr"))e.reason=6;
        if(mode.equals("update")){app.packages.info.lastUpdateTime=2;expected=false;}
        if(mode.equals("closed")){closeReceipt(app,prior);expected=false;}
        if(mode.equals("silent")){Files.writeString(new File(app.files,"totk.env").toPath(),"export TOTK_EXIT_NOTICE='0'\n");expected=false;}
        if(mode.equals("missing")){app.manager.exits.clear();expected=false;}
        if(mode.equals("old-api")){android.os.Build.VERSION.SDK_INT=29;expected=false;}
        Activity a=new Activity(app);ExitNotice n=new ExitNotice();List<Boolean> visible=new ArrayList<>();
        ProbeUi.run(()->n.resume(a,()->true,visible::add));settle(n);
        check(AlertDialog.shown==(expected?1:0),"notice count: "+AlertDialog.shown+" for "+mode);
        if(expected){
            check((e.reason==3?ExitNoticePolicy.LOW_MEMORY_TEXT:ExitNoticePolicy.UNEXPECTED_TEXT).equals(AlertDialog.last.message),"wrong notice text");
            check(AlertDialog.last.focused,"no controller focus");ProbeUi.run(()->AlertDialog.last.controller());
            check(visible.equals(Arrays.asList(true,false)),"controller did not restore focus");
        }
        ProbeUi.run(()->n.resume(a,()->true,visible::add));settle(n);
        check(AlertDialog.shown==(expected?1:0),"same record repeated");
        if(mode.equals("old-api"))check(app.manager.queries.get()==0,"old API read history");
    }
    static void clock(Context app)throws Exception{
        ApplicationExitInfo first=exit();first.stamp=100000;Object r1=receipt(app,90000,first.pid,first.token);
        check(claim(app,r1,first),"first record not handled");
        TimeZone.setDefault(TimeZone.getTimeZone("Pacific/Kiritimati"));
        check(!claim(app,r1,first),"same exit repeated after timezone/reopen");
        // Same PID, a different summary/exit record, and a rolled-back device clock.
        ApplicationExitInfo second=exit();second.token=TOKEN+"-second";second.stamp=2000;
        Object r2=receipt(app,1000,second.pid,second.token);
        check(claim(app,r2,second),"wall-clock rollback/PID reuse suppressed a DIFFERENT exit record");
        check(!claim(app,r2,second),"reconstructed store repeated the same exit record after reboot");
        ApplicationExitInfo during=exit();during.stamp=500;
        check(ExitNoticePolicy.LOW_MEMORY_TEXT.equals(classify(app,90000,during,false,true)),"clock change during session hid LOW_MEMORY");
    }
    static void lifecycle(Context app)throws Exception{
        ApplicationExitInfo e=exit();receipt(app,10000,e.pid,e.token);app.manager.exits.add(e);
        Activity old=new Activity(app),fresh=new Activity(app);ExitNotice before=new ExitNotice(),after=new ExitNotice();
        old.beforeResult=()->{before.pause();old.destroyed=true;after.resume(fresh,()->true,open->{});after.resume(fresh,()->true,open->{});};
        ProbeUi.run(()->{before.resume(old,()->true,open->{});before.resume(old,()->true,open->{});});
        settle(before,after);
        check(AlertDialog.shown==1&&AlertDialog.last.owner==fresh,"destroy/recreate lost or duplicated notice");
        check(app.manager.queries.get()==1,"concurrent/recreated checks were not coalesced: "+app.manager.queries.get());
        ProbeUi.run(()->AlertDialog.last.controller());
        ExitNotice next=new ExitNotice();ProbeUi.run(()->next.resume(fresh,()->true,open->{}));settle(next);
        check(AlertDialog.shown==1,"recreation repeated an acknowledged exit");
    }
    static void io(Context app)throws Exception{
        ApplicationExitInfo e=exit();receipt(app,10000,e.pid,e.token);app.manager.exits.add(e);
        ProbeUi.installIoGuard(app.files.toString());
        Activity a=new Activity(app);ExitNotice n=new ExitNotice();ProbeUi.run(()->n.resume(a,()->true,open->{}));settle(n);
        check(AlertDialog.shown==1,"worker-only delivery missing");ProbeUi.run(()->AlertDialog.last.controller());
        check(android.util.Log.lines.get()==1,"worker diagnostic not recorded once");
    }
    static void missingAndCleared(Context app)throws Exception{
        receipt(app,10000,999999,TOKEN);
        Activity a=new Activity(app);ExitNotice n=new ExitNotice();ProbeUi.run(()->n.resume(a,()->true,open->{}));settle(n);
        check(AlertDialog.shown==0,"rotated-away platform history was fabricated into a notice");
        app.manager.exits.add(exit());clear(app);
        ExitNotice recreated=new ExitNotice();ProbeUi.run(()->recreated.resume(a,()->true,open->{}));settle(recreated);
        check(AlertDialog.shown==0,"cleared stored state replayed retained platform history");
    }
    static void lock(Context app)throws Exception{
        File dir=new File(app.files,"state/exit-notice");dir.mkdirs();
        try(java.nio.channels.FileChannel channel=java.nio.channels.FileChannel.open(new File(dir,"lock").toPath(),StandardOpenOption.CREATE,StandardOpenOption.WRITE);
            java.nio.channels.FileLock held=channel.lock()){
            System.out.println("LOCKED");System.out.flush();System.in.read();
        }
    }
    static void lockedUi(Context app)throws Exception{
        ApplicationExitInfo e=exit();receipt(app,10000,e.pid,e.token);app.manager.exits.add(e);
        ProcessBuilder builder=new ProcessBuilder(System.getProperty("java.home")+"/bin/java","-cp",System.getProperty("java.class.path"),ExitR2Probe.class.getName(),"lock",app.files.toString());
        java.lang.Process child=builder.start();
        try(BufferedReader ready=new BufferedReader(new InputStreamReader(child.getInputStream()))){
            check("LOCKED".equals(ready.readLine()),"lock child failed");
            Activity a=new Activity(app);ExitNotice n=new ExitNotice();ProbeUi.run(()->n.resume(a,()->true,open->{}));
            check(app.manager.queried.await(5,TimeUnit.SECONDS),"history never queried");
            // R1's query result reaches UI before claim; R2 is waiting for the file lock on its worker.
            Thread.sleep(100);
            Future<?> callback=ProbeUi.ui.submit(()->n.pause());
            boolean responsive=true;
            try{callback.get(250,TimeUnit.MILLISECONDS);}catch(TimeoutException blocked){responsive=false;}
            child.getOutputStream().write(1);child.getOutputStream().flush();check(child.waitFor(5,TimeUnit.SECONDS),"lock child stuck");callback.get(5,TimeUnit.SECONDS);
            settle(n);check(responsive,"receipt file lock blocked the UI/lifecycle callback");
            ExitNotice again=new ExitNotice();ProbeUi.run(()->again.resume(a,()->true,open->{}));settle(again);
            check(AlertDialog.shown==1,"cancelled delivery lost the pending notice");
        }finally{if(child.isAlive()){child.getOutputStream().write(1);child.getOutputStream().flush();child.destroy();}}
    }
    public static void main(String[] args)throws Exception{
        Context app=context(args[1]);String mode=args[0];
        switch(mode){
         case "store":contractStore(app);break;
         case "prepare":receipt(app,10000,999999,TOKEN);break;
         case "claim-process":claimChild(app);return;
         case "clock":clock(app);break;case "lifecycle":lifecycle(app);break;case "io":io(app);break;
         case "io-lock":lockedUi(app);break;case "missing-state":missingAndCleared(app);break;case "lock":lock(app);return;
         case "native":ProbeUi.installIoGuard(app.files.toString());nativeEvents(app,args[2]);break;
         case "classify":{
          ApplicationExitInfo e=exit();
          for(int reason:new int[]{0,1,3,4,5,6,10,11,15,16}){
           e.reason=reason;String expected=reason==3?ExitNoticePolicy.LOW_MEMORY_TEXT:(reason==0||reason==4||reason==5||reason==6?ExitNoticePolicy.UNEXPECTED_TEXT:null);
           check(Objects.equals(expected,classify(app,10000,e,false,true)),"classification reason="+reason);
          }
          e.reason=3;check(classify(app,10000,e,true,true)==null,"close receipt ignored");
          check(classify(app,10000,e,false,false)==null,"untracked setup exit warned");break;
         }
         default:showOnce(app,mode);
        }
        ProbeUi.checkFailure();System.out.println("passed "+mode);
    }
}
'''

JNI = r'''#include <jni.h>
#include <pthread.h>
#include <stdlib.h>
typedef struct { JavaVM *vm; JNIEnv *env; jobject clazz; int sdkVersion; } ANativeActivity;
#include "android_exit_notice_jni.h"
JNIEXPORT void JNICALL Java_com_aure_totk_ExitR2Probe_nativeStart(JNIEnv *env,jclass cls,jobject bridge,jbyteArray log,jbyteArray data){
 (void)cls;JavaVM *vm=NULL;(*env)->GetJavaVM(env,&vm);ANativeActivity activity={vm,env,bridge,33};
 jsize nl=(*env)->GetArrayLength(env,log),nd=(*env)->GetArrayLength(env,data);char *l=calloc((size_t)nl+1,1),*d=calloc((size_t)nd+1,1);
 if(!l||!d)abort();(*env)->GetByteArrayRegion(env,log,0,nl,(jbyte*)l);(*env)->GetByteArrayRegion(env,data,0,nd,(jbyte*)d);
 android_exit_notice_start(&activity,l,d);free(l);free(d);
}
JNIEXPORT void JNICALL Java_com_aure_totk_ExitR2Probe_nativeBackground(JNIEnv *env,jclass cls){(void)env;(void)cls;android_exit_notice_event(1);}
static void *close_thread(void *arg){(void)arg;android_exit_notice_event(2);return NULL;}
JNIEXPORT void JNICALL Java_com_aure_totk_ExitR2Probe_nativeClose(JNIEnv *env,jclass cls){
 (void)env;(void)cls;pthread_t thread;if(pthread_create(&thread,NULL,close_thread,NULL))abort();pthread_join(thread,NULL);
}
'''

OLD_NATIVE = r'''#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <dirent.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
static unsigned calls;
static FILE *ui_fopen(const char *path,const char *mode){calls++;return fopen(path,mode);}
static int ui_fsync(int fd){calls++;return fsync(fd);}
#define fopen ui_fopen
#define fsync ui_fsync
#include "android_exit_notice.h"
int main(int argc,char **argv){
 if(argc!=2)return 2;char state[2048],log[2048];snprintf(state,sizeof state,"%s/state",argv[1]);mkdir(state,0700);
 snprintf(log,sizeof log,"%s/totk.log",argv[1]);android_exit_notice_start(argv[1],log,argv[1]);android_exit_notice_background();
 fprintf(stderr,"native UI receipt IO calls=%u\n",calls);return calls?1:0;
}
'''


class Fixture:
    def __init__(self):
        self.root = Path(os.environ.get('TOTK_EXIT_TEST_ROOT', ROOT))
        self.java_dir = self.root / 'games/totk/android/java/com/aure/totk'
        self.modern = 'SUMMARY_PREFIX' in (self.java_dir / 'ExitNoticePolicy.java').read_text()
        scratch = Path(os.environ.get('TOTK_SETUP_WORK', ROOT / '.scratch'))
        scratch.mkdir(parents=True, exist_ok=True)
        self.temp = tempfile.TemporaryDirectory(prefix='exit-r2-', dir=scratch)
        self.work = Path(self.temp.name)
        javac, self.java, cc = shutil.which('javac'), shutil.which('java'), shutil.which('cc')
        if not javac or not self.java or not cc:
            raise RuntimeError('host JDK and C compiler required')
        sources = [self.java_dir / name for name in ('ExitNoticePolicy.java', 'ExitNoticeStore.java', 'ExitNotice.java')]
        for name, content in STUBS.items():
            path = self.work / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(content)
            sources.append(path)
        path = self.work / 'ExitR2Probe.java'
        path.write_text('package com.aure.totk;\nimport java.io.*;import java.nio.file.*;import java.nio.charset.*;import java.util.*;import java.util.concurrent.*;import android.app.*;import android.content.*;\npublic class ExitR2Probe {\n' + (MODERN if self.modern else LEGACY) + PROBE)
        self.classes = self.work / 'classes'
        self.command([javac, '--release', '17', '-d', self.classes, *sources, path])
        runtime = self.root / 'games/totk/runtime'
        if self.modern:
            include = Path(javac).resolve().parents[1] / 'include'
            path = self.work / 'bridge.c'
            path.write_text(JNI)
            self.native = self.work / 'libexitprobe.so'
            self.command([cc, '-std=c11', '-Wall', '-Wextra', '-Werror', '-Wno-misleading-indentation', '-fPIC', '-shared', '-pthread',
                          '-I', include, '-I', include / 'linux', '-I', runtime, path, '-o', self.native])
        else:
            path = self.work / 'native.c'
            path.write_text(OLD_NATIVE)
            self.native = self.work / 'native'
            self.command([cc, '-std=c11', '-Wall', '-Wextra', '-Wno-misleading-indentation', '-I', runtime, path, '-o', self.native])

    @staticmethod
    def command(args):
        result = subprocess.run(list(map(str, args)), capture_output=True, text=True, timeout=60)
        if result.returncode:
            raise AssertionError(result.stdout + result.stderr)
        return result

    def run(self, mode):
        with tempfile.TemporaryDirectory(prefix='case-', dir=self.work) as case:
            if mode == 'native' and not self.modern:
                args = [self.native, case]
            else:
                args = [self.java, '-cp', self.classes, 'com.aure.totk.ExitR2Probe', mode, case, self.native]
            return subprocess.run(list(map(str, args)), capture_output=True, text=True, timeout=35)

    def probe(self, mode):
        result = self.run(mode)
        if result.returncode:
            raise AssertionError(result.stdout + result.stderr)
        return result.stdout

    def close(self):
        self.temp.cleanup()
