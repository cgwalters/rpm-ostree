#!/bin/bash
set -xeuo pipefail

# Prow jobs don't support adding emptydir today
export COSA_SKIP_OVERLAY=1
gitdir=$(pwd)
cd $(mktemp -d)
cosa init --force https://github.com/coreos/fedora-coreos-config/
cosa fetch
cosa build
cosa kola run 'ext.*'
