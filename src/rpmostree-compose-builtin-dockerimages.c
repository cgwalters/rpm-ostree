/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*-
 *
 * Copyright (C) 2013,2014 Colin Walters <walters@verbum.org>
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
#include <json-glib/json-glib.h>
#include <gio/gunixoutputstream.h>
#include <libhif.h>
#include <libhif/hif-context-private.h>
#include <stdio.h>
#include <rpm/rpmmacro.h>

#include "rpmostree-compose-builtins.h"
#include "rpmostree-util.h"
#include "rpmostree-json-parsing.h"
#include "rpmostree-cleanup.h"
#include "pkgworker-install-impl.h"
#include "pkgworker-generated.h"
#include "rpmostree-treepkgdiff.h"
#include "rpmostree-libcontainer.h"
#include "rpmostree-console-progress.h"
#include "rpmostree-postprocess.h"
#include "rpmostree-passwd-util.h"

#include "libgsystem.h"

static char *opt_workdir;
static gboolean opt_workdir_tmpfs;
static gboolean opt_force_nocache;
static char *opt_proxy;
static char *opt_statefile;

static GOptionEntry option_entries[] = {
  { "workdir", 0, 0, G_OPTION_ARG_STRING, &opt_workdir, "Working directory", "WORKDIR" },
  { "workdir-tmpfs", 0, 0, G_OPTION_ARG_NONE, &opt_workdir_tmpfs, "Use tmpfs for working state", NULL },
  { "force-nocache", 0, 0, G_OPTION_ARG_NONE, &opt_force_nocache, "Always create a new OSTree commit, even if nothing appears to have changed", NULL },
  { "proxy", 0, 0, G_OPTION_ARG_STRING, &opt_proxy, "HTTP proxy", "PROXY" },
  { "statefile", 0, 0, G_OPTION_ARG_STRING, &opt_statefile, "Output computed state to PATH", "PATH" },
  { NULL }
};

typedef struct {
  GFile *workdir;
  GFile *contextdir;
} App;

typedef struct {
  char *imagename;
  HifContext *context;
} DockerImageComposeContext;

static void
on_hifstate_percentage_changed (HifState   *hifstate,
                                guint       percentage,
                                gpointer    user_data)
{
  const char *text = user_data;
  rpmostree_console_progress_text_percent (text, percentage);
}

