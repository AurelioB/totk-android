"""Run the real game activity, boot popup, exit notice and hold policy together.

Android windows/queues and unrelated companion/feedback/network adapters are doubles;
the native boot snapshot, pause/hold policy and session-event JNI bridge are production.
No ART, physical display or performance claim is made.
"""
import os
from pathlib import Path
import shutil
import tempfile
import unittest

import android_exit_notice_fixture as exits
import test_android_boot_progress as boot
import test_android_game_hold_runtime as hold
import test_companion_gate as gate

ROOT = Path(__file__).resolve().parents[1]
JAVA = ROOT / 'games/totk/android/java/com/aure/totk'

STUBS = {**gate.STUBS, **boot.STUBS, **exits.STUBS}
for name in ('com/aure/totk/setup/BootProgress.java', 'com/aure/totk/ExitNotice.java', 'com/aure/totk/GameHold.java'):
    del STUBS[name]
STUBS.update({
    'android/util/Log.java': exits.STUBS['android/util/Log.java'].replace(
        'public static int w(', 'public static int w(String t,String m){return 0;}public static int w('),
    'android/app/Activity.java': '''package android.app;
public class Activity extends android.content.Context {
 public static android.content.Context app; public boolean destroyed,finishing;
 public android.view.Window window=new android.view.Window();
 public Activity(){files=app.files;manager=app.manager;packages=app.packages;appInfo=app.appInfo;}
 public boolean isDestroyed(){return destroyed;}public boolean isFinishing(){return finishing;}
 public android.view.Window getWindow(){return window;}
 public android.content.res.Resources getResources(){return new android.content.res.Resources();}
 public void runOnUiThread(Runnable r){com.aure.totk.ProbeUi.post(r);}
}''',
    'android/app/NativeActivity.java': '''package android.app;
public class NativeActivity extends Activity {
 public static Runnable initialize;public static boolean initialized;
 protected void onCreate(android.os.Bundle b){initialize.run();initialized=true;}
 protected void onResume(){com.aure.totk.StartupProbe.lifecycle(false);}
 protected void onPause(){com.aure.totk.StartupProbe.lifecycle(true);com.aure.totk.TotkActivity.gameSessionEvent(1,null,null);}
 protected void onDestroy(){destroyed=true;}protected void onStop(){}
 protected void onNewIntent(android.content.Intent i){}public void setIntent(android.content.Intent i){}
 public void onConfigurationChanged(android.content.res.Configuration c){}
 public android.view.Display getDisplay(){return new android.view.Display();}public boolean hasWindowFocus(){return true;}
 public void onWindowFocusChanged(boolean f){}public void onTopResumedActivityChanged(boolean t){}
}''',
    'android/view/Window.java': '''package android.view;
public class Window {
 public View decor=new View();public WindowManager.LayoutParams attrs=new WindowManager.LayoutParams();
 public View getDecorView(){return decor;}public WindowManager.LayoutParams getAttributes(){return attrs;}
 public void setAttributes(WindowManager.LayoutParams a){attrs=a;}
}''',
    'android/view/View.java': boot.STUBS['android/view/View.java'].replace(
        'public boolean attached', 'public Display getDisplay(){return new Display();}public AttachedSurfaceControl getRootSurfaceControl(){return null;}public boolean attached'),
    'android/view/Display.java': '''package android.view;
public class Display {public int getDisplayId(){return 0;}public Mode[] getSupportedModes(){return new Mode[]{new Mode()};}
 public static class Mode {public float getRefreshRate(){return 120;}}}''',
    'android/os/Handler.java': hold.STUBS['android/os/Handler.java'].replace(
        'public boolean postDelayed', 'public boolean post(Runnable r){return postDelayed(r,0);}public boolean postDelayed'),
    'android/content/Context.java': exits.STUBS['android/content/Context.java'].replace(
        'public File files;', 'public static final String ACTIVITY_SERVICE="activity";public Object getSystemService(String n){return manager;}public void startActivity(android.content.Intent i){throw new AssertionError("unexpected navigation");}public File files;').replace(
        'public Context getApplicationContext(){return this;}', 'public Context getApplicationContext(){return android.app.Activity.app==null?this:android.app.Activity.app;}'),
    'com/aure/totk/companion/CompanionNative.java': '''package com.aure.totk.companion;
public class CompanionNative {public static boolean isEnabled(){return false;}}''',
    'com/aure/totk/companion/PresentationController.java': '''package com.aure.totk.companion;
public class PresentationController {
 public PresentationController(Object a,Object s){}public static int secondScreen(Object a,Object d){return 0;}
 public void ownerTopResumed(boolean t){}public void onDisplayChanged(int id){}public void close(){}
}''',
    'com/aure/totk/companion/DisplayClass.java': 'package com.aure.totk.companion;public class DisplayClass {public static final int NONE=0;}',
    'com/aure/totk/Updates.java': '''package com.aure.totk;
public class Updates {public static boolean pending;public static void check(android.content.Context a,java.util.function.Consumer<String> r){r.accept(pending?"new":null);}}''',
    'com/aure/totk/setup/UpdatePrompt.java': '''package com.aure.totk.setup;
public class UpdatePrompt {
 public static int shown;public static java.util.function.Consumer<Boolean> answer;
 public static void show(android.app.Activity a,String v,java.util.function.Consumer<Boolean> r){
  if(android.widget.PopupWindow.last!=null&&android.widget.PopupWindow.last.showing)throw new AssertionError("update over startup popup");
  shown++;answer=r;
 }
}''',
    'android/app/AlertDialog.java': exits.STUBS['android/app/AlertDialog.java'].replace(
        'shown++;last=this;', 'if(android.widget.PopupWindow.last!=null&&android.widget.PopupWindow.last.showing)throw new AssertionError("notice over startup popup");shown++;last=this;'),
})

