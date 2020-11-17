//! An "origin" declares how we generated an OSTree commit.

/*
 * Copyright (C) 2020 Red Hat, Inc.
 *
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 */

use anyhow::{bail, Result};
use glib::translate::*;
use glib::KeyFile;
use std::{borrow::Cow, pin::Pin};
use std::result::Result as StdResult;
use crate::cxx_bridge_gobject::*;

use std::collections::{BTreeMap, BTreeSet};

const ROJIG_PREFIX: &str = "rojig://";
const ORIGIN: &str = "origin";
const OVERRIDE_COMMIT: &str = "override-commit";

#[derive(Debug, PartialEq, Eq)]
enum RefspecType {
    Ostree(String),
    Rojig(String),
    Checksum(String),
}

impl RefspecType {
    fn as_str(&self) -> &str {
        match self {
            Self::Ostree(s) => s,
            Self::Rojig(s) => s,
            Self::Checksum(s) => s,
        }
        .as_str()
    }
}

#[derive(Debug)]
struct Cache {
    refspec: RefspecType,
    override_commit: Option<String>,
    unconfigured_state: Option<String>,
    rojig_override_version: Option<String>,
    rojig_description: Option<String>,

    packages: BTreeSet<String>,
    packages_local: BTreeMap<String, String>,
    override_remove: BTreeSet<String>,
    override_replace_local: BTreeMap<String, String>,

    initramfs_etc: BTreeSet<String>,
    initramfs_args: Vec<String>,
}

#[derive(Debug)]
pub struct Origin {
    kf: KeyFile,
    cache: Cache,
}

fn keyfile_dup(kf: &KeyFile) -> KeyFile {
    let r = KeyFile::new();
    r.load_from_data(&kf.to_data(), glib::KeyFileFlags::KEEP_COMMENTS)
        .expect("keyfile parse");
    r
}

fn map_keyfile_optional<T>(res: StdResult<T, glib::Error>) -> StdResult<Option<T>, glib::Error> {
    match res {
        Ok(v) => Ok(Some(v)),
        Err(e) => {
            if let Some(t) = e.kind::<glib::KeyFileError>() {
                match t {
                    glib::KeyFileError::GroupNotFound | glib::KeyFileError::KeyNotFound => Ok(None),
                    _ => Err(e.into()),
                }
            } else {
                Err(e.into())
            }
        }
    }
}

fn parse_stringlist(kf: &KeyFile, group: &str, key: &str) -> Result<BTreeSet<String>> {
    let l = if let Some(l) = map_keyfile_optional(kf.get_string_list(group, key))? {
        l
    } else {
        return Ok(Default::default());
    };
    let mut r = BTreeSet::new();
    for it in l {
        r.insert(it.to_string());
    }
    Ok(r)
}

fn parse_localpkglist(kf: &KeyFile, group: &str, key: &str) -> Result<BTreeMap<String, String>> {
    let l = if let Some(l) = map_keyfile_optional(kf.get_string_list(group, key))? {
        l
    } else {
        return Ok(Default::default());
    };
    let mut r = BTreeMap::new();
    for it in l {
        let (nevra, sha256) = crate::utils::decompose_sha256_nevra(it.as_str())?;
        r.insert(nevra.to_string(), sha256.to_string());
    }
    Ok(r)
}

fn keyfile_get_optional_string(kf: &KeyFile, group: &str, key: &str) -> Result<Option<String>> {
    Ok(map_keyfile_optional(kf.get_value(group, key))?.map(|v| v.to_string()))
}

#[allow(dead_code)]
fn keyfile_get_nonempty_optional_string(
    kf: &KeyFile,
    group: &str,
    key: &str,
) -> Result<Option<String>> {
    if let Some(v) = keyfile_get_optional_string(&kf, group, key)? {
        if v.len() > 0 {
            return Ok(Some(v));
        }
    }
    Ok(None)
}

impl Origin {
    #[cfg(test)]
    fn new_from_str<S: AsRef<str>>(s: S) -> Result<Box<Self>> {
        let s = s.as_ref();
        let kf = glib::KeyFile::new();
        kf.load_from_data(s, glib::KeyFileFlags::KEEP_COMMENTS)?;
        Ok(Self::new_parse(&kf)?)
    }

    fn new_parse(kf: &KeyFile) -> Result<Box<Self>> {
        let kf = keyfile_dup(kf);
        let rojig = keyfile_get_optional_string(&kf, "origin", "rojig")?;
        let refspec_str = if let Some(r) = keyfile_get_optional_string(&kf, "origin", "refspec")? {
            Some(r)
        } else {
            keyfile_get_optional_string(&kf, "origin", "baserefspec")?
        };
        let refspec = match (refspec_str, rojig) {
            (Some(refspec), None) => {
                if ostree::validate_checksum_string(&refspec).is_ok() {
                    RefspecType::Checksum(refspec)
                } else {
                    RefspecType::Ostree(refspec)
                }
            },
            (None, Some(rojig)) => RefspecType::Rojig(rojig),
            (None, None) => bail!("No origin/refspec, origin/rojig, or origin/baserefspec in current deployment origin; cannot handle via rpm-ostree"),
            (Some(_), Some(_)) => bail!("Duplicate origin/refspec and origin/rojig in deployment origin"),
        };
        let override_commit = keyfile_get_optional_string(&kf, "origin", "override-commit")?;
        let unconfigured_state = keyfile_get_optional_string(&kf, "origin", "unconfigured-state")?;
        let packages = parse_stringlist(&kf, "packages", "requested")?;
        let packages_local = parse_localpkglist(&kf, "packages", "requested-local")?;
        let override_remove = parse_stringlist(&kf, "overrides", "remove")?;
        let override_replace_local = parse_localpkglist(&kf, "overrides", "replace-local")?;
        let initramfs_etc = parse_stringlist(&kf, "rpmostree", "initramfs-etc")?;
        let initramfs_args =
            map_keyfile_optional(kf.get_string_list("rpmostree", "initramfs-args"))?.map(|v| {
                let r: Vec<String> = v.into_iter().map(|s| s.to_string()).collect();
                r
            }).unwrap_or_default();
        let rojig_override_version =
            keyfile_get_optional_string(&kf, ORIGIN, "rojig-override-version")?;
        let rojig_description = keyfile_get_optional_string(&kf, ORIGIN, "rojig-description")?;
        Ok(Box::new(Self {
            kf,
            cache: Cache {
                refspec: refspec,
                override_commit,
                unconfigured_state,
                rojig_override_version,
                rojig_description,
                packages,
                packages_local,
                override_remove,
                override_replace_local,
                initramfs_etc,
                initramfs_args,
            },
        }))
    }
}

