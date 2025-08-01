#!/bin/bash
# Check open filename arg using perf trace + vfs_getname (exclusive)

# Uses the 'perf test shell' library to add probe:vfs_getname to the system
# then use it with 'perf trace' using 'touch' to write to a temp file, then
# checks that that was captured by the vfs_getname was used by 'perf trace',
# that already handles "probe:vfs_getname" if present, and used in the
# "open" syscall "filename" argument beautifier.

# SPDX-License-Identifier: GPL-2.0
# Arnaldo Carvalho de Melo <acme@kernel.org>, 2017

# shellcheck source=lib/probe.sh
. "$(dirname $0)"/lib/probe.sh

skip_if_no_perf_probe || exit 2
skip_if_no_perf_trace || exit 2
[ "$(id -u)" = 0 ] || exit 2

. "$(dirname $0)"/lib/probe_vfs_getname.sh

trace_open_vfs_getname() {
	evts="$(echo "$(perf list tracepoint 2>/dev/null | grep -E 'syscalls:sys_enter_open(at)? ' | sed -r 's/.*sys_enter_([a-z]+) +\[.*$/\1/')" | sed ':a;N;s:\n:,:g')"
	perf trace -e $evts touch $file 2>&1 | \
	grep -E " +[0-9]+\.[0-9]+ +\( +[0-9]+\.[0-9]+ ms\): +touch/[0-9]+ open(at)?\((dfd: +CWD, +)?filename: +\"?${file}\"?, +flags: CREAT\|NOCTTY\|NONBLOCK\|WRONLY, +mode: +IRUGO\|IWUGO\) += +[0-9]+$"
}

add_probe_vfs_getname
err=$?

if [ $err -eq 1 ] ; then
        skip_if_no_debuginfo
        err=$?
fi

if [ $err -ne 0 ] ; then
	exit $err
fi

file=$(mktemp /tmp/temporary_file.XXXXX)

# Do not use whatever ~/.perfconfig file, it may change the output
# via trace.{show_timestamp,show_prefix,etc}
export PERF_CONFIG=/dev/null

trace_open_vfs_getname
err=$?
rm -f ${file}
cleanup_probe_vfs_getname
exit $err
