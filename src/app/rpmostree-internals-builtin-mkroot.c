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
static char *opt_yum_reposdir = "/etc/yum.repos.d";
static gboolean opt_suid_fcaps = FALSE;
static gboolean opt_owner = FALSE;
static char **opt_enable_yum_repos = NULL;

static GOptionEntry option_entries[] = {
  { "ostree-repo", 0, 0, G_OPTION_ARG_NONE, &opt_ostree_repo, "OSTree repo to use as cache", NULL },
  { "yum-reposdir", 0, 0, G_OPTION_ARG_STRING, &opt_yum_reposdir, "Path to yum repo configs (default: /etc/yum.repos.d)", NULL },
  { "enable-yum-repo", 0, 0, G_OPTION_ARG_STRING_ARRAY, &opt_enable_yum_repos, "Enable yum repository", NULL },
  { "suid-fcaps", 0, 0, G_OPTION_ARG_NONE, &opt_suid_fcaps, "Enable setting suid/sgid and capabilities", NULL },
  { "owner", 0, 0, G_OPTION_ARG_NONE, &opt_owner, "Enable chown", NULL },
  { NULL }
};

static char *
hif_package_relpath (HyPackage package)
{
  return g_strconcat ("repomd/", hy_package_get_reponame (package),
                      "/packages/", glnx_basename (hy_package_get_location (package)), NULL);
}

static gboolean
unpack_one_package (int           rootfs_fd,
                    int           tmpdir_dfd,
                    HifContext   *hifctx,
                    HyPackage     pkg,
                    GCancellable *cancellable,
                    GError      **error)
{
  gboolean ret = FALSE;
  g_autofree char *package_relpath = NULL;
  g_autofree char *pkg_abspath = NULL;
  RpmOstreeUnpackerFlags flags = 0;
  glnx_unref_object RpmOstreeUnpacker *unpacker = NULL;
   
  package_relpath = hif_package_relpath (pkg);
  pkg_abspath = glnx_fdrel_abspath (tmpdir_dfd, package_relpath);

  /* suid implies owner too...anything else is dangerous, as we might write
   * a setuid binary for the caller.
   */
  if (opt_owner || opt_suid_fcaps)
    flags |= RPMOSTREE_UNPACKER_FLAGS_OWNER;
  if (opt_suid_fcaps)
    flags |= RPMOSTREE_UNPACKER_FLAGS_SUID_FSCAPS;
        
  unpacker = rpmostree_unpacker_new_at (tmpdir_dfd, package_relpath, flags, error);
  if (!unpacker)
    goto out;

  if (!rpmostree_unpacker_unpack_to_dfd (unpacker, rootfs_fd, cancellable, error))
    {
      g_autofree char *nevra = hy_package_get_nevra (pkg);
      g_prefix_error (error, "Unpacking %s: ", nevra);
      goto out;
    }
   
  if (TEMP_FAILURE_RETRY (unlinkat (tmpdir_dfd, package_relpath, 0)) < 0)
    {
      glnx_set_error_from_errno (error);
      g_prefix_error (error, "Deleting %s: ", package_relpath);
      goto out;
    }

  ret = TRUE;
 out:
  return ret;
}

typedef void (*UnpackProgressCallback) (gpointer user_data, guint unpacked, guint total);