static HifContext *
setup_context (App    *self,
               JsonObject                       *imagedef,
               GFile                            *installroot,
               GCancellable                     *cancellable,
               GError                          **error)
{
  gs_unref_object HifContext *hifctx = NULL;
  HifContext *hifctx_ret = NULL;

  hifctx = hif_context_new ();
  hif_context_set_install_root (hifctx, gs_file_get_path_cached (installroot));
  hif_context_set_repo_dir (hifctx, gs_file_get_path_cached (self->contextdir));
  hif_context_set_check_disk_space (hifctx, FALSE);
  hif_context_set_check_transaction (hifctx, FALSE);

  { gs_free char *cachedir = g_build_filename (gs_file_get_path_cached (self->workdir),
                                               "cache",
                                               NULL);
    gs_free char *solvdir = g_build_filename (gs_file_get_path_cached (self->workdir),
                                              "solv",
                                              NULL);
    gs_free char *lockdir = g_build_filename (gs_file_get_path_cached (self->workdir),
                                              "lock",
                                              NULL);
    hif_context_set_cache_dir (hifctx, cachedir);
    hif_context_set_solv_dir (hifctx, solvdir);
    hif_context_set_lock_dir (hifctx, lockdir);
  }

  if (!hif_context_setup (hifctx, cancellable, error))
    goto out;

  /* Forcibly override rpm/librepo SIGINT handlers.  We always operate
   * in a fully idempotent/atomic mode, and can be killed at any time.
   */
  signal (SIGINT, SIG_DFL);
  signal (SIGTERM, SIG_DFL);

  /* Bind the json \"repos\" member to the hif state, which looks at the
   * enabled= member of the repos file.  By default we forcibly enable
   * only repos which are specified, ignoring the enabled= flag.
   */
  {
    GPtrArray *sources;
    JsonArray *enable_repos = NULL;
    gs_unref_hashtable GHashTable *enabled_repo_names =
      g_hash_table_new (g_str_hash, g_str_equal);
    gs_unref_hashtable GHashTable *found_enabled_repo_names =
      g_hash_table_new (g_str_hash, g_str_equal);
    guint i;
    guint n;

    sources = hif_context_get_sources (hifctx);

    if (!json_object_has_member (imagedef, "repos"))
      {
        g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                             "Treefile is missing required \"repos\" member");
        goto out;
      }

    enable_repos = json_object_get_array_member (imagedef, "repos");
    n = json_array_get_length (enable_repos);

    for (i = 0; i < n; i++)
      {
        const char *reponame = _rpmostree_jsonutil_array_require_string_element (enable_repos, i, error);
        if (!reponame)
          goto out;
        g_hash_table_add (enabled_repo_names, (char*)reponame);
      }

    for (i = 0; i < sources->len; i++)
      {
        HifSource *src = g_ptr_array_index (sources, i);
        const char *id = hif_source_get_id (src);

        if (!g_hash_table_remove (enabled_repo_names, id))
          hif_source_set_enabled (src, HIF_SOURCE_ENABLED_NONE);
        else
          hif_source_set_enabled (src, HIF_SOURCE_ENABLED_PACKAGES);
      }

    if (g_hash_table_size (enabled_repo_names) > 0)
      {
        /* We didn't find some repos */
        GString *notfound_repos_str = g_string_new ("");
        gboolean prev = FALSE;
        GHashTableIter hashiter;
        gpointer hashkey, hashvalue;

        g_hash_table_iter_init (&hashiter, enabled_repo_names);
        while (g_hash_table_iter_next (&hashiter, &hashkey, &hashvalue))
          {
            if (!prev)
              prev = TRUE;
            else
              g_string_append_c (notfound_repos_str, ' ');
            g_string_append (notfound_repos_str, (char*)hashkey);
          }

        g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                     "Repositories specified not found in context directory %s: %s",
                     gs_file_get_path_cached (self->contextdir),
                     notfound_repos_str->str);
        g_string_free (notfound_repos_str, TRUE);
        goto out;
      }
  }

  /* --- Downloading metadata --- */
  { _cleanup_rpmostree_console_progress_ G_GNUC_UNUSED gpointer dummy;
    gs_unref_object HifState *hifstate = hif_state_new ();
    guint progress_sigid;

    progress_sigid = g_signal_connect (hifstate, "percentage-changed",
                                     G_CALLBACK (on_hifstate_percentage_changed), 
                                     "Downloading metadata:");

    rpmostree_console_progress_start ();

    if (!hif_context_setup_sack (hifctx, hifstate, error))
      goto out;

    g_signal_handler_disconnect (hifstate, progress_sigid);
  }

  hifctx_ret = hifctx;
  hifctx = NULL;
 out:
  return hifctx_ret;
}

static int
ptrarray_sort_compare_strings (gconstpointer ap,
                               gconstpointer bp)
{
  char **asp = (gpointer)ap;
  char **bsp = (gpointer)bp;
  return strcmp (*asp, *bsp);
}

static char *
compute_hashstate_for_image (JsonObject      *imgdef,
                             HyPackageList    pkglist)
{
  GChecksum *state = g_checksum_new (G_CHECKSUM_SHA256);
  gs_unref_object JsonGenerator *generator = json_generator_new ();
  char *object_buf = NULL;
  gsize len;
  guint i;
  HyPackage pkg;
  gs_unref_ptrarray GPtrArray *sorted_pkgs =
    g_ptr_array_new_with_free_func (g_free);

  { JsonNode *rootnode = json_node_new (JSON_NODE_OBJECT);
    json_node_set_object (rootnode, imgdef);
    json_generator_set_root (generator, rootnode);
    json_node_free (rootnode);
  }
  json_generator_set_pretty (generator, TRUE);
  object_buf = json_generator_to_data (generator, &len);

  g_checksum_update (state, (guint8*)object_buf, len);

  FOR_PACKAGELIST(pkg, pkglist, i)
    {
      g_ptr_array_add (sorted_pkgs, hy_package_get_nevra (pkg));
    }
  g_ptr_array_sort (sorted_pkgs, ptrarray_sort_compare_strings);

  for (i = 0; i < sorted_pkgs->len; i++)
    {
      const char *nevra = sorted_pkgs->pdata[i];
      g_checksum_update (state, (guint8*)nevra, strlen (nevra));
    }

  { char *ret = g_strdup (g_checksum_get_string (state));
    g_checksum_free (state);
    return ret;
  }
}
                             

