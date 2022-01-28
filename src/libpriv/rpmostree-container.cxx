/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*-
 *
 * Copyright (C) 2022 Red Hat, Inc.
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

#include "rpmostree-core.h"
#include "rpmostree-container.h"

#include <libglnx.h>

gboolean
rpmostree_container_rebuild (rpmostreecxx::Treefile &treefile,
                             GCancellable           *cancellable,
                             GError                **error)
{
  g_autoptr(RpmOstreeContext) ctx = rpmostree_context_new_container ();
  rpmostree_context_set_treefile (ctx, treefile);

  if (!rpmostree_context_setup (ctx, "/", "/", cancellable, error))
    return FALSE;

  if (!rpmostree_context_prepare (ctx, cancellable, error))
    return FALSE;

  if (!rpmostree_context_download (ctx, cancellable, error))
    return FALSE;

  DnfContext *dnfctx = rpmostree_context_get_dnf (ctx);

  /* can't use cancellable here because it wants to re-set it on the state,
   * which will trigger an assertion; XXX: tweak libdnf */
  if (!dnf_context_run (dnfctx, NULL, error))
    return FALSE;

  return TRUE;
}

gboolean
rpmostree_container_install_packages (char **packages,
                                      GCancellable  *cancellable,
                                      GError       **error)
{
  // XXX: awkward: add a e.g. `treefile_new_client_from_fields()`
  g_autofree char *joined = g_strjoinv ("\",\"", packages);
  g_autofree char *treefile_s = g_strdup_printf ("{\"packages\": [\"%s\"]}", joined);
  auto treefile = CXX_TRY_VAL(treefile_new_from_string (treefile_s, true), error);

  return rpmostree_container_rebuild (*treefile, cancellable, error);
}
