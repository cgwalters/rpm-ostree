#!/bin/bash
set -euo pipefail

# Pin to branch for some reproducibility
BRANCH=f37

dn=$(cd "$(dirname "$0")" && pwd)
topsrcdir=$(cd "$dn/.." && pwd)
commondir=$(cd "$dn/common" && pwd)
export topsrcdir commondir

# shellcheck source=common/libtest-core.sh
. "${commondir}/libtest.sh"
# Work around buggy check for overlayfs on /, but we're not writing to that
unset OSTREE_NO_XATTRS
unset OSTREE_SYSROOT_DEBUG

rm -rf compose-baseimage-test
mkdir compose-baseimage-test
cd compose-baseimage-test
git clone --depth=1 https://pagure.io/workstation-ostree-config --branch "${BRANCH}"

mkdir cache
rpm-ostree compose baseimage --cachedir=cache --touch-if-changed=changed.stamp --initialize workstation-ostree-config/fedora-silverblue.yaml fedora-silverblue.ociarchive
skopeo inspect oci-archive:fedora-silverblue.ociarchive
test -f changed.stamp
rm -f changed.stamp
rpm-ostree compose baseimage --cachedir=cache --touch-if-changed=changed.stamp workstation-ostree-config/fedora-silverblue.yaml fedora-silverblue.ociarchive | tee out.txt
test '!' -f changed.stamp
assert_file_has_content_literal out.txt 'No apparent changes since previous commit'

echo "ok compose baseimage"
