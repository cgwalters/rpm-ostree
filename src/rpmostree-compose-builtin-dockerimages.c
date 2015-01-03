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
static char *opt_cachedir;
static gboolean opt_force_nocache;
static char *opt_proxy;
static char *opt_output_repodata_dir;
static char **opt_metadata_strings;
static char *opt_repo;
static char **opt_override_pkg_repos;
static gboolean opt_print_only;

static GOptionEntry option_entries[] = {
  { "workdir", 0, 0, G_OPTION_ARG_STRING, &opt_workdir, "Working directory", "WORKDIR" },
  { "workdir-tmpfs", 0, 0, G_OPTION_ARG_NONE, &opt_workdir_tmpfs, "Use tmpfs for working state", NULL },
  { "force-nocache", 0, 0, G_OPTION_ARG_NONE, &opt_force_nocache, "Always create a new OSTree commit, even if nothing appears to have changed", NULL },
  { "proxy", 0, 0, G_OPTION_ARG_STRING, &opt_proxy, "HTTP proxy", "PROXY" },
  { NULL }
};

typedef struct {
  GPtrArray *treefile_context_dirs;
  
  GFile *workdir;
  OstreeRepo *repo;
  char *previous_checksum;

  GBytes *serialized_treefile;
} RpmOstreeDockerComposeContext;

static int
ptrarray_sort_compare_strings (gconstpointer ap,
                               gconstpointer bp)
{
  char **asp = (gpointer)ap;
  char **bsp = (gpointer)bp;
  return strcmp (*asp, *bsp);
}

static gboolean
compute_checksum_from_treefile_and_goal (RpmOstreeTreeComposeContext   *self,
                                         HyGoal                         goal,
                                         char                        **out_checksum,
                                         GError                      **error)
{
  gboolean ret = FALSE;
  gs_free char *ret_checksum = NULL;
  GChecksum *checksum = g_checksum_new (G_CHECKSUM_SHA256);
  
  /* Hash in the raw treefile; this means reordering the input packages
   * or adding a comment will cause a recompose, but let's be conservative
   * here.
   */
  { gsize len;
    const guint8* buf = g_bytes_get_data (self->serialized_treefile, &len);

    g_checksum_update (checksum, buf, len);
  }

  /* FIXME; we should also hash the post script */

  /* Hash in each package */
  { _cleanup_hypackagelist_ HyPackageList pkglist = NULL;
    HyPackage pkg;
    guint i;
    gs_unref_ptrarray GPtrArray *nevras = g_ptr_array_new_with_free_func (g_free);

    pkglist = hy_goal_list_installs (goal);

    FOR_PACKAGELIST(pkg, pkglist, i)
      {
        g_ptr_array_add (nevras, hy_package_get_nevra (pkg));
      }

    g_ptr_array_sort (nevras, ptrarray_sort_compare_strings);
    
    for (i = 0; i < nevras->len; i++)
      {
        const char *nevra = nevras->pdata[i];
        g_checksum_update (checksum, (guint8*)nevra, strlen (nevra));
      }
  }

  ret_checksum = g_strdup (g_checksum_get_string (checksum));

  ret = TRUE;
  gs_transfer_out_value (out_checksum, &ret_checksum);
  if (checksum) g_checksum_free (checksum);
  return ret;
}


static void
on_hifstate_percentage_changed (HifState   *hifstate,
                                guint       percentage,
                                gpointer    user_data)
{
  const char *text = user_data;
  rpmostree_console_progress_text_percent (text, percentage);
}

