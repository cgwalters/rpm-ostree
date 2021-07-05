//! APIs for multi-process isolation
// SPDX-License-Identifier: Apache-2.0 OR MIT

use anyhow::{anyhow, Result};
use fn_error_context::context;
use std::borrow::Cow;

const SELF_UNIT: &str = "rpm-ostreed.service";
/// Run as a child process, synchronously.
const BASE_ARGS: &[&str] = &[
    "--collect",
    "--wait",
    "--pipe",
    "--no-ask-password",
    "--quiet",
];

/// Configuration for transient unit.
pub(crate) struct UnitConfig<'a> {
    /// If provided, will be used as the name of the unit
    pub(crate) name: Option<&'a str>,
    /// Unit/Service properties, e.g. DynamicUser=yes
    pub(crate) properties: &'a [&'a str],
    /// The command to execute
    pub(crate) exec_args: &'a [&'a str],
}

impl<'a> UnitConfig<'a> {
    // Generate a `systemd-run` argument list.
    fn into_args(&self) -> Vec<Cow<'a, str>> {
        let mut r = Vec::new();
        if let Some(name) = self.name {
            r.push(Cow::Borrowed("--unit"));
            r.push(Cow::Borrowed(name));
        }
        for prop in self.properties.iter() {
            r.push(Cow::Borrowed("--property"));
            r.push(Cow::Borrowed(prop));
        }
        // This ensures that this unit won't escape our process.
        r.push(Cow::Owned(format!("--property=BindsTo={}", SELF_UNIT)));
        r.push(Cow::Owned(format!("--property=After={}", SELF_UNIT)));
        r.push(Cow::Borrowed("--"));
        r.extend(self.exec_args.iter().map(|s| Cow::Borrowed(*s)));
        r
    }
}

/// Create a child process via `systemd-run` and synchronously wait
/// for its completion.  This runs in `--pipe` mode, so e.g. stdout/stderr
/// will go to the parent process.
/// Use this for isolation, as well as to escape the parent rpm-ostreed.service
/// isolation like `ProtectHome=true`.
#[context("Running systemd worker")]
pub(crate) fn run_systemd_worker_sync(cfg: &UnitConfig) -> Result<()> {
    if !crate::utils::running_in_systemd() {
        return Err(anyhow!("Not running under systemd"));
    }
    let mut cmd = std::process::Command::new("systemd-run");
    cmd.args(BASE_ARGS);
    for arg in cfg.into_args() {
        cmd.arg(&*arg);
    }
    let status = cmd.status()?;
    if !status.success() {
        return Err(anyhow!("{}", status));
    }
    Ok(())
}

pub(crate) struct IsolatedSelfSubprocess {
    child: tokio::process::Child,
}

impl IsolatedSelfSubprocess {
    #[allow(dead_code)]
    pub(crate) async fn spawn_with_config<'a>(cfg: &UnitConfig<'a>) -> Result<Self> {
        let mut cmd = tokio::process::Command::new("systemd-run");
        cmd.args(BASE_ARGS);
        for arg in cfg.into_args() {
            cmd.arg(&*arg);
        }
        let child = cmd.spawn()?;
        Ok(IsolatedSelfSubprocess {
            child
        })
    }
}

impl Drop for IsolatedSelfSubprocess {
    fn drop(&mut self) {
        let _ = self.child.start_kill();
    }
}
