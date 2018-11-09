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
use ipc_channel::ipc::{channel, IpcOneShotServer, IpcReceiver, IpcSender};
use prctl;
use serde::{Deserialize, Serialize};
use std::error::Error;
use std::default::Default;
use std::io::BufRead;
use std::os::unix::process::CommandExt;
use std::{env, fs, io, process};

// This is "bin".  Down the line maybe use systemd dynamic users.
const UNPRIVILEGED_UID : u32 = 1;

#[derive(Serialize, Deserialize, Debug)]
pub enum Message {
    Ping(u32, String),
    Pong(u32, String),
    Terminate,
}

pub struct Worker<S, R>
where
    S: for<'de> Deserialize<'de> + Serialize,
    R: for<'de> Deserialize<'de> + Serialize,
{
    child: process::Child,
    tx: IpcSender<S>,
    rx: IpcReceiver<R>,
}

// Ensure the child doesn't outlive us
fn prctl_set_pdeathsig_term() -> io::Result<()> {
    prctl::set_death_signal(15)
        .map_err(|e| io::Error::new(io::ErrorKind::InvalidInput, e.to_string()))
}

fn preexec_drop_privs() -> io::Result<()> {
    prctl_set_pdeathsig_term()?;
    (|| { nix::unistd::setgid(nix::unistd::Gid::from_raw(1))?;
          nix::unistd::setuid(nix::unistd::Uid::from_raw(1))
    })().map_err(|e| io::Error::new(io::ErrorKind::Other, e.to_string()))?;
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

impl<S, R> Worker<S, R>
where
    S: for<'de> Deserialize<'de> + Serialize,
    R: for<'de> Deserialize<'de> + Serialize,
{
    fn new(opts: WorkerOpts) -> Fallible<Self> {
        let (server, server_name) = IpcOneShotServer::new()?;
        let before_exec = if !opts.privileged && nix::unistd::getuid().is_root() {
            preexec_drop_privs
        } else {
            prctl_set_pdeathsig_term
        };
        let child = process::Command::new("/proc/self/exe")
            .args(&["multiproc-worker", &server_name])
            .before_exec(before_exec)
            .spawn().map_err(failure::Error::from)?;
        let (_, (tx, rx)) = server.accept().map_err(failure::Error::from)?;
        Ok(Self { child, tx, rx })
    }

    fn send(&self, msg: S) -> Fallible<()> {
        self.tx.send(msg)?;
        Ok(())
    }

    fn call(&self, msg: S) -> Fallible<R> {
        self.send(msg)?;
        self.rx.recv().map_err(|e| e.into())
    }
}

fn multiproc_run(argv: &Vec<String>) -> Fallible<()> {
    let app = App::new("rpmostree-multiproc")
        .version("0.1")
        .about("Multiprocessing entrypoint")
        .arg(Arg::with_name("test").long("test"))
        .arg(Arg::with_name("recv").required(true).takes_value(true));
    let matches = app.get_matches_from(argv);
    if matches.is_present("test") {
        let worker = Worker::new(Default::default())?;
        for i in 0..5 {
            let s = format!("hello world {}", i);
            let r = worker.call(Message::Ping(i, s.clone()))?;
            println!("> ping");
            match r {
                Message::Pong(ri, ref rs) => {
                    assert_eq!(i, ri);
                    assert_eq!(&s, rs);
                    println!("< pong");
                }
                v => panic!("Unexpected response: {:?}", v),
            }
        }
        worker.send(Message::Terminate)?;
    } else {
        let basetx = IpcSender::connect(matches.value_of("recv").unwrap().to_string())
            .map_err(failure::Error::from)?;
        let (my_tx, their_rx): (IpcSender<Message>, IpcReceiver<Message>) =
            channel().map_err(failure::Error::from)?;
        let (their_tx, my_rx): (IpcSender<Message>, IpcReceiver<Message>) =
            channel().map_err(failure::Error::from)?;
        basetx
            .send((their_tx, their_rx))
            .map_err(failure::Error::from)?;
        loop {
            match my_rx.recv().map_err(failure::Error::from)? {
                Message::Ping(v, ref s) => {
                    my_tx
                        .send(Message::Pong(v, s.clone()))
                        .map_err(failure::Error::from)?;
                }
                Message::Terminate => break,
                v => panic!("Unexpected message: {:?}", v),
            }
        }
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