PROBE = r'''package com.aure.totk;
import android.app.*;import android.content.*;import android.os.Handler;import android.widget.PopupWindow;
import com.aure.totk.setup.UpdatePrompt;import java.io.*;import java.nio.file.*;import java.nio.charset.*;import java.util.*;import java.util.concurrent.*;
public class StartupProbe {
 public static native void lifecycle(boolean paused);
 static native void start(TotkActivity a,byte[] log,byte[] data,boolean held);
 static native void handoff();static native int flushes();static native boolean paused();
 static void check(boolean ok,String why){if(!ok)throw new AssertionError(why);}
 static boolean tracked(Context app){
  if(app.manager.summary==null)return false;
  try{return new ExitNoticeStore(app.files).receipts().stream().anyMatch(r->r.session.token.equals(new String(app.manager.summary,StandardCharsets.UTF_8)));}
  catch(IOException e){throw new AssertionError(e);}
 }
 static void await(java.util.function.BooleanSupplier ready)throws Exception{
  long end=System.nanoTime()+TimeUnit.SECONDS.toNanos(8);
  while(!ready.getAsBoolean()&&System.nanoTime()<end){ProbeUi.run(()->{});Thread.sleep(10);}
  ProbeUi.checkFailure();check(ready.getAsBoolean(),"worker timeout");
 }
 public static void main(String[] args)throws Exception{
  System.load(args[2]);Context app=new Context();app.files=new File(args[1]);Activity.app=app;
  boolean held=Boolean.parseBoolean(args[3]),update=args[0].equals("update");Updates.pending=update;
  String token=ExitNoticePolicy.SUMMARY_PREFIX+"00000000-0000-0000-0000-000000000001";
  new ExitNoticeStore(app.files).begin(token,999999,123,"old-boot","1:9000:7:/installed/base.apk",
    new File(app.files,"totk.log").toString(),new File(app.files,"totk.env").toString());
  ApplicationExitInfo prior=new ApplicationExitInfo();prior.token=token;app.manager.exits.add(prior);
  TotkActivity a=new TotkActivity();NativeActivity.initialize=()->start(a,new File(app.files,"totk.log").toString().getBytes(StandardCharsets.UTF_8),app.files.toString().getBytes(StandardCharsets.UTF_8),held);
  ProbeUi.run(()->{a.onCreate(null);a.onResume();a.selectFrameRateHint(120);Handler.advance(0);});
  await(()->tracked(app));
  // The new native session is tracked before the first presented frame.
  check(PopupWindow.last!=null&&PopupWindow.last.showing,"startup indicator missing");
  ProbeUi.run(()->Handler.advance(500));
  check(AlertDialog.shown==0&&UpdatePrompt.shown==0,"modal UI appeared before startup finished");
  check(a.window.attrs.preferredRefreshRate==120,"foreground refresh preference lost");
  if(args[0].equals("pause")){
   ProbeUi.run(()->{a.onPause();check(!PopupWindow.last.showing,"background popup stuck");check(a.window.attrs.preferredRefreshRate==0,"background refresh preference stuck");
    if(held)GameHold.settingsActive(a,true);Handler.advance(800);});
   check(paused(),"background game not paused");check(flushes()==(held?0:1),"hold/off background semantics changed");
   ProbeUi.run(()->{if(held)GameHold.settingsActive(a,false);a.onResume();Handler.advance(0);});
   check(PopupWindow.last.showing&&!paused(),"startup resume lost");
  }
  ProbeUi.run(()->{handoff();Handler.advance(250);});
  check(!PopupWindow.last.showing,"first frame did not dismiss indicator");
  if(update){
   check(UpdatePrompt.shown==1&&AlertDialog.shown==0,"pending update must precede notice");
   ProbeUi.run(()->UpdatePrompt.answer.accept(false));
  }
  await(()->AlertDialog.shown==1);check(paused(),"notice did not pause the game");
  ProbeUi.run(()->{AlertDialog.last.controller();Handler.advance(800);});
  check(!paused(),"dismiss did not restore game focus");
  ProbeUi.run(()->{a.onPause();a.onResume();a.onResume();Handler.advance(800);});
  check(AlertDialog.shown==1&&UpdatePrompt.shown==(update?1:0),"duplicate modal after resume");
  check(!PopupWindow.last.showing,"startup indicator resurrected");
  ProbeUi.run(()->{a.onPause();a.onDestroy();Handler.advance(6000);});
  check(Handler.pending()==0,"destroy left startup/hold timers");
  System.out.println("passed "+args[0]+" hold="+held);
 }
}
'''

