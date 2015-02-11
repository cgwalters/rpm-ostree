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

#include <libhif.h>
#include <libhif/hif-utils.h>

#include "rpmostree-cache-builtins.h"
#include "rpmostree-util.h"
#include "rpmostree-cleanup.h"
#include "rpmostree-rpm-util.h"

static char *opt_reposdir;
static char **opt_enable_repos;

static GOptionEntry option_entries[] = {
  { "reposdir", 0, 0, G_OPTION_ARG_STRING, &opt_reposdir,
    "Use PATH as directory for yum repository files", "PATH" },
  { "enable-repo", 0, 0, G_OPTION_ARG_STRING_ARRAY, &opt_enable_repos,
    "Enable REPO for this operation, overriding enabled= flag in configuration", "REPO" },
  { NULL }
};

static void
on_hifstate_percentage_changed (HifState   *hifstate,
                                guint       percentage,
                                gpointer    user_data)
{
  const char *text = user_data;
  glnx_console_progress_text_percent (text, percentage);
}

gboolean
rpmostree_cache_builtin_refresh (int argc, char **argv, GCancellable *cancellable, GError **error)
{
  GOptionContext *context;
  gboolean ret = FALSE;
  gs_unref_object HifContext *hifctx = NULL;

  context = g_option_context_new ("Update cache for enabled repositories");
  if (!rpmostree_option_context_parse (context, option_entries, &argc, &argv, error))
    goto out;

  hifctx = hif_context_new ();
  hif_context_set_http_proxy (hifctx, g_getenv ("http_proxy"));

  if (!opt_reposdir)
    opt_reposdir = "/etc/yum.repos.d";

  hif_context_set_repo_dir (hifctx, opt_reposdir);
  hif_context_set_cache_dir (hifctx, "/var/cache/rpm-ostree/metadata");
  hif_context_set_solv_dir (hifctx, "/var/cache/rpm-ostree/solv");
  hif_context_set_lock_dir (hifctx, "/run/rpm-ostree/lock");

  hif_context_set_check_disk_space (hifctx, FALSE);
  hif_context_set_check_transaction (hifctx, FALSE);

  if (!hif_context_setup (hifctx, NULL, error))
    return FALSE;

  if (opt_enable_repos)
    {
      GPtrArray *sources = hif_context_get_sources (hifctx);
      gs_unref_hashtable GHashTable *enabled_repos = g_hash_table_new (g_str_hash, g_str_equal);
      char **strviter;
      guint i;

      for (strviter = opt_enable_repos; strviter && *strviter; strviter++)
        g_hash_table_add (enabled_repos, *strviter);

      for (i = 0; i < sources->len; i++)
        {
          HifSource *src = sources->pdata[i];
          const char *repoid = hif_source_get_id (src);

          if (!g_hash_table_contains (enabled_repos, repoid))
            hif_source_set_enabled (src, HIF_SOURCE_ENABLED_NONE);
          else
            {
              hif_source_set_enabled (src, HIF_SOURCE_ENABLED_PACKAGES);
              g_hash_table_remove (enabled_repos, repoid);
            }
        }

      if (g_hash_table_size (enabled_repos) > 0)
        {
          GHashTableIter hashiter;
          gpointer hkey, hvalue;
          GString *err = g_string_new ("Repositories enabled but not found:");

          g_hash_table_iter_init (&hashiter, enabled_repos);
          while (g_hash_table_iter_next (&hashiter, &hkey, &hvalue))
            {
              g_string_append_c (err, ' ');
              g_string_append (err, (char*)hkey);
            }
          g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                               err->str);
          g_string_free (err, TRUE);
          goto out;
        }
    }

  { g_auto(GLnxConsoleRef) console = { 0, };
    gs_unref_object HifState *hifstate = hif_state_new ();
    guint progress_sigid;

    progress_sigid = g_signal_connect (hifstate, "percentage-changed",
                                     G_CALLBACK (on_hifstate_percentage_changed), 
                                     "Downloading metadata:");

    glnx_console_lock (&console);

    if (!hif_context_setup_sack (hifctx, hifstate, error))
      goto out;

    g_signal_handler_disconnect (hifstate, progress_sigid);
  }

  ret = TRUE;
out:
  g_option_context_free (context);
  return ret;
}