static gboolean
unpack_packages_in_root (int          rootfs_fd,
                         int          tmpdir_dfd,
                         HifContext  *hifctx,
                         UnpackProgressCallback progress_callback,
                         gpointer         user_data,
                         GCancellable *cancellable,
                         GError      **error)
{
  gboolean ret = FALSE;
  rpmts ts = rpmtsCreate ();
  guint i, n_rpmts_elements;
  g_autoptr(GHashTable) nevra_to_pkg =
    g_hash_table_new_full (g_str_hash, g_str_equal, (GDestroyNotify)g_free, (GDestroyNotify)hy_package_free);
  HyPackage filesystem_package = NULL;   /* It's special... */
  guint n_unpacked = 0;

  /* Tell librpm about each one so it can tsort them.  What we really
   * want is to do this from the rpm-md metadata so that we can fully
   * parallelize download + unpack.
   */
  { g_autoptr(GPtrArray) package_list = NULL;

    package_list = hif_goal_get_packages (hif_context_get_goal (hifctx),
                                          HIF_PACKAGE_INFO_INSTALL,
                                          HIF_PACKAGE_INFO_REINSTALL,
                                          HIF_PACKAGE_INFO_DOWNGRADE,
                                          HIF_PACKAGE_INFO_UPDATE,
                                          -1);

    for (i = 0; i < package_list->len; i++)
      {
        HyPackage pkg = package_list->pdata[i];
        g_autofree char *package_relpath = hif_package_relpath (pkg);
        g_autofree char *pkg_abspath = glnx_fdrel_abspath (tmpdir_dfd, package_relpath);
        glnx_unref_object RpmOstreeUnpacker *unpacker = NULL;
        const gboolean allow_untrusted = TRUE;
        const gboolean is_update = FALSE;

        ret = hif_rpmts_add_install_filename (ts,
                                              pkg_abspath,
                                              allow_untrusted,
                                              is_update,
                                              error);
        if (!ret)
          goto out;

        g_hash_table_insert (nevra_to_pkg, hy_package_get_nevra (pkg), hy_package_link (pkg));

        if (strcmp (hy_package_get_name (pkg), "filesystem") == 0)
          filesystem_package = hy_package_link (pkg);
      }
  }

  rpmtsOrder (ts);
  _rpmostree_reset_rpm_sighandlers ();

  /* Okay so what's going on in Fedora with incestuous relationship
   * between the `filesystem`, `setup`, `libgcc` RPMs is actively
   * ridiculous.  If we unpack libgcc first it writes to /lib64 which
   * is really /usr/lib64, then filesystem blows up since it wants to symlink
   * /lib64 -> /usr/lib64.
   *
   * Really `filesystem` should be first but it depends on `setup` for
   * stupid reasons which is hacked around in `%pretrans` which we
   * don't run.  Just forcibly unpack it first.
   */

  n_rpmts_elements = (guint)rpmtsNElements (ts);
  n_unpacked = 0;
  if (progress_callback)
    progress_callback (user_data, n_unpacked, n_rpmts_elements);

  if (!unpack_one_package (rootfs_fd, tmpdir_dfd, hifctx, filesystem_package,
                           cancellable, error))
    goto out;

  n_unpacked++;
  if (progress_callback)
    progress_callback (user_data, n_unpacked, n_rpmts_elements);

  for (i = 0; i < n_rpmts_elements; i++)
    {
      rpmte te = rpmtsElement (ts, i);
      const char *nevra = rpmteNEVRA (te);
      HyPackage pkg = g_hash_table_lookup (nevra_to_pkg, nevra);

      g_assert (pkg);

      if (pkg == filesystem_package)
        continue;

      if (!unpack_one_package (rootfs_fd, tmpdir_dfd, hifctx, pkg,
                               cancellable, error))
        goto out;
      n_unpacked++;
      if (progress_callback)
        progress_callback (user_data, n_unpacked, n_rpmts_elements);
    }

  ret = TRUE;
 out:
  if (ts)
    rpmtsFree (ts);
  return ret;
}

static void
install_progress_cb (gpointer user_data,
                     guint n_unpacked,
                     guint n_total)
{
  glnx_console_progress_text_percent ("Unpacking: ", ((double)n_unpacked)/n_total * 100);
}

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
  g_auto(GLnxConsoleRef) console = {0,};
  const char *rootpath;
  const char *const*pkgnames;
  glnx_fd_close int rootfs_fd = -1;
  g_autofree char *tmpdir_path = NULL;
  glnx_fd_close int tmpdir_dfd = -1;
  
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

  if (!glnx_opendirat (AT_FDCWD, argv[1], TRUE, &rootfs_fd, error))
    goto out;

  hifctx = _rpmostree_libhif_new_default ();

  tmpdir_path = g_strdup ("/var/tmp/rpm-ostree.XXXXXX");
  if (!glnx_mkdtempat (AT_FDCWD, tmpdir_path, 0700, error))
    goto out;
  if (!glnx_opendirat (AT_FDCWD, tmpdir_path, FALSE, &tmpdir_dfd, error))
    goto out;

  _rpmostree_libhif_set_cache_dfd (hifctx, tmpdir_dfd);
  hif_context_set_install_root (hifctx, rootpath);
  if (opt_yum_reposdir)
    hif_context_set_repo_dir (hifctx, opt_yum_reposdir);
    
  if (!_rpmostree_libhif_setup (hifctx, cancellable, error))
    goto out;
  _rpmostree_libhif_repos_disable_all (hifctx);

  { char **strviter = opt_enable_yum_repos;
    for (; strviter && *strviter; strviter++)
      {
        const char *reponame = *strviter;
        if (!_rpmostree_libhif_repos_enable_by_name (hifctx, reponame, error))
          goto out;
      }
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
  if (!_rpmostree_libhif_console_prepare_install (hifctx, &hifinstall, cancellable, error))
    goto out;

  rpmostree_print_transaction (hifctx);
  g_print ("Will download %u packages\n", hifinstall.packages_to_download->len);

  /* --- Downloading packages --- */
  if (!_rpmostree_libhif_console_download_content (hifctx, -1, &hifinstall,
                                                   cancellable, error))
    goto out;

  glnx_console_lock (&console);

  if (!unpack_packages_in_root (rootfs_fd, tmpdir_dfd, hifctx, install_progress_cb, NULL,
                                cancellable, error))
    goto out;

  exit_status = EXIT_SUCCESS;
 out:
  return exit_status;
}
