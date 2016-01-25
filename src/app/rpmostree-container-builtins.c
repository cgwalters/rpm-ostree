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

typedef struct {
  char *userroot_base;
  int userroot_dfd;

  OstreeRepo *repo;
  HifContext *hifctx;

  int rpmmd_dfd;
} ROContainerContext;

#define RO_CONTAINER_CONTEXT_INIT { .userroot_dfd = -1, .rpmmd_dfd = -1 }

static gboolean
roc_context_init_core (ROContainerContext *rocctx,
                       GError            **error)
{
  gboolean ret = FALSE;

  rocctx->userroot_base = get_current_dir_name ();
  if (!glnx_opendirat (AT_FDCWD, rocctx->userroot_base, TRUE, &rocctx->userroot_dfd, error))
    goto out;

  { g_autofree char *repo_pathstr = g_strconcat (rocctx->userroot_base, "/repo", NULL);
    g_autoptr(GFile) repo_path = g_file_new_for_path (repo_pathstr);
    rocctx->repo = ostree_repo_new (repo_path);
  }

  ret = TRUE;
 out:
  return ret;
}

static gboolean
roc_context_init (ROContainerContext *rocctx,
                  GError            **error)
{
  gboolean ret = FALSE;
  
  if (!roc_context_init_core (rocctx, error))
    goto out;

  if (!ostree_repo_open (rocctx->repo, NULL, error))
    goto out;

  if (!glnx_opendirat (rocctx->userroot_dfd, "rpm-md", FALSE, &rocctx->rpmmd_dfd, error))
    goto out;

  ret = TRUE;
 out:
  return ret;
}

static gboolean
roc_context_prepare_for_root (ROContainerContext *rocctx,
                              const char         *target,
                              GError            **error)
{
  gboolean ret = FALSE;
  g_autofree char *abs_instroot = glnx_fdrel_abspath (rocctx->userroot_dfd, target);

  rocctx->hifctx = _rpmostree_libhif_new (rocctx->rpmmd_dfd, abs_instroot, NULL,
                                          NULL, NULL, error);
  if (!rocctx->hifctx)
    goto out;

  ret = TRUE;
 out:
  return ret;
}

static void
roc_context_deinit (ROContainerContext *rocctx)
{
  g_free (rocctx->userroot_base);
  if (rocctx->userroot_dfd)
    (void) close (rocctx->userroot_dfd);
  g_clear_object (&rocctx->repo);
  if (rocctx->rpmmd_dfd)
    (void) close (rocctx->rpmmd_dfd);
  g_clear_object (&rocctx->hifctx);
}

G_DEFINE_AUTO_CLEANUP_CLEAR_FUNC(ROContainerContext, roc_context_deinit)

int
rpmostree_container_builtin_init (int             argc,
                                  char          **argv,
                                  GCancellable   *cancellable,
                                  GError        **error)
{
  int exit_status = EXIT_FAILURE;
  g_auto(ROContainerContext) rocctx_data = RO_CONTAINER_CONTEXT_INIT;
  ROContainerContext *rocctx = &rocctx_data;
  GOptionContext *context = g_option_context_new ("");
  static const char* const directories[] = { "repo", "rpm-md", "roots", "tmp" };
  guint i;
  
  if (!rpmostree_option_context_parse (context,
                                       init_option_entries,
                                       &argc, &argv,
                                       RPM_OSTREE_BUILTIN_FLAG_LOCAL_CMD,
                                       cancellable,
                                       NULL,
                                       error))
    goto out;

  if (!roc_context_init_core (rocctx, error))
    goto out;

  for (i = 0; i < G_N_ELEMENTS (directories); i++)
    {
      if (!glnx_shutil_mkdir_p_at (rocctx->userroot_dfd, directories[i], 0755, cancellable, error))
        goto out;
    }

  if (!ostree_repo_create (rocctx->repo, OSTREE_REPO_MODE_BARE_USER, cancellable, error))
    goto out;

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
  g_auto(ROContainerContext) rocctx_data = RO_CONTAINER_CONTEXT_INIT;
  ROContainerContext *rocctx = &rocctx_data;
  g_auto(RpmOstreeHifInstall) hifinstall = {0,};
  const char *name;
  struct stat stbuf;
  const char *const*pkgnames;
  g_autofree char *commit = NULL;
  const char *target_rootdir;
  
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

  if (!roc_context_init (rocctx, error))
    goto out;

  target_rootdir = glnx_strjoina ("roots/", name);

  if (fstatat (rocctx->userroot_dfd, target_rootdir, &stbuf, AT_SYMLINK_NOFOLLOW) < 0)
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

  if (!roc_context_prepare_for_root (rocctx, target_rootdir, error))
    goto out;

  /* --- Downloading metadata --- */
  if (!_rpmostree_libhif_console_download_metadata (rocctx->hifctx, cancellable, error))
    goto out;

  { const char *const*strviter = pkgnames;
    for (; strviter && *strviter; strviter++)
      {
        const char *pkgname = *strviter;
        if (!hif_context_install (rocctx->hifctx, pkgname, error))
          goto out;
      }
  }

  /* --- Resolving dependencies --- */
  if (!_rpmostree_libhif_console_prepare_install (rocctx->hifctx, rocctx->repo, &hifinstall,
                                                  cancellable, error))
    goto out;

  /* --- Download and import as necessary --- */
  if (!_rpmostree_libhif_console_download_import (rocctx->hifctx, rocctx->repo, &hifinstall,
                                                  cancellable, error))
    goto out;

  { glnx_fd_close int tmpdir_dfd = -1;

    if (!glnx_opendirat (rocctx->userroot_dfd, "tmp", TRUE, &tmpdir_dfd, error))
      goto out;
    
    if (!_rpmostree_libhif_console_assemble_commit (rocctx->hifctx, tmpdir_dfd,
                                                    rocctx->repo, name,
                                                    &hifinstall,
                                                    &commit,
                                                    cancellable, error))
      goto out;
  }

  g_print ("Checking out %s @ %s...\n", name, commit);

  { OstreeRepoCheckoutOptions opts = { OSTREE_REPO_CHECKOUT_MODE_USER,
                                       OSTREE_REPO_CHECKOUT_OVERWRITE_UNION_FILES, };

    /* For now... to be crash safe we'd need to duplicate some of the
     * boot-uuid/fsync gating at a higher level.
     */
    opts.disable_fsync = TRUE;

    if (!ostree_repo_checkout_tree_at (rocctx->repo, &opts, rocctx->userroot_dfd, target_rootdir,
                                       commit, cancellable, error))
      goto out;
  }

  g_print ("Checking out %s @ %s...done\n", name, commit);

  exit_status = EXIT_SUCCESS;
 out:
  return exit_status;
}

gboolean
rpmostree_container_builtin_upgrade (int argc, char **argv, GCancellable *cancellable, GError **error)
{
  int exit_status = EXIT_FAILURE;

  exit_status = EXIT_SUCCESS;
 out:
  return exit_status;
}
