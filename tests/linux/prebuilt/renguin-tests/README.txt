Renguin test programs (Linux x86-64, static), built and branded 2026-11-07
from Techniix/Renguin branch stage5-libc-dynamic. All pass on real Linux.

On Haiku with Renguin installed:
    cd renguin-tests
    ./run-tests.sh .        -> expect "12 passed, 0 failed"
    ./libc hi               -> "hello from libc, argc=2 hi", exit status 3
    grep -i "renguin\|personalit" /var/log/syslog

Run from a writable directory: fileio and libc create and remove temp files.
See docs/TESTING.md in the repository for the full guide.
