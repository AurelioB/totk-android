"""Crash handler chaining (games/totk/runtime/crash_chain.h), the bookkeeping loader.c's crash and abort_report use.
A small C driver installs a fake "previous" handler (standing in for Android's debuggerd handler), then ours with
crash_chain_install, and crashes: the previous handler must get the signal after our report, with the original
siginfo for a real fault, and only one of two crashing threads may report.

CC accepts a shell-quoted compiler command; TMPDIR selects scratch output.
"""
import os
from pathlib import Path
import resource
import shlex
import shutil
import signal
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]

# The driver prints one line per event, through write(2) so nothing is lost at the end:
#   ours <first|nested|other> <thread>   our handler's claim
#   prev <signal> code=<kernel|sent> <thread>   the fake previous handler (which then _exits 40)
DRIVER = r'''
#define _GNU_SOURCE
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "crash_chain.h"
static __thread const char *who = "main";
static void say(const char *a, const char *b, const char *c) {
    char line[128]; size_t n = 0;
    for (const char *p[] = {a, " ", b, " ", c, "\n"}, **q = p; q < p + 6; q++) { size_t k = strlen(*q); memcpy(line + n, *q, k); n += k; }
    if (write(1, line, n) != (ssize_t)n) _exit(9);
}
static const char *name(int num) { return num == SIGSEGV ? "SEGV" : num == SIGABRT ? "ABRT" : num == SIGTRAP ? "TRAP" : "?"; }
static void previous(int num, siginfo_t *info, void *context) {
    (void)context; say("prev", name(num), info->si_code > 0 ? "code=kernel" : "code=sent"); say("by", who, ""); _exit(40);
}
static int hold_ms;
static void ours(int num, siginfo_t *info, void *context) {
    (void)context;
    int claim = crash_chain_claim();
    say("ours", claim == CRASH_CHAIN_FIRST ? "first" : claim == CRASH_CHAIN_NESTED ? "nested" : "other", who);
    if (claim == CRASH_CHAIN_OTHER) crash_chain_wait();
    if (claim == CRASH_CHAIN_FIRST && hold_ms) usleep(hold_ms * 1000);   /* a long report */
    crash_chain_forward(num, info);
}
static void install(int num, int fake_previous, int ignore_previous) {
    struct sigaction a = {0}; sigemptyset(&a.sa_mask);
    if (fake_previous) { a.sa_sigaction = previous; a.sa_flags = SA_SIGINFO; sigaction(num, &a, NULL); }
    if (ignore_previous) { a.sa_handler = SIG_IGN; a.sa_flags = 0; sigaction(num, &a, NULL); }
    a.sa_sigaction = ours; a.sa_flags = SA_SIGINFO | SA_ONSTACK;
    if (crash_chain_install(num, &a)) _exit(3);
}
static void fault(void) { *(volatile int *)(uintptr_t)8 = 1; }
static void *crasher(void *arg) { who = arg; fault(); return NULL; }
static void *claimer(void *arg) { (void)arg; say("claim", crash_chain_claim() == CRASH_CHAIN_OTHER ? "other" : "?", "worker"); return NULL; }
int main(int argc, char **argv) {
    if (argc != 2) return 2;
    const char *mode = argv[1];
    if (!strcmp(mode, "claims")) {
        say("claim", crash_chain_claim() == CRASH_CHAIN_FIRST ? "first" : "?", "main");
        say("claim", crash_chain_claim() == CRASH_CHAIN_NESTED ? "nested" : "?", "main");
        pthread_t t; pthread_create(&t, NULL, claimer, NULL); pthread_join(t, NULL);
        return 0;
    }
    if (!strcmp(mode, "fault")) { install(SIGSEGV, 1, 0); fault(); }
    if (!strcmp(mode, "sent")) { install(SIGSEGV, 1, 0); raise(SIGSEGV); }
    if (!strcmp(mode, "abort")) { install(SIGABRT, 1, 0); abort(); }
    if (!strcmp(mode, "trap")) { install(SIGTRAP, 1, 0); raise(SIGTRAP); }
    if (!strcmp(mode, "default")) { install(SIGSEGV, 0, 0); fault(); }
    if (!strcmp(mode, "ignored")) { install(SIGABRT, 0, 1); raise(SIGABRT); }
    if (!strcmp(mode, "nested")) {
        install(SIGSEGV, 1, 0); install(SIGABRT, 1, 0);
        crash_chain_claim();   /* this thread is already reporting a crash */
        abort();
    }
    if (!strcmp(mode, "race")) {
        install(SIGSEGV, 1, 0); hold_ms = 300;
        pthread_t a, b; pthread_create(&a, NULL, crasher, "A"); usleep(100000); pthread_create(&b, NULL, crasher, "B");
        pthread_join(a, NULL);
    }
    say("returned", mode, "");   /* the crash was swallowed */
    return 1;
}
'''