static gboolean
install_packages_in_root (RpmOstreeTreeComposeContext  *self,
                          JsonObject      *treedata,
                          GFile           *yumroot,
                          char           **packages,
                          gboolean        *out_unmodified,
                          char           **out_new_inputhash,
                          GCancellable    *cancellable,
                          GError         **error)
{
  gboolean ret = FALSE;
  guint progress_sigid;
  char **strviter;
  GFile *contextdir = self->treefile_context_dirs->pdata[0];
  gs_unref_object HifContext *hifctx = NULL;
  gs_free char *cachedir = g_build_filename (gs_file_get_path_cached (self->workdir),
                                             "cache",
                                             NULL);
  gs_free char *solvdir = g_build_filename (gs_file_get_path_cached (self->workdir),
                                            "solv",
                                            NULL);
  gs_free char *lockdir = g_build_filename (gs_file_get_path_cached (self->workdir),
                                            "lock",
                                            NULL);
  gs_free char *ret_new_inputhash = NULL;

  /* Apparently there's only one process-global macro context;
   * realistically, we're going to have to refactor all of the RPM
   * stuff to a subprocess.
   */
  hifctx = hif_context_new ();

  hif_context_set_install_root (hifctx, gs_file_get_path_cached (yumroot));

  hif_context_set_cache_dir (hifctx, cachedir);
  hif_context_set_solv_dir (hifctx, solvdir);
  hif_context_set_lock_dir (hifctx, lockdir);
  hif_context_set_check_disk_space (hifctx, FALSE);
  hif_context_set_check_transaction (hifctx, FALSE);

  hif_context_set_repo_dir (hifctx, gs_file_get_path_cached (contextdir));

  { JsonNode *install_langs_n =
      json_object_get_member (treedata, "install-langs");

    if (install_langs_n != NULL)
      {
        JsonArray *instlangs_a = json_node_get_array (install_langs_n);
        guint len = json_array_get_length (instlangs_a);
        guint i;
        GString *opt = g_string_new ("");

        for (i = 0; i < len; i++)
          {
            g_string_append (opt, json_array_get_string_element (instlangs_a, i));
            if (i < len - 1)
              g_string_append_c (opt, ':');
          }

        hif_context_set_rpm_macro (hifctx, "_install_langs", opt->str);
        g_string_free (opt, TRUE);
      }
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

    if (!json_object_has_member (treedata, "repos"))
      {
        g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                             "Treefile is missing required \"repos\" member");
        goto out;
      }

    enable_repos = json_object_get_array_member (treedata, "repos");
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
                     gs_file_get_path_cached (contextdir),
                     notfound_repos_str->str);
        g_string_free (notfound_repos_str, TRUE);
        goto out;
      }
  }

  /* --- Downloading metadata --- */
  { _cleanup_rpmostree_console_progress_ G_GNUC_UNUSED gpointer dummy;
    gs_unref_object HifState *hifstate = hif_state_new ();

    progress_sigid = g_signal_connect (hifstate, "percentage-changed",
                                     G_CALLBACK (on_hifstate_percentage_changed), 
                                     "Downloading metadata:");

    rpmostree_console_progress_start ();

    if (!hif_context_setup_sack (hifctx, hifstate, error))
      goto out;

    g_signal_handler_disconnect (hifstate, progress_sigid);
  }

  for (strviter = packages; strviter && *strviter; strviter++)
    {
      if (!hif_context_install (hifctx, *strviter, error))
        goto out;
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

  if (!compute_checksum_from_treefile_and_goal (self, hif_context_get_goal (hifctx),
                                                &ret_new_inputhash, error))
    goto out;

  if (self->previous_checksum)
    {
      gs_unref_variant GVariant *commit_v = NULL;
      gs_unref_variant GVariant *commit_metadata = NULL;
      const char *previous_inputhash = NULL;
      
      if (!ostree_repo_load_variant (self->repo, OSTREE_OBJECT_TYPE_COMMIT,
                                     self->previous_checksum,
                                     &commit_v, error))
        goto out;

      commit_metadata = g_variant_get_child_value (commit_v, 0);
      if (g_variant_lookup (commit_metadata, "rpmostree.inputhash", "&s", &previous_inputhash))
        {
          if (strcmp (previous_inputhash, ret_new_inputhash) == 0)
            {
              *out_unmodified = TRUE;
              ret = TRUE;
              goto out;
            }
        }
      else
        g_print ("Previous commit found, but without rpmostree.inputhash metadata key\n");
    }

  /* --- Downloading packages --- */
  { _cleanup_rpmostree_console_progress_ G_GNUC_UNUSED gpointer dummy;
    gs_unref_object HifState *hifstate = hif_state_new ();

    progress_sigid = g_signal_connect (hifstate, "percentage-changed",
                                     G_CALLBACK (on_hifstate_percentage_changed), 
                                     "Downloading packages:");

    rpmostree_console_progress_start ();

    if (!hif_transaction_download (hif_context_get_transaction (hifctx), hifstate, error))
      goto out;

    g_signal_handler_disconnect (hifstate, progress_sigid);
  }

  { gs_unref_object PkgWorkerInstall *pkgworker_install = NULL;
    gs_free char *msg = NULL;
    
    if (!pkg_worker_install_impl_spawn (&pkgworker_install, cancellable, error))
      goto out;

    if (!pkg_worker_install_call_hello_install_sync (pkgworker_install, &msg,
                                                     cancellable, error))
      goto out;
    
    g_assert_cmpstr (msg, ==, "hi");
  }

  { _cleanup_rpmostree_console_progress_ G_GNUC_UNUSED gpointer dummy;
    gs_unref_object HifState *hifstate = hif_state_new ();

    progress_sigid = g_signal_connect (hifstate, "percentage-changed",
                                     G_CALLBACK (on_hifstate_percentage_changed), 
                                     "Installing packages:");

    rpmostree_console_progress_start ();

    if (!hif_transaction_commit (hif_context_get_transaction (hifctx),
                                 hif_context_get_goal (hifctx),
                                 hifstate,
                                 error))
      goto out;

    g_signal_handler_disconnect (hifstate, progress_sigid);
  }
      
  ret = TRUE;
  *out_unmodified = FALSE;
  gs_transfer_out_value (out_new_inputhash, &ret_new_inputhash);
 out:
  return ret;
}

