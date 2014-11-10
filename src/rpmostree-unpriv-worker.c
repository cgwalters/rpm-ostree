/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*-
 *
 * Copyright (C) 2014 Colin Walters <walters@verbum.org>
 *
 * Licensed under the GNU Lesser General Public License Version 2.1
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301 USA
 */

#include "config.h"

#include "rpmostree-unpriv-worker.h"

#include "libgsystem.h"

struct _RpmOstreeUnprivWorker
{
  GObject parent;
  uid_t uid;
  GDBusConnection *child_connection;
};

struct _RpmOstreeUnprivWorkerClass
{
  GObjectClass parent_class;
};

G_DEFINE_TYPE (RpmOstreeUnprivWorker, rpmostree_unpriv_worker, G_TYPE_OBJECT)

static void
rpmostree_unpriv_worker_finalize (GObject *object)
{
  RpmOstreeUnprivWorker *repos = RPMOSTREE_UNPRIV_WORKER (object);

  G_OBJECT_CLASS (rpmostree_unpriv_worker_parent_class)->finalize (object);
}

static void
rpmostree_unpriv_worker_init (RpmOstreeUnprivWorker *repos)
{
}

static void
rpmostree_unpriv_worker_class_init (RpmOstreeUnprivWorkerClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->finalize = rpmostree_unpriv_worker_finalize;
}

RpmOstreeUnprivWorker *
rpmostree_unpriv_worker_new (uid_t           uid,
                             GCancellable   *cancellable,
                             GError        **error)
{
  return g_object_new (RPMOSTREE_TYPE_UNPRIV_WORKER, NULL);
}
