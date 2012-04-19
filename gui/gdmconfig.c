/* -*- Mode: C; tab-width: 8; indent-tabs-mode: t; c-basic-offset: 8 -*-
 *
 * GDM - The GNOME Display Manager
 * Copyright (C) 1999, 2000 Martin K. Petersen <mkp@mkp.net>
 *
 * This file Copyright (c) 2003 George Lebl
 * - Common routines for the greeters.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <gtk/gtk.h>

#include "config.h"

#include "gdm.h"
#include "gdmcommon.h"
#include "gdmcomm.h"
#include "gdmconfig.h"

#include "gdm-common.h"
#include "gdm-log.h"

#include "server.h"

static GHashTable *int_hash       = NULL;
static GHashTable *bool_hash      = NULL;
static GHashTable *string_hash    = NULL;
static gboolean gdm_never_cache   = FALSE;
static int comm_tries             = 5;

/**
 * gdm_config_never_cache
 *
 * Most programs want config data to be cached to avoid constantly
 * grabbing the information over the wire and are happy calling 
 * gdm_update_config to update a key value.  However, gdmsetup
 * really does want the latest value each time it accesses a 
 * config option.  To avoid needing to call update_config 
 * for each key to be retrieved, just calling this function will
 * let the config system know to always get the value via the
 * sockets connection.
 */
void
gdm_config_never_cache (gboolean never_cache)
{
   gdm_never_cache = never_cache;
}

/**
 * gdm_config_set_comm_retries
 *
 * If a client wants to specify how many times it will retry to
 * get a config value, this function can be used.
 */
void
gdm_config_set_comm_retries (int tries)
{
    comm_tries = tries;
}

/**
 * gdm_config_hash_lookup
 *
 * Accesses hash with key, stripping it so it doesn't contain
 * a default value.
 */
static gpointer
gdm_config_hash_lookup (GHashTable *hash,
			const gchar *key)
{
	gchar *p;
	gpointer *ret;
	gchar *newkey = g_strdup (key);

	g_strstrip (newkey);
	p = strchr (newkey, '=');
	if (p != NULL)
		*p = '\0';

	ret = g_hash_table_lookup (hash, newkey);
	g_free (newkey);
	return (ret);
}

/**
 * gdm_config_add_hash
 *
 * Adds value to hash, stripping the key so it doesn't contain
 * a default value.
 */
static void
gdm_config_add_hash (GHashTable *hash,
		     const gchar *key,
		     gpointer value)
{
	gchar *p;
	gchar *newkey = g_strdup (key);

	g_strstrip (newkey);
	p = strchr (newkey, '=');
	if (p != NULL)
		*p = '\0';

	/* Do not free the newkey since it is going into the hash. */
	g_hash_table_insert (hash, newkey, value);
}

/**
 * gdm_config_get_result
 *
 * Calls daemon to get config result, stripping the key so it
 * doesn't contain a default value.
 */
static gchar *
gdm_config_get_result (const gchar *key)
{
	gchar *p;
	gchar *newkey  = g_strdup (key);
	gchar *command = NULL;
	gchar *result  = NULL;
	static char *display = NULL;

	g_strstrip (newkey);
	p = strchr (newkey, '=');
	if (p != NULL)
		*p = '\0';

	display = g_strdup (g_getenv ("DISPLAY"));
	if (display == NULL)
		command = g_strdup_printf ("GET_CONFIG %s", newkey);
	else
		command = g_strdup_printf ("GET_CONFIG %s %s", newkey, display);

	result  = gdmcomm_call_gdm (command, NULL /* auth cookie */,
	          "2.13.0.1", comm_tries);

	g_free (display);
	g_free (command);
	g_free (newkey);
	return result;
}

/**
 * gdm_config_get_xserver_details
 *
 * Calls daemon to get details for an xserver config.
 */
static gchar *
gdm_config_get_xserver_details (const gchar *xserver,
				const gchar *key)
{
	gchar *command = NULL;
	gchar *result  = NULL;
	gchar *temp;

	command = g_strdup_printf ("GET_SERVER_DETAILS %s %s", xserver, key);
	result = gdmcomm_call_gdm (command, NULL /* auth cookie */,
		"2.13.0.1", comm_tries);

	g_free (command);

	if (! result || ve_string_empty (result) ||
	    strncmp (result, "OK ", 3) != 0) {

		gdm_common_error ("Could not access xserver configuration");

		if (result)
			g_free (result);
		return NULL;
	}

	/* skip the "OK " */
	temp = g_strdup (result + 3);
	g_free (result);

	return temp;
}

