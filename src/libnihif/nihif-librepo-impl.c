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

#include "nihif-librepo-impl.h"

#include <string.h>
#include <glib-unix.h>
#include <sys/prctl.h>
#include <signal.h>
#include <grp.h>
#include <sys/types.h>
#include <unistd.h>
#include <sys/socket.h>

#include <librepo/librepo.h>

#include "nihif-generated.h"
#include "rpmostree-util.h"
#include "libgsystem.h"

struct _NihifLibRepoImpl
{
  NihifLibRepoWorkerSkeleton parent_instance;

  GMutex lock;
  
  GThread *op_thread;
};

typedef struct _NihifLibRepoImplClass
{
  NihifLibRepoWorkerSkeletonClass parent_class;
} NihifLibRepoImplClass;

static void nihif_librepo_worker_iface_init (NihifLibRepoWorkerIface *iface);

G_DEFINE_TYPE_WITH_CODE (NihifLibRepoImpl, nihif_librepo_impl,
                         NIHIF_TYPE_LIB_REPO_WORKER_SKELETON,
                         G_IMPLEMENT_INTERFACE (NIHIF_TYPE_LIB_REPO_WORKER, nihif_librepo_worker_iface_init));

static void
nihif_librepo_impl_init (NihifLibRepoImpl *self)
{
  g_mutex_init (&self->lock);
}

static void
nihif_librepo_impl_finalize (GObject *object)
{
  NihifLibRepoImpl *self = (NihifLibRepoImpl *)object;
  g_mutex_clear (&self->lock);
  G_OBJECT_CLASS (nihif_librepo_impl_parent_class)->finalize (object);
}

static void
nihif_librepo_impl_constructed (GObject *object)
{
  if (G_OBJECT_CLASS (nihif_librepo_impl_parent_class)->constructed != NULL)
    G_OBJECT_CLASS (nihif_librepo_impl_parent_class)->constructed (object);
}

static void
nihif_librepo_impl_class_init (NihifLibRepoImplClass *klass)
{
  GObjectClass *gobject_class;

  gobject_class = G_OBJECT_CLASS (klass);
  gobject_class->finalize = nihif_librepo_impl_finalize;
  gobject_class->constructed = nihif_librepo_impl_constructed;
}

static gboolean
handle_fetch_md (NihifLibRepoWorker *worker,
                 GDBusMethodInvocation *invocation,
                 const char *outputdir,
                 guint urltype_u,
                 const char *url,
                 GVariant *urlvars,
                 const char * const *downloadlist);

static void
nihif_librepo_worker_iface_init (NihifLibRepoWorkerIface *iface)
{
  iface->handle_fetch_md = handle_fetch_md;
}

typedef struct {
  NihifLibRepoImpl *self;
  char *outputdir;
  NihifLibRepoImplUrlType urltype;
  char *url;
  GVariant *urlvars;
  char **downloadlist;
  LrResult *lr_result;
  GError *error;

  GMutex progress_lock;
  GSource *progress_source;
  gdouble now_downloaded;
  gdouble total_to_download;
} FetchMdThreadData;

static gboolean
idle_emit_progress (gpointer user_data)
{
  FetchMdThreadData *data = user_data;
  gdouble now_downloaded;
  gdouble total_to_download;

  g_mutex_lock (&data->progress_lock);
  now_downloaded = data->now_downloaded;
  total_to_download = data->total_to_download;
  data->progress_source = NULL;
  g_mutex_unlock (&data->progress_lock);

  nihif_lib_repo_worker_emit_fetch_md_progress ((NihifLibRepoWorker*)data->self, now_downloaded, total_to_download);

  return FALSE;
}

static int
on_lr_progress_cb (void    *user_data,
                   gdouble  total_to_download,
                   gdouble  now_downloaded)
{
  FetchMdThreadData *data = user_data;

  if (total_to_download < 0)
    return 0;

  g_mutex_lock (&data->progress_lock);
  if (!data->progress_source)
    {
      data->progress_source = g_timeout_source_new_seconds (1);
      g_source_set_callback (data->progress_source, idle_emit_progress, data, NULL);
      g_source_attach (data->progress_source, NULL);
      g_source_unref (data->progress_source);
    }
  data->now_downloaded = now_downloaded;
  data->total_to_download = total_to_download;
  g_mutex_unlock (&data->progress_lock);

  return 0;
}

