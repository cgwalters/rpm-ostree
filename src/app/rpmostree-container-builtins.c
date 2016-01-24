/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*-
 *
 * Copyright (C) 2015,2016 Colin Walters <walters@verbum.org>
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
#include <rpm/rpmts.h>
#include <stdio.h>
#include <libglnx.h>
#include <rpm/rpmmacro.h>

#include "rpmostree-container-builtins.h"
#include "rpmostree-util.h"
#include "rpmostree-hif.h"
#include "rpmostree-cleanup.h"
#include "rpmostree-libbuiltin.h"
#include "rpmostree-rpm-util.h"
#include "rpmostree-unpacker.h"

#include "libgsystem.h"


static GOptionEntry init_option_entries[] = {
  { NULL }
};

static GOptionEntry assemble_option_entries[] = {
  { NULL }
};

int
rpmostree_container_builtin_init (int             argc,
                                  char          **argv,
                                  GCancellable   *cancellable,
                                  GError        **error)
{
  int exit_status = EXIT_FAILURE;
  GOptionContext *context = g_option_context_new ("");
  g_autofree char *userroot_base = get_current_dir_name ();
  glnx_fd_close int userroot_dfd = -1;
  glnx_unref_object OstreeRepo *ostreerepo = NULL;
  static const char* const directories[] = { "repo", "rpm-md", "roots" };
  guint i;
  
  if (!rpmostree_option_context_parse (context,
                                       init_option_entries,
                                       &argc, &argv,
                                       RPM_OSTREE_BUILTIN_FLAG_LOCAL_CMD,
                                       cancellable,
                                       NULL,
                                       error))
    goto out;

  if (!glnx_opendirat (AT_FDCWD, userroot_base, TRUE, &userroot_dfd, error))
    goto out;

  for (i = 0; i < G_N_ELEMENTS (directories); i++)
    {
      if (!glnx_shutil_mkdir_p_at (userroot_dfd, directories[i], 0755, cancellable, error))
        goto out;
    }

  { g_autofree char *repo_pathstr = g_strconcat (userroot_base, "repo", NULL);
    g_autoptr(GFile) repo_path = g_file_new_for_path (repo_pathstr);
    ostreerepo = ostree_repo_new (repo_path);
    if (!ostree_repo_create (ostreerepo, OSTREE_REPO_MODE_BARE_USER, cancellable, error))
      goto out;
  }

  exit_status = EXIT_SUCCESS;
 out:
  return exit_status;
}

int
rpmostree_container_builtin_assemble (int             argc,
                                      char          **argv,
                                      GCancellable   *cancellable,
                                      GError        **error)
{
  int exit_status = EXIT_FAILURE;
  GOptionContext *context = g_option_context_new ("NAME [PKGNAME PKGNAME...]");
  glnx_unref_object HifContext *hifctx = NULL;
  g_auto(RpmOstreeHifInstall) hifinstall = {0,};
  g_autofree char *userroot_base = get_current_dir_name ();
  const char *name;
  struct stat stbuf;
  const char *const*pkgnames;
  g_autofree char *target_path = NULL;
  const char *target_rootdir;
  glnx_fd_close int userroot_dfd = -1;
  glnx_fd_close int rpmmd_dfd = -1;
  glnx_unref_object OstreeRepo *ostreerepo = NULL;
  
  if (!rpmostree_option_context_parse (context,
                                       assemble_option_entries,
                                       &argc, &argv,
                                       RPM_OSTREE_BUILTIN_FLAG_LOCAL_CMD,
                                       cancellable,
                                       NULL,
                                       error))
    goto out;

  if (argc < 1)
    {
      rpmostree_usage_error (context, "NAME must be specified", error);
      goto out;
    }

  name = argv[1];
  if (argc == 2)
    pkgnames = (const char *const*)argv + 1;
  else
    pkgnames = (const char *const*)argv + 2;

  if (!glnx_opendirat (AT_FDCWD, userroot_base, TRUE, &userroot_dfd, error))
    goto out;

  target_rootdir = glnx_strjoina ("roots/", name);

  if (fstatat (userroot_dfd, target_rootdir, &stbuf, AT_SYMLINK_NOFOLLOW) < 0)
    {
      if (errno != ENOENT)
        {
          glnx_set_error_from_errno (error);
          goto out;
        }
    }
  else
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Tree %s already exists", target_rootdir);
      goto out;
    }

  { g_autofree char *repo_pathstr = g_strconcat (userroot_base, "repo", NULL);
    g_autoptr(GFile) repo_path = g_file_new_for_path (repo_pathstr);
    ostreerepo = ostree_repo_new (repo_path);
    if (!ostree_repo_open (ostreerepo, cancellable, error))
      goto out;
  }

  if (!glnx_opendirat (userroot_dfd, "rpm-md", FALSE, &rpmmd_dfd, error))
    goto out;
  {
    g_autofree char *abs_instroot = glnx_fdrel_abspath (userroot_dfd, target_rootdir);
    hifctx = _rpmostree_libhif_new (rpmmd_dfd, abs_instroot, NULL,
                                    NULL,
                                    cancellable, error);
    if (!hifctx)
      goto out;
  }

  /* --- Downloading metadata --- */
  if (!_rpmostree_libhif_console_download_metadata (hifctx, cancellable, error))
    goto out;

  { const char *const*strviter = pkgnames;
    for (; strviter && *strviter; strviter++)
      {
        const char *pkgname = *strviter;
        if (!hif_context_install (hifctx, pkgname, error))
          goto out;
      }
  }

  /* --- Resolving dependencies --- */
  if (!_rpmostree_libhif_console_prepare_install (hifctx, ostreerepo, &hifinstall, cancellable, error))
    goto out;

  /* --- Download and import as necessary --- */
  if (!_rpmostree_libhif_console_download_import (hifctx, ostreerepo, &hifinstall,
                                                  cancellable, error))
    goto out;

  if (!_rpmostree_libhif_console_mkroot (hifctx, ostreerepo, userroot_dfd, target_rootdir, &hifinstall,
                                         cancellable, error))
    goto out;

  exit_status = EXIT_SUCCESS;
 out:
  return exit_status;
}