gboolean
rpmostree_compose_builtin_dockerimages (int             argc,
                                        char          **argv,
                                        GCancellable   *cancellable,
                                        GError        **error)
{
  gboolean ret = FALSE;
  GOptionContext *context = g_option_context_new ("- Assemble docker images from RPMs");
  App selfdata = { NULL, };
  App *self = &selfdata;
  JsonNode *imgdef_rootval = NULL;
  JsonNode *output_rootval = NULL;
  JsonObject *output_root = NULL;
  JsonObject *imgdef = NULL;
  JsonObject *images = NULL;
  gs_unref_object GFile *cachedir = NULL;
  gs_unref_object GFile *target_root = NULL;
  gs_unref_object GFile *imgdef_path = NULL;
  gs_unref_object JsonParser *imgdef_parser = NULL;
  gs_unref_ptrarray GHashTable *common_base = NULL;
  gs_unref_ptrarray GHashTable *image_packages =
    g_hash_table_new_full (g_str_hash, g_str_equal, g_free, (GDestroyNotify)g_hash_table_unref);
  gs_unref_ptrarray GHashTable *image_hashes =
    g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_free);
  gboolean workdir_is_tmp = FALSE;
  guint progress_sigid;
  gs_unref_object HifContext *hifctx = NULL;

  if (!rpmostree_option_context_parse (context, option_entries, &argc, &argv, error))
    goto out;

  if (argc < 2)
    {
      g_printerr ("usage: rpm-ostree compose dockerimages IMAGESDEF.json\n");
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Option processing failed");
      goto out;
    }

  if (!opt_statefile)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "--statefile is required");
      goto out;
    }

  imgdef_path = g_file_new_for_path (argv[1]);

  if (opt_workdir)
    {
      self->workdir = g_file_new_for_path (opt_workdir);
    }
  else
    {
      gs_free char *tmpd = g_mkdtemp (g_strdup ("/var/tmp/rpm-ostree.XXXXXX"));
      self->workdir = g_file_new_for_path (tmpd);
      workdir_is_tmp = TRUE;

      if (opt_workdir_tmpfs)
        {
          if (mount ("tmpfs", tmpd, "tmpfs", 0, (const void*)"mode=755") != 0)
            {
              _rpmostree_set_prefix_error_from_errno (error, errno,
                                                      "mount(tmpfs): ");
              goto out;
            }
        }
    }

  self->contextdir = g_file_get_parent (imgdef_path);

  imgdef_parser = json_parser_new ();
  if (!json_parser_load_from_file (imgdef_parser,
                                   gs_file_get_path_cached (imgdef_path),
                                   error))
    goto out;

  imgdef_rootval = json_parser_get_root (imgdef_parser);
  if (!JSON_NODE_HOLDS_OBJECT (imgdef_rootval))
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "IMGDEF root is not an object");
      goto out;
    }
  imgdef = json_node_get_object (imgdef_rootval);

  images = json_object_get_object_member (imgdef, "images");

  { gs_free_list GList *members =
      json_object_get_members (images);
    GList *iter;

    for (iter = members; iter; iter = iter->next)
      {
        const char *imageid = iter->data;
        JsonObject *imagetarget = json_object_get_object_member (images, imageid);
        gs_unref_object HifContext *hifctx = NULL;
        gs_unref_object GFile *targetroot = g_file_get_child (self->workdir, "rootfs");
        gs_unref_ptrarray GPtrArray *packages = NULL;
        gs_free char *imghash = NULL;

        if (!gs_shutil_rm_rf (targetroot, cancellable, error))
          goto out;

        hifctx = setup_context (self, imgdef, targetroot,
                                cancellable, error);
        if (!hifctx)
          goto out;

        packages = g_ptr_array_new ();
        if (!_rpmostree_jsonutil_append_string_array_to (imagetarget, "packages", packages, error))
          goto out;

        { guint i;
          for (i = 0; i < packages->len; i++)
            {
              if (!hif_context_install (hifctx, packages->pdata[i], error))
                goto out;
            }
        }

        /* --- Resolving dependencies --- */
        { _cleanup_rpmostree_console_progress_ G_GNUC_UNUSED gpointer dummy;
          gs_unref_object HifState *hifstate = hif_state_new ();

          progress_sigid = g_signal_connect (hifstate, "percentage-changed",
                                             G_CALLBACK (on_hifstate_percentage_changed), 
                                             "Resolving dependencies:");

          rpmostree_console_progress_start ();

          if (!hif_transaction_depsolve (hif_context_get_transaction (hifctx),
                                         hif_context_get_goal (hifctx),
                                         hifstate, error))
            goto out;

          g_signal_handler_disconnect (hifstate, progress_sigid);
        }

        { _cleanup_hypackagelist_ HyPackageList pkglist = NULL;
          HyPackage pkg;
          guint i;
          gs_unref_ptrarray GPtrArray *nevras = g_ptr_array_new_with_free_func (g_free);
          gboolean is_first = FALSE;

          pkglist = hy_goal_list_installs (hif_context_get_goal (hifctx));

          if (!common_base)
            {
              is_first = TRUE;
              common_base = g_hash_table_new_full (g_str_hash, g_str_equal, NULL, g_free);
              FOR_PACKAGELIST(pkg, pkglist, i)
                {
                  char *nevra = hy_package_get_nevra (pkg);
                  g_hash_table_add (common_base, nevra);
                }
            }

          g_hash_table_insert (image_hashes, g_strdup (imageid),
                               compute_hashstate_for_image (imgdef, pkglist));

          { GHashTableIter hashiter;
            gpointer hashkey, hashvalue;
            gs_unref_hashtable GHashTable *pkgset =
              g_hash_table_new_full (g_str_hash, g_str_equal, NULL, g_free);

            FOR_PACKAGELIST(pkg, pkglist, i)
              {
                char *nevra = hy_package_get_nevra (pkg);
                g_hash_table_add (pkgset, nevra);
              }

            g_hash_table_insert (image_packages, g_strdup (imageid), g_hash_table_ref (pkgset));

            if (!is_first)
              {
                g_hash_table_iter_init (&hashiter, common_base);
                while (g_hash_table_iter_next (&hashiter, &hashkey, &hashvalue))
                  {
                    const char *pkg = hashkey;
                    if (!g_hash_table_lookup (pkgset, pkg))
                      {
                        g_print ("-common: %s\n", pkg);
                        g_hash_table_iter_remove (&hashiter);
                      }
                  }
              }
          }
        }
      }
  }

  output_rootval = json_node_new (JSON_NODE_OBJECT);
  output_root = json_object_new ();
  json_node_set_object (output_rootval, output_root);

  { GHashTableIter hashiter;
    gpointer hashkey, hashvalue;
    JsonObject *common = json_object_new ();
    JsonArray *common_packages = json_array_new ();
    JsonObject *images_out = json_object_new ();

    json_object_set_object_member (output_root, "common", common);
    json_object_set_object_member (output_root, "images", images_out);

    g_hash_table_iter_init (&hashiter, common_base);
    while (g_hash_table_iter_next (&hashiter, &hashkey, &hashvalue))
      {
        const char *pkg = hashkey;
        json_array_add_string_element (common_packages, pkg);
      }

    json_object_set_array_member (common, "packages", common_packages);

    g_hash_table_iter_init (&hashiter, image_packages);
    while (g_hash_table_iter_next (&hashiter, &hashkey, &hashvalue))
      {
        const char *imageid = hashkey;
        GHashTable *image_packages = hashvalue;
        GHashTableIter subhashiter;
        gpointer subhashkey, subhashvalue;
        const char *hashstate;
        JsonObject *imgout = json_object_new ();
        JsonArray *imgout_packages = json_array_new ();

        json_object_set_object_member (images_out, imageid, imgout);

        hashstate = g_hash_table_lookup (image_hashes, imageid);
        g_assert (hashstate);
        json_object_set_string_member (imgout, "hashstate", hashstate);

        json_object_set_array_member (imgout, "packages", imgout_packages);

        g_hash_table_iter_init (&subhashiter, image_packages);
        while (g_hash_table_iter_next (&subhashiter, &subhashkey, &subhashvalue))
          {
            const char *pkg = subhashkey;
            if (!g_hash_table_lookup (common_base, pkg))
              {
                json_array_add_string_element (imgout_packages, pkg);
              }
          }
      }
  }

  { gs_unref_object JsonGenerator *generator = json_generator_new ();
    gs_free char *outbuf = NULL;
    gsize len;

    json_generator_set_root (generator, output_rootval);
    json_generator_set_pretty (generator, TRUE);
    outbuf = json_generator_to_data (generator, &len);
    g_print ("%s\n", outbuf);
  }
  
 out:

  if (workdir_is_tmp)
    {
      if (opt_workdir_tmpfs)
        (void) umount (gs_file_get_path_cached (self->workdir));
      (void) gs_shutil_rm_rf (self->workdir, NULL, NULL);
    }
  if (self)
    {
      g_clear_object (&self->workdir);
    }
  return ret;
}