static gboolean
idle_emit_fetch_md_complete (gpointer user_data)
{
  FetchMdThreadData *data = user_data;

  data->self->op_thread = NULL;

  if (data->error)
    {
      nihif_lib_repo_worker_emit_fetch_md_complete ((NihifLibRepoWorker*)data->self, FALSE,
                                                         data->error->message);
      g_clear_error (&data->error);
    }
  else
    {
      nihif_lib_repo_worker_emit_fetch_md_complete ((NihifLibRepoWorker*)data->self,
                                                         TRUE, "");
    }

  g_free (data->outputdir);
  g_free (data->url);
  g_variant_unref (data->urlvars);
  g_strfreev (data->downloadlist);
  lr_result_free (data->lr_result);
  g_mutex_clear (&data->progress_lock);
  g_free (data);
  
  return FALSE;
}

static gpointer
fetch_md_thread (gpointer user_data)
{
  FetchMdThreadData *data = user_data;
  GError **error = &data->error;
  LrHandle *lr_handle;

  lr_handle = lr_handle_init ();
  data->lr_result = lr_result_init ();

  if (!lr_handle_setopt (lr_handle, error, LRO_REPOTYPE, LR_YUMREPO))
    goto out;
  if (!lr_handle_setopt (lr_handle, error, LRO_YUMDLIST, data->downloadlist))
    goto out;
  if (!lr_handle_setopt (lr_handle, error, LRO_USERAGENT, "rpmostree"))
    goto out;
  if (!lr_handle_setopt (lr_handle, error, LRO_LOCAL, FALSE))
    goto out;
  if (!lr_handle_setopt (lr_handle, error, LRO_DESTDIR, data->outputdir))
    goto out;
  if (!lr_handle_setopt (lr_handle, error, LRO_PROGRESSDATA, data))
    goto out;
  if (!lr_handle_setopt (lr_handle, error, LRO_PROGRESSCB, on_lr_progress_cb))
    goto out;

  switch (data->urltype)
    {
    case NIHIF_LIBREPO_IMPL_URLTYPE_BASEURL:
      {
        char *urls[] = { data->url, NULL };
	if (!lr_handle_setopt (lr_handle, error, LRO_URLS, urls))
          return FALSE;
        break;
      }
    case NIHIF_LIBREPO_IMPL_URLTYPE_METALINK:
      {
        if (!lr_handle_setopt (lr_handle, error, LRO_METALINKURL, data->url))
          return FALSE;
      }
    default:
      g_assert_not_reached ();
    }

  {
    LrUrlVars *urlvars = NULL;
    GVariantIter viter;
    const char *key;
    const char *value;

    g_variant_iter_init (&viter, data->urlvars);
    while (g_variant_iter_loop (&viter, "{&s&s}", &key, &value))
      urlvars = lr_urlvars_set (urlvars, key, value);

    if (!lr_handle_setopt (lr_handle, error, LRO_VARSUB, urlvars))
      goto out;
  }

  if (!lr_handle_perform (lr_handle, data->lr_result, error))
    goto out;

 out:
  g_idle_add (idle_emit_fetch_md_complete, data);
  return NULL;
}

static gboolean
handle_fetch_md (NihifLibRepoWorker *worker,
                 GDBusMethodInvocation *invocation,
                 const char *outputdir,
                 guint urltype_u,
                 const char *url,
                 GVariant *urlvars,
                 const char * const *downloadlist)
{
  NihifLibRepoImpl *self = NIHIF_LIBREPO_IMPL (worker);
  GError *local_error = NULL;
  GError **error = &local_error;
  FetchMdThreadData *threaddata;

  if (self->op_thread)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "An operation is already pending");
      goto out;
    }

  if (urltype_u > NIHIF_LIBREPO_IMPL_URLTYPE_LAST)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Invalid urltype '%u'", urltype_u);
      goto out;
    }

  threaddata = g_new0 (FetchMdThreadData, 1);
  threaddata->self = g_object_ref (self);
  threaddata->outputdir = g_strdup (outputdir);
  threaddata->urltype = urltype_u;
  threaddata->url = g_strdup (url);
  threaddata->urlvars = g_variant_ref (urlvars);
  threaddata->downloadlist = g_strdupv ((char**)downloadlist);
  g_mutex_init (&threaddata->progress_lock);

  self->op_thread = g_thread_new ("fetchmd", fetch_md_thread, threaddata);

 out:
  if (local_error)
    g_dbus_method_invocation_take_error (invocation, local_error);
  else
    g_dbus_method_invocation_return_value (invocation, NULL);
  return TRUE;
}

