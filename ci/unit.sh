#!/bin/bash
set -euo pipefail

dn=$(dirname $0)
. ${dn}/libbuild.sh

ci/installdeps.sh
ci/build.sh
