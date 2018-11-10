#!/usr/bin/bash
# Used by rpmostree-core.c to intercept `groupadd` operations so
# we can convert to systemd-sysusers.
set -euo pipefail
ls /proc/self/fd/$RPMOSTREE_USERADD_FD >/dev/null
(echo groupadd
 for x in "$@"; do
     echo $x
 done
 echo
) > /proc/self/fd/$RPMOSTREE_USERADD_FD
exec /usr/sbin/groupadd.rpmostreesave "$@"
