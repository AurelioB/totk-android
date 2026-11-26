"""R2 regressions also run unchanged against 253aca1 via TOTK_EXIT_TEST_ROOT.

Every method is a behavioral regression (not an unavailable-new-API compile failure).
"""
import unittest
from android_exit_notice_fixture import Fixture


class ExitNoticeReviewRegressions(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.fixture = Fixture()
        cls.addClassCleanup(cls.fixture.close)

    def test_all_receipt_and_diagnostic_io_leaves_ui_responsive(self):
        failures = []
        for mode in ('native', 'io', 'io-lock'):
            result = self.fixture.run(mode)
            if result.returncode:
                failures.append(f'{mode}:\n{result.stdout}{result.stderr}')
        self.assertFalse(failures, '\n'.join(failures))

    def test_exit_record_identity_survives_rollback_timezone_reboot_and_pid_reuse(self):
        self.fixture.probe('clock')

    def test_recreation_and_concurrent_requests_preserve_one_delivery(self):
        self.fixture.probe('lifecycle')

    def test_rotated_history_and_cleared_state_prefer_no_notice(self):
        self.fixture.probe('missing-state')


if __name__ == '__main__':
    unittest.main()
