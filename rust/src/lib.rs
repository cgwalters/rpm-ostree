/*
 * Copyright (C) 2018 Red Hat, Inc.
 *
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 */

#![deny(unused_must_use)]

// pub(crate) utilities
mod ffiutil;
mod cxx_bridge_gobject;
pub use cxx_bridge_gobject::*;
mod includes;

mod core;
use crate::core::*;

#[cxx::bridge(namespace = "rpmostreecxx")]
mod ffi {
    // core.rs
    extern "Rust" {
        type TempEtcGuard;

        fn prepare_tempetc_guard(rootfs: i32) -> Result<Box<TempEtcGuard>>;
        fn undo(self: &TempEtcGuard) -> Result<()>;
    }

    extern "C++" {
        include!("src/libpriv/rpmostree-cxxrs-prelude.h");

        type OstreeRepo = crate::FFIOstreeRepo;
        type OstreeDeployment = crate::FFIOstreeDeployment;
    }

    // origin.rs
    extern "Rust" {
        type Origin;

        fn origin_parse_deployment(deployment: Pin<&mut OstreeDeployment>) -> Result<Box<Origin>>;
        fn is_rojig(&self) -> bool;
        fn get_override_local_pkgs(&self) -> Vec<String>;
    }
}

mod cliwrap;
pub use cliwrap::*;
mod composepost;
pub use self::composepost::*;
mod history;
pub use self::history::*;
mod journal;
pub use self::journal::*;
mod initramfs;
pub use self::initramfs::ffi::*;
mod lockfile;
pub use self::lockfile::*;
mod livefs;
pub use self::livefs::*;
mod origin;
mod ostree_diff;
mod ostree_utils;
pub use self::origin::*;
mod progress;
pub use self::progress::*;
mod testutils;
pub use self::testutils::*;
mod treefile;
pub use self::treefile::*;
mod utils;
pub use self::utils::*;
