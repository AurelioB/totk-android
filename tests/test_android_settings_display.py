"""Execute production Settings handoff callbacks with recording framework doubles.

Task movement/reuse remains an Android device acceptance test; this checks that
we target our own task and preserve cold recovery / live gated actions.
"""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / 'games/totk/android/java/com/aure/totk/setup/SetupActivity.java'

FRAMEWORK = r'''
import java.util.*;
class Bundle { int display; Bundle(int id) { display=id; } }
class Display { static final int DEFAULT_DISPLAY=0; int id; Display(int id) { this.id=id; } int getDisplayId(){return id;} }
class Intent {
    static final int FLAG_ACTIVITY_NEW_TASK=0x10000000, FLAG_ACTIVITY_SINGLE_TOP=0x20000000;
    final Map<String,Object> extras=new HashMap<>(); int flags;
    Intent putExtra(String key,boolean value){extras.put(key,value);return this;}
    Intent() {} Intent(Intent other){extras.putAll(other.extras); flags=other.flags;}
    Intent addFlags(int value){flags|=value;return this;}
    boolean getBooleanExtra(String key,boolean fallback){return (boolean)extras.getOrDefault(key,fallback);}
    String getStringExtra(String key){return (String)extras.get(key);}
}
class ActivityOptions {
    int display; static ActivityOptions makeBasic(){return new ActivityOptions();}
    ActivityOptions setLaunchDisplayId(int id){display=id;return this;} Bundle toBundle(){return new Bundle(display);}
}
class ActivityManager {
    final List<AppTask> tasks=new ArrayList<>();
    List<AppTask> getAppTasks(){return tasks;}
    static class TaskInfo {int taskId; TaskInfo(int id){taskId=id;}}
    static class AppTask {
        final int id; int starts; Intent launched; Bundle options; boolean fail;
        AppTask(int id){this.id=id;} TaskInfo getTaskInfo(){return new TaskInfo(id);}
        void startActivity(Activity owner,Intent intent,Bundle bundle){
            starts++; launched=intent; options=bundle;
            if(fail)throw new SecurityException("denied");
        }
    }
}
class Activity {
    final ActivityManager manager=new ActivityManager(); Display display=new Display(4); int taskId=17;
    Display getDisplay(){return display;} int getTaskId(){return taskId;}
    <T> T getSystemService(Class<T> type){return type.cast(manager);}
    void setIntent(Intent intent){} protected void onNewIntent(Intent intent){}
}
class SettingsStore { static boolean dual=true; static boolean secondaryDisplay(Activity owner){return dual;} }
class InstallState { static InstallState read(Activity owner){return new InstallState();} boolean installed(){return true;} }
class Log { static void w(String tag,String message,Throwable error){} }
'''

