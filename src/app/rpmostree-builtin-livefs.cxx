/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*-
 *
 * Copyright (C) 2017 Colin Walters <walters@verbum.org>
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

#include "rpmostree-ex-builtins.h"
#include "rpmostree-libbuiltin.h"
#include "rpmostree-dbus-helpers.h"
#include "rpmostree-rust.h"
#include "rpmostree-cxxrs.h"

#include <libglnx.h>

static char *opt_target;
static gboolean opt_reset;

static GOptionEntry option_entries[] = {
  { "target", 0, 0, G_OPTION_ARG_STRING, &opt_target, "Target provided commit instead of pending deployment", NULL },
  { "reset", 0, 0, G_OPTION_ARG_NONE, &opt_reset, "Reset back to booted commit", NULL },
  { NULL }
};

static GVariant *
get_args_variant (GError **error)
{
  GVariantDict dict;

  g_variant_dict_init (&dict, NULL);
  if (opt_target)
    {
      if (opt_reset)
        return (GVariant*)glnx_null_throw (error, "Cannot specify both --target and --reset");
      g_variant_dict_insert (&dict, "target", "s", opt_target);
    }
  else if (opt_reset)
    {
      OstreeSysroot *sysroot = ostree_sysroot_new_default ();
      if (!ostree_sysroot_load (sysroot, NULL, error))
        return FALSE;
      OstreeDeployment *booted = ostree_sysroot_get_booted_deployment (sysroot);
      if (!booted)
        return (GVariant*)glnx_null_throw (error, "Not in a booted OSTree deployment");
      g_variant_dict_insert (&dict, "target", "s", ostree_deployment_get_csum (booted));
    }

  return g_variant_ref_sink (g_variant_dict_end (&dict));
}

gboolean
rpmostree_ex_builtin_livefs (int             argc,
                             char          **argv,
                             RpmOstreeCommandInvocation *invocation,
                             GCancellable   *cancellable,
                             GError        **error)
{
  _cleanup_peer_ GPid peer_pid = 0;
  glnx_unref_object RPMOSTreeSysroot *sysroot_proxy = NULL;
  g_autoptr(GOptionContext) context = g_option_context_new ("");
  if (!rpmostree_option_context_parse (context,
                                       option_entries,
                                       &argc, &argv,
                                       invocation,
                                       cancellable,
                                       NULL, NULL,
                                       &sysroot_proxy,
                                       &peer_pid,
                                       NULL,
                                       error))
    return FALSE;

  glnx_unref_object RPMOSTreeOS *os_proxy = NULL;
  glnx_unref_object RPMOSTreeOSExperimental *osexperimental_proxy = NULL;
  if (!rpmostree_load_os_proxies (sysroot_proxy, NULL,
                                  cancellable, &os_proxy,
                                  &osexperimental_proxy, error))
    return FALSE;

  g_autofree char *transaction_address = NULL;
  g_autoptr(GVariant) args = get_args_variant (error);
  if (!args)
    return FALSE;
  if (!rpmostree_osexperimental_call_live_fs_sync (osexperimental_proxy,
                                                   args,
                                                   &transaction_address,
                                                   cancellable,
                                                   error))
    return FALSE;

  if (!rpmostree_transaction_get_response_sync (sysroot_proxy,
                                                transaction_address,
                                                cancellable,
                                                error))
    return FALSE;

  /* TODO - we compute all this client side right now for multiple reasons.
   * - The diff printing code all lives on the client right now
   * - We don't bind `rpmostree_output_message()` into Rust yet
   * - We've historically accessed RPM diffs client side
   */
  g_autoptr(OstreeSysroot) sysroot = ostree_sysroot_new_default ();
  if (!ostree_sysroot_load (sysroot, cancellable, error))
    return FALSE;
  g_autoptr(OstreeRepo) repo = NULL;
  if (!ostree_sysroot_get_repo (sysroot, &repo, cancellable, error))
    return FALSE;
  OstreeDeployment *booted_deployment = ostree_sysroot_get_booted_deployment (sysroot);
  g_assert (booted_deployment);
  const char *booted_commit = ostree_deployment_get_csum (booted_deployment);

  auto live_state = rpmostreecxx::get_live_apply_state(*sysroot, *booted_deployment);
  g_assert (live_state.commit.length() > 0);

  gboolean have_target = FALSE;
  if (!ostree_repo_has_object (repo, OSTREE_OBJECT_TYPE_COMMIT, live_state.commit.c_str(), &have_target, NULL, error))
    return FALSE;
  /* It might happen that the live target commit was GC'd somehow; we're not writing
   * an explicit ref for it.  In that case skip the diff.
   */
  if (have_target)
    {
      g_autoptr(GPtrArray) removed = NULL;
      g_autoptr(GPtrArray) added = NULL;
      g_autoptr(GPtrArray) modified_old = NULL;
      g_autoptr(GPtrArray) modified_new = NULL;
      if (!rpm_ostree_db_diff (repo, booted_commit, live_state.commit.c_str(),
                              &removed, &added, &modified_old, &modified_new,
                              cancellable, error))
        return FALSE;
      rpmostree_diff_print_formatted (RPMOSTREE_DIFF_PRINT_FORMAT_FULL_MULTILINE, NULL, 0,
                                      removed, added, modified_old, modified_new);
    }

  g_print ("Successfully updated running filesystem tree; some services may need to be restarted.\n");

  return TRUE;
}
