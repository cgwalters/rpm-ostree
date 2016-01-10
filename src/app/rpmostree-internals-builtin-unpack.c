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
  rpmts ts = NULL;
  rpmfi fi = NULL;
  int r;
  FD_t rpmfd = NULL;
  Header hdr;
  const char *rpmpath;
  glnx_fd_close int rootfs_fd = -1;
  
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

  ts = rpmtsCreate ();
  rpmtsSetVSFlags (ts, _RPMVSF_NOSIGNATURES);

  rpmfd = Fopen (rpmpath, "r.fdio");
  if (rpmfd == NULL)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Failed to open %s", rpmpath);
      goto out;
    }
  if (Ferror (rpmfd))
    {
      g_set_error (error,
                   G_IO_ERROR,
                   G_IO_ERROR_FAILED,
                   "Opening %s: %s",
                   rpmpath,
                   Fstrerror (rpmfd));
      goto out;
    }
  
  if ((r = rpmReadPackageFile (ts, rpmfd, rpmpath, &hdr)) != RPMRC_OK)
    {
      g_set_error (error,
                   G_IO_ERROR,
                   G_IO_ERROR_FAILED,
                   "Verification of %s failed",
                    rpmpath);
      goto out;
    }

  fi = rpmfiNew (ts, hdr, RPMTAG_BASENAMES, (RPMFI_NOHEADER | RPMFI_FLAGS_INSTALL));
  fi = rpmfiInit (fi, 0);
  while (rpmfiNext (fi) >= 0)
    {
      rpmfileAttrs fflags = rpmfiFFlags (fi);
      rpm_mode_t fmode = rpmfiFMode (fi);
      rpm_loff_t fsize = rpmfiFSize (fi);
      const char *fn = rpmfiFN (fi); 
      const char *fuser = rpmfiFUser (fi);
      const char *fgroup = rpmfiFGroup (fi);
      const char *fcaps = rpmfiFCaps (fi);

      g_print ("%s %s:%s mode=%u size=%" G_GUINT64_FORMAT " attrs=%u",
               fn, fuser, fgroup, fmode, (guint64) fsize, fflags);
      if (fcaps)
        g_print (" fcaps=\"%s\"", fcaps);
      g_print ("\n");

      if (S_ISDIR (fmode))
        {
        }
      else if (S_ISLNK (fmode))
        {
        }
      else if (S_ISREG (fmode))
        {
        }
      else
        {
          g_set_error (error,
                       G_IO_ERROR,
                       G_IO_ERROR_FAILED,
                       "RPM %s contains non-regular/non-symlink file %s",
                       rpmpath,
                       fn);
          goto out;
        }
    }

  exit_status = EXIT_SUCCESS;
 out:
  if (fi)
    rpmfiFree (fi);
  if (rpmfd)
    Fclose (rpmfd);
  if (ts)
    rpmtsFree (ts);
  return exit_status;
}
