#!/bin/sh

srcdir=$1
shift
PKG_VER=$1
shift
GITREV=$1
shift

TARFILE=${PKG_VER}.tar
TARFILE_TMP=$(pwd)/${TARFILE}.tmp

set -x
set -e

test -n "${srcdir}"
test -n "${PKG_VER}"
test -n "${GITREV}"

TOP=$(git rev-parse --show-toplevel)

echo "Archiving ${PKG_VER} at ${GITREV} to ${TARFILE_TMP}"
(cd ${TOP}; git archive --format=tar --prefix=${PKG_VER}/ ${GITREV}) > ${TARFILE_TMP}
ls -al ${TARFILE_TMP}
(cd ${TOP}; git submodule status) | while read line; do
    rev=$(echo ${line} | cut -f 1 -d ' '); path=$(echo ${line} | cut -f 2 -d ' ')
    echo "Archiving ${path} at ${rev}"
    (cd ${srcdir}/${path}; git archive --format=tar --prefix=${PKG_VER}/${path}/ ${rev}) > submodule.tar
    tar -A -f ${TARFILE_TMP} submodule.tar
    rm submodule.tar
done
tmpd=$(mktemp -d)
touch ${tmpd}/.tmp
rm -f rust.tar
(cd ${tmpd}
 mkdir -p .cargo vendor
 cargo vendor -q --sync ${TOP}/rust/Cargo.toml vendor
 cp ${TOP}/rust/Cargo.lock .
 cp ${TOP}/rust/cargo-vendor-config .cargo/config
 tar --transform='s,^,rust/,' -vrf ${TARFILE_TMP} .
 )
test -f "${tmpd}/.tmp" && rm -rf "${tmpd}"
mv ${TARFILE_TMP} ${TARFILE}
