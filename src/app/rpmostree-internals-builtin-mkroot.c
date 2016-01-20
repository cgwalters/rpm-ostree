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
  { "ostree-repo", 0, 0, G_OPTION_ARG_STRING, &opt_ostree_repo, "OSTree repo to use as cache at PATH", "PATH" },
  { "yum-reposdir", 0, 0, G_OPTION_ARG_STRING, &opt_yum_reposdir, "Path to yum repo configs (default: /etc/yum.repos.d)", NULL },
  { "enable-yum-repo", 0, 0, G_OPTION_ARG_STRING_ARRAY, &opt_enable_yum_repos, "Enable yum repository", NULL },
  { "suid-fcaps", 0, 0, G_OPTION_ARG_NONE, &opt_suid_fcaps, "Enable setting suid/sgid and capabilities", NULL },
  { "owner", 0, 0, G_OPTION_ARG_NONE, &opt_owner, "Enable chown", NULL },
  { NULL }
};

static gboolean
unpack_one_package (int           rootfs_fd,
                    HifContext   *hifctx,
                    HyPackage     pkg,
                    OstreeRepo   *ostreerepo,
                    const char   *pkg_ostree_commit,
                    GCancellable *cancellable,
                    GError      **error)
{
  gboolean ret = FALSE;
  OstreeRepoCheckoutOptions opts = { OSTREE_REPO_CHECKOUT_MODE_USER,
                                     OSTREE_REPO_CHECKOUT_OVERWRITE_NONE, };

  if (!ostree_repo_checkout_tree_at (ostreerepo, &opts, rootfs_fd, ".",
                                     pkg_ostree_commit, cancellable, error))
    goto out;

  ret = TRUE;
 out:
  return ret;
}

typedef void (*UnpackProgressCallback) (gpointer user_data, guint unpacked, guint total);

static gboolean
unpack_packages_in_root (int          rootfs_fd,
                         OstreeRepo  *ostreerepo,
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
  g_autoptr(GHashTable) pkg_to_ostree_commit =
    g_hash_table_new_full (NULL, NULL, (GDestroyNotify)hy_package_free, (GDestroyNotify)g_free);
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
        glnx_unref_object RpmOstreeUnpacker *unpacker = NULL;
        g_autofree char *cachebranch = _rpmostree_get_cache_branch_pkg (pkg);
        g_autofree char *cached_rev = NULL; 
        g_autoptr(GVariant) pkg_commit = NULL;
        g_autoptr(GVariant) header_variant = NULL;

        if (!ostree_repo_resolve_rev (ostreerepo, cachebranch, FALSE, &cached_rev, error))
          goto out;

        if (!ostree_repo_load_variant (ostreerepo, OSTREE_OBJECT_TYPE_COMMIT, cached_rev,
                                       &pkg_commit, error))
          goto out;

        { g_autoptr(GVariant) pkg_meta = g_variant_get_child_value (pkg_commit, 0);
          g_autoptr(GVariantDict) pkg_meta_dict = g_variant_dict_new (pkg_meta);

          if (!g_variant_dict_lookup (pkg_meta_dict, "rpmostree.header", "@ay", &header_variant))
            {
              g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                           "Unable to find 'rpmostree.header' key in commit %s of %s",
                           cached_rev, hif_package_get_id (pkg));
              goto out;
            }
        }

        { Header hdr = headerImport ((void*)g_variant_get_data (header_variant),
                                     g_variant_get_size (header_variant),
                                     HEADERIMPORT_COPY);
          int r = rpmtsAddInstallElement (ts, hdr, hif_package_get_filename (pkg), TRUE, NULL);
          headerFree (hdr);
          if (r != 0)
            {
              g_set_error (error,
                           G_IO_ERROR,
                           G_IO_ERROR_FAILED,
                           "Failed to add install element for %s",
                           hif_package_get_filename (pkg));
		goto out;
            }
        }
          
        g_hash_table_insert (nevra_to_pkg, hy_package_get_nevra (pkg), hy_package_link (pkg));
        g_hash_table_insert (pkg_to_ostree_commit, hy_package_link (pkg), g_steal_pointer (&pkg_commit));

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

  if (!unpack_one_package (rootfs_fd, hifctx, filesystem_package, ostreerepo,
                           g_hash_table_lookup (pkg_to_ostree_commit, filesystem_package),
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

      if (!unpack_one_package (rootfs_fd, hifctx, pkg, ostreerepo,
                               g_hash_table_lookup (pkg_to_ostree_commit, pkg),
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
  const char *rootpath;
  const char *const*pkgnames;
  glnx_fd_close int rootfs_fd = -1;
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

  if (!glnx_opendirat (AT_FDCWD, argv[1], TRUE, &rootfs_fd, error))
    goto out;

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
  if (!_rpmostree_libhif_console_prepare_install (hifctx, ostreerepo, &hifinstall, cancellable, error))
    goto out;

  rpmostree_print_transaction (hifctx);
  g_print ("Will download %u packages\n", hifinstall.packages_to_download->len);

  /* --- Download and import as necessary --- */
  if (!_rpmostree_libhif_console_download_import (hifctx, ostreerepo, &hifinstall,
                                                  cancellable, error))
    goto out;

  { g_auto(GLnxConsoleRef) console = {0,};
    glnx_console_lock (&console);
    
    if (!unpack_packages_in_root (rootfs_fd, ostreerepo, hifctx, install_progress_cb, NULL,
                                  cancellable, error))
      goto out;
  }

  exit_status = EXIT_SUCCESS;
 out:
  return exit_status;
}