PROBE = r'''
public final class SettingsDisplayProbe {
    static void check(boolean yes,String why){if(!yes)throw new AssertionError(why);}
    static Intent settings(){Intent i=new Intent();i.extras.put(SetupActivity.EXTRA_SETTINGS,true);return i;}
    static SetupActivity owner(){
        SettingsStore.dual=true;
        SetupActivity a=new SetupActivity();
        a.manager.tasks.add(new ActivityManager.AppTask(99));
        a.manager.tasks.add(new ActivityManager.AppTask(17));return a;
    }
    static void delivered(SetupActivity a,Intent intent){a.display=new Display(0);a.onNewIntent(intent);}
    public static void main(String[] args){
        if(args[0].equals("target")){
            SetupActivity a=owner(); Intent i=settings();
            check(a.moveSettingsToMain(i),"bottom Settings not handed off");
            ActivityManager.AppTask own=a.manager.tasks.get(1);
            check(a.manager.tasks.get(0).starts==0 && own.starts==1,"not bound to menu task");
            check(own.options.display==0 && own.launched.flags==(Intent.FLAG_ACTIVITY_NEW_TASK|Intent.FLAG_ACTIVITY_SINGLE_TOP),"wrong display/reuse flags");
            check(!a.moveSettingsToMain(i) && own.starts==1,"recursive handoff");
            delivered(a,i);check(!a.settingsMoving,"move guard not reset");
            a.display=new Display(4);a.settingsMoving=false;own.fail=true;
            check(!a.moveSettingsToMain(i) && !a.settingsMoving,"failed handoff latched");
            own.fail=false;check(a.moveSettingsToMain(i),"failed handoff cannot retry");
            a=owner();a.manager.tasks.clear();check(!a.moveSettingsToMain(i),"missing task used unbound fallback");
        }else if(args[0].equals("cold")){
            for(SetupActivity.Screen recovery:new SetupActivity.Screen[]{SetupActivity.Screen.ERROR,SetupActivity.Screen.INTERRUPTED}){
                for(boolean configFirst:new boolean[]{false,true}){
                    SetupActivity a=owner();a.recovery=recovery;Intent i=settings();
                    check(a.moveSettingsToMain(i) && !a.ui,"cold handoff initialized bottom UI");
                    Intent handoff=a.manager.tasks.get(1).launched;
                    if(configFirst){a.display=new Display(0);a.completeSettingsMove();}
                    delivered(a,handoff);
                    check(a.ui && a.shown==1 && a.screen==recovery && a.replacements==0,"cold recovery overwritten with Home");
                }
            }
        }else if(args[0].equals("pending")){
            SetupActivity a=owner();a.ui=true;a.screen=SetupActivity.Screen.DATA;
            Runnable retry=()->{};a.blockedAction=retry;Intent i=settings();
            check(a.moveSettingsToMain(i),"warm handoff missing");delivered(a,i);
            check(a.screen==SetupActivity.Screen.DATA && a.blockedAction==retry && a.replacements==0,"pending action lost");
            a.closingGame=true;a.display=new Display(4);a.onNewIntent(i);
            check(a.screen==SetupActivity.Screen.DATA && a.blockedAction==retry && a.manager.tasks.get(1).starts==1,"safe close interrupted");
            a=owner();a.ui=true;a.screen=SetupActivity.Screen.DATA;check(a.moveSettingsToMain(i),"warm settings handoff missing");delivered(a,i);
            check(a.screen==SetupActivity.Screen.HOME && a.replacements==1,"ordinary warm Settings no longer returns Home");
        }else if(args[0].equals("unchanged")){
            SetupActivity a=owner();Intent i=settings();a.display=new Display(0);
            check(!a.moveSettingsToMain(i),"main Settings moved");a.display=new Display(4);SettingsStore.dual=false;
            check(!a.moveSettingsToMain(i),"single/external screen changed");SettingsStore.dual=true;
            check(!a.moveSettingsToMain(new Intent()),"ordinary menu moved");
        }else throw new AssertionError(args[0]);
        System.out.println("pass: "+args[0]);
    }
}
'''


class SettingsDisplayTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        javac, cls.java = shutil.which('javac'), shutil.which('java')
        if not javac or not cls.java:
            raise unittest.SkipTest('javac/java not found')
        cls.temp = tempfile.TemporaryDirectory(prefix='totk-settings-display-')
        cls.addClassCleanup(cls.temp.cleanup)
        cls.work = Path(cls.temp.name)
        source = SOURCE.read_text()
        move = source[source.index('    private boolean settingsMoving;'):source.index('    /** A launcher tap')]
        new_intent = source[source.index('    @Override protected void onNewIntent(Intent intent)'):source.index('    @Override protected void onStart()')]
        body = (move + new_intent).replace('private boolean', 'boolean').replace('private void', 'void').replace('android.view.Display', 'Display').replace('android.util.Log', 'Log')
        wrapper = r'''
class SetupActivity extends Activity {
    static final String EXTRA_SETTINGS="settings",EXTRA_GAME_ERROR="error";
    enum Screen {HOME,WELCOME,DATA,INSTALL,ERROR,INTERRUPTED}
    boolean ui,closingGame; Runnable blockedAction; Screen screen,recovery=Screen.HOME;
    String gameError; int shown,replacements;
    void showUi(){ui=true;shown++;screen=recovery;}
    boolean returnToRunningGame(Intent intent){return false;}
    void replace(Screen value){screen=value;replacements++;} void render(){}
'''
        path = cls.work / 'SettingsDisplayProbe.java'
        path.write_text(FRAMEWORK + wrapper + body + '}\n' + PROBE)
        cls.run_command([javac, '--release', '17', '-d', cls.work, path])

    @classmethod
    def run_command(cls, command):
        result = subprocess.run(command, capture_output=True, text=True, timeout=60)
        if result.returncode:
            raise AssertionError(result.stdout + result.stderr)
        return result.stdout

    def probe(self, case):
        self.assertIn('pass: ' + case, self.run_command([self.java, '-cp', self.work, 'SettingsDisplayProbe', case]))

    def test_settings_handoff_targets_own_task_and_recovers_from_failure(self):
        self.probe('target')

    def test_cold_redirect_keeps_error_and_interrupted_recovery(self):
        self.probe('cold')

    def test_warm_handoff_keeps_gated_action_and_safe_close(self):
        self.probe('pending')

    def test_main_single_external_and_normal_menu_unchanged(self):
        self.probe('unchanged')


if __name__ == '__main__':
    unittest.main()
