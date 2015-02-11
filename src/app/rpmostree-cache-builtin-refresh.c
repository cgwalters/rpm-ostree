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

#include "rpmostree-hif.h"
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

  hifctx = _rpmostree_libhif_get_default ();

  if (opt_reposdir)
    hif_context_set_repo_dir (hifctx, opt_reposdir);

  if (!_rpmostree_libhif_setup (hifctx, cancellable, error))
    return FALSE;

  if (opt_enable_repos)
    {
      char **strviter;

      _rpmostree_libhif_repos_disable_all (hifctx);

      for (strviter = opt_enable_repos; strviter && *strviter; strviter++)
        {
          const char *repoid = *strviter;
          if (!hif_context_repo_enable (hifctx, repoid, error))
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

