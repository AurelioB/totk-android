"""Production exit policy, receipt IO and Android adapter against fabricated exit history.

No game, device, platform kill or render thread is exercised. Android window/history
objects are contract doubles; the policy, durable once ledger and adapter are real.
"""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
GAME = ROOT / 'games/totk'
JAVA = GAME / 'android/java/com/aure/totk'


def command(args):
    result = subprocess.run(list(map(str, args)), capture_output=True, text=True, timeout=60)
    if result.returncode:
        raise AssertionError(result.stdout + result.stderr)
    return result.stdout


STUBS = {
    'android/os/Build.java': 'package android.os; public class Build { public static class VERSION { public static int SDK_INT=33; } }',
    'android/content/pm/PackageInfo.java': 'package android.content.pm; public class PackageInfo { public long lastUpdateTime; }',
    'android/content/pm/PackageManager.java': '''package android.content.pm;
public class PackageManager { public final PackageInfo info=new PackageInfo(); public PackageInfo getPackageInfo(String name,int flags){return info;} }''',
    'android/app/ApplicationExitInfo.java': '''package android.app;
public class ApplicationExitInfo {
    public String name="com.aure.totk"; public int pid=999999,reason=3;
    public String getProcessName(){return name;} public int getPid(){return pid;} public int getReason(){return reason;}
    public int getStatus(){return 0;} public int getImportance(){return 400;} public long getTimestamp(){return 2000;}
    public long getPss(){return 1234;} public long getRss(){return 2345;}
}''',
    'android/app/ActivityManager.java': '''package android.app;
import java.util.*;
public class ActivityManager {
    public final List<ApplicationExitInfo> exits=new ArrayList<>(); public int queries;
    public boolean unavailable;
    public List<ApplicationExitInfo> getHistoricalProcessExitReasons(String name,int pid,int limit){
        if(!Thread.currentThread().getName().equals("TotkExitNotice"))throw new AssertionError("history on UI/game thread");
        queries++; if(unavailable)throw new SecurityException("history unavailable"); return exits;
    }
    public List<RunningAppProcessInfo> getRunningAppProcesses(){return Collections.emptyList();}
    public static class RunningAppProcessInfo {public int pid;public String processName;}
}''',
    'android/app/Activity.java': '''package android.app;
import java.io.*;import java.util.concurrent.*;import android.content.pm.*;
public class Activity {
    public final ActivityManager manager=new ActivityManager(); public final PackageManager packages=new PackageManager();
    public final BlockingQueue<Runnable> main=new LinkedBlockingQueue<>();public File files;
    public File getFilesDir(){return files;} public String getPackageName(){return "com.aure.totk";}
    public <T>T getSystemService(Class<T> type){return type.cast(manager);} public PackageManager getPackageManager(){return packages;}
    public boolean isFinishing(){return false;}public boolean isDestroyed(){return false;}
    public void runOnUiThread(Runnable action){main.add(action);}
    public void drain()throws Exception{Runnable next=main.poll(10,TimeUnit.SECONDS);if(next==null)throw new AssertionError("no worker completion");next.run();}
}''',
    'android/view/KeyEvent.java': '''package android.view;
public class KeyEvent {public static final int KEYCODE_BUTTON_A=96,KEYCODE_BUTTON_B=97,ACTION_UP=1;
 public int getAction(){return ACTION_UP;}public boolean isCanceled(){return false;}}''',
    'android/app/AlertDialog.java': '''package android.app;
import android.view.KeyEvent;
public class AlertDialog {
    public static final int BUTTON_POSITIVE=-1;public static int shown;public static AlertDialog last;
    public String message;public boolean focused;private Dismiss dismiss;private Key key;
    public interface Click{void run(AlertDialog d,int which);}public interface Dismiss{void run(AlertDialog d);}
    public interface Key{boolean run(AlertDialog d,int code,KeyEvent event);}
    public static class Builder{
        final AlertDialog d=new AlertDialog(); public Builder(Activity a){}
        public Builder setTitle(String s){return this;} public Builder setMessage(String s){d.message=s;return this;}
        public Builder setPositiveButton(String s,Click c){return this;}public AlertDialog create(){return d;}
    }
    public void setCanceledOnTouchOutside(boolean b){}public void setOnDismissListener(Dismiss d){dismiss=d;}
    public void setOnKeyListener(Key k){key=k;}public void show(){shown++;last=this;}
    public class Button{public void requestFocus(){focused=true;}}
    public Button getButton(int which){return new Button();}public void dismiss(){if(dismiss!=null){Dismiss d=dismiss;dismiss=null;d.run(this);}}
    public void controller(){if(!key.run(this,KeyEvent.KEYCODE_BUTTON_A,new KeyEvent()))throw new AssertionError("controller not consumed");}
}''',
    'android/util/Log.java': '''package android.util;
public class Log{public static int lines;public static String last;
 public static int i(String t,String m){lines++;last=m;return 0;}public static int w(String t,String m,Throwable e){return 0;}}''',
}

