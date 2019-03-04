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

use failure::{Fallible, ResultExt};
use clap::{App, Arg};
use prctl;
use bincode;
use serde::{Deserialize, Serialize};
use std::error::Error;
use std::default::Default;
use std::io::BufRead;
use std::os::unix::net::UnixStream;
use std::os::unix::process::CommandExt;
use std::os::unix::io::{FromRawFd, IntoRawFd};
use std::{env, fs, io, process};

// This is "bin".  Down the line maybe use systemd dynamic users.
const UNPRIVILEGED_UID : u32 = 1;

#[derive(Serialize, Deserialize, Debug)]
pub enum Message {
    Terminate,
    TestMessage(Vec<u8>),
//    HttpFetchMessage(Vec<u8>),
}

#[derive(Serialize, Deserialize, Debug)]
struct TestMessage(u32, String);

pub struct Worker
{
    child: process::Child,
    sock: UnixStream,
}

// Ensure the child doesn't outlive us
fn prctl_set_pdeathsig_term() -> io::Result<()> {
    prctl::set_death_signal(15)
        .map_err(|e| io::Error::new(io::ErrorKind::InvalidInput, e.to_string()))
}

fn new_io_err<D: std::fmt::Display>(e: D) -> io::Error {
    io::Error::new(io::ErrorKind::Other, e.to_string())
}

fn preexec_drop_privs() -> io::Result<()> {
    (|| { nix::unistd::setgid(nix::unistd::Gid::from_raw(1))?;
          nix::unistd::setuid(nix::unistd::Uid::from_raw(1))
    })().map_err(new_io_err)?;
    Ok(())
}

#[derive(Clone, Debug, Default)]
pub struct WorkerOpts {
    privileged: bool,
}

impl WorkerOpts {
    pub fn privileged(mut self) -> Self {
        self.privileged = true;
        self
    }
}

impl Worker
{
    fn new(opts: WorkerOpts) -> Fallible<Self> {
        let (sock, childsock) = UnixStream::pair()?;
        let drop_privs : bool = !opts.privileged && nix::unistd::getuid().is_root();
        let child = process::Command::new("/proc/self/exe")
            .args(&["multiproc-worker"])
            .before_exec(move || {
                let childsock = childsock.into_raw_fd();
                if childsock != 3 {
                    nix::unistd::dup2(childsock, 3).map_err(|e| new_io_err(e))?;
                }
                prctl_set_pdeathsig_term()?;
                if drop_privs {
                    preexec_drop_privs()?;
                }
                Ok(())
            })
            .spawn().map_err(failure::Error::from)?;
        Ok(Self { child, sock })
    }

    fn send(&self, msg: Message) -> Fallible<()> {
        bincode::serialize_into(self.sock, &msg)?;
        Ok(())
    }

    fn call(&self, msg: Message) -> Fallible<Message> {
        bincode::serialize_into(self.sock, &msg)?;
        Ok(bincode::deserialize_from(self.sock)?)
    }
}

struct WorkerImpl {
    sock: UnixStream,
}

impl WorkerImpl {
    fn new(sock: UnixStream) -> Self {
        Self { sock, }
    }

    fn impl_testmessage(&self, msg: Vec<u8>) -> Fallible<()> {
        let mut msg : TestMessage = bincode::deserialize(&msg)?;
        msg.0 += 1;
        msg.1.insert(0, 'x');
        bincode::serialize_into(self.sock, &msg)?;
        Ok(())
    }

    fn run(&self) -> Fallible<()> {
        loop {
            let msg = bincode::deserialize_from(self.sock)?;
            match msg {
                Message::Terminate => return Ok(()),
                Message::TestMessage(buf) => {
                    self.impl_testmessage(buf)?;
                }
            }
        }
    }
}

fn multiproc_run(argv: &Vec<String>) -> Fallible<()> {
    let app = App::new("rpmostree-multiproc")
        .version("0.1")
        .about("Multiprocessing entrypoint")
        .arg(Arg::with_name("test").long("test"));
    let matches = app.get_matches_from(argv);
    if matches.is_present("test") {
        let worker = Worker::new(Default::default())?;
        for i in 0..5 {
            let s = format!("hello world {}", i);
            let r = worker.call(Message::TestMessage(bincode::serialize(&TestMessage(i, s.clone()))?))?;
            println!("> ping");
            match r {
                Message::TestMessage(m) => {
                    let m : TestMessage = bincode::deserialize(&m)?;
                    assert_eq!(i, m.0);
                    assert_eq!(&s, &m.1);
                }
                _ => panic!("Unexpected message: {:?}", r)
            }
            println!("< pong");
        }
        worker.send(Message::Terminate)?;
    } else {
        let workerimpl = WorkerImpl::new(UnixStream::from_raw_fd(3));
        workerimpl.run()?;
    }
    Ok(())
}

fn multiproc_main(argv: &Vec<String>) {
    if let Err(ref e) = multiproc_run(argv) {
        eprintln!("error: {}", e);
        process::exit(1)
    }
}

#[cfg(test)]
mod tests {
    use super::*;

}

mod ffi {
    use super::*;
    use ffiutil::*;
    use glib;
    use libc;

    #[no_mangle]
    pub extern "C" fn ror_multiproc_entrypoint(argv: *mut *mut libc::c_char) {
        let v: Vec<String> = unsafe { glib::translate::FromGlibPtrContainer::from_glib_none(argv) };
        multiproc_main(&v)
    }
}
pub use self::ffi::*;
