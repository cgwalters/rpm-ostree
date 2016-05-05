## Package systems versus image systems

Broadly speaking, software update systems for operating systems tend
to fall cleanly into one of two camps: package-based or image-based.

### Package system benefits and drawbacks

 * + Highly dynamic, fast access to wide array of software
 * + State management in `/etc` and `/var` is well understood 
 * + Can swap between major/minor system states (`apt-get upgrade` is similar to `apt-get dist-upgrade`)
 * + Generally supports any filesystem or partition layout
 * - As package set grows, testing becomes combinatorially more expensive
 * - Live system mutation, no rollbacks

### Image benefits and drawbacks

 * + Ensures all users are running a known state
 * + Rollback supported
 * + Can achieve efficient security via things like [dm-verity](http://lwn.net/Articles/459420/)
 * - Many image systems have a read-only `/etc`, and writable partitions elsewhere
 * - Must reboot for updates
 * - Usually operate at block level, so require fixed partition layout and filesystem
 * - Many use a "dual root" mode which wastes space and is inflexible
 * - Often paired with a separate application mechanism, but misses out on things that aren't apps
 * - Administrators still need to know content inside

## How RPM-OSTree provides a middle ground

rpm-ostree in its default mode feels more like image replication, but
the underlying architecture allows a lot of package-like flexibility.

In this default mode, packages are composed on a server, and clients
can replicate that state reliably.  For example, if one adds a package
on the compose server, clients get it.  If one removes a package, it's
also removed when clients upgrade.

One simple mental model for rpm-ostree is: imagine taking a set of
packages on the server side, install them to a chroot, then doing `git commit`
on the result.  And imagine clients just `git pull -r` from
that.  What OSTree adds to this picture is support for file uid/gid,
extended attributes, handling of bootloader configuration, and merges
of `/etc`.

To emphasize, replication is at a filesystem level - that means things 
like SELinux labels and uid/gid mappings are assigned on
the server side.

On the other hand, rpm-ostree works on top of any Unix filesystem.  It
will not interfere with any filesystem or block-level snapshots or
backups such as LVM or BTRFS.

## Who should use this?

Currently, `rpm-ostree` operates on a read-only mode on installed
systems; it is not possible to add or remove anything on the client
system's `/usr`.  If this matches your deployment scenario, rpm-ostree
is a good choice.  Classic examples of this are fixed purpose server
farms, "corporate standard build" laptop/desktops, and embedded
devices.

Of course, one can pair it with a dynamic application mechanism such
as [Docker](https://www.docker.com/), and have a reliable base, with a
flexible application tool.  This is the rationale behind
[Project Atomic](http://www.projectatomic.io/).

Container technology is flexible enough for "privileged" containers to
affect the host.  For example, using the `atomic` command, one can
`atomic run centos/tools` and have a flexible shell with access to
`/host`.

## Is it worth supporting composes both on client and server?

In short, our belief is yes.  Long term, rpm-ostree offers a potential
unified tooling via package layering.
