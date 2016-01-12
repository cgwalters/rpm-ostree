/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*-
 *
 * Copyright (C) 2015 Colin Walters <walters@verbum.org>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published
 * by the Free Software Foundation; either version 2 of the licence or (at
 * your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General
 * Public License along with this library; if not, write to the
 * Free Software Foundation, Inc., 59 Temple Place, Suite 330,
 * Boston, MA 02111-1307, USA.
 */

#include "config.h"

#include <string.h>
#include <glib-unix.h>
#include <gio/gunixoutputstream.h>
#include <libhif.h>
#include <libhif/hif-utils.h>
#include <rpm/rpmts.h>
#include <stdio.h>
#include <libglnx.h>
#include <rpm/rpmmacro.h>

#include "rpmostree-internals-builtins.h"
#include "rpmostree-util.h"
#include "rpmostree-hif.h"
#include "rpmostree-cleanup.h"
#include "rpmostree-libbuiltin.h"
#include "rpmostree-rpm-util.h"
#include "rpmostree-unpacker.h"

#include "libgsystem.h"

static GOptionEntry option_entries[] = {
  { NULL }
};

int
rpmostree_internals_builtin_unpack (int             argc,
                                    char          **argv,
                                    GCancellable   *cancellable,
                                    GError        **error)
{
  int exit_status = EXIT_FAILURE;
  GOptionContext *context = g_option_context_new ("ROOT RPM");
  glnx_unref_object RpmOstreeUnpacker *unpacker = NULL;
  const char *rpmpath;
  glnx_fd_close int rootfs_fd = -1;
  glnx_fd_close int rpm_fd = -1;
  
  if (!rpmostree_option_context_parse (context,
                                       option_entries,
                                       &argc, &argv,
                                       RPM_OSTREE_BUILTIN_FLAG_LOCAL_CMD,
                                       cancellable,
                                       NULL,
                                       error))
    goto out;

  if (argc < 3)
    {
      rpmostree_usage_error (context, "ROOT and RPM must be specified", error);
      goto out;
    }
  
  if (getuid () == 0)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "This program should not run as root");
      goto out;
    }

  if (!glnx_opendirat (AT_FDCWD, argv[1], TRUE, &rootfs_fd, error))
    goto out;

  rpmpath = argv[2];

  if ((rpm_fd = openat (AT_FDCWD, rpmpath, O_RDONLY | O_CLOEXEC | O_NOCTTY)) < 0)
    {
      glnx_set_error_from_errno (error);
      g_prefix_error (error, "Opening %s: ", rpmpath);
      goto out;
    }

  unpacker = rpmostree_unpacker_new_at (AT_FDCWD, rpmpath, 0, error);
  if (!unpacker)
    goto out;

  if (!rpmostree_unpacker_unpack_to_dfd (unpacker, rootfs_fd, cancellable, error))
    goto out;

  exit_status = EXIT_SUCCESS;
 out:
  return exit_status;
}
