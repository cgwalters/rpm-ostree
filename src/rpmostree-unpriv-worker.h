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

#pragma once

#include <gio/gio.h>

#define RPMOSTREE_TYPE_UNPRIV_WORKER			(rpmostree_unpriv_worker_get_type())
#define RPMOSTREE_UNPRIV_WORKER(obj)			(G_TYPE_CHECK_INSTANCE_CAST((obj), RPMOSTREE_TYPE_UNPRIV_WORKER, RpmOstreeUnprivWorker))
#define RPMOSTREE_UNPRIV_WORKER_CLASS(cls)		(G_TYPE_CHECK_CLASS_CAST((cls), RPMOSTREE_TYPE_UNPRIV_WORKER, RpmOstreeUnprivWorkerClass))
#define RPMOSTREE_IS_UNPRIV_WORKER(obj)		(G_TYPE_CHECK_INSTANCE_TYPE((obj), RPMOSTREE_TYPE_UNPRIV_WORKER))
#define RPMOSTREE_IS_UNPRIV_WORKER_CLASS(cls)	(G_TYPE_CHECK_CLASS_TYPE((cls), RPMOSTREE_TYPE_UNPRIV_WORKER))
#define RPMOSTREE_UNPRIV_WORKER_GET_CLASS(obj)	(G_TYPE_INSTANCE_GET_CLASS((obj), RPMOSTREE_TYPE_UNPRIV_WORKER, RpmOstreeUnprivWorkerClass))

G_BEGIN_DECLS

typedef struct _RpmOstreeUnprivWorker		RpmOstreeUnprivWorker;
typedef struct _RpmOstreeUnprivWorkerClass	RpmOstreeUnprivWorkerClass;

GType		 rpmostree_unpriv_worker_get_type		(void);
RpmOstreeUnprivWorker	*rpmostree_unpriv_worker_new (uid_t           uid,
                                                      GCancellable   *cancellable,
                                                      GError        **error);

GDBusConnection *rpmostree_unpriv_worker_get_connection (RpmOstreeUnprivWorker  *worker);

G_END_DECLS
