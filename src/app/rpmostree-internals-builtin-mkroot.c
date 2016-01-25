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
#include <libhif/hif-package.h>
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

static char *opt_ostree_repo;
static char *opt_rpmmd_cachedir;
static char *opt_yum_reposdir = "/etc/yum.repos.d";
static gboolean opt_suid_fcaps = FALSE;
static gboolean opt_owner = FALSE;
static char **opt_enable_yum_repos = NULL;

static GOptionEntry option_entries[] = {
  { "ostree-repo", 0, 0, G_OPTION_ARG_STRING, &opt_ostree_repo, "OSTree repo to use as cache at PATH", "PATH" },
  { "rpmmd-cachedir", 0, 0, G_OPTION_ARG_STRING, &opt_rpmmd_cachedir, "Path to rpm-md cache", NULL },
  { "yum-reposdir", 0, 0, G_OPTION_ARG_STRING, &opt_yum_reposdir, "Path to yum repo configs (default: /etc/yum.repos.d)", NULL },
  { "enable-yum-repo", 0, 0, G_OPTION_ARG_STRING_ARRAY, &opt_enable_yum_repos, "Enable yum repository", NULL },
  { "suid-fcaps", 0, 0, G_OPTION_ARG_NONE, &opt_suid_fcaps, "Enable setting suid/sgid and capabilities", NULL },
  { "owner", 0, 0, G_OPTION_ARG_NONE, &opt_owner, "Enable chown", NULL },
  { NULL }
};

int
rpmostree_internals_builtin_mkroot (int             argc,
                                    char          **argv,
                                    GCancellable   *cancellable,
                                    GError        **error)
{
  int exit_status = EXIT_FAILURE;
  GOptionContext *context = g_option_context_new ("ROOT PKGNAME [PKGNAME...]");
  glnx_unref_object HifContext *hifctx = NULL;
  g_auto(RpmOstreeHifInstall) hifinstall = {0,};
  const char *rootpath;
  const char *const*pkgnames;
  g_autofree char *tmpdir_path = NULL;
  glnx_fd_close int tmpdir_dfd = -1;
  glnx_unref_object OstreeRepo *ostreerepo = NULL;
  
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
      rpmostree_usage_error (context, "ROOT and at least one PKGNAME must be specified", error);
      goto out;
    }

  rootpath = argv[1];
  pkgnames = (const char *const*)argv + 2;

  if (opt_ostree_repo)
    {
      g_autoptr(GFile) repo_path = g_file_new_for_path (opt_ostree_repo);
      ostreerepo = ostree_repo_new (repo_path);
      if (!ostree_repo_open (ostreerepo, cancellable, error))
        goto out;
    }
  else
    {
      rpmostree_usage_error (context, "--ostree-repo is required", error);
      goto out;
    }

  if (g_file_test (rootpath, G_FILE_TEST_EXISTS))
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Target root %s already exists", rootpath);
      goto out;
    }


  if (opt_rpmmd_cachedir)
    {
      if (!glnx_opendirat (AT_FDCWD, opt_rpmmd_cachedir, FALSE, &tmpdir_dfd, error))
        goto out;
    }
  else
    {
      tmpdir_path = g_strdup ("/var/tmp/rpm-ostree.XXXXXX");
      if (!glnx_mkdtempat (AT_FDCWD, tmpdir_path, 0700, error))
        goto out;
      if (!glnx_opendirat (AT_FDCWD, tmpdir_path, FALSE, &tmpdir_dfd, error))
        goto out;
    }

  hifctx = _rpmostree_libhif_new (tmpdir_dfd, rootpath, opt_yum_reposdir,
                                  (const char * const *)opt_enable_yum_repos,
                                  cancellable, error);
  if (!hifctx)
    goto out;

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

  if (!_rpmostree_libhif_console_assemble_commit (hifctx, ostreerepo, AT_FDCWD, rootpath, &hifinstall,
                                                  &commit,
                                                  cancellable, error))
    goto out;

  exit_status = EXIT_SUCCESS;
 out:
  if (tmpdir_path)
    (void) glnx_shutil_rm_rf_at (AT_FDCWD, tmpdir_path, cancellable, NULL);
  return exit_status;
}