class CrashChain(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler = shlex.split(os.environ.get('CC', 'cc'))
        if not compiler or not shutil.which(compiler[0]):
            raise unittest.SkipTest('A native C compiler is required')
        scratch = Path(os.environ.get('TMPDIR', ROOT / '.scratch'))
        scratch.mkdir(parents=True, exist_ok=True)
        cls.temp = tempfile.TemporaryDirectory(prefix='crash-chain-', dir=scratch)
        cls.addClassCleanup(cls.temp.cleanup)
        source = Path(cls.temp.name) / 'driver.c'
        source.write_text(DRIVER)
        cls.driver = Path(cls.temp.name) / 'driver'
        result = subprocess.run([*compiler, '-std=c11', '-O1', '-Wall', '-Wextra', '-Werror', '-pthread', '-I',
                                 str(ROOT / 'games/totk/runtime'), str(source), '-o', str(cls.driver)],
                                capture_output=True, text=True)
        if result.returncode:
            raise AssertionError(result.stdout + result.stderr)

    def crash(self, mode):
        # No core file from the default-action cases.
        result = subprocess.run([str(self.driver), mode], capture_output=True, text=True, timeout=30, cwd=self.temp.name,
                                preexec_fn=lambda: resource.setrlimit(resource.RLIMIT_CORE, (0, 0)))
        return result.returncode, result.stdout.splitlines()

    def test_claims(self):
        self.assertEqual(self.crash('claims'), (0, ['claim first main', 'claim nested main', 'claim other worker']))

    def test_real_fault_refaults_into_the_previous_handler_with_its_siginfo(self):
        self.assertEqual(self.crash('fault'), (40, ['ours first main', 'prev SEGV code=kernel', 'by main ']))

    def test_sent_signals_and_aborts_are_raised_again(self):
        self.assertEqual(self.crash('sent'), (40, ['ours first main', 'prev SEGV code=sent', 'by main ']))
        self.assertEqual(self.crash('abort'), (40, ['ours first main', 'prev ABRT code=sent', 'by main ']))
        self.assertEqual(self.crash('trap'), (40, ['ours first main', 'prev TRAP code=sent', 'by main ']))

    def test_default_and_ignored_previous_actions_end_the_process(self):
        self.assertEqual(self.crash('default'), (-signal.SIGSEGV, ['ours first main']))
        self.assertEqual(self.crash('ignored'), (-signal.SIGABRT, ['ours first main']))

    def test_a_crash_inside_the_report_goes_straight_on(self):
        self.assertEqual(self.crash('nested'), (40, ['ours nested main', 'prev ABRT code=sent', 'by main ']))

    def test_only_the_first_crashing_thread_reports(self):
        code, lines = self.crash('race')
        self.assertEqual((code, lines), (40, ['ours first A', 'ours other B', 'prev SEGV code=kernel', 'by A ']))


if __name__ == '__main__':
    unittest.main()
