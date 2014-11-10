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

#include "rpmostree-rpmutil.h"

#include <string.h>
#include <glib-unix.h>
#include <librepo/librepo.h>

#define MAX_NATIVE_ARCHES	12

static gboolean initialized;

gboolean
rpmostree_rpmutil_init (GError **error)
{
  if (initialized)
    return TRUE;

  if (rpmReadConfigFiles (NULL, NULL))
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "rpm failed to init: %s", rpmlogMessage());
      return FALSE;
    }

  initialized = TRUE;

  return TRUE;
}

const char *
rpmostree_get_base_arch (void)
{
  const char *rpmarch;
  guint i,j;

  /* data taken from https://github.com/rpm-software-management/dnf/blob/master/dnf/arch.py */
  /* data re-taken from libhif */
  const struct {
    const gchar	*base;
    const gchar	*native[MAX_NATIVE_ARCHES];
  } arch_map[] =  {
    { "aarch64",	{ "aarch64", NULL } },
    { "alpha",	{ "alpha", "alphaev4", "alphaev45", "alphaev5",
                  "alphaev56", "alphaev6", "alphaev67",
                  "alphaev68", "alphaev7", "alphapca56", NULL } },
    { "arm",	{ "armv5tejl", "armv5tel", "armv6l", "armv7l", NULL } },
    { "armhfp",	{ "armv7hl", "armv7hnl", NULL } },
    { "i386",	{ "i386", "athlon", "geode", "i386",
                  "i486", "i586", "i686", NULL } },
    { "ia64",	{ "ia64", NULL } },
    { "noarch",	{ "noarch", NULL } },
    { "ppc",	{ "ppc", NULL } },
    { "ppc64",	{ "ppc64", "ppc64iseries", "ppc64p7",
                  "ppc64pseries", NULL } },
    { "ppc64le",	{ "ppc64le", NULL } },
    { "s390",	{ "s390", NULL } },
    { "s390x",	{ "s390x", NULL } },
    { "sh3",	{ "sh3", NULL } },
    { "sh4",	{ "sh4", "sh4a", NULL } },
    { "sparc",	{ "sparc", "sparc64", "sparc64v", "sparcv8",
                  "sparcv9", "sparcv9v", NULL } },
    { "x86_64",	{ "x86_64", "amd64", "ia32e", NULL } },
    { NULL,		{ NULL } }
  };

  g_return_val_if_fail (initialized, NULL);

  rpmGetArchInfo (&rpmarch, NULL);

  g_assert (rpmarch);

  for (i = 0; arch_map[i].base != NULL; i++)
    {
      for (j = 0; arch_map[i].native[j] != NULL; j++)
        {
          if (g_strcmp0 (arch_map[i].native[j], rpmarch) == 0)
            {
              return arch_map[i].base;
            }
        }
    }

  g_error ("Failed to determine basearch for rpm arch '%s'", rpmarch);
  return NULL;
}
