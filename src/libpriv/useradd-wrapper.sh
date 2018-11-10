#!/usr/bin/bash
# Used by rpmostree-core.c to intercept `useradd` operations so
# we can convert to systemd-sysusers.
set -euo pipefail
if ! ls /proc/self/fd/$RPMOSTREE_USERADD_FD >/dev/null; then
    ls -al /proc/self/fd
    exit 1
fi
(echo useradd
 for x in "$@"; do
     echo $x
 done) > /proc/self/fd/$RPMOSTREE_USERADD_FD
exec /usr/sbin/useradd.rpmostreesave "$@"
