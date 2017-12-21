/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*-
 *
 * Copyright (C) 2017 Red Hat, Inc.
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

#include "libglnx.h"
#include "rpmostree-jigdo-core.h"
#include "rpmostree-core.h"

struct _RpmOstreeContext {
  GObject parent;

  RpmOstreeTreespec *spec;
  gboolean empty;

  /* jigdo-mode data */
  const char *jigdo_spec; /* The jigdo spec like: repoid:package */
  const char *jigdo_version; /* Optional */
  gboolean jigdo_pure; /* There is only jigdo */
  DnfPackage *jigdo_pkg;
  char *jigdo_checksum;

  gboolean pkgcache_only;
  DnfContext *dnfctx;
  OstreeRepo *ostreerepo;
  OstreeRepo *pkgcache_repo;
  OstreeRepoDevInoCache *devino_cache;
  gboolean unprivileged;
  OstreeSePolicy *sepolicy;
  char *passwd_dir;

  gboolean async_running;
  GCancellable *async_cancellable;
  GError *async_error;
  GPtrArray *pkgs; /* All packages */
  GPtrArray *pkgs_to_download;
  GPtrArray *pkgs_to_import;
  guint n_async_pkgs_imported;
  GPtrArray *pkgs_to_relabel;
  guint n_async_pkgs_relabeled;

  GHashTable *pkgs_to_remove;  /* pkgname --> gv_nevra */
  GHashTable *pkgs_to_replace; /* new gv_nevra --> old gv_nevra */

  GLnxTmpDir tmpdir;

  int tmprootfs_dfd; /* Borrowed */
  GLnxTmpDir repo_tmpdir; /* Used to assemble+commit if no base rootfs provided */
};

