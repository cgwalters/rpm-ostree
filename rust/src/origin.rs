//! An "origin" declares how we generated an OSTree commit.

/*
 * Copyright (C) 2020 Red Hat, Inc.
 *
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 */

use crate::includes::RefspecType as CRefspecType;
use anyhow::{bail, Result};
use glib::translate::*;
use glib::KeyFile;
use std::borrow::Cow;
use std::result::Result as StdResult;

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
    fn to_c(&self) -> CRefspecType {
        match self {
            Self::Ostree(_) => CRefspecType::Ostree,
            Self::Rojig(_) => CRefspecType::Rojig,
            Self::Checksum(_) => CRefspecType::Checksum,
        }
    }

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
    fn remove_transient_state(&mut self) {
        unsafe {
            ostree_sys::ostree_deployment_origin_remove_transient_state(self.kf.to_glib_none().0)
        }
        self.set_override_commit(None)
    }

    fn set_override_commit(&mut self, checksum: Option<(&str, Option<&str>)>) {
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

    fn set_rojig_version(&mut self, version: Option<&str>) {
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

    fn get_prefixed_refspec(&self) -> Cow<'_, str> {
        match &self.cache.refspec {
            RefspecType::Rojig(s) => Cow::Owned(format!("{}{}", ROJIG_PREFIX, s)),
            o => Cow::Borrowed(o.as_str()),
        }
    }

    fn get_regenerate_initramfs(&self) -> bool {
        match map_keyfile_optional(self.kf.get_boolean("rpmostree", "regenerate-initramfs")) {
            Ok(Some(v)) => v,
            Ok(None) => false,
            Err(_) => false,  // FIXME Should propagate errors here in the future
        }
    }

    fn may_require_local_assembly(&self) -> bool {
        self.get_regenerate_initramfs() ||
            !self.cache.initramfs_etc.is_empty() ||
            !self.cache.packages.is_empty() || 
            !self.cache.packages_local.is_empty() ||
            !self.cache.override_replace_local.is_empty() ||
            !self.cache.override_remove.is_empty()
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

mod ffi {
    use super::*;
    use crate::ffiutil::*;
    use glib::GString;

    #[no_mangle]
    pub extern "C" fn ror_origin_parse_keyfile(
        kf: *mut glib_sys::GKeyFile,
        gerror: *mut *mut glib_sys::GError,
    ) -> *mut Origin {
        let kf: Borrowed<KeyFile> = unsafe { from_glib_borrow(kf) };
        ptr_glib_error(Origin::new_parse(&kf), gerror)
    }

    #[no_mangle]
    pub extern "C" fn ror_origin_dup(origin: *mut Origin) -> *mut Origin {
        let origin = ref_from_raw_ptr(origin);
        Box::into_raw(Origin::new_parse(&origin.kf).expect("parse"))
    }

    #[no_mangle]
    pub extern "C" fn ror_origin_remove_transient_state(origin: *mut Origin) {
        let origin = ref_from_raw_ptr(origin);
        origin.remove_transient_state();
    }

    #[no_mangle]
    pub extern "C" fn ror_origin_set_rojig_version(
        origin: *mut Origin,
        version: *mut libc::c_char,
    ) {
        let origin = ref_from_raw_ptr(origin);
        let version: Borrowed<Option<GString>> = unsafe { from_glib_borrow(version) };
        origin.set_rojig_version(version.as_ref().as_ref().map(|v| v.as_str()));
    }

    #[no_mangle]
    pub extern "C" fn ror_origin_get_refspec(origin: *mut Origin) -> *mut libc::c_char {
        let origin = ref_from_raw_ptr(origin);
        let s = match &origin.cache.refspec {
            RefspecType::Ostree(s) => s,
            RefspecType::Rojig(s) => s,
            RefspecType::Checksum(s) => s,
        };
        s.to_glib_full()
    }

    #[no_mangle]
    pub extern "C" fn ror_origin_get_full_refspec(
        origin: *mut Origin,
        out_type: *mut libc::c_int,
    ) -> *mut libc::c_char {
        let origin = ref_from_raw_ptr(origin);
        unsafe {
            if !out_type.is_null() {
                *out_type = origin.cache.refspec.to_c() as i32;
            }
        }
        origin.get_prefixed_refspec().to_glib_full()
    }

    #[no_mangle]
    pub extern "C" fn ror_origin_classify_refspec(
        origin: *mut Origin,
        out_type: *mut libc::c_int,
        out_data: *mut *mut libc::c_char,
    ) {
        let origin = ref_from_raw_ptr(origin);
        unsafe {
            if !out_type.is_null() {
                *out_type = origin.cache.refspec.to_c() as i32;
            }
            if !out_data.is_null() {
                *out_data = origin.cache.refspec.as_str().to_glib_full();
            }
        }
    }

    #[no_mangle]
    pub extern "C" fn ror_origin_is_rojig(origin: *mut Origin) -> libc::c_int {
        let origin = ref_from_raw_ptr(origin);
        match &origin.cache.refspec {
            RefspecType::Rojig(_) => 1,
            _ => 0,
        }
    }

    #[no_mangle]
    pub extern "C" fn ror_origin_get_rojig_version(origin: *mut Origin) -> *mut libc::c_char {
        let origin = ref_from_raw_ptr(origin);
        origin.cache.rojig_override_version.to_glib_full()
    }

    #[no_mangle]
    pub extern "C" fn ror_origin_get_custom_description(
        origin: *mut Origin,
        custom_type: *mut *mut libc::c_char,
        custom_description: *mut *mut libc::c_char,
    ) {
        let origin = ref_from_raw_ptr(origin);
        unsafe {
            match keyfile_get_nonempty_optional_string(&origin.kf, ORIGIN, "custom-url") {
                Ok(v) => {
                    *custom_type = v.to_glib_full();
                }
                Err(_) => {
                    return;
                }
            }
            match keyfile_get_nonempty_optional_string(&origin.kf, ORIGIN, "custom-description") {
                Ok(v) => {
                    *custom_description = v.to_glib_full();
                }
                Err(_) => {
                    return;
                }
            }
        }
    }

    #[no_mangle]
    pub extern "C" fn ror_origin_get_packages(
        origin: *mut Origin
    ) -> *mut glib_sys::GHashTable {
        let origin = ref_from_raw_ptr(origin);
        btreeset_to_hashtable(&origin.cache.packages)
    }

    #[no_mangle]
    pub extern "C" fn ror_origin_get_local_packages(
        origin: *mut Origin
    ) -> *mut glib_sys::GHashTable {
        let origin = ref_from_raw_ptr(origin);
        btreemap_to_hashtable(&origin.cache.packages_local)
    }

    #[no_mangle]
    pub extern "C" fn ror_origin_get_overrides_remove(
        origin: *mut Origin
    ) -> *mut glib_sys::GHashTable {
        let origin = ref_from_raw_ptr(origin);
        btreeset_to_hashtable(&origin.cache.packages)
    }

    #[no_mangle]
    pub extern "C" fn ror_origin_get_overrides_local_replace(
        origin: *mut Origin
    ) -> *mut glib_sys::GHashTable {
        let origin = ref_from_raw_ptr(origin);
        btreemap_to_hashtable(&origin.cache.override_replace_local)
    }

    #[no_mangle]
    pub extern "C" fn ror_origin_get_override_commit(
        origin: *mut Origin
    ) -> *mut libc::c_char {
        let origin = ref_from_raw_ptr(origin);
        origin.cache.override_commit.to_glib_full()
    }

    #[no_mangle]
    pub extern "C" fn ror_origin_get_initramfs_etc_files(
        origin: *mut Origin
    ) -> *mut glib_sys::GHashTable {
        let origin = ref_from_raw_ptr(origin);
        btreeset_to_hashtable(&origin.cache.initramfs_etc)
    }

    #[no_mangle]
    pub extern "C" fn ror_origin_get_regenerate_initramfs(
        origin: *mut Origin
    ) -> libc::c_int {
        let origin = ref_from_raw_ptr(origin);
        if origin.get_regenerate_initramfs() {
            1
        } else {
            0
        }
    }

    #[no_mangle]
    pub extern "C" fn ror_origin_get_initramfs_args(
        origin: *mut Origin
    ) -> *mut *mut libc::c_char {
        let origin = ref_from_raw_ptr(origin);
        origin.cache.initramfs_args.to_glib_full()
    }


    #[no_mangle]
    pub extern "C" fn ror_origin_get_unconfigured_state(
        origin: *mut Origin
    ) -> *mut libc::c_char {
        let origin = ref_from_raw_ptr(origin);
        origin.cache.unconfigured_state.to_glib_full()
    }

    #[no_mangle]
    pub extern "C" fn ror_origin_may_require_local_assembly(
        origin: *mut Origin
    ) -> libc::c_int {
        let origin = ref_from_raw_ptr(origin);
        if origin.may_require_local_assembly() {
            1
        } else {
            0
        }
    }

    #[no_mangle]
    pub extern "C" fn ror_origin_dup_keyfile(
        origin: *mut Origin
    ) -> *mut glib_sys::GKeyFile {
        let origin = ref_from_raw_ptr(origin);
        origin.kf.to_glib_full()
    }
}
pub use self::ffi::*;
