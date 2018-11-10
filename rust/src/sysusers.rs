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

use clap::{App, Arg};
use failure::Fallible;
use std::borrow::Cow;
use std::io::prelude::*;
use std::{fmt, fs, io, process};

static AUTOPATH: &str = "usr/lib/sysusers.d/rpmostree-auto.conf";

#[derive(PartialEq, Debug)]
enum IdSpecification {
    Unspecified,
    Specified(u32),
    Path(String),
}

impl IdSpecification {
    fn parse(buf: &str) -> Fallible<Self> {
        if buf.starts_with("/") {
            Ok(IdSpecification::Path(buf.to_string()))
        } else if buf == "-" {
            Ok(IdSpecification::Unspecified)
        } else {
            Ok(IdSpecification::Specified(buf.parse::<u32>()?))
        }
    }

    fn format_sysusers<'a>(&'a self) -> Cow<'a, str> {
        match self {
            IdSpecification::Unspecified => Cow::Borrowed("-"),
            IdSpecification::Specified(u) => Cow::Owned(format!("{}", u)),
            IdSpecification::Path(ref s) => Cow::Borrowed(s),
        }
    }
}

// For now, we don't parse all of the entry data; we'd need to handle quoting
// etc. See extract-word.c in the systemd source. All we need is the
// user/group → {u,g}id mapping to ensure we're not creating duplicate entries.
#[derive(PartialEq, Debug)]
enum PartialSysuserEntry {
    User { name: String, uid: IdSpecification },
    Group { name: String, gid: IdSpecification },
    GroupMember { uname: String, gname: String },
}

/// The full version that we get from `useradd`.
#[derive(PartialEq, Debug)]
enum SysuserEntry {
    User {
        name: String,
        uid: IdSpecification,
        gecos: Option<String>,
        homedir: Option<String>,
        shell: Option<String>,
    },
    Group {
        name: String,
        gid: IdSpecification,
    },
    GroupMember {
        uname: String,
        gname: String,
    },
}

impl SysuserEntry {
    fn format_sysusers(&self) -> String {
        match self {
            SysuserEntry::User {
                name,
                uid,
                gecos,
                homedir,
                shell,
            } => {
                fn optional_quoted_string<'a>(s: &'a Option<String>) -> Cow<'a, str> {
                    match s.as_ref() {
                        Some(s) => {
                            let mut elts = s.split_whitespace();
                            let first = elts.next();
                            if first.is_none() {
                                return Cow::Borrowed("-");
                            }
                            match elts.next() {
                                Some(_) => Cow::Owned(format!("\"{}\"", s)),
                                None => Cow::Borrowed(s),
                            }
                        }
                        None => Cow::Borrowed("-"),
                    }
                }
                format!(
                    "{} {} {} {} {}",
                    name,
                    uid.format_sysusers(),
                    optional_quoted_string(&gecos),
                    optional_quoted_string(&homedir),
                    optional_quoted_string(&shell),
                )
            }
            SysuserEntry::Group { name, gid } => format!("{} {}", name, gid.format_sysusers()),
            SysuserEntry::GroupMember { uname, gname } => format!("{} {}", uname, gname),
        }
    }
}

/// (Partially) parse single a single line from a sysusers.d file
fn parse_entry(line: &str) -> Fallible<PartialSysuserEntry> {
    let err = || format_err!("Invalid sysusers entry: \"{}\"", line);
    let mut elts = line.split_whitespace().fuse();
    match elts.next().ok_or_else(err)? {
        "u" => {
            let name = elts.next().ok_or_else(err)?.to_string();
            let uidstr = elts.next().ok_or_else(err)?;
            let uid = IdSpecification::parse(uidstr)?;
            Ok(PartialSysuserEntry::User { name, uid })
        }
        "g" => {
            let name = elts.next().ok_or_else(err)?.to_string();
            let gidstr = elts.next().ok_or_else(err)?;
            let gid = IdSpecification::parse(gidstr)?;
            Ok(PartialSysuserEntry::Group { name, gid })
        }
        "m" => {
            let uname = elts.next().ok_or_else(err)?.to_string();
            let gname = elts.next().ok_or_else(err)?.to_string();
            Ok(PartialSysuserEntry::GroupMember { uname, gname })
        }
        _ => Err(err()),
    }
}

/// Parse a sysusers.d file (as a stream)
fn parse<I: io::BufRead>(stream: I) -> Fallible<Vec<PartialSysuserEntry>> {
    let mut res = Vec::new();
    for line in stream.lines() {
        let line = line?;
        if line.starts_with("#") || line.is_empty() {
            continue;
        }
        res.push(parse_entry(&line)?);
    }
    Ok(res)
}

