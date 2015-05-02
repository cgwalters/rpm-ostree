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

#include "string.h"

#include "rpmostree-libbuiltin.h"
#include "rpmostree.h"
#include "rpmostree-cleanup.h"

gboolean
rpmostree_print_treepkg_diff (OstreeSysroot    *sysroot,
                              GCancellable     *cancellable,
                              GError          **error)
{
  gboolean ret = FALSE;
  OstreeDeployment *booted_deployment;
  OstreeDeployment *new_deployment;
  gs_unref_ptrarray GPtrArray *deployments = 
    ostree_sysroot_get_deployments (sysroot);

  booted_deployment = ostree_sysroot_get_booted_deployment (sysroot);
  
  g_assert (deployments->len > 1);
  new_deployment = deployments->pdata[0];

  if (booted_deployment && new_deployment != booted_deployment)
    {
      gs_unref_object OstreeRepo *repo = NULL;
      const char *from_rev = ostree_deployment_get_csum (booted_deployment);
      const char *to_rev = ostree_deployment_get_csum (new_deployment);
      g_autoptr(GPtrArray) removed = NULL;
      g_autoptr(GPtrArray) added = NULL;
      g_autoptr(GPtrArray) modified_old = NULL;
      g_autoptr(GPtrArray) modified_new = NULL;
      guint i;
      
      if (!ostree_sysroot_get_repo (sysroot, &repo, cancellable, error))
        goto out;
      
      if (!rpm_ostree_db_diff (repo, from_rev, to_rev,
                               &removed, &added, &modified_old, &modified_new,
                               cancellable, error))
        goto out;

      if (modified_old->len > 0)
        g_print ("Changed:\n");

      for (i = 0; i < modified_old->len; i++)
        {
          RpmOstreePackage *oldpkg = modified_old->pdata[i];
          RpmOstreePackage *newpkg;
          const char *name = rpm_ostree_package_get_name (oldpkg);

          g_assert_cmpuint (i, <, modified_new->len);

          newpkg = modified_old->pdata[i];

          g_print ("  %s %s -> %s\n", name,
                   rpm_ostree_package_get_evr (oldpkg),
                   rpm_ostree_package_get_evr (newpkg));
        }

      if (removed->len > 0)
        g_print ("Removed:\n");
      for (i = 0; i < removed->len; i++)
        {
          RpmOstreePackage *pkg = removed->pdata[i];
          const char *nevra = rpm_ostree_package_get_nevra (pkg);

          g_print ("  %s\n", nevra);
        }

      if (added->len > 0)
        g_print ("Added:\n");
      for (i = 0; i < added->len; i++)
        {
          RpmOstreePackage *pkg = added->pdata[i];
          const char *nevra = rpm_ostree_package_get_nevra (pkg);

          g_print ("  %s\n", nevra);
        }
    }

  ret = TRUE;
 out:
  return ret;
}

static void
get_values_from_diff_variant (GVariant *v,
                              gchar **out_name,
                              guint *out_type,
                              gchar **out_prev_evra,
                              gchar **out_cur_evra)
{
  gs_unref_variant GVariant *options = NULL;

  gs_free gchar *prev_evra = NULL;
  gs_free gchar *cur_evra = NULL;
  gs_free gchar *prev_evr = NULL;
  gs_free gchar *cur_evr = NULL;
  gs_free gchar *prev_arch = NULL;
  gs_free gchar *cur_arch = NULL;

  g_variant_get_child (v, 0, "s", out_name),
  g_variant_get_child (v, 1, "u", out_type);
  options = g_variant_get_child_value (v, 2);

  g_variant_lookup (options, "PreviousPackage",
                    "(sss)", NULL, &prev_evr, &prev_arch);
  g_variant_lookup (options, "NewPackage",
                    "(sss)", NULL, &cur_evr, &cur_arch);

  if (prev_evr && prev_arch)
    prev_evra = g_strdup_printf ("%s.%s", prev_evr, prev_arch);

  if (cur_evr && cur_arch)
    cur_evra = g_strdup_printf ("%s.%s", cur_evr, cur_arch);

  gs_transfer_out_value (out_prev_evra, &prev_evra);
  gs_transfer_out_value (out_cur_evra, &cur_evra);
}

void
rpmostree_print_pkg_diff_variant_by_type (GVariant *variant)
{
  // This is the default for now so no need to sort;

  gint prev_type = -1;
  guint i;
  guint n = g_variant_n_children (variant);

  for (i = 0; i < n; i++)
    {
      gs_free gchar *prev_evra = NULL;
      gs_free gchar *cur_evra = NULL;
      gs_free gchar *name = NULL;
      gs_unref_variant GVariant *v = NULL;
      guint type;

      v = g_variant_get_child_value (variant, i);
      get_values_from_diff_variant (v, &name, &type,
                                    &prev_evra, &cur_evra);

      if (prev_type != type)
        {
          switch (type)
            {
              case RPM_OSTREE_PACKAGE_ADDED:
                g_print ("Added:\n");
                break;
              case RPM_OSTREE_PACKAGE_REMOVED:
                g_print ("Removed:\n");
                break;
              case RPM_OSTREE_PACKAGE_DOWNGRADED:
                g_print ("Downgraded:\n");
                break;
              default:
                g_print ("Upgraded:\n");
            }
        }

      prev_type = type;

      if (prev_evra && cur_evra)
          g_print ("  %s %s -> %s\n", name, prev_evra, cur_evra);
      else if (cur_evra)
          g_print ("  %s-%s\n", name, cur_evra);
      else
          g_print ("  %s-%s\n", name, prev_evra);
    }
}

void
rpmostree_print_pkg_diff_variant_by_name (GVariant *variant)
{
  guint i;
  guint n = g_variant_n_children (variant);
  g_autoptr(GPtrArray) l = NULL;

  // resort variant array by name
  l = g_ptr_array_new_with_free_func((GDestroyNotify) g_variant_unref);
  for (i = 0; i < n; i++)
    {
      GVariant *v = g_variant_get_child_value (variant, i);
      g_ptr_array_add (l, v);
    }
  g_ptr_array_sort (l, rpm_ostree_db_diff_variant_compare_by_name);

  for (i = 0; i < n; i++)
    {
      gs_free gchar *prev_evra = NULL;
      gs_free gchar *cur_evra = NULL;
      gs_free gchar *name = NULL;
      guint type;
      GVariant *v = l->pdata[i];

      get_values_from_diff_variant (v, &name, &type,
                                    &prev_evra, &cur_evra);

      if (prev_evra && cur_evra)
          g_print ("!%s-%s\n=%s-%s\n", name, prev_evra, name, cur_evra);
      else if (cur_evra)
          g_print ("+%s-%s\n", name, cur_evra);
      else
          g_print ("-%s-%s\n", name, prev_evra);
    }
}
