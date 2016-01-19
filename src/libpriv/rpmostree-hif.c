/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*-
 *
 * Copyright (C) 2014 Colin Walters <walters@verbum.org>
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

#include <glib-unix.h>
#include <rpm/rpmsq.h>
#include <gio/gunixoutputstream.h>

#include "rpmostree-hif.h"
#include "rpmostree-cleanup.h"

HifContext *
_rpmostree_libhif_new_default (void)
{
  HifContext *hifctx;

  /* We can be control-c'd at any time */
#if BUILDOPT_HAVE_RPMSQ_SET_INTERRUPT_SAFETY
  rpmsqSetInterruptSafety (FALSE);
#endif

  hifctx = hif_context_new ();
  hif_context_set_http_proxy (hifctx, g_getenv ("http_proxy"));

  hif_context_set_repo_dir (hifctx, "/etc/yum.repos.d");
  hif_context_set_cache_age (hifctx, G_MAXUINT);
  hif_context_set_cache_dir (hifctx, "/var/cache/rpm-ostree/metadata");
  hif_context_set_solv_dir (hifctx, "/var/cache/rpm-ostree/solv");
  hif_context_set_lock_dir (hifctx, "/run/rpm-ostree/lock");

  hif_context_set_check_disk_space (hifctx, FALSE);
  hif_context_set_check_transaction (hifctx, FALSE);
  hif_context_set_yumdb_enabled (hifctx, FALSE);

  return hifctx;
}

void
_rpmostree_libhif_set_cache_dfd (HifContext *hifctx, int dfd)
{
  g_autofree char *repomddir =
    glnx_fdrel_abspath (dfd, "repomd");
  g_autofree char *solvdir =
    glnx_fdrel_abspath (dfd, "solv");
  g_autofree char *lockdir =
    glnx_fdrel_abspath (dfd, "lock");

  hif_context_set_cache_dir (hifctx, repomddir);
  hif_context_set_solv_dir (hifctx, solvdir);
  hif_context_set_lock_dir (hifctx, lockdir);
}

gboolean
_rpmostree_libhif_setup (HifContext    *context,
                         GCancellable  *cancellable,
                         GError       **error)
{
  if (!hif_context_setup (context, cancellable, error))
    return FALSE;

  /* Forcibly override rpm/librepo SIGINT handlers.  We always operate
   * in a fully idempotent/atomic mode, and can be killed at any time.
   */
  signal (SIGINT, SIG_DFL);
  signal (SIGTERM, SIG_DFL);
  
  return TRUE;
}

void
_rpmostree_libhif_repos_disable_all (HifContext    *context)
{
  GPtrArray *sources;
  guint i;

  sources = hif_context_get_sources (context);
  for (i = 0; i < sources->len; i++)
    {
      HifSource *src = sources->pdata[i];
      
      hif_source_set_enabled (src, HIF_SOURCE_ENABLED_NONE);
    }
}

gboolean
_rpmostree_libhif_repos_enable_by_name (HifContext    *context,
                                        const char    *name,
                                        GError       **error)
{
  gboolean ret = FALSE;
  GPtrArray *sources;
  guint i;
  gboolean found = FALSE;

  sources = hif_context_get_sources (context);
  for (i = 0; i < sources->len; i++)
    {
      HifSource *src = sources->pdata[i];
      const char *id = hif_source_get_id (src);

      if (strcmp (name, id) != 0)
        continue;
      
      hif_source_set_enabled (src, HIF_SOURCE_ENABLED_PACKAGES);
#ifdef HAVE_HIF_SOURCE_SET_REQUIRED
      hif_source_set_required (src, TRUE);
#endif
      found = TRUE;
      break;
    }

  if (!found)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Unknown rpm-md repository: %s", name);
      goto out;
    }

  ret = TRUE;
 out:
  return ret;
}

static void
on_hifstate_percentage_changed (HifState   *hifstate,
                                guint       percentage,
                                gpointer    user_data)
{
  const char *text = user_data;
  glnx_console_progress_text_percent (text, percentage);
}

gboolean
_rpmostree_libhif_console_download_metadata (HifContext     *hifctx,
                                             GCancellable   *cancellable,
                                             GError        **error)
{
  gboolean ret = FALSE;
  guint progress_sigid;
  g_auto(GLnxConsoleRef) console = { 0, };
  gs_unref_object HifState *hifstate = hif_state_new ();

  progress_sigid = g_signal_connect (hifstate, "percentage-changed",
                                     G_CALLBACK (on_hifstate_percentage_changed), 
                                     "Downloading metadata:");

  glnx_console_lock (&console);

  if (!hif_context_setup_sack (hifctx, hifstate, error))
    goto out;

  g_signal_handler_disconnect (hifstate, progress_sigid);

  ret = TRUE;
 out:
  return ret;
}
  