/**
 * gdm_config_get_xservers
 *
 * Calls daemon to get xserver config.
 */
GSList *
gdm_config_get_xservers (gboolean flexible)
{
	GSList *xservers = NULL;
        gchar **splitstr, **sec;
	gchar *command = NULL;
	gchar *result  = NULL;
	gchar *temp;

	command = g_strdup_printf ("GET_SERVER_LIST");
	result = gdmcomm_call_gdm (command, NULL /* auth cookie */,
		"2.13.0.1", comm_tries);

	g_free (command);

	if (! result || ve_string_empty (result) ||
	    strncmp (result, "OK ", 3) != 0) {

		gdm_common_error ("Could not access xserver configuration");

		if (result)
			g_free (result);
		return NULL;
	}

	/* skip the "OK " */
	splitstr = g_strsplit (result + 3, ";", 0);
	sec      = splitstr;
	g_free (result);

	while (sec != NULL && *sec != NULL) {
		GdmXserver *svr = g_new0 (GdmXserver, 1);

		temp = gdm_config_get_xserver_details (*sec, "ID");
		if (temp == NULL) {
			g_free (svr);
			continue;
		}
		svr->id = temp;
		temp = gdm_config_get_xserver_details (*sec, "NAME");
		if (temp == NULL) {
			g_free (svr);
			continue;
		}
		svr->name = temp;
		temp = gdm_config_get_xserver_details (*sec, "COMMAND");
		if (temp == NULL) {
			g_free (svr);
			continue;
		}
		svr->command = temp;

		temp = gdm_config_get_xserver_details (*sec, "FLEXIBLE");
		if (temp == NULL) {
			g_free (svr);
			continue;
		} else if (g_strncasecmp (ve_sure_string (temp), "true", 4) == 0)
			svr->flexible = TRUE;
		else
			svr->flexible = FALSE;
		g_free (temp);

		temp = gdm_config_get_xserver_details (*sec, "CHOOSABLE");
		if (temp == NULL) {
			g_free (svr);
			continue;
		} else if (g_strncasecmp (temp, "true", 4) == 0)
			svr->choosable = TRUE;
		else
			svr->choosable = FALSE;
		g_free (temp);

		temp = gdm_config_get_xserver_details (*sec, "HANDLED");
		if (temp == NULL) {
			g_free (svr);
			continue;
		} else if (g_strncasecmp (temp, "true", 4) == 0)
			svr->handled = TRUE;
		else
			svr->handled = FALSE;
		g_free (temp);

		temp = gdm_config_get_xserver_details (*sec, "CHOOSER");
		if (temp == NULL) {
			g_free (svr);
			continue;
		} else if (g_strncasecmp (temp, "true", 4) == 0)
			svr->chooser = TRUE;
		else
			svr->chooser = FALSE;
		g_free (temp);

		temp = gdm_config_get_xserver_details (*sec, "PRIORITY");
		if (temp == NULL) {
			g_free (svr);
			continue;
		} else {
			svr->priority = atoi (temp);
		}
		g_free (temp);

		sec++;

		/* If only flexible was requested, then skip if not flexible */
		if (flexible && !svr->flexible) {
			g_free (svr);
			continue;
		}

		xservers = g_slist_append (xservers, svr);
	}

	g_strfreev (splitstr);
	return xservers;
}

/**
 * gdm_config_get_string
 *
 * Gets string configuration value from daemon via GET_CONFIG
 * socket command.  It stores the value in a hash so subsequent
 * access is faster.
 */
static gchar *
_gdm_config_get_string (const gchar *key,
			gboolean reload,
			gboolean *changed,
			gboolean doing_translated)
{
	gchar *hashretval = NULL;
	gchar *result = NULL;
	gchar *temp;

        if (string_hash == NULL)
		string_hash = g_hash_table_new (g_str_hash, g_str_equal);

	hashretval = gdm_config_hash_lookup (string_hash, key);

	if (reload == FALSE && hashretval != NULL)
		return hashretval;

	result = gdm_config_get_result (key);

	if ( ! result || ve_string_empty (result) ||
	    strncmp (result, "OK ", 3) != 0) {

		gchar *getdefault;

		/*
		 * If looking for a translated string, and not found, just return
		 * NULL.
		 */
		if (doing_translated) {
			if (result)
				g_free (result);
			return NULL;
		}

		gdm_common_error ("Could not access configuration key <%s>", key);

		/* Return the compiled in value associated with the key, if available. */
		getdefault = strchr (key, '=');
		if (getdefault != NULL)
			getdefault++;

		temp = g_strdup (getdefault);

		gdm_common_error ("Using compiled in value <%s> for <%s>", temp, key);
	} else {

		/* skip the "OK " */
		temp = g_strdup (result + 3);
	}

	if (result)
		g_free (result);

	if (hashretval == NULL) {

		if (changed != NULL)
			*changed = TRUE;

		gdm_config_add_hash (string_hash, key, temp);
	} else {
		if (changed != NULL) {
			if (strcmp (ve_sure_string (hashretval), temp) != 0)
				*changed = TRUE;
			else
				*changed = FALSE;
		}
		g_hash_table_replace (string_hash, (void *)key, temp);
	}
	return temp;
}

