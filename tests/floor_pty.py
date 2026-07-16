#!/usr/bin/env python3
# Terminal-floor test driver (designs/techdesign-terminal-floor.md §7). Modes:
#   winsize <rows> <cols> -- <cmd...>   run on a pty sized rows x cols; expect "RxC"
#   winch                 -- <cmd...>   resize the pty 5x rapidly, then SIGUSR1;
#                                       expect a coalesced final "winch=40x120"
#   rawkill               -- <cmd...>   raw mode + external SIGTERM; assert the
#                                       pty's termios is restored (ICANON+ECHO)
#   signal <signo> <want> -- <cmd...>   plain pipe; send <signo>, expect <want>
import os, sys, pty, termios, struct, fcntl, select, time, signal, subprocess

def split(argv):
    i = argv.index("--")
    return argv[:i], argv[i+1:]

def set_winsize(fd, rows, cols):
    fcntl.ioctl(fd, termios.TIOCSWINSZ, struct.pack("HHHH", rows, cols, 0, 0))

def wait_done(pid):
    # True once the child has exited (and never double-reaps / raises).
    try:
        return os.waitpid(pid, os.WNOHANG)[0] != 0
    except ChildProcessError:
        return True

def pty_run(cmd, setup=None, driver=None):
    # Fork a child with a pty as stdin/stdout/stderr. `setup(slave)` runs before
    # exec (e.g. set an initial winsize); `driver(master, pid, rd, slave)` after.
    m, s = os.openpty()
    a = termios.tcgetattr(s)
    a[3] |= (termios.ICANON | termios.ECHO)      # start canonical+echo
    termios.tcsetattr(s, termios.TCSANOW, a)
    if setup: setup(s)
    pid = os.fork()
    if pid == 0:
        os.close(m); os.setsid()
        os.dup2(s, 0); os.dup2(s, 1); os.dup2(s, 2); os.close(s)
        os.execvp(cmd[0], cmd); os._exit(127)
    def rd(t):
        r, _, _ = select.select([m], [], [], t)
        if not r: return b""
        try: return os.read(m, 4096)
        except OSError: return b""
    result = driver(m, pid, rd, s) if driver else None
    dl = time.time() + 2                          # ensure the child is gone
    while time.time() < dl and not wait_done(pid):
        rd(0.1)
    return "", s, result

def mode_winsize(rows, cols, cmd):
    def drv(m, pid, rd, s):
        b = bytearray()
        dl = time.time() + 3
        while time.time() < dl:
            b += rd(0.1)
            if wait_done(pid): break
        return b
    out, s, res = pty_run(cmd, setup=lambda fd: set_winsize(fd, rows, cols),
                          driver=drv)
    txt = (bytes(res).decode(errors="replace") if res else out)
    want = f"{rows}x{cols}"
    ok = want in txt
    print(f"winsize want={want} got={txt.strip()!r} -> {'ok' if ok else 'FAIL'}")
    return ok

def mode_winch(cmd):
    def drv(m, pid, rd, s):
        # let it reach signal::on + the loop, then storm 5 resizes
        time.sleep(0.6)
        for (r, c) in [(20, 60), (22, 70), (30, 90), (35, 110), (40, 120)]:
            set_winsize(s, r, c)
            os.kill(pid, signal.SIGWINCH)
            time.sleep(0.02)
        time.sleep(0.4)
        os.kill(pid, signal.SIGUSR1)          # end the program
        b = bytearray()
        dl = time.time() + 3
        while time.time() < dl:
            b += rd(0.1)
            if wait_done(pid): break
        return b
    out, s, res = pty_run(cmd, driver=drv)
    txt = (bytes(res).decode(errors="replace") if res else out)
    winch_lines = [ln for ln in txt.splitlines() if ln.startswith("winch=")]
    ok = len(winch_lines) >= 1 and winch_lines[-1] == "winch=40x120"
    print(f"winch ticks={len(winch_lines)} last={winch_lines[-1] if winch_lines else None!r} -> {'ok' if ok else 'FAIL'}")
    return ok