fn useradd<'a, I>(args: I) -> Fallible<Vec<SysuserEntry>>
where
    I: IntoIterator<Item = &'a str>,
{
    let app = App::new("useradd")
        .version("0.1")
        .about("rpm-ostree useradd wrapper")
        .arg(Arg::with_name("system").short("r"))
        .arg(Arg::with_name("uid").short("u").takes_value(true))
        .arg(Arg::with_name("gid").short("g").takes_value(true))
        .arg(Arg::with_name("groups").short("G").takes_value(true))
        .arg(Arg::with_name("home-dir").short("d").takes_value(true))
        .arg(Arg::with_name("comment").short("c").takes_value(true))
        .arg(Arg::with_name("shell").short("s").takes_value(true))
        .arg(Arg::with_name("username").takes_value(true).required(true));
    let matches = app.get_matches_from_safe(args)?;

    let mut uid = IdSpecification::Unspecified;
    if let Some(ref uidstr) = matches.value_of("uid") {
        uid = IdSpecification::Specified(uidstr.parse::<u32>()?);
    }
    let name = matches.value_of("username").unwrap().to_string();
    let gecos = matches.value_of("comment").map(|s| s.to_string());
    let homedir = matches.value_of("home-dir").map(|s| s.to_string());
    let shell = matches.value_of("shell").map(|s| s.to_string());

    let mut res = vec![SysuserEntry::User {
        name: name.to_string(),
        uid,
        gecos,
        homedir,
        shell,
    }];
    if let Some(primary_group) = matches.value_of("gid") {
        if primary_group != name {
            bail!(
                "Unable to represent user with group '{}' != username '{}'",
                primary_group,
                name
            );
        }
    }
    if let Some(gnames) = matches.value_of("groups") {
        for gname in gnames.split(",").filter(|&n| n != name) {
            res.push(SysuserEntry::GroupMember {
                uname: name.to_string(),
                gname: gname.to_string(),
            });
        }
    }
    Ok(res)
}

fn groupadd<'a, I>(args: I) -> Fallible<SysuserEntry>
where
    I: IntoIterator<Item = &'a str>,
{
    let app = App::new("useradd")
        .version("0.1")
        .about("rpm-ostree groupadd wrapper")
        .arg(Arg::with_name("system").short("r"))
        .arg(Arg::with_name("gid").short("g").takes_value(true))
        .arg(Arg::with_name("groupname").takes_value(true).required(true));
    let matches = app.get_matches_from_safe(args)?;

    let mut gid = IdSpecification::Unspecified;
    if let Some(ref gidstr) = matches.value_of("gid") {
        gid = IdSpecification::Specified(gidstr.parse::<u32>()?);
    }
    let name = matches.value_of("groupname").unwrap().to_string();
    Ok(SysuserEntry::Group { name, gid })
}

fn useradd_main(sysusers_file: &mut fs::File, args: &Vec<&str>) -> Fallible<()> {
    eprintln!("useradd_main: {:?}", args);
    let r = useradd(args.iter().map(|x| *x))?;
    for elt in r {
        writeln!(sysusers_file, "{}", elt.format_sysusers())?;
    }
    Ok(())
}

fn groupadd_main(sysusers_file: &mut fs::File, args: &Vec<&str>) -> Fallible<()> {
    eprintln!("groupadd_main: {:?}", args);
    let r = groupadd(args.iter().map(|x| *x))?;
    writeln!(sysusers_file, "{}", r.format_sysusers())?;
    Ok(())
}