gchar *
gdm_config_get_string (const gchar *key)
{
   if (gdm_never_cache == TRUE)
      return _gdm_config_get_string (key, TRUE, NULL, FALSE);
   else
      return _gdm_config_get_string (key, FALSE, NULL, FALSE);
}

/**
 * gdm_config_get_translated_string
 *
 * Gets translated string configuration value from daemon via
 * GET_CONFIG socket command.  It stores the value in a hash so
 * subsequent access is faster.  This does similar logic to
 * ve_config_get_trasnlated_string, requesting the value for 
 * each language and returning the default value if none is
 * found.
 */ 
static gchar *
_gdm_config_get_translated_string (const gchar *key,
				   gboolean reload,
				   gboolean *changed)
{
	const char * const *langs;
        char *newkey;
        char *def;
	int   i;

	/* Strip key */
	newkey = g_strdup (key);
	def    = strchr (newkey, '=');
	if (def != NULL)
		*def = '\0';

	langs = g_get_language_names ();

	for (i = 0; langs[i] != NULL; i++) {
                gchar *full;
		gchar *val;

		/*
		 * Pass TRUE for last argument so it doesn't print errors for
		 * failing to find the key, since this is expected
		 */
		full = g_strdup_printf ("%s[%s]", newkey, langs[i]);
		val = _gdm_config_get_string (full, reload, changed, TRUE);

		g_free (full);

                if (val != NULL) {
			g_free (newkey);
			return val;
		}
        }

	g_free (newkey);

	/* Print error if it fails this time */
	return _gdm_config_get_string (key, reload, changed, FALSE);
}

gchar *
gdm_config_get_translated_string (const gchar *key)
{
   if (gdm_never_cache == TRUE)
      return _gdm_config_get_translated_string (key, TRUE, NULL);
   else
      return _gdm_config_get_translated_string (key, FALSE, NULL);
}

/**
 * gdm_config_get_int
 *
 * Gets int configuration value from daemon via GET_CONFIG
 * socket command.  It stores the value in a hash so subsequent
 * access is faster.
 */
static gint
_gdm_config_get_int (const gchar *key,
		     gboolean reload,
		     gboolean *changed)
{
	gint  *hashretval = NULL;
	gchar *result = NULL;
	gint  temp;

        if (int_hash == NULL)
		int_hash = g_hash_table_new (g_str_hash, g_str_equal);

	hashretval = gdm_config_hash_lookup (int_hash, key);
	if (reload == FALSE && hashretval != NULL)
		return *hashretval;

	result = gdm_config_get_result (key);

	if ( ! result || ve_string_empty (result) ||
	    strncmp (result, "OK ", 3) != 0) {

		gchar *getdefault;

		gdm_common_error ("Could not access configuration key <%s>", key);

		/* Return the compiled in value associated with the key, if available. */
		getdefault = strchr (key, '=');
		if (getdefault != NULL)
			getdefault++;

		temp = atoi (getdefault);

		gdm_common_error ("Using compiled in value <%d> for <%s>", temp, key);

	} else {

		/* skip the "OK " */
		temp = atoi (result + 3);
	}

	if (result)
		g_free (result);

	if (hashretval == NULL) {
		gint *intval = g_new0 (gint, 1);
		*intval      = temp;
		gdm_config_add_hash (int_hash, key, intval);

		if (changed != NULL)
			*changed = TRUE;

		return *intval;
	} else {
		if (changed != NULL) {
			if (*hashretval != temp)
				*changed = TRUE;
			else
				*changed = FALSE;
		}

		*hashretval = temp;
		return *hashretval;
	}
}

gint
gdm_config_get_int (const gchar *key)
{
   if (gdm_never_cache == TRUE)
      return _gdm_config_get_int (key, TRUE, NULL);
   else
      return _gdm_config_get_int (key, FALSE, NULL);
}

/**
 * gdm_config_get_bool
 *
 * Gets int configuration value from daemon via GET_CONFIG
 * socket command.  It stores the value in a hash so subsequent
 * access is faster.
 */