def mode_rawkill(cmd):
    def drv(m, pid, rd, s):
        # wait for RAWREADY (compiled backends) or fall back to a fixed delay
        # (the interpreters buffer stdout, so RAWREADY isn't visible yet)
        b = bytearray(); dl = time.time() + 1.5
        while time.time() < dl:
            b += rd(0.1)
            if b"RAWREADY" in b: break
        time.sleep(0.3)
        os.kill(pid, signal.SIGTERM)          # external kill; safety handler restores
        time.sleep(0.4)
        return b
    out, s, res = pty_run(cmd, driver=drv)
    lf = termios.tcgetattr(s)[3]
    restored = bool(lf & termios.ICANON) and bool(lf & termios.ECHO)
    print(f"rawkill termios_restored={restored} -> {'ok' if restored else 'FAIL'}")
    return restored

def mode_signal(signo, want, cmd):
    p = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    time.sleep(1.0)
    if p.poll() is not None:
        print(f"signal FAIL early-exit {p.stdout.read()!r}"); return False
    p.send_signal(int(signo))
    try:
        out, _ = p.communicate(timeout=5)
    except subprocess.TimeoutExpired:
        p.kill(); print("signal FAIL timeout"); return False
    ok = want in out
    print(f"signal want={want!r} got={out.strip()!r} -> {'ok' if ok else 'FAIL'}")
    return ok

# SU-1 unsubscribe isolation: send <signo> TWICE (gap between), then collect all
# output. Assert every string in <wantcsv> appears and every string in <nocsv>
# does NOT — proving a subscriber closed on the first delivery skips the second
# while a sibling keeps receiving (and the shared fd stays open, so the second
# signal is watched, not a default-disposition kill).
def mode_signal_twice(signo, wantcsv, nocsv, cmd):
    p = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    time.sleep(1.0)
    if p.poll() is not None:
        print(f"signal2 FAIL early-exit {p.stdout.read()!r}"); return False
    p.send_signal(int(signo))
    time.sleep(0.6)
    p.send_signal(int(signo))
    try:
        out, _ = p.communicate(timeout=5)
    except subprocess.TimeoutExpired:
        p.kill(); print("signal2 FAIL timeout"); return False
    want = [w for w in wantcsv.split(",") if w]
    no   = [w for w in nocsv.split(",") if w]
    ok = all(w in out for w in want) and all(w not in out for w in no)
    print(f"signal2 want={want} no={no} got={out.strip()!r} -> {'ok' if ok else 'FAIL'}")
    return ok

# SU-1 loop-drain exit: run with NO signal; the program subscribes under `using`
# and lets the scope close it, releasing the last watch. Assert it EXITS on its
# own (a live signal watch would pin the loop forever) with the wanted output.
def mode_drain_exit(wantcsv, cmd):
    p = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    try:
        out, _ = p.communicate(timeout=5)
    except subprocess.TimeoutExpired:
        p.kill(); print("drain FAIL hang (loop not drained)"); return False
    want = [w for w in wantcsv.split(",") if w]
    ok = p.returncode == 0 and all(w in out for w in want)
    print(f"drain rc={p.returncode} want={want} got={out.strip()!r} -> {'ok' if ok else 'FAIL'}")
    return ok

def main():
    head, cmd = split(sys.argv[1:])
    mode = head[0]
    if mode == "winsize": ok = mode_winsize(int(head[1]), int(head[2]), cmd)
    elif mode == "winch":  ok = mode_winch(cmd)
    elif mode == "rawkill": ok = mode_rawkill(cmd)
    elif mode == "signal": ok = mode_signal(head[1], head[2], cmd)
    elif mode == "signal2": ok = mode_signal_twice(head[1], head[2], head[3], cmd)
    elif mode == "drain": ok = mode_drain_exit(head[1], cmd)
    else: print("unknown mode", mode); ok = False
    sys.exit(0 if ok else 1)

main()
