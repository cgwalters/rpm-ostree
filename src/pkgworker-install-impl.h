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

#pragma once

#include "pkgworker-generated.h"

G_BEGIN_DECLS

#define PKG_WORKER_TYPE_INSTALL_IMPL  (pkg_worker_install_impl_get_type ())
#define PKG_WORKER_INSTALL_IMPL(o)    (G_TYPE_CHECK_INSTANCE_CAST ((o), PKG_WORKER_TYPE_INSTALL_IMPL, PkgWorkerInstallImpl))
#define PKG_WORKER_IS_INSTALL_IMPL(o) (G_TYPE_CHECK_INSTANCE_TYPE ((o), PKG_WORKER_TYPE_INSTALL_IMPL))

struct _PkgWorkerInstallImpl;
typedef struct _PkgWorkerInstallImpl PkgWorkerInstallImpl;

GType                     pkg_worker_install_impl_get_type   (void) G_GNUC_CONST;

gboolean                  pkg_worker_install_impl_main (GError **error);

gboolean                  pkg_worker_install_impl_spawn (PkgWorkerInstall         **out_worker,
                                                         GCancellable              *cancellable,
                                                         GError                   **error);

G_END_DECLS
