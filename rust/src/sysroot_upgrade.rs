//! Rust portion of `rpmostree-sysroot-upgrader.cxx`.

// SPDX-License-Identifier: Apache-2.0 OR MIT

use crate::cxxrsutil::*;
use crate::ffi::ContainerImport;
use std::convert::TryInto;
use std::pin::Pin;
use tokio::runtime::Handle;

/// Import ostree commit in container image using ostree-rs-ext's API.
pub(crate) fn import_container(
    mut repo: Pin<&mut crate::FFIOstreeRepo>,
    imgref: String,
) -> CxxResult<Box<ContainerImport>> {
    // TODO: take a GCancellable and monitor it, and drop the import task (which is how async cancellation works in Rust).
    let repo = repo.gobj_wrap();
    let imgref = imgref.as_str().try_into()?;
    let imported = Handle::current()
        .block_on(async { ostree_ext::container::import(&repo, &imgref, None).await })?;
    Ok(Box::new(ContainerImport {
        ostree_commit: imported.ostree_commit,
        image_digest: imported.image_digest,
    }))
}

/// Fetch the image digest for `imgref` using ostree-rs-ext's API.
pub(crate) fn fetch_digest(imgref: String) -> CxxResult<String> {
    let imgref = imgref.as_str().try_into()?;
    let digest = Handle::current()
        .block_on(async { ostree_ext::container::fetch_manifest_info(&imgref).await })?;
    Ok(digest.manifest_digest)
}
