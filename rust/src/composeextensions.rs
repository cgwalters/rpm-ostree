/*
 * Copyright (C) 2020 Red Hat, Inc.
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

use anyhow::Result;
use structopt::StructOpt;

#[derive(Debug, StructOpt)]
struct DownloadOpts {
    /// Path to treefile
    treefile: String,
    /// Path to extensions dir
    destdir: String,
}

#[derive(Debug, StructOpt)]
#[structopt(rename_all = "kebab-case")]
enum Opt {
    /// Download listed extensions
    Download(DownloadOpts),
}

fn download(opts: &DownloadOpts) -> Result<()> {
    dbg!("here");
    Ok(())
}

fn compose_extensions_main(args: &Vec<String>) -> Result<()> {
    let opt = Opt::from_iter(args.iter());
    match opt {
        Opt::Download(ref opts) => download(opts)?,
    };
    Ok(())
}

mod ffi {
    use super::*;
    use crate::ffiutil::*;
    use anyhow::Context;
    use glib;
    use lazy_static::lazy_static;
    use libc;
    use std::ffi::CString;

    #[no_mangle]
    pub extern "C" fn ror_compose_extensions_entrypoint(
        argv: *mut *mut libc::c_char,
        gerror: *mut *mut glib_sys::GError,
    ) -> libc::c_int {
        let v: Vec<String> = unsafe { glib::translate::FromGlibPtrContainer::from_glib_none(argv) };
        int_glib_error(compose_extensions_main(&v), gerror)
    }
}
pub use self::ffi::*;
