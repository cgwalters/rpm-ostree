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

#include "rpmostree-yumrepo.h"
#include "libgsystem.h"

/**
 * hif_repos_load_multiline_key_file:
 **/
static GKeyFile *
hif_repos_load_multiline_key_file (const gchar *filename, GError **error)
{
	GKeyFile *file = NULL;
	gboolean ret;
	gsize len;
	guint i;
        GString *string = NULL;
	gs_free gchar *data = NULL;
	gs_strfreev gchar **lines = NULL;

	/* load file */
	if (!g_file_get_contents (filename, &data, &len, error))
		return NULL;

	/* split into lines */
	string = g_string_new ("");
	lines = g_strsplit (data, "\n", -1);
	for (i = 0; lines[i] != NULL; i++) {
		/* if a line starts with whitespace, then append it on
		 * the previous line */
		g_strdelimit (lines[i], "\t", ' ');
		if (lines[i][0] == ' ' && string->len > 0) {
			g_string_set_size (string, string->len - 1);
			g_string_append_printf (string,
						";%s\n",
						g_strchug (lines[i]));
		} else {
			g_string_append_printf (string,
						"%s\n",
						lines[i]);
		}
	}

	/* remove final newline */
	if (string->len > 0)
		g_string_set_size (string, string->len - 1);

	/* load modified lines */
	file = g_key_file_new ();
	ret = g_key_file_load_from_data (file,
					 string->str,
					 -1,
					 G_KEY_FILE_KEEP_COMMENTS,
					 error);
        g_string_free (string, TRUE);
	if (!ret) {
		g_key_file_free (file);
		return NULL;
	}
	return file;
}

gboolean
_rpmostree_load_yum_repo_file (GFile         *repo_path,
                               GKeyFile     **out_keyfile,
                               GCancellable  *cancellable,
                               GError       **error)
{
  gboolean ret = TRUE;

  *out_keyfile = hif_repos_load_multiline_key_file (gs_file_get_path_cached (repo_path), error);
  if (!*out_keyfile)
    goto out;
    
  ret = TRUE;
 out:
  return ret;
}