JNI = r'''
#include "android_exit_notice_jni.h"
JNIEXPORT void JNICALL Java_com_aure_totk_StartupProbe_start(JNIEnv *e,jclass c,jobject a,jbyteArray l,jbyteArray d,jboolean held){
 (void)c;game_hold.enabled=held;android_boot_update("Starting game",0,0);JavaVM *vm=NULL;(*e)->GetJavaVM(e,&vm);ANativeActivity activity={vm,e,a,33};
 jsize nl=(*e)->GetArrayLength(e,l),nd=(*e)->GetArrayLength(e,d);char *log=calloc((size_t)nl+1,1),*data=calloc((size_t)nd+1,1);
 if(!log||!data)abort();(*e)->GetByteArrayRegion(e,l,0,nl,(jbyte*)log);(*e)->GetByteArrayRegion(e,d,0,nd,(jbyte*)data);
 android_exit_notice_start(&activity,log,data);free(log);free(data);
}
JNIEXPORT void JNICALL Java_com_aure_totk_StartupProbe_lifecycle(JNIEnv *e,jclass c,jboolean p){(void)e;(void)c;android_game_lifecycle_pause(p);}
JNIEXPORT void JNICALL Java_com_aure_totk_StartupProbe_handoff(JNIEnv *e,jclass c){(void)e;(void)c;atomic_fetch_add(&presented,1);}
JNIEXPORT jint JNICALL Java_com_aure_totk_StartupProbe_flushes(JNIEnv *e,jclass c){(void)e;(void)c;int n=0;for(int i=0;i<background_calls;i++)n+=background[i];return n;}
JNIEXPORT jboolean JNICALL Java_com_aure_totk_StartupProbe_paused(JNIEnv *e,jclass c){(void)e;(void)c;return game_pause.paused;}
JNIEXPORT jint JNICALL Java_com_aure_totk_TotkActivity_nativeFeedbackEnabled(JNIEnv *e,jclass c){(void)e;(void)c;return 0;}
JNIEXPORT void JNICALL Java_com_aure_totk_TotkActivity_nativeGameFocus(JNIEnv *e,jclass c,jboolean f,jboolean s){runtime_android_game_focus(e,c,f,s);}
JNIEXPORT jstring JNICALL Java_com_aure_totk_TotkActivity_nativeBootProgress(JNIEnv *e,jclass c,jlongArray a){return runtime_android_boot_progress(e,c,a);}
'''