PROBE = r'''package com.aure.totk;
import java.io.*;import java.nio.file.*;import java.util.*;import android.app.*;
public class ExitProbe {
    static void check(boolean value,String why){if(!value)throw new AssertionError(why);}
    static ExitNoticePolicy.Exit exit(String name,int pid,int reason,long stamp){return new ExitNoticePolicy.Exit(name,pid,reason,0,400,stamp,1234,2345);}
    static void policy(String mode){
        ExitNoticePolicy.Session session=new ExitNoticePolicy.Session(42,123,1000);
        ExitNoticePolicy.Exit low=exit("com.aure.totk",42,3,2000);
        ExitNoticePolicy.Exit[] history={low};String expected=ExitNoticePolicy.LOW_MEMORY_TEXT;
        boolean alive=false,close=false,handled=false;long update=0,cutoff=3000;
        switch(mode){
        case "low":break;
        case "setup-only":session=null;history=new ExitNoticePolicy.Exit[]{exit("com.aure.totk:setup",42,3,2000)};expected=null;break;
        case "missing-game":history=new ExitNoticePolicy.Exit[]{exit("com.aure.totk:setup",42,3,2000)};expected=ExitNoticePolicy.UNEXPECTED_TEXT;break;
        case "user":history=new ExitNoticePolicy.Exit[]{exit("com.aure.totk",42,10,2000)};expected=null;break;
        case "stopped":history=new ExitNoticePolicy.Exit[]{exit("com.aure.totk",42,11,2000)};expected=null;break;
        case "failed-self-exit":history=new ExitNoticePolicy.Exit[]{new ExitNoticePolicy.Exit("com.aure.totk",42,1,1,400,2000,1234,2345)};expected=ExitNoticePolicy.UNEXPECTED_TEXT;break;
        case "normal":history=new ExitNoticePolicy.Exit[]{exit("com.aure.totk",42,1,2000)};expected=null;break;
        case "crash":history=new ExitNoticePolicy.Exit[]{exit("com.aure.totk",42,4,2000)};expected=ExitNoticePolicy.UNEXPECTED_TEXT;break;
        case "anr":history=new ExitNoticePolicy.Exit[]{exit("com.aure.totk",42,6,2000)};expected=ExitNoticePolicy.UNEXPECTED_TEXT;break;
        case "unknown":history=new ExitNoticePolicy.Exit[]{exit("com.aure.totk",42,0,2000)};expected=ExitNoticePolicy.UNEXPECTED_TEXT;break;
        case "update":update=1500;expected=null;break;
        case "update-reason":history=new ExitNoticePolicy.Exit[]{exit("com.aure.totk",42,16,2000)};expected=null;break;
        case "none":history=new ExitNoticePolicy.Exit[0];expected=ExitNoticePolicy.UNEXPECTED_TEXT;break;
        case "no-session":session=null;expected=null;break;
        case "duplicate":history=new ExitNoticePolicy.Exit[]{low,low};handled=true;expected=null;break;
        case "duplicate-history":history=new ExitNoticePolicy.Exit[]{low,low};break;
        case "live":alive=true;expected=null;break;
        case "receipt":close=true;expected=null;break;
        case "stale-pid":history=new ExitNoticePolicy.Exit[]{exit("com.aure.totk",42,3,999)};expected=ExitNoticePolicy.UNEXPECTED_TEXT;break;
        case "reused-pid":cutoff=1500;expected=ExitNoticePolicy.UNEXPECTED_TEXT;break;
        case "wrong-pid":history=new ExitNoticePolicy.Exit[]{exit("com.aure.totk",43,3,2000)};expected=ExitNoticePolicy.UNEXPECTED_TEXT;break;
        default:throw new AssertionError(mode);
        }
        ExitNoticePolicy.Decision result=ExitNoticePolicy.classify(session,"com.aure.totk",update,cutoff,alive,close,handled,history);
        check(Objects.equals(expected,result.message),mode+": "+result.message);
    }
    static File receipt(File files,long start,int pid,long ticks)throws Exception{
        File directory=new File(files,"state/exit-notice");directory.mkdirs();
        File file=new File(directory,"session-"+start+"-"+pid+"-"+ticks);
        Files.writeString(file.toPath(),"1\n"+pid+"\n"+ticks+"\n"+start+"\n"+new File(files,"totk.log")+"\n"+new File(files,"totk.env")+"\n");return file;
    }
    public static void main(String[] args)throws Exception{
        String mode=args[0];File files=new File(args[1]);
        if(mode.startsWith("policy-")){policy(mode.substring(7));}
        else if(mode.equals("claim")){
            ExitNoticeStore store=new ExitNoticeStore(files);System.out.println(store.claim(store.receipts().get(0)));return;
        }else if(mode.equals("store")){
            File old=receipt(files,1000,42,123);receipt(files,3000,43,456);
            ExitNoticeStore store=new ExitNoticeStore(files);List<ExitNoticeStore.Receipt> receipts=store.receipts();
            check(receipts.size()==2&&old.exists(),"direct start overwrote previous unchecked session");
            check(store.claim(receipts.get(0)),"first claim missing");
            check(!new ExitNoticeStore(files).claim(receipts.get(0)),"recreation/cross-process ledger repeated notice");
            Files.writeString(new File(old+".closed").toPath(),"closed\n");check(receipts.get(0).orderly(),"close receipt missing");
            store.retire(receipts.get(0));check(!old.exists()&&!new File(old+".closed").exists(),"retired receipt remained");
            check(store.claim(receipts.get(1)),"newest normal/silenced claim failed");
            receipt(files,2000,44,789);
            check(!new ExitNoticeStore(files).claim(store.receipts().get(0)),"retired latest session resurrected an older interruption");
            File env=new File(files,"totk.env");check(ExitNoticeStore.enabled(env),"notice not default on");
            for(String value:new String[]{"0","'0'","\"0\""}){
                Files.writeString(env.toPath(),"# comment\nexport TOTK_EXIT_NOTICE="+value+"\n");
                check(!ExitNoticeStore.enabled(env),"exact zero did not silence");
            }
            for(String value:new String[]{"1","00","false"," 0","0 # comment"}){
                Files.writeString(env.toPath(),"TOTK_EXIT_NOTICE="+value+"\n");check(ExitNoticeStore.enabled(env),"nonzero silenced");
            }
            Files.writeString(env.toPath(),"TOTK_EXIT_NOTICE=0\nTOTK_EXIT_NOTICE=1\n");check(ExitNoticeStore.enabled(env),"last assignment lost");
        }else{
            Activity activity=new Activity();activity.files=files;
            File prior=receipt(files,1000,999999,123);
            ApplicationExitInfo exit=new ApplicationExitInfo();activity.manager.exits.add(exit);
            boolean notice=true;int diagnostics=1;String expected=ExitNoticePolicy.LOW_MEMORY_TEXT;
            if(mode.equals("adapter-normal")){exit.reason=1;notice=false;}
            if(mode.equals("adapter-later-close")){
                File latest=receipt(files,3000,999998,124);Files.writeString(new File(latest+".closed").toPath(),"closed\n");
                notice=false;
            }
            if(mode.equals("adapter-update")){activity.packages.info.lastUpdateTime=1500;notice=false;}
            if(mode.equals("adapter-silent")){Files.writeString(new File(files,"totk.env").toPath(),"TOTK_EXIT_NOTICE=0\n");notice=false;}
            if(mode.equals("adapter-missing")){activity.manager.unavailable=true;expected=ExitNoticePolicy.UNEXPECTED_TEXT;}
            if(mode.equals("adapter-old-api")){
                android.os.Build.VERSION.SDK_INT=29;
                new ExitNotice().resume(activity,()->true,open->{});
                check(activity.manager.queries==0&&AlertDialog.shown==0&&prior.exists(),"older API did exit work");System.out.println("passed "+mode);return;
            }
            ExitNotice controller=new ExitNotice();List<Boolean> visible=new ArrayList<>();
            if(mode.equals("adapter-deferred")){
                controller.resume(activity,()->false,visible::add);activity.drain();
                check(AlertDialog.shown==0&&prior.exists(),"unready launcher consumed notice");
            }
            controller.resume(activity,()->true,visible::add);
            if(mode.equals("adapter-lifecycle")){controller.pause();controller.resume(activity,()->true,visible::add);activity.drain();}
            activity.drain();
            check(AlertDialog.shown==(notice?1:0),"notice count");
            if(notice){
                check(expected.equals(AlertDialog.last.message)&&AlertDialog.last.focused,"wrong text/unfocusable notice");
                AlertDialog.last.controller();check(visible.equals(Arrays.asList(true,false)),"controller dismissal did not restore game focus");
            }
            check(!prior.exists()&&android.util.Log.lines==diagnostics,"diagnostic/once consumption missing");
            check(android.util.Log.last.contains("reason=")&&android.util.Log.last.contains("importance=")
                &&android.util.Log.last.contains("pss_kib=")&&android.util.Log.last.contains("rss_kib=")
                &&android.util.Log.last.contains("timestamp_ms="),"diagnostic fields missing");
            controller.resume(activity,()->true,visible::add);activity.drain();
            check(AlertDialog.shown==(notice?1:0)&&android.util.Log.lines==diagnostics,"resume repeated notice or diagnostic");
        }
        System.out.println("passed "+mode);
    }
}
'''

