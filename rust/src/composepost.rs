/*
 * Copyright (C) 2018 Red Hat, Inc.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301 USA
 */

use failure::Fallible;
use openat;
use rayon::prelude::*;
use std::io::{BufRead, Write};
use std::path::Path;
use std::{fs, io};

/// Helper functions for openat::Dir
trait OpenatDirExt {
    // IMO should propose this at least in a "utils" bit of the openat crate;
    // Like 95% of the time I'm looking at errno (with files) it's for ENOENT,
    // and Rust has an elegant way to map that with Option<>.  Every other
    // error I usually just want to propagate back up.
    fn open_file_optional<P: openat::AsPath>(&self, p: P) -> io::Result<Option<fs::File>>;
}

impl OpenatDirExt for openat::Dir {
    fn open_file_optional<P: openat::AsPath>(&self, p: P) -> io::Result<Option<fs::File>> {
        match self.open_file(p) {
            Ok(f) => Ok(Some(f)),
            Err(e) => {
                if e.kind() == io::ErrorKind::NotFound {
                    Ok(None)
                } else {
                    Err(e)
                }
            }
        }
    }
}

// rpm-ostree uses /home → /var/home by default as generated by our
// rootfs; we don't expect people to change this.  Let's be nice
// and also fixup the $HOME entries generated by `useradd` so
// that `~` shows up as expected in shells, etc.
//
// https://github.com/coreos/fedora-coreos-config/pull/18
// https://pagure.io/workstation-ostree-config/pull-request/121
// https://discussion.fedoraproject.org/t/adapting-user-home-in-etc-passwd/487/6
// https://github.com/justjanne/powerline-go/issues/94
fn postprocess_useradd(rootfs_dfd: &openat::Dir) -> Fallible<()> {
    let path = Path::new("usr/etc/default/useradd");
    if let Some(f) = rootfs_dfd.open_file_optional(path)? {
        let mut f = io::BufReader::new(f);
        let tmp_path = path.parent().unwrap().join("useradd.tmp");
        let o = rootfs_dfd.write_file(&tmp_path, 0644)?;
        let mut bufw = io::BufWriter::new(&o);
        for line in f.lines() {
            let line = line?;
            if !line.starts_with("HOME=") {
                bufw.write(line.as_bytes())?;
            } else {
                bufw.write("HOME=/var/home".as_bytes())?;
            }
            bufw.write("\n".as_bytes())?;
        }
        bufw.flush()?;
        rootfs_dfd.local_rename(&tmp_path, path)?;
    }
    Ok(())
}

// We keep hitting issues with the ostree-remount preset not being
// enabled; let's just do this rather than trying to propagate the
// preset everywhere.
fn postprocess_presets(rootfs_dfd: &openat::Dir) -> Fallible<()> {
    let mut o = rootfs_dfd.write_file("usr/lib/systemd/system-preset/40-rpm-ostree-auto.preset", 0644)?;
    o.write(r"@@@# Written by rpm-ostree compose tree
enable ostree-remount.service
enable ostree-finalize-staged.path
@@@".as_bytes())?;
    o.flush()?;
    Ok(())
}

// This function is called from rpmostree_postprocess_final(); think of
// it as the bits of that function that we've chosen to implement in Rust.
fn compose_postprocess_final(rootfs_dfd: &openat::Dir) -> Fallible<()> {
    let tasks = [postprocess_useradd, postprocess_presets];
    tasks.par_iter().try_for_each(|f| f(rootfs_dfd))
}

mod ffi {
    use super::*;
    use ffiutil::*;
    use glib_sys;
    use libc;

    #[no_mangle]
    pub extern "C" fn ror_compose_postprocess_final(
        rootfs_dfd: libc::c_int,
        gerror: *mut *mut glib_sys::GError,
    ) -> libc::c_int {
        let rootfs_dfd = ffi_view_openat_dir(rootfs_dfd);
        int_glib_error(compose_postprocess_final(&rootfs_dfd), gerror)
    }
}
pub use self::ffi::*;