gboolean
rpmostree_compose_builtin_dockerimages (int             argc,
                                        char          **argv,
                                        GCancellable   *cancellable,
                                        GError        **error)
{
  gboolean ret = FALSE;
  GError *temp_error = NULL;
  GOptionContext *context = g_option_context_new ("- Assemble docker images from RPMs");
  const char *ref;
  RpmOstreeDockerImageComposeContext selfdata = { NULL, };
  RpmOstreeDockerImageComposeContext *self = &selfdata;
  JsonNode *imgdef_rootval = NULL;
  JsonObject *imgdef = NULL;
  gs_unref_object GFile *cachedir = NULL;
  gs_unref_object GFile *target_root = NULL;
  gs_unref_ptrarray GPtrArray *packages = NULL;
  gs_unref_object GFile *imgdef_path = NULL;
  gs_unref_object JsonParser *imgdef_parser = NULL;
  gboolean workdir_is_tmp = FALSE;

  if (!rpmostree_option_context_parse (context, option_entries, &argc, &argv, error))
    goto out;

  if (argc < 2)
    {
      g_printerr ("usage: rpm-ostree compose dockerimages IMAGESDEF.json\n");
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Option processing failed");
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

  ref = _rpmostree_jsonutil_object_require_string_member (imgdef, "ref", error);
  if (!ref)
    goto out;

  if (!ostree_repo_read_commit (repo, ref, &previous_root, &previous_checksum,
                                cancellable, &temp_error))
    {
      if (g_error_matches (temp_error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND))
        { 
          g_clear_error (&temp_error);
          g_print ("No previous commit for %s\n", ref);
        }
      else
        {
          g_propagate_error (error, temp_error);
          goto out;
        }
    }
  else
    g_print ("Previous commit: %s\n", previous_checksum);

  self->previous_checksum = previous_checksum;

  yumroot = g_file_get_child (self->workdir, "rootfs.tmp");
  if (!gs_shutil_rm_rf (yumroot, cancellable, error))
    goto out;
  targetroot = g_file_get_child (self->workdir, "rootfs");

  bootstrap_packages = g_ptr_array_new ();
  packages = g_ptr_array_new ();

  if (json_object_has_member (imgdef, "bootstrap_packages"))
    {
      if (!_rpmostree_jsonutil_append_string_array_to (imgdef, "bootstrap_packages", packages, error))
        goto out;
    }
  if (!_rpmostree_jsonutil_append_string_array_to (imgdef, "packages", packages, error))
    goto out;
  g_ptr_array_add (packages, NULL);

  {
    gs_unref_object JsonGenerator *generator = json_generator_new ();
    char *imgdef_buf = NULL;
    gsize len;

    json_generator_set_root (generator, imgdef_rootval);
    json_generator_set_pretty (generator, TRUE);
    imgdef_buf = json_generator_to_data (generator, &len);

    self->serialized_imgdef = g_bytes_new_take (imgdef_buf, len);
  }

  if (previous_root != NULL)
    {
      gboolean generate_from_previous = TRUE;

      if (!_rpmostree_jsonutil_object_get_optional_boolean_member (imgdef,
                                                                   "preserve-passwd",
                                                                   &generate_from_previous,
                                                                   error))
        goto out;

      if (generate_from_previous)
        {
          if (!rpmostree_generate_passwd_from_previous (repo, yumroot, previous_root,
                                                        cancellable, error))
            goto out;
        }
    }

  { gboolean unmodified = FALSE;

    if (!install_packages_in_root (self, imgdef, yumroot,
                                   (char**)packages->pdata,
                                   &unmodified,
                                   &new_inputhash,
                                   cancellable, error))
      goto out;

    if (unmodified)
      {
        g_print ("No apparent changes since previous commit; use --force-nocache to override\n");
        ret = TRUE;
        goto out;
      }
  }

  if (g_strcmp0 (g_getenv ("RPM_OSTREE_BREAK"), "post-yum") == 0)
    goto out;

  if (!rpmostree_imgdef_postprocessing (yumroot, self->imgdef_context_dirs->pdata[0],
                                          self->serialized_imgdef, imgdef,
                                          cancellable, error))
    goto out;

  if (!rpmostree_prepare_rootfs_for_commit (yumroot, imgdef, cancellable, error))
    goto out;

  if (!rpmostree_check_passwd (repo, yumroot, imgdef_path, imgdef,
                               cancellable, error))
    goto out;

  if (!rpmostree_check_groups (repo, yumroot, imgdef_path, imgdef,
                               cancellable, error))
    goto out;

  {
    const char *gpgkey;
    gs_unref_variant GVariant *metadata = NULL;

    g_variant_builder_add (metadata_builder, "{sv}",
                           "rpmostree.inputhash",
                           g_variant_new_string (new_inputhash));

    metadata = g_variant_ref_sink (g_variant_builder_end (metadata_builder));

    if (!_rpmostree_jsonutil_object_get_optional_string_member (imgdef, "gpg_key", &gpgkey, error))
      goto out;

    if (!rpmostree_commit (yumroot, repo, ref, metadata, gpgkey,
                           json_object_get_boolean_member (imgdef, "selinux"),
                           cancellable, error))
      goto out;
  }

  g_print ("Complete\n");
  
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
      g_clear_pointer (&self->serialized_imgdef, g_bytes_unref);
    }
  return ret;
}
