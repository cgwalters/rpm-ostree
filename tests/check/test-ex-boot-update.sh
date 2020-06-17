#!/bin/bash
#
# Copyright (C) 2020 Colin Walters <walters@verbum.org>
#
# This library is free software; you can redistribute it and/or
# modify it under the terms of the GNU Lesser General Public
# License as published by the Free Software Foundation; either
# version 2 of the License, or (at your option) any later version.
#
# This library is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
# Lesser General Public License for more details.
#
# You should have received a copy of the GNU Lesser General Public
# License along with this library; if not, write to the
# Free Software Foundation, Inc., 59 Temple Place - Suite 330,
# Boston, MA 02111-1307, USA.

set -euo pipefail

. ${commondir}/libtest.sh

echo "1..1"

bootefi=root/boot/efi
ostefi=root/usr/lib/ostree-boot/efi
mkdir -p "${bootefi}" "${ostefi}"
for dir in "${bootefi}" "${ostefi}"; do
  (cd ${dir}
   mkdir -p EFI/fedora
   cd EFI/fedora
   for x in grubx64.efi shim.efi shimx64.efi; do
    echo "some code for ${x}" > ${x}
   done
  )
done

validate_v0() {
  runv rpm-ostree ex-boot-update status --sysroot=root --component=EFI | tee out.txt
  assert_file_has_content_literal out.txt 'Component EFI'
  assert_file_has_content_literal out.txt '  Unmanaged: digest=2hPQ3dBBMqi7nnYYLj7yFwLNZAyMf8Wf6PMSfGYHa8idiHRAUskVD1NkKu8na2ZySu5BeVLAu5Sgh2sS6TFzeP5'
  assert_file_has_content_literal out.txt 'Update: At latest version'
  assert_not_file_has_content out.txt 'Component BIOS'
}

# This hack avoids us depending on having an ostree sysroot set up for now
export BOOT_UPDATE_TEST_TIMESTAMP=$(date -u --iso-8601=seconds)
validate_v0

echo 'v2 code for grubx64.efi' > "${ostefi}"/EFI/fedora/grubx64.efi
runv rpm-ostree ex-boot-update status --sysroot=root --component=EFI | tee out.txt
assert_file_has_content_literal out.txt 'Update: Available: '${BOOT_UPDATE_TEST_TIMESTAMP}
assert_file_has_content_literal out.txt '    Diff: changed=1 added=0 removed=0'

# Revert back
echo 'some code for grubx64.efi' > "${ostefi}"/EFI/fedora/grubx64.efi
validate_v0

runv rpm-ostree ex-boot-update adopt --sysroot=root | tee out.txt
assert_file_has_content_literal out.txt "Adopting: EFI"

runv rpm-ostree ex-boot-update adopt --sysroot=root | tee out.txt
assert_not_file_has_content_literal out.txt "Adopting: EFI"
assert_file_has_content_literal out.txt "Nothing to do"

echo "ok error ex-boot-update"