impl Origin {
    pub fn remove_transient_state(&mut self) {
        unsafe {
            ostree_sys::ostree_deployment_origin_remove_transient_state(self.kf.to_glib_none().0)
        }
        self.set_override_commit(None)
    }

    pub fn set_override_commit(&mut self, checksum: Option<(&str, Option<&str>)>) {
        match checksum {
            Some((checksum, ver)) => {
                self.kf.set_string(ORIGIN, OVERRIDE_COMMIT, checksum);
                self.cache.override_commit = Some(checksum.to_string());
                if let Some(ver) = ver {
                    let comment = format!("Version {}", ver);
                    // Ignore errors here, shouldn't happen
                    let _ =
                        self.kf
                            .set_comment(Some(ORIGIN), Some(OVERRIDE_COMMIT), comment.as_str());
                }
            }
            None => {
                // Ignore errors here, should only be failure to remove a nonexistent key.
                let _ = self.kf.remove_key(ORIGIN, OVERRIDE_COMMIT);
                self.cache.override_commit = None;
            }
        }
    }

    pub fn set_rojig_version(&mut self, version: Option<&str>) {
        match version {
            Some(version) => {
                self.kf
                    .set_string(ORIGIN, "rojig-override-version", version);
                self.cache.rojig_override_version = Some(version.to_string());
            }
            None => {
                let _ = self.kf.remove_key(ORIGIN, "rojig-override-version");
                self.cache.rojig_override_version = None;
            }
        }
    }

    pub fn get_prefixed_refspec(&self) -> Cow<'_, str> {
        match &self.cache.refspec {
            RefspecType::Rojig(s) => Cow::Owned(format!("{}{}", ROJIG_PREFIX, s)),
            o => Cow::Borrowed(o.as_str()),
        }
    }

    pub fn get_regenerate_initramfs(&self) -> bool {
        match map_keyfile_optional(self.kf.get_boolean("rpmostree", "regenerate-initramfs")) {
            Ok(Some(v)) => v,
            Ok(None) => false,
            Err(_) => false,  // FIXME Should propagate errors here in the future
        }
    }

    pub fn may_require_local_assembly(&self) -> bool {
        self.get_regenerate_initramfs() ||
            !self.cache.initramfs_etc.is_empty() ||
            !self.cache.packages.is_empty() || 
            !self.cache.packages_local.is_empty() ||
            !self.cache.override_replace_local.is_empty() ||
            !self.cache.override_remove.is_empty()
    }

    pub fn is_rojig(&self) -> bool {
        match self.cache.refspec {
            RefspecType::Rojig(_) => true,
            _ => false,
        }
    }

    // Binding for cxx
    pub fn get_override_local_pkgs(&self) -> Vec<String> {
        let mut r = Vec::new();
        for v in self.cache.override_replace_local.values() {
            r.push(v.clone())
        }
        r
    }
}

pub fn origin_parse_deployment(deployment: Pin<&mut crate::ffi::OstreeDeployment>) -> Result<Box<Origin>> {
    let deployment = deployment.get_mut().gobj_wrap();
    match deployment.get_origin() {
        Some(o) => Origin::new_parse(&o),
        None => {
            anyhow::bail!("No origin for deployment");
        }
    }
}

#[cfg(test)]
mod test {
    use super::*;

    #[test]
    fn test_basic() -> Result<()> {
        let o = Origin::new_from_str(
            "[origin]
refspec=foo:bar/x86_64/baz
",
        )?;
        assert_eq!(
            o.cache.refspec,
            RefspecType::Ostree("foo:bar/x86_64/baz".to_string())
        );
        assert_eq!(o.cache.packages.len(), 0);
        assert!(!o.may_require_local_assembly());
        assert!(!o.get_regenerate_initramfs());
    
        let mut o = Origin::new_from_str(
            r#"
[origin]
baserefspec=fedora/33/x86_64/silverblue

[packages]
requested=virt-manager;libvirt;pcsc-lite-ccid
"#,
        )?;
        assert_eq!(
            o.cache.refspec,
            RefspecType::Ostree("fedora/33/x86_64/silverblue".to_string())
        );
        assert!(o.may_require_local_assembly());
        assert!(!o.get_regenerate_initramfs());
        assert_eq!(o.cache.packages.len(), 3);
        assert!(o.cache.packages.contains("libvirt"));

        let override_commit = ("126539c731acf376359aced177dc5dff598dd6714a0a8faf753c727559adc8b5", Some("42.3"));
        assert!(o.cache.override_commit.is_none());
        o.set_override_commit(Some(override_commit));
        assert_eq!(o.cache.override_commit.as_ref().expect("override"), override_commit.0);
        Ok(())
    }
}
