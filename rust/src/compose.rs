//! Logic for server-side builds; corresponds to rpmostree-builtin-compose-tree.cxx

// SPDX-License-Identifier: Apache-2.0 OR MIT

use anyhow::{anyhow, Result};
use camino::{Utf8Path, Utf8PathBuf};
use clap::Parser;
use ostree::gio;
use ostree_ext::container as ostree_container;
use ostree_ext::ostree;

use crate::cxxrsutil::CxxResult;

#[derive(clap::ValueEnum, Clone, Debug)]
enum OutputFormat {
    Ociarchive,
}

impl Default for OutputFormat {
    fn default() -> Self {
        Self::Ociarchive
    }
}

impl Into<ostree_container::Transport> for OutputFormat {
    fn into(self) -> ostree_container::Transport {
        match self {
            OutputFormat::Ociarchive => ostree_container::Transport::OciArchive,
        }
    }
}

#[derive(Debug, Parser)]
struct Opt {
    #[clap(long)]
    #[clap(value_parser)]
    /// Directory to use for caching downloaded packages and other data
    cachedir: Option<Utf8PathBuf>,

    #[clap(long, short = 'i')]
    /// Do not query previous image in target location; use this for the first build
    initialize: bool,

    #[clap(long, value_enum, default_value_t)]
    format: OutputFormat,

    #[clap(long)]
    /// Force a build
    force_nocache: bool,

    #[clap(long)]
    /// Operate only on cached data, do not access network repositories
    offline: bool,

    #[clap(long, value_parser)]
    /// Update the timestamp or create this file on changes
    touch_if_changed: Option<Utf8PathBuf>,

    #[clap(value_parser)]
    /// Path to the manifest file
    manifest: Utf8PathBuf,

    #[clap(value_parser)]
    /// Target path to write
    output: Utf8PathBuf,
}

/// Metadata relevant to base image builds that we extract from the container metadata.
struct ImageMetadata {
    version: Option<String>,
    inputhash: String,
}

/// Fetch the previous metadata from the container image metadata.
fn fetch_previous_metadata(imgref: &ostree_container::ImageReference) -> Result<ImageMetadata> {
    let handle = tokio::runtime::Handle::current();
    let (_manifest, _digest, config) = handle.block_on(async {
        let proxy = containers_image_proxy::ImageProxy::new().await?;
        let oi = &proxy.open_image(&imgref.to_string()).await?;
        let (digest, manifest) = proxy.fetch_manifest(oi).await?;
        let config = proxy.fetch_config(oi).await?;
        Ok::<_, anyhow::Error>((manifest, digest, config))
    })?;
    const INPUTHASH_KEY: &str = "rpmostree.inputhash";
    let labels = config
        .config()
        .as_ref()
        .ok_or_else(|| anyhow!("Missing config"))?
        .labels()
        .as_ref()
        .ok_or_else(|| anyhow!("Missing config labels"))?;

    let inputhash = labels
        .get(INPUTHASH_KEY)
        .ok_or_else(|| anyhow!("Missing config label {INPUTHASH_KEY}"))?
        .to_owned();
    Ok(ImageMetadata {
        version: labels.get("version").map(ToOwned::to_owned),
        inputhash,
    })
}

pub(crate) fn compose_baseimage(args: Vec<String>) -> CxxResult<()> {
    use crate::isolation::self_command;
    let cancellable = gio::NONE_CANCELLABLE;

    let opt = Opt::parse_from(args.iter().skip(1));

    let tempdir = tempfile::tempdir()?;
    let tempdir = Utf8Path::from_path(tempdir.path()).unwrap();

    let (_cachetempdir, cachedir) = match opt.cachedir {
        Some(p) => (None, p),
        None => {
            let t = tempfile::tempdir_in("/var/tmp")?;
            let p = Utf8Path::from_path(t.path()).unwrap().to_owned();
            (Some(t), p)
        }
    };
    let cachedir = &cachedir;
    let treecachedir = cachedir.join("v0");
    if !treecachedir.exists() {
        std::fs::create_dir(&treecachedir)?;
    }
    let repo = cachedir.join("repo");
    if !repo.exists() {
        let _repo = ostree::Repo::create_at(
            libc::AT_FDCWD,
            repo.as_str(),
            ostree::RepoMode::BareUser,
            None,
            cancellable,
        )?;
    }

    let target_imgref = ostree_container::ImageReference {
        transport: opt.format.clone().into(),
        name: opt.output.to_string(),
    };
    let previous_meta = (!opt.initialize)
        .then(|| fetch_previous_metadata(&target_imgref))
        .transpose()?;
    let mut compose_args_extra = Vec::new();
    if let Some(m) = previous_meta.as_ref() {
        compose_args_extra.extend(["--previous-inputhash", m.inputhash.as_str()]);
        if let Some(v) = m.version.as_ref() {
            compose_args_extra.extend(["--previous-version", v])
        }
    }

    let commitid_path = tempdir.join("commitid");
    let changed_path = tempdir.join("changed");

    let s = self_command()
        .args(&[
            "compose",
            "tree",
            "--unified-core",
            "--repo",
            repo.as_str(),
            "--write-commitid-to",
            commitid_path.as_str(),
            "--touch-if-changed",
            changed_path.as_str(),
            "--cachedir",
            treecachedir.as_str(),
        ])
        .args(opt.force_nocache.then(|| "--force-nocache"))
        .args(opt.offline.then(|| "--cache-only"))
        .args(compose_args_extra)
        .arg(opt.manifest.as_str())
        .status()?;
    if !s.success() {
        return Err(anyhow::anyhow!("compose tree failed: {:?}", s).into());
    }

    if !changed_path.exists() {
        return Ok(());
    }

    if let Some(p) = opt.touch_if_changed.as_ref() {
        std::fs::write(p, "")?;
    }

    let commitid = std::fs::read_to_string(&commitid_path)?;
    let target_imgref = target_imgref.to_string();

    let s = self_command()
        .args(&[
            "compose",
            "container-encapsulate",
            "--repo",
            repo.as_str(),
            commitid.as_str(),
            target_imgref.as_str(),
        ])
        .status()?;
    if !s.success() {
        return Err(anyhow::anyhow!("container-encapsulate failed: {:?}", s).into());
    }

    println!("Wrote: {target_imgref}");

    Ok(())
}