static gboolean
_gdm_config_get_bool (const gchar *key,
		      gboolean reload,
		      gboolean *changed)
{
	gboolean *hashretval = NULL;
	gchar    *result;
	gboolean temp;

        if (bool_hash == NULL)
           bool_hash = g_hash_table_new (g_str_hash, g_str_equal);

	hashretval = gdm_config_hash_lookup (bool_hash, key);
	if (reload == FALSE && hashretval != NULL)
		return *hashretval;

	result = gdm_config_get_result (key);

	if ( ! result || ve_string_empty (result) ||
	    strncmp (result, "OK ", 3) != 0) {

		gchar *getdefault;

		gdm_common_error ("Could not access configuration key <%s>", key);

		/* Return the compiled in value associated with the key, if available. */
		getdefault = strchr (key, '=');
		if (getdefault != NULL)
			getdefault++;

		/* Same logic as used in ve_config_get_bool */
		if (getdefault != NULL &&
		   (getdefault[0] == 'T' ||
		    getdefault[0] == 't' ||
		    getdefault[0] == 'Y' ||
		    getdefault[0] == 'y' ||
		    atoi (getdefault) != 0)) {
			temp = TRUE;
			gdm_common_error ("Using compiled in value <TRUE> for <%s>", key);
		} else {
			temp = FALSE;
			gdm_common_error ("Using compiled in value <FALSE> for <%s>", key);
		}
	} else {

		/* skip the "OK " */
		if (strcmp (ve_sure_string (result + 3), "true") == 0)
			temp = TRUE;
		else
			temp = FALSE;
	}

	if (result)
		g_free (result);

	if (hashretval == NULL) {
		gboolean *boolval = g_new0 (gboolean, 1);
		*boolval          = temp;
		gdm_config_add_hash (bool_hash, key, boolval);

		if (changed != NULL)
			*changed = TRUE;

		return *boolval;
	} else {
		if (changed != NULL) {
			if (*hashretval != temp)
				*changed = TRUE;
			else
				*changed = FALSE;
		}

		*hashretval = temp;
		return *hashretval;
	}
}

gboolean
gdm_config_get_bool (const gchar *key)
{
   if (gdm_never_cache == TRUE)
      return _gdm_config_get_bool (key, TRUE, NULL);
   else
      return _gdm_config_get_bool (key, FALSE, NULL);
}

/**
 * gdm_config_reload_string
 * gdm_config_reload_int
 * gdm_config_reload_bool
 * 
 * Reload values returning TRUE if value changed, FALSE
 * otherwise.
 */
gboolean
gdm_config_reload_string (const gchar *key)
{
	gboolean changed;
	_gdm_config_get_string (key, TRUE, &changed, FALSE);
	return changed;
}

gboolean
gdm_config_reload_int (const gchar *key)
{
	gboolean changed;
	_gdm_config_get_int (key, TRUE, &changed);
	return changed;
}

gboolean
gdm_config_reload_bool (const gchar *key)
{
	gboolean changed;
	_gdm_config_get_bool (key, TRUE, &changed);
	return changed;
}

void
gdm_save_customlist_data (const gchar *file,
			  const gchar *key,
			  const gchar *id)
{
	GKeyFile *cfg;

	gdm_debug ("Saving custom configuration data to file=%s, key=%s",
		file, key);
	cfg = gdm_common_config_load (file, NULL);
	if (cfg == NULL) {
		gint fd = -1;
                gdm_debug ("creating file: %s", file);
		VE_IGNORE_EINTR (fd = g_open (file,
			O_CREAT | O_TRUNC | O_RDWR, 0644));

		if (fd < 0)
			return;

		write (fd, "\n", 2);
		close (fd);
		cfg = gdm_common_config_load (file, NULL);
		if (cfg == NULL) {
			return;
		}
	}

	g_key_file_set_string (cfg, "GreeterInfo", key, ve_sure_string (id));
	gdm_common_config_save (cfg, file, NULL);
	g_key_file_free (cfg);
}

gchar *
gdm_get_theme_greeter (const gchar *file,
		       const char *fallback)
{
	GKeyFile *config;
	gchar *s;

	config = gdm_common_config_load (file, NULL);
	s = NULL;
	gdm_common_config_get_translated_string (config, "GdmGreeterTheme/Greeter", &s, NULL);

	if (s == NULL || s[0] == '\0') {
		g_free (s);
		s = g_strdup_printf ("%s.xml", fallback);
	}

	g_key_file_free (config);

	return s;
}

