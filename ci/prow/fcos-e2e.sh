#!/bin/bash
set -xeuo pipefail

# Prow jobs don't support adding emptydir today
export COSA_SKIP_OVERLAY=1
# And suppress depcheck since we didn't install via RPM
export COSA_SUPPRESS_DEPCHECK=1
ls -al /usr/bin/rpm-ostree
rpm-ostree --version
cd $(mktemp -d)
cosa init https://github.com/coreos/fedora-coreos-config/
cp /cosa/component-rpms/*.rpm overrides/rpm
cosa fetch
cosa build
cosa kola run 'ext.rpm-ostree.*'
