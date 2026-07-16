#!/usr/bin/env bash
# Track B — B-M4 item 2 (doc-2 §6): build the runtime archive for a cross
# target into runtime/<triple>/liblvrt.a. The driver (`leviathan --build-native
# --target <triple>`) resolves that archive by triple; --runtime overrides.
#
# The runtime is C17 (doc-2 hurdle B-H10) — this compiles it with a cross *C*
# compiler, never a C++ one. Two toolchains are supported, matching the
# driver's own cross linker probe order:
#   1. clang -target <triple>   (one clang retargets to any triple; needs a
#                                sysroot for the target's headers/libs)
#   2. <triple>-gcc             (a distro cross-gcc, e.g. aarch64-linux-gnu-gcc)
#
# Overrides (env):
#   LVRT_CROSS_CC   full compiler invocation (skips auto-detect entirely)
#   LVRT_SYSROOT    passed as --sysroot=<dir> (clang path; usually required)
#   LVRT_AR         archiver (else <triple>-ar, llvm-ar, then host ar)
#   LVRT_OUT_DIR    output dir (default: <repo>/runtime/<triple>)
#
# usage: runtime/build-triple.sh [<triple>]   (default aarch64-linux-gnu)
set -euo pipefail

triple="${1:-aarch64-linux-gnu}"
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
out_dir="${LVRT_OUT_DIR:-$here/$triple}"

# The LLVM target triple (what --target/emitObject and the archive directory
# are keyed by) and the distro cross-toolchain's binary prefix are the same
# string for aarch64-linux-gnu, but NOT for Windows: the MinGW-w64 packages
# install as `x86_64-w64-mingw32-{gcc,g++,ar}`, never `x86_64-pc-windows-gnu-*`
# (no such package name exists). Map the LLVM triple to the toolchain prefix
# actually on PATH; every other triple maps to itself (unchanged behavior).
case "$triple" in
  x86_64-pc-windows-gnu|x86_64-*-mingw32*) gnu_prefix="x86_64-w64-mingw32" ;;
  *)                                       gnu_prefix="$triple" ;;
esac

# --- select the cross C compiler ---------------------------------------------
cc="${LVRT_CROSS_CC:-}"
if [ -z "$cc" ]; then
  if command -v clang >/dev/null 2>&1; then
    cc="clang -target $triple"
    if [ -n "${LVRT_SYSROOT:-}" ]; then
      cc="$cc --sysroot=$LVRT_SYSROOT"
    else
      echo "build-triple.sh: warning: using clang -target $triple with no" \
           "LVRT_SYSROOT; set it if headers/libs for $triple are not on the" \
           "default search path" >&2
    fi
  elif command -v "${gnu_prefix}-gcc" >/dev/null 2>&1; then
    cc="${gnu_prefix}-gcc"
  else
    echo "build-triple.sh: no cross C compiler for '$triple' found" \
         "(looked for 'clang' and '${gnu_prefix}-gcc'); install one or set" \
         "LVRT_CROSS_CC" >&2
    exit 3
  fi
fi

# --- select the archiver (target-aware ranlib index preferred) ---------------
ar_tool="${LVRT_AR:-}"
if [ -z "$ar_tool" ]; then
  for cand in "${gnu_prefix}-ar" "llvm-ar" "ar"; do
    if command -v "$cand" >/dev/null 2>&1; then ar_tool="$cand"; break; fi
  done
fi

# --- compile the four C17 runtime sources and archive them -------------------
# The platform floor is target-selected: a Windows/MinGW triple swaps the POSIX
# floor for the Win32 one (B-M5, doc-2 §7). NOTE: the win32 branch is UNTESTED
# on this host (no MinGW-w64), and the driver's link step still needs a Windows
# port (-lws2_32, .exe) before an end-to-end Windows build works — see §10.
case "$triple" in
  *windows*|*mingw*) floor=lv_plat_win32.c ;;
  *)                 floor=lv_plat_posix.c ;;
esac

# LA-2 (techdesign-tls-crypto.md §5.2): TLS provider selection for the cross
# archive. Default is the not-built provider (lv_tls_none.c) — a cross triple
# rarely has a target OpenSSL on its search path, and a plaintext binary must
# always build. LVRT_TLS=system opts into the OpenSSL provider (target -lssl
# -lcrypto must be reachable via the sysroot); LVRT_TLS=off forces none; the
# default (auto) enables it only if a target pkg-config reports openssl.
# lvrt.link (written beside the archive) carries the extra link flags the
# driver's --build-native step appends; empty for the none provider.
tls_mode="${LVRT_TLS:-auto}"
tls_src=lv_tls_none.c
link_flags=""
# LA-2 §8: the win32 floor's lv_plat_random uses BCryptGenRandom -> -lbcrypt.
case "$triple" in *windows*|*mingw*) link_flags="-lbcrypt" ;; esac
if [ "$tls_mode" = "system" ]; then
  tls_src=lv_tls_openssl.c; link_flags="$link_flags -lssl -lcrypto"
elif [ "$tls_mode" != "off" ]; then
  if command -v "${gnu_prefix}-pkg-config" >/dev/null 2>&1 \
     && "${gnu_prefix}-pkg-config" --exists openssl 2>/dev/null; then
    tls_src=lv_tls_openssl.c
    link_flags="$link_flags $("${gnu_prefix}-pkg-config" --libs openssl)"
  fi
fi

# lv_thread.c completes the archive (lv_runtime.c calls lv_thread_* since Track
# 10 landed); $tls_src is the selected TLS provider.
# LA-30 doc 2: lv_runtime.c/lv_loop.c/lv_entry.c reference the task substrate
# (lvrt_await parks; the entry/loop install scheduler hooks), so lv_task.c is
# in every archive. On a Windows triple it compiles the _WIN32 stub block
# (tasks report disabled; the engine keeps the pump) and needs no context
# switch; POSIX triples add the .S switches, which self-guard by arch.
case "$triple" in
  *windows*|*mingw*) task_srcs=(lv_task.c) ;;
  *)                 task_srcs=(lv_task.c lv_ctx_x86_64.S lv_ctx_aarch64.S) ;;
esac
srcs=(lv_runtime.c "$floor" lv_loop.c lv_thread.c lv_entry.c "${task_srcs[@]}" "$tls_src")
mkdir -p "$out_dir"
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT

objs=()
for s in "${srcs[@]}"; do
  o="$work/${s%.*}.o"
  echo "  CC[$triple] $s"
  # -std=gnu17 matches the main CMake build (CMAKE_C_STANDARD 17 with the
  # default C_EXTENSIONS ON resolves to gnu17). Strict -std=c17 would hide
  # POSIX symbols the floor needs (clock_gettime/CLOCK_MONOTONIC, mmap flags),
  # since glibc gates them behind _DEFAULT_SOURCE, which gnu17 enables and
  # c17 does not — so this must stay gnu17 to build the same runtime CI tests.
  # shellcheck disable=SC2086  # $cc may legitimately carry -target/--sysroot
  $cc -std=gnu17 -O2 -Wall -Wextra -I"$here" -c "$here/$s" -o "$o"
  objs+=("$o")
done

archive="$out_dir/liblvrt.a"
rm -f "$archive"
"$ar_tool" rcs "$archive" "${objs[@]}"
# LA-2: the link-flag sidecar the driver appends for this archive's providers.
printf '%s\n' "$link_flags" > "$out_dir/lvrt.link"
echo "built $archive ($ar_tool, $cc; tls=$tls_src)"