static GPtrArray *
get_packages_to_download (HifContext  *hifctx)
{
  guint i;
  g_autoptr(GPtrArray) packages = NULL;
  g_autoptr(GPtrArray) packages_to_download = NULL;
  GPtrArray *sources = hif_context_get_sources (hifctx);
  
  packages = hif_goal_get_packages (hif_context_get_goal (hifctx),
                                    HIF_PACKAGE_INFO_INSTALL,
                                    HIF_PACKAGE_INFO_REINSTALL,
                                    HIF_PACKAGE_INFO_DOWNGRADE,
                                    HIF_PACKAGE_INFO_UPDATE,
                                    -1);
  packages_to_download = g_ptr_array_new_with_free_func ((GDestroyNotify)hy_package_free);
  
  for (i = 0; i < packages->len; i++)
    {
      HyPackage pkg = packages->pdata[i];
      HifSource *src = NULL;
      const char *cachepath;
      guint j;

      /* get correct package source */

      /* Hackily look up the source...we need a hash table */
      for (j = 0; j < sources->len; j++)
        {
          HifSource *tmpsrc = sources->pdata[j];
          if (g_strcmp0 (hy_package_get_reponame (pkg),
                         hif_source_get_id (tmpsrc)) == 0)
            {
              src = tmpsrc;
              break;
            }
        }

      g_assert (src);
      hif_package_set_source (pkg, src);

      /* this is a local file */
      if (hif_source_is_local (src) || 
          g_strcmp0 (hy_package_get_reponame (pkg), HY_CMDLINE_REPO_NAME) == 0)
        continue;

      cachepath = hif_package_get_filename (pkg);

      /* Right now we're not re-checksumming cached RPMs, we assume
       * they are valid.  This is a change from the current libhif
       * behavior, but I think it's right.  We should record validity
       * once, then ensure it's immutable after that.
       */
      if (g_file_test (cachepath, G_FILE_TEST_EXISTS))
        continue;

      g_ptr_array_add (packages_to_download, hy_package_link (pkg));
    }

  return g_steal_pointer (&packages_to_download);
}

gboolean
_rpmostree_libhif_console_prepare_install (HifContext     *hifctx,
                                           RpmOstreeHifInstall *out_install,
                                           GCancellable   *cancellable,
                                           GError       **error)
{
  gboolean ret = FALSE;

  printf ("%s", "Resolving dependencies: ");
  fflush (stdout);

  if (!hif_goal_depsolve (hif_context_get_goal (hifctx), error))
    {
      printf ("%s", "failed\n");
      goto out;
    }
  printf ("%s", "done\n");

  { GPtrArray *packages_to_download = get_packages_to_download (hifctx);
    out_install->packages_to_download = g_steal_pointer (&packages_to_download);
  }

  ret = TRUE;
 out:
  return ret;
}

struct GlobalDownloadState {
  struct RpmOstreeHifInstall *install;
  gchar *last_mirror_url;
  gchar *last_mirror_failure_message;
};

struct PkgDownloadState {
  struct GlobalDownloadState *gdlstate;
  gboolean added_total;
  guint64 last_bytes_fetched;
  gchar *last_mirror_url;
  gchar *last_mirror_failure_message;
};

static int
package_download_update_state_cb (void *user_data,
				  gdouble total_to_download,
				  gdouble now_downloaded)
{
  struct PkgDownloadState *dlstate = user_data;
  struct RpmOstreeHifInstall *install = dlstate->gdlstate->install;
  if (!dlstate->added_total)
    {
      dlstate->added_total = TRUE;
      install->n_bytes_to_fetch += (guint64) total_to_download;
    }

  install->n_bytes_fetched += ((guint64)now_downloaded) - dlstate->last_bytes_fetched;
  dlstate->last_bytes_fetched = ((guint64)now_downloaded);
  return LR_CB_OK;
}

static int
mirrorlist_failure_cb (void *user_data,
		       const char *message,
		       const char *url)
{
  struct PkgDownloadState *dlstate = user_data;
  struct GlobalDownloadState *gdlstate = dlstate->gdlstate;

  if (gdlstate->last_mirror_url)
    goto out;

  gdlstate->last_mirror_url = g_strdup (url);
  gdlstate->last_mirror_failure_message = g_strdup (message);
 out:
  return LR_CB_OK;
}

static int
package_download_complete_cb (void *user_data,
                              LrTransferStatus status,
                              const char *msg)
{
  struct PkgDownloadState *dlstate = user_data;
  switch (status)
    {
    case LR_TRANSFER_SUCCESSFUL:
    case LR_TRANSFER_ALREADYEXISTS:
      dlstate->gdlstate->install->n_packages_fetched++;
      return LR_CB_OK;
    case LR_TRANSFER_ERROR:
      return LR_CB_ERROR; 
    default:
      g_assert_not_reached ();
      return LR_CB_ERROR;
    }
}

/**
 * hif_source_checksum_hy_to_lr:
 **/
static LrChecksumType
hif_source_checksum_hy_to_lr (int checksum_hy)
{
	if (checksum_hy == HY_CHKSUM_MD5)
		return LR_CHECKSUM_MD5;
	if (checksum_hy == HY_CHKSUM_SHA1)
		return LR_CHECKSUM_SHA1;
	if (checksum_hy == HY_CHKSUM_SHA256)
		return LR_CHECKSUM_SHA256;
	return LR_CHECKSUM_UNKNOWN;
}

