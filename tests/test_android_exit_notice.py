"""Production exit notice policy, worker delivery, durable state and native bridge coverage."""
import subprocess
import tempfile
import unittest
from android_exit_notice_fixture import Fixture, ROOT


class ExitNoticeTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.fixture = Fixture()
        cls.addClassCleanup(cls.fixture.close)

    def test_fabricated_exit_classification(self):
        self.fixture.probe('classify')

    def test_persistent_once_receipt_ordering_and_exact_silence_switch(self):
        self.fixture.probe('store')

    def test_android_adapter_on_both_cold_paths_and_controller_dismissal(self):
        for mode in ('low', 'normal', 'user', 'crash', 'anr', 'update', 'silent', 'closed'):
            with self.subTest(mode=mode):
                self.fixture.probe(mode)

    def test_older_android_does_nothing(self):
        self.fixture.probe('old-api')

    def test_two_processes_claim_only_once(self):
        fixture = self.fixture
        with tempfile.TemporaryDirectory(prefix='race-', dir=fixture.work) as case:
            base = [fixture.java, '-cp', str(fixture.classes), 'com.aure.totk.ExitR2Probe']
            fixture.command([*base, 'prepare', case])
            children = [subprocess.Popen([*base, 'claim-process', case], stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True) for _ in range(2)]
            results = [child.communicate(timeout=15) for child in children]
            self.assertEqual([child.returncode for child in children], [0, 0], results)
            self.assertEqual(sorted(out.strip() for out, err in results), ['false', 'true'])

    def test_native_atomic_start_background_close_and_bounded_receipts(self):
        self.fixture.probe('native')

    def test_receipts_are_not_a_destruction_or_render_thread_contract(self):
        source = (ROOT / 'games/totk/runtime/frontend_android.c').read_text()
        start = source[source.index('static int start_game('):source.index('/* ---------- input ----------')]
        self.assertIn('if (a->sdkVersion >= 30) android_exit_notice_start(a, g_log_path, g_data);', start)
        self.assertLess(start.index('pthread_create(&t, &at, game_thread, NULL)'), start.index('android_exit_notice_start('))
        self.assertLess(start.index('android_exit_notice_start('), start.index('android_close_requests_start(priv);'))
        close = source[source.index('int runtime_front_request_close(void)'):source.index('/* One stat per 500 ms')]
        self.assertLess(close.index('runtime_fs_begin_close()'), close.index('vk_pcache_shutdown();'))
        self.assertLess(close.index('vk_pcache_shutdown();'), close.index('android_exit_notice_event(2);'))
        self.assertLess(close.index('android_exit_notice_event(2);'), close.index('_exit(0);'))
        destroy = source[source.index('static void on_destroy('):source.index('__attribute__((visibility("default"))) void ANativeActivity_onCreate')]
        self.assertNotIn('android_exit_notice', destroy)
        self.assertNotIn('callbacks->onDestroy =', source)
        self.assertEqual(source.count('android_exit_notice_event(1);'), 1)
        for name in ('TotkActivity.java', 'setup/SetupActivity.java'):
            activity = (ROOT / 'games/totk/android/java/com/aure/totk' / name).read_text()
            self.assertIn('exitNotice.resume(', activity)
            self.assertIn('exitNotice.pause();', activity)


if __name__ == '__main__':
    unittest.main()