gboolean
nihif_librepo_impl_main (GError **error)
{
  gboolean ret = FALSE;
  GCancellable *cancellable = NULL;
  gs_unref_object GSocket *sock = NULL;
  gs_unref_object GSocketConnection *sockconn = NULL;
  gs_unref_object GDBusConnection *dbusconn = NULL;
  gs_unref_object NihifLibRepoImpl *impl = NULL;

  prctl (PR_SET_PDEATHSIG, SIGTERM);

  sock = g_socket_new_from_fd (0, error);
  if (!sock)
    goto out;
  sockconn = g_socket_connection_factory_create_connection (sock);

  dbusconn = g_dbus_connection_new_sync ((GIOStream*)sockconn, NULL, G_DBUS_CONNECTION_FLAGS_NONE,
                                         NULL, cancellable, error);
  if (!dbusconn)
    goto out;

  impl = g_object_new (NIHIF_TYPE_LIBREPO_IMPL, NULL);
  if (!g_dbus_interface_skeleton_export ((GDBusInterfaceSkeleton*)impl, dbusconn,
                                         "/nihif/librepoworker", error))
    goto out;

  while (TRUE)
    g_main_context_iteration (NULL, TRUE);

  ret = TRUE;
 out:
  return ret;
}

typedef struct {
  struct passwd *unpriv_user;
  int stdin_fd;
} ChildSetupData;

static void
librepo_child_setup (gpointer datap)
{
  ChildSetupData *data = datap;
  struct passwd *unpriv_user = data->unpriv_user;
  int r;

  if (setgroups (0, NULL) != 0)
    _rpmostree_perror_fatal ("setgroups: ");

  if (initgroups (unpriv_user->pw_name, unpriv_user->pw_gid) != 0)
    _rpmostree_perror_fatal ("initgroups: ");

  if (setregid (unpriv_user->pw_gid, unpriv_user->pw_gid) != 0)
    _rpmostree_perror_fatal ("setregid: ");

  if (setreuid (unpriv_user->pw_uid, unpriv_user->pw_uid) != 0)
    _rpmostree_perror_fatal ("setreuid: ");

  if ((geteuid () != unpriv_user->pw_uid) || (getuid () != unpriv_user->pw_uid) ||
      (getegid () != unpriv_user->pw_gid) || (getgid () != unpriv_user->pw_gid))
    {
      fprintf (stderr, "Failed to setreuid/setregid\n");
      exit (1);
    }

  do
    r = dup2 (data->stdin_fd, 0);
  while (G_UNLIKELY (r == -1 && errno == ENOENT));
}

gboolean
nihif_librepo_impl_spawn (NihifLibRepoWorker  **out_worker,
                              GCancellable             *cancellable,
                              GError                  **error)
{
  gboolean ret = FALSE;
  int spair[2];
  GPid librepo_impl_pid;
  gs_unref_object GSocket *sock = NULL;
  gs_unref_object GSocketConnection *sockconn = NULL;
  gs_unref_object GDBusConnection *dbusconn = NULL;
  gs_unref_object NihifLibRepoWorker *ret_worker = NULL;
  const char *const spawn_argv[] = { "rpm-ostree", "helper-process-librepo", NULL };
  gs_free struct passwd *unpriv_user = NULL;
  ChildSetupData childdata;

  if (!_rpmostree_getpwnam_alloc (RPMOSTREE_UNPRIV_USER, &unpriv_user, error))
    goto out;

  childdata.unpriv_user = unpriv_user;

  if (socketpair (AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, spair) != 0)
    {
      int errsv = errno;
      g_set_error_literal (error, G_IO_ERROR, g_io_error_from_errno (errsv),
                           g_strerror (errsv));
      goto out;
    }

  childdata.stdin_fd = spair[1];

  if (!g_spawn_async ("/", (char**)spawn_argv, NULL,
                      G_SPAWN_DO_NOT_REAP_CHILD | G_SPAWN_SEARCH_PATH,
                      librepo_child_setup, &childdata,
                      &librepo_impl_pid, error))
    goto out;

  (void) close (spair[1]);

  sock = g_socket_new_from_fd (spair[0], error);
  if (!sock)
    goto out;
  sockconn = g_socket_connection_factory_create_connection (sock);

  dbusconn = g_dbus_connection_new_sync ((GIOStream*)sockconn, NULL, G_DBUS_CONNECTION_FLAGS_NONE,
                                         NULL, cancellable, error);
  if (!dbusconn)
    goto out;
  
  ret_worker = nihif_lib_repo_worker_proxy_new_sync (dbusconn, 0, NULL,
                                                     "/nihif/librepoworker",
                                                     cancellable, error);
  if (!ret_worker)
    goto out;

  ret = TRUE;
  gs_transfer_out_value (out_worker, &ret_worker);
 out:
  return ret;
}