fn process_useradd_invocation(rootfs: openat::Dir, argf: fs::File) -> Fallible<()> {
    let mut argf = io::BufReader::new(argf);
    let mut mode = String::new();
    if argf.read_line(&mut mode)? == 0 {
        // An empty stream means no users added
        return Ok(())
    }
    let mode = mode.trim_right();
    let mut args = vec![];
    for line in argf.lines() {
        let line = line?;
        args.push(line);
    }
    let args : Vec<&str> = args.iter().map(|v| v.as_str()).collect();
    let mut sysusers_autof = rootfs.append_file(AUTOPATH, 0644)?;
    match mode {
        "useradd" => useradd_main(&mut sysusers_autof, &args),
        "groupadd" => groupadd_main(&mut sysusers_autof, &args),
        x => bail!("Unknown command: {}", x),
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    // from "man sysusers.d"
    static SYSUSERS1: &str = r###"
u     httpd    404            "HTTP User"
u     authd    /usr/bin/authd "Authorization user"
u     postgres -              "Postgresql Database" /var/lib/pgsql /usr/libexec/postgresdb
g     input    -              -
m     authd    input
u     root     0              "Superuser"           /root          /bin/zsh
"###;

    #[test]
    fn test_parse() {
        let buf = io::BufReader::new(SYSUSERS1.as_bytes());
        let r = parse(buf).unwrap();
        assert_eq!(
            r[0],
            PartialSysuserEntry::User {
                name: "httpd".to_string(),
                uid: IdSpecification::Specified(404)
            }
        );
        assert_eq!(
            r[1],
            PartialSysuserEntry::User {
                name: "authd".to_string(),
                uid: IdSpecification::Path("/usr/bin/authd".to_string())
            }
        );
        assert_eq!(
            r[2],
            PartialSysuserEntry::User {
                name: "postgres".to_string(),
                uid: IdSpecification::Unspecified
            }
        );
        assert_eq!(
            r[3],
            PartialSysuserEntry::Group {
                name: "input".to_string(),
                gid: IdSpecification::Unspecified
            }
        );
        assert_eq!(
            r[4],
            PartialSysuserEntry::GroupMember {
                uname: "authd".to_string(),
                gname: "input".to_string()
            }
        );
        assert_eq!(
            r[5],
            PartialSysuserEntry::User {
                name: "root".to_string(),
                uid: IdSpecification::Specified(0)
            }
        );
    }

    #[test]
    fn test_useradd_wesnoth() {
        let r = useradd(
            vec![
                "useradd",
                "-c",
                "Wesnoth server",
                "-s",
                "/sbin/nologin",
                "-r",
                "-d",
                "/var/run/wesnothd",
                "wesnothd",
            ].iter()
            .map(|v| *v),
        ).unwrap();
        assert_eq!(
            r,
            vec![SysuserEntry::User {
                name: "wesnothd".to_string(),
                uid: IdSpecification::Unspecified,
                gecos: Some("Wesnoth server".to_string()),
                shell: Some("/sbin/nologin".to_string()),
                homedir: Some("/var/run/wesnothd".to_string()),
            }]
        );
        assert_eq!(r.len(), 1);
        assert_eq!(
            r[0].format_sysusers(),
            r##"wesnothd - "Wesnoth server" /var/run/wesnothd /sbin/nologin"##
        );
    }

    #[test]
    fn test_useradd_tss() {
        let r = useradd(
            vec![
                "useradd",
                "-r",
                "-u",
                "59",
                "-g",
                "tss",
                "-d",
                "/dev/null",
                "-s",
                "/sbin/nologin",
                "-c",
                "comment",
                "tss",
            ].iter()
            .map(|v| *v),
        ).unwrap();
        assert_eq!(r.len(), 1);
        assert_eq!(
            r[0].format_sysusers(),
            r##"tss 59 comment /dev/null /sbin/nologin"##
        );
    }

    #[test]
    fn test_groupadd_basics() {
        assert_eq!(
            groupadd(vec!["groupadd", "-r", "wireshark",].iter().map(|v| *v)).unwrap(),
            SysuserEntry::Group {
                name: "wireshark".to_string(),
                gid: IdSpecification::Unspecified,
            },
        );
        assert_eq!(
            groupadd(
                vec!["groupadd", "-r", "-g", "112", "vhostmd",]
                    .iter()
                    .map(|v| *v)
            ).unwrap(),
            SysuserEntry::Group {
                name: "vhostmd".to_string(),
                gid: IdSpecification::Specified(112),
            },
        );
    }
}

mod ffi {
    use super::*;
    use ffiutil::*;
    use glib;
    use libc;
    use std::env;
    use std::os::unix::io::FromRawFd;

    #[no_mangle]
    pub extern "C" fn ror_sysusers_process_useradd(rootfs_dfd: libc::c_int,
                                                   useradd_fd: libc::c_int,
                                                   gerror: *mut *mut glib_sys::GError) -> libc::c_int {
        let rootfs_dfd = ffi_view_openat_dir(rootfs_dfd);
        // Ownership is transferred
        let useradd_fd = unsafe { fs::File::from_raw_fd(useradd_fd) };
        int_glib_error(process_useradd_invocation(rootfs_dfd, useradd_fd), gerror)
    }
}
pub use self::ffi::*;