static gboolean
source_download_packages (HifSource *source,
                          GPtrArray *packages,
                          RpmOstreeHifInstall *install,
                          int        target_dfd,
                          GCancellable *cancellable,
                          GError **error)
{
  gboolean ret = FALSE;
  char *checksum_str = NULL;
  const unsigned char *checksum;
  guint i;
  int checksum_type;
  LrPackageTarget *target = NULL;
  GSList *package_targets = NULL;
  struct GlobalDownloadState gdlstate = { 0, };
  g_autoptr(GArray) pkg_dlstates = g_array_new (FALSE, TRUE, sizeof (struct PkgDownloadState));
  LrHandle *handle;
  g_autoptr(GError) error_local = NULL;
  g_autofree char *target_dir = NULL;

  handle = hif_source_get_lrhandle (source);

  gdlstate.install = install;

  g_array_set_size (pkg_dlstates, packages->len);

  for (i = 0; i < packages->len; i++)
    {
      g_autofree char *target_dir = NULL;
      HyPackage pkg = packages->pdata[i];
      struct PkgDownloadState *dlstate;

      if (target_dfd == -1)
        {
          target_dir = g_build_filename (hif_source_get_location (source), "packages/", NULL);
          if (!glnx_shutil_mkdir_p_at (AT_FDCWD, target_dir, 0755, cancellable, error))
            goto out;
        }
      else
        target_dir = glnx_fdrel_abspath (target_dfd, ".");
      
      checksum = hy_package_get_chksum (pkg, &checksum_type);
      checksum_str = hy_chksum_str (checksum, checksum_type);
      
      dlstate = &g_array_index (pkg_dlstates, struct PkgDownloadState, i);
      dlstate->gdlstate = &gdlstate;

      target = lr_packagetarget_new_v2 (handle,
                                        hy_package_get_location (pkg),
                                        target_dir,
                                        hif_source_checksum_hy_to_lr (checksum_type),
                                        checksum_str,
                                        0, /* size unknown */
                                        hy_package_get_baseurl (pkg),
                                        TRUE,
                                        package_download_update_state_cb,
                                        dlstate,
                                        package_download_complete_cb,
                                        mirrorlist_failure_cb,
                                        error);
      if (target == NULL)
        goto out;
	
      package_targets = g_slist_prepend (package_targets, target);
    }

  if (!lr_download_packages (package_targets, LR_PACKAGEDOWNLOAD_FAILFAST, &error_local))
    {
      if (g_error_matches (error_local,
                           LR_PACKAGE_DOWNLOADER_ERROR,
                           LRE_ALREADYDOWNLOADED))
        {
          /* ignore */
          g_clear_error (&error_local);
        }
      else
        {
          if (gdlstate.last_mirror_failure_message)
            {
              g_autofree gchar *orig_message = error_local->message;
              error_local->message = g_strconcat (orig_message, "; Last error: ", gdlstate.last_mirror_failure_message, NULL);
            }
          g_propagate_error (error, g_steal_pointer (&error_local));
          goto out;
        }
  } 

  ret = TRUE;
 out:
  g_free (gdlstate.last_mirror_failure_message);
  g_free (gdlstate.last_mirror_url);
  g_slist_free_full (package_targets, (GDestroyNotify)lr_packagetarget_free);
  hy_free (checksum_str);
  return ret;
}

gboolean
_rpmostree_libhif_console_download_content (HifContext     *hifctx,
                                            int             target_dfd,
                                            RpmOstreeHifInstall *install,
                                            GCancellable   *cancellable,
                                            GError        **error)
{
  gboolean ret = FALSE;
  gs_unref_object HifState *hifstate = hif_state_new ();
  g_autoptr(GHashTable) source_to_packages = g_hash_table_new_full (NULL, NULL, NULL, (GDestroyNotify)g_ptr_array_unref);
  GHashTableIter hiter;
  gpointer key, value;
  guint i;

  for (i = 0; i < install->packages_to_download->len; i++)
    {
      HyPackage pkg = install->packages_to_download->pdata[i];
      HifSource *src = hif_package_get_source (pkg);
      GPtrArray *source_packages;
      
      g_assert (src);
                     
      source_packages = g_hash_table_lookup (source_to_packages, src);
      if (!source_packages)
        {
          source_packages = g_ptr_array_new ();
          g_hash_table_insert (source_to_packages, src, source_packages);
        }
      g_ptr_array_add (source_packages, pkg);
    }

  g_hash_table_iter_init (&hiter, source_to_packages);
  while (g_hash_table_iter_next (&hiter, &key, &value))
    {
      HifSource *src = key;
      GPtrArray *src_packages = value;
      
      if (!source_download_packages (src, src_packages, install, target_dfd,
                                     cancellable, error))
        goto out;
    }

  ret = TRUE;
 out:
  return ret;
}
