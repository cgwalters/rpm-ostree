/*
 * Copyright (C) 2017 Red Hat, Inc.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301 USA
 */

#pragma once

#include <ostree.h>

#include "rpmostreed-types.h"

/* Used by the upgrader to hold a strong ref temporarily to a base commit */
#define RPMOSTREE_TMP_BASE_REF "rpmostree/base/tmp"
/* Where we check out a new rootfs */
#define RPMOSTREE_TMP_ROOTFS_DIR "extensions/rpmostree/commit"

gboolean
rpmostree_syscore_cleanup (OstreeSysroot            *sysroot,
                           OstreeRepo               *repo,
                           GCancellable             *cancellable,
                           GError                  **error);

OstreeDeployment *rpmostree_syscore_get_origin_merge_deployment (OstreeSysroot *self, const char *osname);

gboolean rpmostree_syscore_bump_mtime (OstreeSysroot *self, GError **error);

GPtrArray *rpmostree_syscore_add_deployment (OstreeSysroot      *sysroot,
                                             OstreeDeployment   *new_deployment,
                                             OstreeDeployment   *merge_deployment,
                                             gboolean            pushing_rollback);

gboolean rpmostree_syscore_write_deployments (OstreeSysroot          *sysroot,
                                              OstreeRepo              *repo,
                                              GPtrArray              *new_deployments,
                                              GCancellable           *cancellable,
                                              GError                **error);
