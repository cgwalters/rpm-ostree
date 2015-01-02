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

#include "pkgworker-install-impl.h"

#include <glib-unix.h>
#include <libhif.h>
#include <string.h>
#include <sys/prctl.h>
#include <signal.h>
#include <grp.h>
#include <sys/types.h>
#include <unistd.h>
#include <sys/socket.h>

#include "pkgworker-generated.h"
#include "rpmostree-util.h"
#include "libgsystem.h"

struct _PkgWorkerInstallImpl
{
  PkgWorkerInstallSkeleton parent_instance;

  GMutex lock;
  
  GThread *op_thread;
};

typedef struct _PkgWorkerInstallImplClass
{
  PkgWorkerInstallSkeletonClass parent_class;
} PkgWorkerInstallImplClass;

static void pkg_worker_install_iface_init (PkgWorkerInstallIface *iface);

G_DEFINE_TYPE_WITH_CODE (PkgWorkerInstallImpl, pkg_worker_install_impl,
                         PKG_WORKER_TYPE_INSTALL_SKELETON,
                         G_IMPLEMENT_INTERFACE (PKG_WORKER_TYPE_INSTALL, pkg_worker_install_iface_init));

static void
pkg_worker_install_impl_init (PkgWorkerInstallImpl *self)
{
}

static void
pkg_worker_install_impl_finalize (GObject *object)
{
  G_GNUC_UNUSED PkgWorkerInstallImpl *self = (PkgWorkerInstallImpl *)object;
  G_OBJECT_CLASS (pkg_worker_install_impl_parent_class)->finalize (object);
}

static void
pkg_worker_install_impl_constructed (GObject *object)
{
  if (G_OBJECT_CLASS (pkg_worker_install_impl_parent_class)->constructed != NULL)
    G_OBJECT_CLASS (pkg_worker_install_impl_parent_class)->constructed (object);
}

static void
pkg_worker_install_impl_class_init (PkgWorkerInstallImplClass *klass)
{
  GObjectClass *gobject_class;

  gobject_class = G_OBJECT_CLASS (klass);
  gobject_class->finalize = pkg_worker_install_impl_finalize;
  gobject_class->constructed = pkg_worker_install_impl_constructed;
}

static gboolean
handle_hello_install (PkgWorkerInstall *worker,
                      GDBusMethodInvocation *invocation)
{
  G_GNUC_UNUSED PkgWorkerInstallImpl *self = PKG_WORKER_INSTALL_IMPL (worker);
  g_dbus_method_invocation_return_value (invocation, g_variant_new ("(s)", "hi"));
  return TRUE;
}

static void
pkg_worker_install_iface_init (PkgWorkerInstallIface *iface)
{
  iface->handle_hello_install = handle_hello_install;
}

gboolean
pkg_worker_install_impl_main (GError **error)
{
  gboolean ret = FALSE;
  GCancellable *cancellable = NULL;
  gs_unref_object GSocket *sock = NULL;
  gs_unref_object GSocketConnection *sockconn = NULL;
  gs_unref_object GDBusConnection *dbusconn = NULL;
  gs_unref_object PkgWorkerInstallImpl *impl = NULL;

  /* Always forcibly die if our parent did */
  prctl (PR_SET_PDEATHSIG, SIGKILL);

  sock = g_socket_new_from_fd (0, error);
  if (!sock)
    goto out;
  sockconn = g_socket_connection_factory_create_connection (sock);

  dbusconn = g_dbus_connection_new_sync ((GIOStream*)sockconn, NULL, G_DBUS_CONNECTION_FLAGS_NONE,
                                         NULL, cancellable, error);
  if (!dbusconn)
    goto out;

  impl = g_object_new (PKG_WORKER_TYPE_INSTALL_IMPL, NULL);
  if (!g_dbus_interface_skeleton_export ((GDBusInterfaceSkeleton*)impl, dbusconn,
                                         "/pkgworker/install", error))
    goto out;

  while (TRUE)
    g_main_context_iteration (NULL, TRUE);

  ret = TRUE;
 out:
  return ret;
}

typedef struct {
  int stdin_fd;
} ChildSetupData;

static void
pkgworker_child_setup (gpointer datap)
{
  ChildSetupData *data = datap;
  int r;

  do
    r = dup2 (data->stdin_fd, 0);
  while (G_UNLIKELY (r == -1 && errno == ENOENT));
}

gboolean
pkg_worker_install_impl_spawn (PkgWorkerInstall  **out_worker,
                               GCancellable             *cancellable,
                               GError                  **error)
{
  gboolean ret = FALSE;
  int spair[2];
  GPid impl_pid;
  gs_unref_object GSocket *sock = NULL;
  gs_unref_object GSocketConnection *sockconn = NULL;
  gs_unref_object GDBusConnection *dbusconn = NULL;
  gs_unref_object PkgWorkerInstall *ret_worker = NULL;
  const char *const spawn_argv[] = { "rpm-ostree", "helper-process-install", NULL };
  ChildSetupData childdata;

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
                      pkgworker_child_setup, &childdata,
                      &impl_pid, error))
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
  
  ret_worker = pkg_worker_install_proxy_new_sync (dbusconn, 0, NULL,
                                                  "/pkgworker/install",
                                                  cancellable, error);
  if (!ret_worker)
    goto out;

  ret = TRUE;
  gs_transfer_out_value (out_worker, &ret_worker);
 out:
  return ret;
}