class AndroidStartupLifecycle(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        javac, cls.java, cc = shutil.which('javac'), shutil.which('java'), shutil.which('cc')
        if not javac or not cls.java or not cc:
            raise unittest.SkipTest('host JDK and C compiler required')
        include = boot._jni_headers(javac)
        scratch = Path(os.environ.get('TOTK_SETUP_WORK', os.environ.get('TMPDIR', ROOT / '.scratch')))
        scratch.mkdir(parents=True, exist_ok=True)
        temp = tempfile.TemporaryDirectory(prefix='startup-lifecycle-', dir=scratch)
        cls.addClassCleanup(temp.cleanup)
        cls.work = Path(temp.name)
        paths = []
        for name, text in {**STUBS, 'com/aure/totk/StartupProbe.java': PROBE}.items():
            path = cls.work / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(text)
            paths.append(path)
        cls.classes = cls.work / 'classes'
        paths += [JAVA / name for name in ('TotkActivity.java', 'ExitNotice.java', 'ExitNoticePolicy.java',
                  'ExitNoticeStore.java', 'GameHold.java', 'TextRules.java', 'setup/BootProgress.java')]
        exits.Fixture.command([javac, '--release', '17', '-d', cls.classes, *paths])
        frontend = (ROOT / 'games/totk/runtime/frontend_android.c').read_text()
        pause = hold.section(frontend, 'static int game_activity_paused, game_focus_paused;', '/* Top-resumed is global;')
        pause += hold.section(frontend, 'static void android_game_lifecycle_pause(', 'static void on_pause(')
        snapshot = hold.section(frontend, 'static pthread_mutex_t boot_progress_mu', '/* One Looper owns')
        native = cls.work / 'bridge.c'
        native.write_text('#include <jni.h>\n#include <pthread.h>\n#include <stdatomic.h>\n'
                          'typedef struct {JavaVM *vm;JNIEnv *env;jobject clazz;int sdkVersion;} ANativeActivity;\n'
                          'static _Atomic unsigned presented;\n' + hold.C_PRELUDE + pause + snapshot + JNI)
        cls.library = cls.work / 'libstartup.so'
        exits.Fixture.command([cc, '-std=c11', '-Wall', '-Wextra', '-Werror', '-Wno-misleading-indentation',
                               '-fPIC', '-shared', '-pthread', '-I', include, '-I', include / 'linux',
                               '-I', ROOT / 'games/totk/runtime', native, '-o', cls.library])

    def probe(self, mode):
        for held in ('false', 'true'):
            with self.subTest(hold=held), tempfile.TemporaryDirectory(prefix='case-', dir=self.work) as case:
                result = exits.Fixture.command([self.java, '-cp', self.classes, 'com.aure.totk.StartupProbe',
                                               mode, case, self.library, held])
                self.assertIn('passed ' + mode, result.stdout)

    def test_indicator_dismisses_before_notice_and_never_reappears(self):
        self.probe('notice')

    def test_pending_update_then_notice_follow_indicator_dismissal(self):
        self.probe('update')

    def test_background_before_first_frame_preserves_hold_and_refresh_lifecycle(self):
        self.probe('pause')