C_PROBE = r'''#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include "android_exit_notice.h"
int main(int argc,char **argv){
    assert(argc==2);char state[1500],log[1500],data[1500];
    snprintf(state,sizeof state,"%s/state",argv[1]);assert(!mkdir(state,0700));
    snprintf(log,sizeof log,"%s/totk.log",argv[1]);snprintf(data,sizeof data,"%s/game",argv[1]);
    android_exit_notice_close();assert(!android_exit_session[0]);
    android_exit_notice_start(argv[1],log,data);assert(android_exit_start_ticks()>0&&android_exit_session[0]);
    char original[1400];strcpy(original,android_exit_session);
    FILE *in=fopen(original,"r");assert(in);int version,pid;unsigned long long ticks;long long stamp;
    assert(fscanf(in,"%d\n%d\n%llu\n%lld",&version,&pid,&ticks,&stamp)==4);fclose(in);
    assert(version==1&&pid==getpid()&&ticks==android_exit_start_ticks()&&stamp>0);
    snprintf(state,sizeof state,"%s/state/save-generation",argv[1]);FILE *out=fopen(state,"w");assert(out);fputs("7\n",out);fclose(out);
    android_exit_notice_background();snprintf(state,sizeof state,"%s.background",original);
    in=fopen(state,"r");assert(in);char text[256];assert(fgets(text,sizeof text,in)&&strstr(text,"last_observed_commit=7"));fclose(in);
    android_exit_notice_close();snprintf(state,sizeof state,"%s.closed",original);assert(!access(state,F_OK));
    for(int i=0;i<20;i++){
        snprintf(state,sizeof state,"%s/state/exit-notice/session-%d-123-456",argv[1],i+1);
        assert(android_exit_atomic(state,"old\n"));
    }
    snprintf(state,sizeof state,"%s/state/exit-notice",argv[1]);android_exit_prune(state);
    DIR *dir=opendir(state);assert(dir);int count=0;struct dirent *entry;
    while((entry=readdir(dir))){long long time;int p,n=0;unsigned long long t;
        if(sscanf(entry->d_name,"session-%lld-%d-%llu%n",&time,&p,&t,&n)==3&&!entry->d_name[n])count++;
    }closedir(dir);assert(count==15&&!access(original,F_OK));puts("passed receipts");
}
'''


class ExitNoticeTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        javac, cls.java, cc = shutil.which('javac'), shutil.which('java'), shutil.which('cc')
        if not javac or not cls.java or not cc:
            raise unittest.SkipTest('host JDK and C compiler required')
        scratch = Path(os.environ.get('TOTK_SETUP_WORK', ROOT / '.scratch'))
        scratch.mkdir(parents=True, exist_ok=True)
        cls.temp = tempfile.TemporaryDirectory(prefix='exit-', dir=scratch)
        cls.addClassCleanup(cls.temp.cleanup)
        cls.work = Path(cls.temp.name)
        sources = [JAVA / name for name in ('ExitNoticePolicy.java', 'ExitNoticeStore.java', 'ExitNotice.java')]
        for name, content in STUBS.items():
            target = cls.work / name
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_text(content)
            sources.append(target)
        probe = cls.work / 'ExitProbe.java'
        probe.write_text(PROBE)
        cls.classes = cls.work / 'classes'
        command([javac, '--release', '17', '-d', cls.classes, *sources, probe])
        native = cls.work / 'receipts.c'
        native.write_text(C_PROBE)
        cls.native = cls.work / 'receipts'
        command([cc, '-std=c11', '-Wall', '-Wextra', '-Werror', '-I', GAME / 'runtime', native, '-o', cls.native])

    def probe(self, mode):
        with tempfile.TemporaryDirectory(prefix='case-', dir=self.work) as case:
            self.assertIn('passed ' + mode, command([self.java, '-cp', self.classes, 'com.aure.totk.ExitProbe', mode, case]))

    def test_fabricated_exit_classification(self):
        for mode in ('low', 'setup-only', 'missing-game', 'user', 'stopped', 'normal', 'crash', 'anr',
                     'unknown', 'failed-self-exit', 'update', 'update-reason', 'none', 'no-session', 'duplicate', 'duplicate-history',
                     'live', 'receipt', 'stale-pid', 'reused-pid', 'wrong-pid'):
            with self.subTest(mode=mode):
                self.probe('policy-' + mode)

    def test_persistent_once_receipt_ordering_and_exact_silence_switch(self):
        self.probe('store')

    def test_android_adapter_on_both_cold_paths_and_controller_dismissal(self):
        for mode in ('adapter-low', 'adapter-normal', 'adapter-update', 'adapter-silent',
                     'adapter-missing', 'adapter-deferred', 'adapter-lifecycle', 'adapter-later-close'):
            with self.subTest(mode=mode):
                self.probe(mode)

    def test_older_android_does_nothing(self):
        self.probe('adapter-old-api')

    def test_two_processes_claim_only_once(self):
        with tempfile.TemporaryDirectory(prefix='race-', dir=self.work) as case:
            directory = Path(case) / 'state/exit-notice'
            directory.mkdir(parents=True)
            (directory / 'session-1000-42-123').write_text(f'1\n42\n123\n1000\n{case}/totk.log\n{case}/totk.env\n')
            args = [self.java, '-cp', str(self.classes), 'com.aure.totk.ExitProbe', 'claim', case]
            children = [subprocess.Popen(args, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True) for _ in range(2)]
            results = [child.communicate(timeout=15) for child in children]
            self.assertEqual([child.returncode for child in children], [0, 0], results)
            self.assertEqual(sorted(out.strip() for out, err in results), ['false', 'true'])

    def test_native_atomic_start_background_close_and_bounded_receipts(self):
        with tempfile.TemporaryDirectory(prefix='native-', dir=self.work) as case:
            self.assertIn('passed receipts', command([self.native, case]))

    def test_receipts_are_not_a_destruction_or_render_thread_contract(self):
        source = (GAME / 'runtime/frontend_android.c').read_text()
        start = source[source.index('static int start_game('):source.index('/* ---------- input ----------')]
        self.assertIn('if (a->sdkVersion >= 30) android_exit_notice_start(priv, g_log_path, g_data);', start)
        self.assertLess(start.index('android_exit_notice_start('), start.index('android_close_requests_start(priv);'))
        close = source[source.index('int runtime_front_request_close(void)'):source.index('/* One stat per 500 ms')]
        self.assertLess(close.index('runtime_fs_begin_close()'), close.index('vk_pcache_shutdown();'))
        self.assertLess(close.index('vk_pcache_shutdown();'), close.index('android_exit_notice_close();'))
        self.assertLess(close.index('android_exit_notice_close();'), close.index('_exit(0);'))
        destroy = source[source.index('static void on_destroy('):source.index('__attribute__((visibility("default"))) void ANativeActivity_onCreate')]
        self.assertNotIn('android_exit_notice_close', destroy)
        self.assertNotIn('callbacks->onDestroy =', source)  # Deliberately not assumed to be registered.
        self.assertEqual(source.count('android_exit_notice_background();'), 1)
        self.assertIn('android_exit_notice_background(); }', source[source.index('static void on_pause('):source.index('static void on_stop(')])
        for name in ('TotkActivity.java', 'setup/SetupActivity.java'):
            activity = (JAVA / name).read_text()
            self.assertIn('exitNotice.resume(', activity)
            self.assertIn('exitNotice.pause();', activity)


if __name__ == '__main__':
    unittest.main()
