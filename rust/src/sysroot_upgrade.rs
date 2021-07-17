//! Rust portion of `rpmostree-sysroot-upgrader.cxx`.

// SPDX-License-Identifier: Apache-2.0 OR MIT

use crate::cxxrsutil::*;
use crate::ffi::ContainerImport;
use anyhow::{Context, Result};
use gio::prelude::*;
use std::convert::TryInto;
use std::pin::Pin;

/// Import ostree commit in container image using ostree-rs-ext's API.
pub(crate) fn import_container(
    mut sysroot: Pin<&mut crate::FFIOstreeSysroot>,
    imgref: String,
    mut cancellable: Pin<&mut crate::FFIGCancellable>,
) -> CxxResult<Box<ContainerImport>> {
    // TODO: take a GCancellable and monitor it, and drop the import task (which is how async cancellation works in Rust).
    let sysroot = &sysroot.gobj_wrap();
    let cancellable = &cancellable.gobj_wrap();
    let repo = &sysroot.get_repo(Some(cancellable))?;
    let imgref = imgref.as_str().try_into()?;
    let imported = build_runtime()?.block_on(async {
        let (tx, rs) = tokio::sync::oneshot::channel();
        cancellable.connect_cancelled(move |c| drop(tx.send(())));

        tokio::select! {
            import = ostree_ext::container::import(&repo, &imgref, None) => {
                import
            }
            _ = rs => {
                Err(anyhow::anyhow!("Operation was cancelled"))
            }
        }
    })?;
    Ok(Box::new(ContainerImport {
        ostree_commit: imported.ostree_commit,
        image_digest: imported.image_digest,
    }))
}

/// Fetch the image digest for `imgref` using ostree-rs-ext's API.
pub(crate) fn fetch_digest(imgref: String) -> CxxResult<String> {
    let imgref = imgref.as_str().try_into()?;
    let digest = build_runtime()?
        .block_on(async { ostree_ext::container::fetch_manifest_info(&imgref).await })?;
    Ok(digest.manifest_digest)
}

fn build_runtime() -> Result<tokio::runtime::Runtime> {
    tokio::runtime::Builder::new_multi_thread()
        .enable_all()
        .build()
        .context("Failed to build tokio runtime")
}
