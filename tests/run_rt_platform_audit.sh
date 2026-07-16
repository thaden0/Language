#!/usr/bin/env bash
# Track B platform-floor audit (doc-2 §5): lv_runtime.c and lv_loop.c must
# never touch an OS primitive directly — everything routes through
# lv_plat_*.c, so a new target is a new lv_plat file, nothing else. This is
# the B-M1 grep (was manual, prose-only) now extended over both files and
# kept in CI as doc-2 §5 suggests.
root="$1"
fail=0
for f in "$root/runtime/lv_runtime.c" "$root/runtime/lv_loop.c" "$root/runtime/lv_entry.c"; do
  hits=$(grep -nE '\b(mmap|munmap|open|close|read|write|socket|bind|listen|accept|connect|send|recv|poll|fcntl|clock_gettime|stat|_exit)\s*\(' "$f" \
         | grep -v 'lv_plat_\|lvrt_\|lv_loop_\|lv_die\|lv_invoke1')
  if [ -n "$hits" ]; then
    echo "FAIL $f: direct OS call(s) outside lv_plat_*:"
    echo "$hits"
    fail=1
  fi
  hdrs=$(grep -nE '#include\s*<(unistd|fcntl|sys/socket|sys/mman|sys/stat|netinet/in|arpa/inet|poll|time)\.h>' "$f")
  if [ -n "$hdrs" ]; then
    echo "FAIL $f: OS-specific header included outside lv_plat_*:"
    echo "$hdrs"
    fail=1
  fi
done
if [ $fail -eq 0 ]; then
  echo "platform-floor audit clean: lv_runtime.c and lv_loop.c touch no OS primitive directly"
fi
exit $fail
