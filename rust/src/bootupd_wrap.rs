/*
 * Copyright (C) 2020 Red Hat, Inc.
 *
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 */

use glib_sys;
use libc;

use crate::ffiutil::*;

#[no_mangle]
pub extern "C" fn ror_boot_update_entrypoint(
    argv: *mut *mut libc::c_char,
    gerror: *mut *mut glib_sys::GError,
) -> libc::c_int {
    let v: Vec<String> = unsafe { glib::translate::FromGlibPtrContainer::from_glib_none(argv) };
    int_glib_error(bootupd::boot_update_main(&v), gerror)
}
