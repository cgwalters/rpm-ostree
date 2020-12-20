/*
 * Copyright (C) 2018 Red Hat, Inc.
 *
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 */

#![deny(unused_must_use)]

// pub(crate) utilities
mod ffiutil;
mod includes;

#[cxx::bridge(namespace = "rpmostreecxx")]
mod ffi {
    // core.rs
    extern "Rust" {
        type TempEtcGuard;

        fn prepare_tempetc_guard(rootfs: i32) -> Result<Box<TempEtcGuard>>;
        fn undo(self: &TempEtcGuard) -> Result<()>;

        fn get_systemctl_wrapper() -> &'static [u8];
    }

    // initramfs.rs
    extern "Rust" {
        fn get_dracut_random_cpio() -> &'static [u8];
    }

    // utils.rs
    extern "Rust" {
        fn download_to_fd(url: &str) -> Result<i32>;
    }

    // rpmutils.rs
    extern "Rust" {
        fn cache_branch_to_nevra(nevra: &str) -> String;
    }

    // rpmostree-rpm-util.h
    unsafe extern "C++" {
        include!("rpmostree-rpm-util.h");
        fn nevra_to_cache_branch(nevra: &CxxString) -> Result<UniquePtr<CxxString>>;
    }
}

mod cliwrap;
pub use cliwrap::*;
mod composepost;
pub use self::composepost::*;
mod core;
use crate::core::*;
mod history;
pub use self::history::*;
mod journal;
pub use self::journal::*;
mod initramfs;
pub use self::initramfs::*;
mod lockfile;
pub use self::lockfile::*;
mod livefs;
pub use self::livefs::*;
mod ostree_diff;
mod ostree_utils;
mod progress;
pub use self::progress::*;
mod rpmutils;
pub(crate) use self::rpmutils::*;
mod testutils;
pub use self::testutils::*;
mod treefile;
pub use self::treefile::*;
mod utils;
pub use self::utils::*;
