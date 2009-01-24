/***************************************************************************
 *            burn-session.c
 *
 *  mer aoû  9 22:22:16 2006
 *  Copyright  2006  Rouquier Philippe
 *  brasero-app@wanadoo.fr
 ***************************************************************************/

/*
 *  Brasero is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  Brasero is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU Library General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to:
 * 	The Free Software Foundation, Inc.,
 * 	51 Franklin Street, Fifth Floor
 * 	Boston, MA  02110-1301, USA.
 */

#ifdef HAVE_CONFIG_H
#  include <config.h>
#endif

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>

#include <glib.h>
#include <glib-object.h>
#include <glib/gstdio.h>
#include <glib/gi18n-lib.h>

#include "burn-session.h"
#include "burn-basics.h"
#include "burn-debug.h"
#include "burn-track.h"
#include "brasero-medium.h"
#include "brasero-drive.h"
#include "brasero-medium-monitor.h"

G_DEFINE_TYPE (BraseroBurnSession, brasero_burn_session, G_TYPE_OBJECT);
#define BRASERO_BURN_SESSION_PRIVATE(o)  (G_TYPE_INSTANCE_GET_PRIVATE ((o), BRASERO_TYPE_BURN_SESSION, BraseroBurnSessionPrivate))

struct _BraseroSessionSetting {
	BraseroDrive *burner;

	/**
	 * Used when outputting an image instead of burning
	 */
	BraseroImageFormat format;
	gchar *image;
	gchar *toc;

	/**
	 * Used when burning
	 */
	gchar *label;
	guint64 rate;

	gchar *tmpdir;

	BraseroBurnFlag flags;
};
typedef struct _BraseroSessionSetting BraseroSessionSetting;

struct _BraseroBurnSessionPrivate {
	FILE *session;
	gchar *session_path;

	GSList *tmpfiles;

	BraseroSessionSetting settings [1];
	GSList *pile_settings;

	BraseroTrackType input;

	GHashTable *tags;

	guint src_added_sig;
	guint src_removed_sig;
	guint dest_added_sig;
	guint dest_removed_sig;

	GSList *tracks;
	GSList *pile_tracks;
};
typedef struct _BraseroBurnSessionPrivate BraseroBurnSessionPrivate;

#define BRASERO_BURN_SESSION_WRITE_TO_DISC(priv)	(priv->settings->burner &&			\
							!brasero_drive_is_fake (priv->settings->burner))
#define BRASERO_BURN_SESSION_WRITE_TO_FILE(priv)	(priv->settings->burner &&			\
							 brasero_drive_is_fake (priv->settings->burner))
#define BRASERO_STR_EQUAL(a, b)	((!(a) && !(b)) || ((a) && (b) && !strcmp ((a), (b))))

typedef enum {
	INPUT_CHANGED_SIGNAL,
	OUTPUT_CHANGED_SIGNAL,
	LAST_SIGNAL
} BraseroBurnSessionSignalType;

static guint brasero_burn_session_signals [LAST_SIGNAL] = { 0 };
static GObjectClass *parent_class = NULL;

static void
brasero_session_settings_clean (BraseroSessionSetting *settings)
{
	if (settings->image)
		g_free (settings->image);

	if (settings->toc)
		g_free (settings->toc);

	if (settings->tmpdir)
		g_free (settings->tmpdir);

	if (settings->label)
		g_free (settings->label);

	if (settings->burner)
		g_object_unref (settings->burner);

	memset (settings, 0, sizeof (BraseroSessionSetting));
}

void
brasero_session_settings_copy (BraseroSessionSetting *dest,
			       BraseroSessionSetting *original)
{
	brasero_session_settings_clean (dest);

	memcpy (dest, original, sizeof (BraseroSessionSetting));

	g_object_ref (dest->burner);
	dest->image = g_strdup (original->image);
	dest->toc = g_strdup (original->toc);
	dest->label = g_strdup (original->label);
	dest->tmpdir = g_strdup (original->tmpdir);
}

static void
brasero_session_settings_free (BraseroSessionSetting *settings)
{
	brasero_session_settings_clean (settings);
	g_free (settings);
}

static void
brasero_burn_session_src_media_added (BraseroDrive *drive,
				      BraseroMedium *medium,
				      BraseroBurnSession *self)
{
	g_signal_emit (self,
		       brasero_burn_session_signals [INPUT_CHANGED_SIGNAL],
		       0);
}

static void
brasero_burn_session_src_media_removed (BraseroDrive *drive,
					BraseroMedium *medium,
					BraseroBurnSession *self)
{
	g_signal_emit (self,
		       brasero_burn_session_signals [INPUT_CHANGED_SIGNAL],
		       0);
}

static void
brasero_burn_session_start_src_drive_monitoring (BraseroBurnSession *self)
{
	BraseroDrive *drive;
	BraseroBurnSessionPrivate *priv;

	if (brasero_burn_session_get_input_type (self, NULL) != BRASERO_TRACK_TYPE_DISC)
		return;

	drive = brasero_burn_session_get_src_drive (self);
	if (!drive)
		return;

	priv = BRASERO_BURN_SESSION_PRIVATE (self);
	priv->src_added_sig = g_signal_connect (drive,
						"medium-added",
						G_CALLBACK (brasero_burn_session_src_media_added),
						self);
	priv->src_removed_sig = g_signal_connect (drive,
						  "medium-removed",
						  G_CALLBACK (brasero_burn_session_src_media_removed),
						  self);
}

static void
brasero_burn_session_stop_src_drive_monitoring (BraseroBurnSession *self)
{
	BraseroDrive *drive;
	BraseroBurnSessionPrivate *priv;

	if (brasero_burn_session_get_input_type (self, NULL) != BRASERO_TRACK_TYPE_DISC)
		return;

	drive = brasero_burn_session_get_src_drive (self);
	if (!drive)
		return;

	priv = BRASERO_BURN_SESSION_PRIVATE (self);

	if (priv->src_added_sig) {
		g_signal_handler_disconnect (drive, priv->src_added_sig);
		priv->src_added_sig = 0;
	}

	if (priv->src_removed_sig) {
		g_signal_handler_disconnect (drive, priv->src_removed_sig);
		priv->src_removed_sig = 0;
	}
}

void
brasero_burn_session_free_tracks (BraseroBurnSession *self)
{
	BraseroBurnSessionPrivate *priv;

	g_return_if_fail (BRASERO_IS_BURN_SESSION (self));

	priv = BRASERO_BURN_SESSION_PRIVATE (self);

	brasero_burn_session_stop_src_drive_monitoring (self);

	g_slist_foreach (priv->tracks, (GFunc) brasero_track_unref, NULL);
	g_slist_free (priv->tracks);
	priv->tracks = NULL;

	g_signal_emit (self,
		       brasero_burn_session_signals [INPUT_CHANGED_SIGNAL],
		       0);
}

void
brasero_burn_session_clear_current_track (BraseroBurnSession *self)
{
	BraseroBurnSessionPrivate *priv;

	g_return_if_fail (BRASERO_IS_BURN_SESSION (self));

	priv = BRASERO_BURN_SESSION_PRIVATE (self);

	brasero_burn_session_stop_src_drive_monitoring (self);
	g_slist_foreach (priv->tracks, (GFunc) brasero_track_unref, NULL);
	g_slist_free (priv->tracks);
	priv->tracks = NULL;
}

BraseroBurnResult
brasero_burn_session_add_track (BraseroBurnSession *self,
				BraseroTrack *new_track)
{
	BraseroBurnSessionPrivate *priv;

	g_return_val_if_fail (BRASERO_IS_BURN_SESSION (self), BRASERO_BURN_ERR);

	priv = BRASERO_BURN_SESSION_PRIVATE (self);

	brasero_track_ref (new_track);
	if (!priv->tracks) {
		BraseroTrackType new_type;

		brasero_track_get_type (new_track, &new_type);

		/* we only need to emit the signal here since if there are
		 * multiple tracks they must be exactly of the same time */
		priv->tracks = g_slist_prepend (NULL, new_track);
		brasero_burn_session_start_src_drive_monitoring (self);

		/* if (!brasero_track_type_equal (priv->input, &new_type)) */
		g_signal_emit (self,
			       brasero_burn_session_signals [INPUT_CHANGED_SIGNAL],
			       0);

		return BRASERO_BURN_OK;
	}

	brasero_burn_session_stop_src_drive_monitoring (self);

	/* if there is already a track, then we replace it on condition that it
	 * has the same type and it's not AUDIO (only one allowed to have many)
	 */
	if (brasero_track_get_type (new_track, NULL) != BRASERO_TRACK_TYPE_AUDIO
	||  brasero_burn_session_get_input_type (self, NULL) != BRASERO_TRACK_TYPE_AUDIO) {
		g_slist_foreach (priv->tracks, (GFunc) brasero_track_unref, NULL);
		g_slist_free (priv->tracks);

		priv->tracks = g_slist_prepend (NULL, new_track);
		brasero_burn_session_start_src_drive_monitoring (self);

		g_signal_emit (self,
			       brasero_burn_session_signals [INPUT_CHANGED_SIGNAL],
			       0);
	}
	else
		priv->tracks = g_slist_append (priv->tracks, new_track);

	return BRASERO_BURN_OK;
}

GSList *
brasero_burn_session_get_tracks (BraseroBurnSession *self)
{
	BraseroBurnSessionPrivate *priv;

	g_return_val_if_fail (BRASERO_IS_BURN_SESSION (self), NULL);

	priv = BRASERO_BURN_SESSION_PRIVATE (self);

	return priv->tracks;
}

void
brasero_burn_session_set_input_type (BraseroBurnSession *self,
				     BraseroTrackType *type)
{
	BraseroBurnSessionPrivate *priv;
	BraseroTrackType input = { 0, };

	g_return_if_fail (BRASERO_IS_BURN_SESSION (self));
	g_return_if_fail (type != NULL);

	priv = BRASERO_BURN_SESSION_PRIVATE (self);

	brasero_burn_session_get_input_type (self, &input);
	memcpy (&priv->input, type, sizeof (BraseroTrackType));

	if (brasero_track_type_equal (&input, type))
		return;

	if (!priv->tracks)
		g_signal_emit (self,
			       brasero_burn_session_signals [INPUT_CHANGED_SIGNAL],
			       0);
}

BraseroTrackDataType
brasero_burn_session_get_input_type (BraseroBurnSession *self,
				     BraseroTrackType *type)
{
	BraseroTrack *track;
	BraseroBurnSessionPrivate *priv;

	g_return_val_if_fail (BRASERO_IS_BURN_SESSION (self), BRASERO_TRACK_TYPE_NONE);

	priv = BRASERO_BURN_SESSION_PRIVATE (self);

	if (!priv->tracks) {
		if (type)
			memcpy (type, &priv->input, sizeof (BraseroTrackType));

		return priv->input.type;
	}

	/* there can be many tracks (in case of audio) but they must be
	 * all of the same kind for the moment */
	track = priv->tracks->data;
	return brasero_track_get_type (track, type);
}

/**
 *
 */

static void
brasero_burn_session_dest_media_added (BraseroDrive *drive,
				       BraseroMedium *medium,
				       BraseroBurnSession *self)
{
	g_signal_emit (self,
		       brasero_burn_session_signals [OUTPUT_CHANGED_SIGNAL],
		       0);
}

static void
brasero_burn_session_dest_media_removed (BraseroDrive *drive,
					 BraseroMedium *medium,
					 BraseroBurnSession *self)
{
	g_signal_emit (self,
		       brasero_burn_session_signals [OUTPUT_CHANGED_SIGNAL],
		       0);
}

void
brasero_burn_session_set_burner (BraseroBurnSession *self,
				 BraseroDrive *drive)
{
	BraseroBurnSessionPrivate *priv;

	g_return_if_fail (BRASERO_IS_BURN_SESSION (self));

	priv = BRASERO_BURN_SESSION_PRIVATE (self);

	if (drive == priv->settings->burner)
		return;

	if (priv->settings->burner) {
		if (priv->dest_added_sig) {
			g_signal_handler_disconnect (priv->settings->burner,
						     priv->dest_added_sig);
			priv->dest_added_sig = 0;
		}

		if (priv->dest_removed_sig) {
			g_signal_handler_disconnect (priv->settings->burner,
						     priv->dest_removed_sig);
			priv->dest_removed_sig = 0;	
		}

		g_object_unref (priv->settings->burner);
	}

	if (drive) {
		priv->dest_added_sig = g_signal_connect (drive,
							 "medium-added",
							 G_CALLBACK (brasero_burn_session_dest_media_added),
							 self);
		priv->dest_removed_sig = g_signal_connect (drive,
							   "medium-removed",
							   G_CALLBACK (brasero_burn_session_dest_media_removed),
							   self);
		g_object_ref (drive);
	}

	priv->settings->burner = drive;

	g_signal_emit (self,
		       brasero_burn_session_signals [OUTPUT_CHANGED_SIGNAL],
		       0);
}

BraseroDrive *
brasero_burn_session_get_burner (BraseroBurnSession *self)
{
	BraseroBurnSessionPrivate *priv;

	g_return_val_if_fail (BRASERO_IS_BURN_SESSION (self), NULL);

	priv = BRASERO_BURN_SESSION_PRIVATE (self);
	return priv->settings->burner;
}

BraseroBurnResult
brasero_burn_session_set_rate (BraseroBurnSession *self, guint64 rate)
{
	BraseroBurnSessionPrivate *priv;

	g_return_val_if_fail (BRASERO_IS_BURN_SESSION (self), BRASERO_BURN_ERR);

	priv = BRASERO_BURN_SESSION_PRIVATE (self);

	if (!BRASERO_BURN_SESSION_WRITE_TO_DISC (priv))
		return BRASERO_BURN_ERR;

	priv->settings->rate = rate;
	return BRASERO_BURN_OK;
}

guint64
brasero_burn_session_get_rate (BraseroBurnSession *self)
{
	BraseroBurnSessionPrivate *priv;
	BraseroMedium *medium;
	gint64 max_rate;

	g_return_val_if_fail (BRASERO_IS_BURN_SESSION (self), 0);

	priv = BRASERO_BURN_SESSION_PRIVATE (self);

	if (!BRASERO_BURN_SESSION_WRITE_TO_DISC (priv))
		return 0;

	medium = brasero_drive_get_medium (priv->settings->burner);
	max_rate = brasero_medium_get_max_write_speed (medium);
	if (priv->settings->rate <= 0)
		return max_rate;
	else
		return MIN (max_rate, priv->settings->rate);
}

/**
 * This function returns a path only if we should output to a file image
 * and not burn.
 */

BraseroBurnResult
brasero_burn_session_get_output (BraseroBurnSession *self,
				 gchar **image_ret,
				 gchar **toc_ret,
				 GError **error)
{
	gchar *toc = NULL;
	gchar *image = NULL;
	BraseroBurnSessionPrivate *priv;

	g_return_val_if_fail (BRASERO_IS_BURN_SESSION (self), BRASERO_BURN_ERR);

	priv = BRASERO_BURN_SESSION_PRIVATE (self);

	if (!BRASERO_BURN_SESSION_WRITE_TO_FILE (priv)) {
		BRASERO_BURN_LOG ("no file disc");
		return BRASERO_BURN_ERR;
	}

	image = g_strdup (priv->settings->image);
	toc = g_strdup (priv->settings->toc);

	if (!image && !toc)
		return BRASERO_BURN_ERR;

	if (image_ret) {
		/* output paths were set so test them and returns them if OK */
		if (!image && toc) {
			gchar *complement;
			BraseroImageFormat format;

			/* get the cuesheet complement */
			format = brasero_burn_session_get_output_format (self);
			complement = brasero_image_format_get_complement (format, toc);
			if (!complement) {
				BRASERO_BURN_LOG ("no output specified");

				g_set_error (error,
					     BRASERO_BURN_ERROR,
					     BRASERO_BURN_ERROR_OUTPUT_NONE,
					     _("No path was specified for the image output"));

				g_free (toc);
				return BRASERO_BURN_ERR;
			}

			*image_ret = complement;
		}
		else if (image)
			*image_ret = image;
		else {
			BRASERO_BURN_LOG ("no output specified");

			g_set_error (error,
				     BRASERO_BURN_ERROR,
				     BRASERO_BURN_ERROR_OUTPUT_NONE,
				     _("No path was specified for the image output"));
			return BRASERO_BURN_ERR;
		}
	}
	else
		g_free (image);

	if (toc_ret)
		*toc_ret = toc;
	else
		g_free (toc);

	return BRASERO_BURN_OK;
}

BraseroImageFormat
brasero_burn_session_get_output_format (BraseroBurnSession *self)
{
	BraseroBurnSessionPrivate *priv;

	g_return_val_if_fail (BRASERO_IS_BURN_SESSION (self), BRASERO_IMAGE_FORMAT_NONE);

	priv = BRASERO_BURN_SESSION_PRIVATE (self);

	if (!BRASERO_BURN_SESSION_WRITE_TO_FILE (priv))
		return BRASERO_IMAGE_FORMAT_NONE;

	return priv->settings->format;
}

/**
 * This function allows to tell where we should write the image. Depending on
 * the type of image it can be a toc (cue) or the path of the image (all others)
 */

BraseroBurnResult
brasero_burn_session_set_image_output_full (BraseroBurnSession *self,
					    BraseroImageFormat format,
					    const gchar *image,
					    const gchar *toc)
{
	BraseroBurnSessionPrivate *priv;

	g_return_val_if_fail (BRASERO_IS_BURN_SESSION (self), BRASERO_BURN_ERR);

	priv = BRASERO_BURN_SESSION_PRIVATE (self);

	if (priv->settings->format == format
	&&  BRASERO_STR_EQUAL (image, priv->settings->image)
	&&  BRASERO_STR_EQUAL (toc, priv->settings->toc)) {
		if (!BRASERO_BURN_SESSION_WRITE_TO_FILE (priv)) {
			BraseroMediumMonitor *monitor;
			BraseroDrive *drive;
			GSList *list;

			monitor = brasero_medium_monitor_get_default ();
			list = brasero_medium_monitor_get_media (monitor, BRASERO_MEDIA_TYPE_FILE);
			drive = brasero_medium_get_drive (list->data);
			brasero_burn_session_set_burner (self, drive);
			g_object_unref (monitor);
			g_slist_free (list);
		}

		return BRASERO_BURN_OK;
	}

	if (priv->settings->image)
		g_free (priv->settings->image);

	if (image)
		priv->settings->image = g_strdup (image);
	else
		priv->settings->image = NULL;

	if (priv->settings->toc)
		g_free (priv->settings->toc);

	if (toc)
		priv->settings->toc = g_strdup (toc);
	else
		priv->settings->toc = NULL;

	priv->settings->format = format;

	if (!BRASERO_BURN_SESSION_WRITE_TO_FILE (priv)) {
		BraseroMediumMonitor *monitor;
		BraseroDrive *drive;
		GSList *list;

		monitor = brasero_medium_monitor_get_default ();
		list = brasero_medium_monitor_get_media (monitor,BRASERO_MEDIA_TYPE_FILE);
		drive = brasero_medium_get_drive (list->data);
		brasero_burn_session_set_burner (self, drive);
		g_object_unref (monitor);
		g_slist_free (list);
	}
	else
		g_signal_emit (self,
			       brasero_burn_session_signals [OUTPUT_CHANGED_SIGNAL],
			       0);

	return BRASERO_BURN_OK;
}

/**
 *
 */

BraseroBurnResult
brasero_burn_session_set_tmpdir (BraseroBurnSession *self,
				 const gchar *path)
{
	BraseroBurnSessionPrivate *priv;

	g_return_val_if_fail (BRASERO_IS_BURN_SESSION (self), BRASERO_BURN_ERR);

	priv = BRASERO_BURN_SESSION_PRIVATE (self);

	if (priv->settings->tmpdir)
		g_free (priv->settings->tmpdir);

	if (path)
		priv->settings->tmpdir = g_strdup (path);
	else
		priv->settings->tmpdir = NULL;

	return BRASERO_BURN_OK;
}

const gchar *
brasero_burn_session_get_tmpdir (BraseroBurnSession *self)
{
	BraseroBurnSessionPrivate *priv;

	g_return_val_if_fail (BRASERO_IS_BURN_SESSION (self), NULL);

	priv = BRASERO_BURN_SESSION_PRIVATE (self);
	return priv->settings->tmpdir? priv->settings->tmpdir:g_get_tmp_dir ();
}

BraseroBurnResult
brasero_burn_session_get_tmp_dir (BraseroBurnSession *self,
				  gchar **path,
				  GError **error)
{
	gchar *tmp;
	const gchar *tmpdir;
	BraseroBurnSessionPrivate *priv;

	g_return_val_if_fail (BRASERO_IS_BURN_SESSION (self), BRASERO_BURN_ERR);

	priv = BRASERO_BURN_SESSION_PRIVATE (self);

	/* create a working directory in tmp */
	tmpdir = priv->settings->tmpdir ?
		 priv->settings->tmpdir :
		 g_get_tmp_dir ();

	tmp = g_build_path (G_DIR_SEPARATOR_S,
			    tmpdir,
			    BRASERO_BURN_TMP_FILE_NAME,
			    NULL);

	*path = mkdtemp (tmp);
	if (*path == NULL) {
                int errsv = errno;

		BRASERO_BURN_LOG ("Impossible to create tmp directory");
		g_free (tmp);
		if (errsv != EACCES)
			g_set_error (error, 
				     BRASERO_BURN_ERROR,
				     BRASERO_BURN_ERROR_GENERAL,
				     "%s",
				     g_strerror (errsv));
		else
			g_set_error (error,
				     BRASERO_BURN_ERROR,
				     BRASERO_BURN_ERROR_PERMISSION,
				     _("You do not have the required permission to write at this location"));
		return BRASERO_BURN_ERR;
	}

	/* this must be removed when session is completly unreffed */
	priv->tmpfiles = g_slist_prepend (priv->tmpfiles, g_strdup (tmp));

	return BRASERO_BURN_OK;
}

BraseroBurnResult
brasero_burn_session_get_tmp_file (BraseroBurnSession *self,
				   const gchar *suffix,
				   gchar **path,
				   GError **error)
{
	BraseroBurnSessionPrivate *priv;
	const gchar *tmpdir;
	gchar *name;
	gchar *tmp;
	int fd;

	g_return_val_if_fail (BRASERO_IS_BURN_SESSION (self), BRASERO_BURN_ERR);

	priv = BRASERO_BURN_SESSION_PRIVATE (self);

	if (!path)
		return BRASERO_BURN_OK;

	/* takes care of the output file */
	tmpdir = priv->settings->tmpdir ?
		 priv->settings->tmpdir :
		 g_get_tmp_dir ();

	name = g_strconcat (BRASERO_BURN_TMP_FILE_NAME, suffix, NULL);
	tmp = g_build_path (G_DIR_SEPARATOR_S,
			    tmpdir,
			    name,
			    NULL);
	g_free (name);

	fd = g_mkstemp (tmp);
	if (fd == -1) {
                int errsv = errno;

		g_free (tmp);
		BRASERO_BURN_LOG ("Impossible to create tmp file");
		if (errsv != EACCES)
			g_set_error (error, 
				     BRASERO_BURN_ERROR,
				     BRASERO_BURN_ERROR_GENERAL,
				     "%s",
				     g_strerror (errsv));
		else
			g_set_error (error, 
				     BRASERO_BURN_ERROR,
				     BRASERO_BURN_ERROR_PERMISSION,
				     _("You do not have the required permission to write at this location"));

		return BRASERO_BURN_ERR;
	}

	/* this must be removed when session is completly unreffed */
	priv->tmpfiles = g_slist_prepend (priv->tmpfiles,
					  g_strdup (tmp));

	close (fd);
	*path = tmp;
	return BRASERO_BURN_OK;
}

static gchar *
brasero_burn_session_get_image_complement (BraseroBurnSession *self,
					   BraseroImageFormat format,
					   const gchar *path)
{
	gchar *retval = NULL;
	BraseroBurnSessionPrivate *priv;

	priv = BRASERO_BURN_SESSION_PRIVATE (self);

	if (format == BRASERO_IMAGE_FORMAT_CLONE)
		retval = g_strdup_printf ("%s.toc", path);
	else if (format == BRASERO_IMAGE_FORMAT_CUE) {
		if (g_str_has_suffix (path, ".bin"))
			retval = g_strdup_printf ("%.*scue",
						  strlen (path) - 3,
						  path);
		else
			retval = g_strdup_printf ("%s.cue", path);
	}
	else if (format == BRASERO_IMAGE_FORMAT_CDRDAO) {
		if (g_str_has_suffix (path, ".bin"))
			retval = g_strdup_printf ("%.*stoc",
						  strlen (path) - 3,
						  path);
		else
			retval = g_strdup_printf ("%s.toc", path);
	}
	else
		retval = NULL;

	return retval;
}

BraseroBurnResult
brasero_burn_session_get_tmp_image (BraseroBurnSession *self,
				    BraseroImageFormat format,
				    gchar **image,
				    gchar **toc,
				    GError **error)
{
	BraseroBurnSessionPrivate *priv;
	BraseroBurnResult result;
	gchar *complement = NULL;
	gchar *path = NULL;

	g_return_val_if_fail (BRASERO_IS_BURN_SESSION (self), BRASERO_BURN_ERR);

	priv = BRASERO_BURN_SESSION_PRIVATE (self);

	/* Image tmp file */
	result = brasero_burn_session_get_tmp_file (self,
						    (format == BRASERO_IMAGE_FORMAT_CLONE)? NULL:".bin",
						    &path,
						    error);
	if (result != BRASERO_BURN_OK)
		return result;

	if (format != BRASERO_IMAGE_FORMAT_BIN) {
		/* toc tmp file */
		complement = brasero_burn_session_get_image_complement (self, format, path);
		if (complement) {
			/* That shouldn't happen ... */
			if (g_file_test (complement, G_FILE_TEST_EXISTS)) {
				g_free (complement);
				return BRASERO_BURN_ERR;
			}
		}
	}

	if (complement)
		priv->tmpfiles = g_slist_prepend (priv->tmpfiles,
						  g_strdup (complement));

	if (image)
		*image = path;
	else
		g_free (path);

	if (toc)
		*toc = complement;
	else
		g_free (complement);

	return BRASERO_BURN_OK;
}

/**
 * used to modify session flags.
 */

void
brasero_burn_session_set_flags (BraseroBurnSession *self,
			        BraseroBurnFlag flags)
{
	BraseroBurnSessionPrivate *priv;

	g_return_if_fail (BRASERO_IS_BURN_SESSION (self));

	priv = BRASERO_BURN_SESSION_PRIVATE (self);
	priv->settings->flags = flags;
}

void
brasero_burn_session_add_flag (BraseroBurnSession *self,
			       BraseroBurnFlag flag)
{
	BraseroBurnSessionPrivate *priv;

	g_return_if_fail (BRASERO_IS_BURN_SESSION (self));

	if (flag & BRASERO_BURN_FLAG_BURNPROOF)
		g_warning ("REACHEd\n");
	priv = BRASERO_BURN_SESSION_PRIVATE (self);
	priv->settings->flags |= flag;
}

void
brasero_burn_session_remove_flag (BraseroBurnSession *self,
				  BraseroBurnFlag flag)
{
	BraseroBurnSessionPrivate *priv;

	g_return_if_fail (BRASERO_IS_BURN_SESSION (self));

	priv = BRASERO_BURN_SESSION_PRIVATE (self);
	priv->settings->flags &= ~flag;
}

BraseroBurnFlag
brasero_burn_session_get_flags (BraseroBurnSession *self)
{
	BraseroBurnSessionPrivate *priv;

	g_return_val_if_fail (BRASERO_IS_BURN_SESSION (self), BRASERO_BURN_ERR);

	priv = BRASERO_BURN_SESSION_PRIVATE (self);
	return priv->settings->flags;
}

/**
 * Used to set the label or the title of an album. 
 */
 
void
brasero_burn_session_set_label (BraseroBurnSession *self,
				const gchar *label)
{
	BraseroBurnSessionPrivate *priv;

	g_return_if_fail (BRASERO_IS_BURN_SESSION (self));

	priv = BRASERO_BURN_SESSION_PRIVATE (self);
	if (priv->settings->label)
		g_free (priv->settings->label);

	priv->settings->label = NULL;

	if (label) {
		if (strlen (label) > 32) {
			const gchar *delim;
			gchar *next_char;

			/* find last possible character. We can't just do a tmp 
			 * + 32 since we don't know if we are at the start of a
			 * character */
			delim = label;
			while ((next_char = g_utf8_find_next_char (delim, NULL))) {
				if (next_char - label > 32)
					break;

				delim = next_char;
			}

			priv->settings->label = g_strndup (label, delim - label);
		}
		else
			priv->settings->label = g_strdup (label);
	}
}

const gchar *
brasero_burn_session_get_label (BraseroBurnSession *self)
{
	BraseroBurnSessionPrivate *priv;

	g_return_val_if_fail (BRASERO_IS_BURN_SESSION (self), NULL);

	priv = BRASERO_BURN_SESSION_PRIVATE (self);
	return priv->settings->label;
}

static void
brasero_burn_session_tag_value_free (gpointer user_data)
{
	GValue *value = user_data;

	g_value_reset (value);
	g_free (value);
}

BraseroBurnResult
brasero_burn_session_tag_remove (BraseroBurnSession *self,
				 const gchar *tag)
{
	BraseroBurnSessionPrivate *priv;

	g_return_val_if_fail (BRASERO_IS_BURN_SESSION (self), BRASERO_BURN_ERR);

	priv = BRASERO_BURN_SESSION_PRIVATE (self);
	if (!priv->tags)
		return BRASERO_BURN_ERR;

	g_hash_table_remove (priv->tags, tag);
	return BRASERO_BURN_OK;
}

BraseroBurnResult
brasero_burn_session_tag_add (BraseroBurnSession *self,
			      const gchar *tag,
			      GValue *value)
{
	BraseroBurnSessionPrivate *priv;

	g_return_val_if_fail (BRASERO_IS_BURN_SESSION (self), BRASERO_BURN_ERR);

	priv = BRASERO_BURN_SESSION_PRIVATE (self);
	if (!priv->tags)
		priv->tags = g_hash_table_new_full (g_str_hash,
						    g_str_equal,
						    g_free,
						    brasero_burn_session_tag_value_free);
	g_hash_table_insert (priv->tags, g_strdup (tag), value);
	return BRASERO_BURN_OK;
}

BraseroBurnResult
brasero_burn_session_tag_lookup (BraseroBurnSession *self,
				 const gchar *tag,
				 GValue **value)
{
	BraseroBurnSessionPrivate *priv;
	gpointer data;

	g_return_val_if_fail (BRASERO_IS_BURN_SESSION (self), BRASERO_BURN_ERR);

	priv = BRASERO_BURN_SESSION_PRIVATE (self);
	if (!value)
		return BRASERO_BURN_ERR;

	if (!priv->tags)
		return BRASERO_BURN_ERR;

	data = g_hash_table_lookup (priv->tags, tag);
	if (!data)
		return BRASERO_BURN_ERR;

	*value = data;
	return BRASERO_BURN_OK;
}

/**
 * Used to save and restore settings/sources
 */

void
brasero_burn_session_push_settings (BraseroBurnSession *self)
{
	BraseroSessionSetting *settings;
	BraseroBurnSessionPrivate *priv;

	g_return_if_fail (BRASERO_IS_BURN_SESSION (self));

	priv = BRASERO_BURN_SESSION_PRIVATE (self);

	/* NOTE: don't clean the settings so no need to issue a signal */
	settings = g_new0 (BraseroSessionSetting, 1);
	brasero_session_settings_copy (settings, priv->settings);
	priv->pile_settings = g_slist_prepend (priv->pile_settings, settings);
}

void
brasero_burn_session_pop_settings (BraseroBurnSession *self)
{
	BraseroSessionSetting *settings;
	BraseroBurnSessionPrivate *priv;

	g_return_if_fail (BRASERO_IS_BURN_SESSION (self));

	priv = BRASERO_BURN_SESSION_PRIVATE (self);

	if (priv->dest_added_sig) {
		g_signal_handler_disconnect (priv->settings->burner,
					     priv->dest_added_sig);
		priv->dest_added_sig = 0;
	}

	if (priv->dest_removed_sig) {
		g_signal_handler_disconnect (priv->settings->burner,
					     priv->dest_removed_sig);
		priv->dest_removed_sig = 0;	
	}

	brasero_session_settings_clean (priv->settings);

	if (!priv->pile_settings)
		return;

	settings = priv->pile_settings->data;
	priv->pile_settings = g_slist_remove (priv->pile_settings, settings);
	brasero_session_settings_copy (priv->settings, settings);

	brasero_session_settings_free (settings);

	if (priv->settings->burner) {
		priv->dest_added_sig = g_signal_connect (priv->settings->burner,
							 "medium-added",
							 G_CALLBACK (brasero_burn_session_dest_media_added),
							 self);
		priv->dest_removed_sig = g_signal_connect (priv->settings->burner,
							   "medium-removed",
							   G_CALLBACK (brasero_burn_session_dest_media_removed),
							   self);
	}

	g_signal_emit (self,
		       brasero_burn_session_signals [OUTPUT_CHANGED_SIGNAL],
		       0);
}

void
brasero_burn_session_push_tracks (BraseroBurnSession *self)
{
	BraseroBurnSessionPrivate *priv;

	g_return_if_fail (BRASERO_IS_BURN_SESSION (self));

	priv = BRASERO_BURN_SESSION_PRIVATE (self);

	brasero_burn_session_stop_src_drive_monitoring (self);

	priv->pile_tracks = g_slist_prepend (priv->pile_tracks,
					     priv->tracks);
	priv->tracks = NULL;

	g_signal_emit (self,
		       brasero_burn_session_signals [INPUT_CHANGED_SIGNAL],
		       0);
}

void
brasero_burn_session_pop_tracks (BraseroBurnSession *self)
{
	GSList *sources;
	BraseroBurnSessionPrivate *priv;

	g_return_if_fail (BRASERO_IS_BURN_SESSION (self));

	priv = BRASERO_BURN_SESSION_PRIVATE (self);

	if (priv->tracks) {
		brasero_burn_session_stop_src_drive_monitoring (self);

		g_slist_foreach (priv->tracks, (GFunc) brasero_track_unref, NULL);
		g_slist_free (priv->tracks);
		priv->tracks = NULL;

		if (!priv->pile_tracks) {
			g_signal_emit (self,
				       brasero_burn_session_signals [INPUT_CHANGED_SIGNAL],
				       0);
			return;
		}
	}

	if (!priv->pile_tracks)
		return;

	sources = priv->pile_tracks->data;
	priv->pile_tracks = g_slist_remove (priv->pile_tracks, sources);
	priv->tracks = sources;

	brasero_burn_session_start_src_drive_monitoring (self);

	g_signal_emit (self,
		       brasero_burn_session_signals [INPUT_CHANGED_SIGNAL],
		       0);
}

/**
 *
 */

gboolean
brasero_burn_session_is_dest_file (BraseroBurnSession *self)
{
	BraseroBurnSessionPrivate *priv;

	g_return_val_if_fail (BRASERO_IS_BURN_SESSION (self), FALSE);

	priv = BRASERO_BURN_SESSION_PRIVATE (self);
	return BRASERO_BURN_SESSION_WRITE_TO_FILE (priv);
}

BraseroMedia
brasero_burn_session_get_dest_media (BraseroBurnSession *self)
{
	BraseroBurnSessionPrivate *priv;
	BraseroMedium *medium;

	g_return_val_if_fail (BRASERO_IS_BURN_SESSION (self), BRASERO_MEDIUM_NONE);

	priv = BRASERO_BURN_SESSION_PRIVATE (self);

	if (BRASERO_BURN_SESSION_WRITE_TO_FILE (priv))
		return BRASERO_MEDIUM_FILE;

	medium = brasero_drive_get_medium (priv->settings->burner);

	return brasero_medium_get_status (medium);
}

BraseroMedium *
brasero_burn_session_get_src_medium (BraseroBurnSession *self)
{
	BraseroTrack *track;
	BraseroBurnSessionPrivate *priv;

	g_return_val_if_fail (BRASERO_IS_BURN_SESSION (self), NULL);

	priv = BRASERO_BURN_SESSION_PRIVATE (self);

	/* to be able to burn to a DVD we must:
	 * - have only one track
	 * - not have any audio track */

	if (!priv->tracks)
		return NULL;

	if (g_slist_length (priv->tracks) != 1)
		return NULL;

	track = priv->tracks->data;
	if (brasero_track_get_type (track, NULL) != BRASERO_TRACK_TYPE_DISC)
		return NULL;

	return brasero_track_get_medium_source (track);
}

BraseroDrive *
brasero_burn_session_get_src_drive (BraseroBurnSession *self)
{
	BraseroTrack *track;
	BraseroBurnSessionPrivate *priv;

	g_return_val_if_fail (BRASERO_IS_BURN_SESSION (self), NULL);

	priv = BRASERO_BURN_SESSION_PRIVATE (self);

	/* to be able to burn to a DVD we must:
	 * - have only one track
	 * - not have any audio track */

	if (!priv->tracks)
		return NULL;

	if (g_slist_length (priv->tracks) != 1)
		return NULL;

	track = priv->tracks->data;
	if (brasero_track_get_type (track, NULL) != BRASERO_TRACK_TYPE_DISC)
		return NULL;

	return brasero_track_get_drive_source (track);
}

gboolean
brasero_burn_session_same_src_dest_drive (BraseroBurnSession *self)
{
	BraseroTrack *track;
	BraseroDrive *drive;
	BraseroBurnSessionPrivate *priv;

	g_return_val_if_fail (BRASERO_IS_BURN_SESSION (self), FALSE);

	priv = BRASERO_BURN_SESSION_PRIVATE (self);

	/* to be able to burn to a DVD we must:
	 * - have only one track
	 * - not have any audio track 
	 */

	if (!priv->tracks)
		return FALSE;

	if (g_slist_length (priv->tracks) > 1)
		return FALSE;

	track = priv->tracks->data;
	if (brasero_track_get_type (track, NULL) != BRASERO_TRACK_TYPE_DISC)
		return FALSE;

	drive = brasero_track_get_drive_source (track);
	if (!drive)
		return FALSE;

	return (priv->settings->burner == drive);
}


/****************************** this part is for log ***************************/
void
brasero_burn_session_logv (BraseroBurnSession *self,
			   const gchar *format,
			   va_list arg_list)
{
	gchar *message;
	gchar *offending;
	BraseroBurnSessionPrivate *priv;

	g_return_if_fail (BRASERO_IS_BURN_SESSION (self));

	priv = BRASERO_BURN_SESSION_PRIVATE (self);

	if (!format)
		return;

	if (!priv->session)
		return;

	message = g_strdup_vprintf (format, arg_list);

	/* we also need to validate the messages to be in UTF-8 */
	if (!g_utf8_validate (message, -1, (const gchar**) &offending))
		*offending = '\0';

	if (fwrite (message, strlen (message), 1, priv->session) != 1)
		g_warning ("Some log data couldn't be written: %s\n", message);

	g_free (message);

	fwrite ("\n", 1, 1, priv->session);
}

void
brasero_burn_session_log (BraseroBurnSession *self,
			  const gchar *format,
			  ...)
{
	va_list args;
	BraseroBurnSessionPrivate *priv;

	g_return_if_fail (BRASERO_IS_BURN_SESSION (self));

	priv = BRASERO_BURN_SESSION_PRIVATE (self);

	va_start (args, format);
	brasero_burn_session_logv (self, format, args);
	va_end (args);
}

void
brasero_burn_session_set_log_path (BraseroBurnSession *self,
				   const gchar *session_path)
{
	BraseroBurnSessionPrivate *priv;

	g_return_if_fail (BRASERO_IS_BURN_SESSION (self));

	priv = BRASERO_BURN_SESSION_PRIVATE (self);
	if (priv->session_path) {
		g_free (priv->session_path);
		priv->session_path = NULL;
	}

	if (session_path)
		priv->session_path = g_strdup (session_path);
}

const gchar *
brasero_burn_session_get_log_path (BraseroBurnSession *self)
{
	BraseroBurnSessionPrivate *priv;

	g_return_val_if_fail (BRASERO_IS_BURN_SESSION (self), NULL);

	priv = BRASERO_BURN_SESSION_PRIVATE (self);
	return priv->session_path;
}

gboolean
brasero_burn_session_start (BraseroBurnSession *self)
{
	BraseroTrackType type;
	BraseroBurnSessionPrivate *priv;

	g_return_val_if_fail (BRASERO_IS_BURN_SESSION (self), FALSE);

	priv = BRASERO_BURN_SESSION_PRIVATE (self);

	if (!priv->session_path) {
		int fd;
		const gchar *tmpdir;

		/* takes care of the output file */
		tmpdir = priv->settings->tmpdir ?
			 priv->settings->tmpdir :
			 g_get_tmp_dir ();

		/* This must obey the path of the temporary directory */
		priv->session_path = g_build_path (G_DIR_SEPARATOR_S,
						   tmpdir,
						   BRASERO_BURN_TMP_FILE_NAME,
						   NULL);

		fd = g_mkstemp (priv->session_path);
		priv->session = fdopen (fd, "w");
	}
	else
		priv->session = fopen (priv->session_path, "w");

	if (!priv->session) {
		g_warning ("Impossible to open a session file\n");
		return FALSE;
	}


	BRASERO_BURN_LOG ("Session starting:");

	brasero_burn_session_get_input_type (self, &type);
	BRASERO_BURN_LOG_TYPE (&type, "Input\t=");

	BRASERO_BURN_LOG_FLAGS (priv->settings->flags, "flags\t=");

	if (!brasero_burn_session_is_dest_file (self)) {
		BraseroMedium *medium;

		medium = brasero_drive_get_medium (priv->settings->burner);
		BRASERO_BURN_LOG_DISC_TYPE (brasero_medium_get_status (medium), "media type\t=");
		BRASERO_BURN_LOG ("speed\t= %i", priv->settings->rate);
	}
	else {
		type.type = BRASERO_TRACK_TYPE_IMAGE;
		type.subtype.img_format = brasero_burn_session_get_output_format (self);
		BRASERO_BURN_LOG_TYPE (&type, "output format\t=");
	}

	return TRUE;
}

void
brasero_burn_session_stop (BraseroBurnSession *self)
{
	BraseroBurnSessionPrivate *priv;

	g_return_if_fail (BRASERO_IS_BURN_SESSION (self));

	priv = BRASERO_BURN_SESSION_PRIVATE (self);
	if (priv->session) {
		fclose (priv->session);
		priv->session = NULL;
	}
}

/**
 *
 */

static void
brasero_burn_session_track_list_free (GSList *list)
{
	g_slist_foreach (list, (GFunc) brasero_track_unref, NULL);
	g_slist_free (list);
}

/**
 * Utility to clean tmp files
 */

static gboolean
brasero_burn_session_clean (const gchar *path);

static gboolean
brasero_burn_session_clean_directory (const gchar *path)
{
	GDir *dir;
	const gchar *name;

	dir = g_dir_open (path, 0, NULL);
	if (!dir)
		return FALSE;

	while ((name = g_dir_read_name (dir))) {
		gchar *tmp;

		tmp = g_build_filename (G_DIR_SEPARATOR_S,
					path,
					name,
					NULL);

		if (!brasero_burn_session_clean (tmp)) {
			g_dir_close (dir);
			g_free (tmp);
			return FALSE;
		}

		g_free (tmp);
	}

	g_dir_close (dir);
	return TRUE;
}

static gboolean
brasero_burn_session_clean (const gchar *path)
{
	gboolean result = TRUE;

	if (!path)
		return TRUE;

	BRASERO_BURN_LOG ("Cleaning %s", path);

	/* NOTE: g_file_test follows symbolic links */
	if (g_file_test (path, G_FILE_TEST_IS_DIR)
	&& !g_file_test (path, G_FILE_TEST_IS_SYMLINK))
		brasero_burn_session_clean_directory (path);

	/* NOTE : we don't follow paths as certain files are simply linked */
	if (g_remove (path)) {
		BRASERO_BURN_LOG ("Cannot remove file %s (%s)", path, g_strerror (errno));
		result = FALSE;
	}

	return result;
}

static void
brasero_burn_session_finalize (GObject *object)
{
	BraseroBurnSessionPrivate *priv;
	GSList *iter;

	priv = BRASERO_BURN_SESSION_PRIVATE (object);

	if (priv->tags) {
		g_hash_table_destroy (priv->tags);
		priv->tags = NULL;
	}

	if (priv->dest_added_sig) {
		g_signal_handler_disconnect (priv->settings->burner,
					     priv->dest_added_sig);
		priv->dest_added_sig = 0;
	}

	if (priv->dest_removed_sig) {
		g_signal_handler_disconnect (priv->settings->burner,
					     priv->dest_removed_sig);
		priv->dest_removed_sig = 0;	
	}

	brasero_burn_session_stop_src_drive_monitoring (BRASERO_BURN_SESSION (object));

	if (priv->pile_tracks) {
		g_slist_foreach (priv->pile_tracks,
				(GFunc) brasero_burn_session_track_list_free,
				NULL);

		g_slist_free (priv->pile_tracks);
		priv->pile_tracks = NULL;
	}

	if (priv->tracks) {
		g_slist_foreach (priv->tracks,
				 (GFunc) brasero_track_unref,
				 NULL);
		g_slist_free (priv->tracks);
		priv->tracks = NULL;
	}

	if (priv->pile_settings) {
		g_slist_foreach (priv->pile_settings,
				(GFunc) brasero_session_settings_free,
				NULL);
		g_slist_free (priv->pile_settings);
		priv->pile_settings = NULL;
	}

	/* clean tmpfiles */
	for (iter = priv->tmpfiles; iter; iter = iter->next) {
		gchar *tmpfile;

		tmpfile = iter->data;

		brasero_burn_session_clean (tmpfile);
		g_free (tmpfile);
	}
	g_slist_free (priv->tmpfiles);

	if (priv->session) {
		fclose (priv->session);
		priv->session = NULL;
	}

	if (priv->session_path) {
		g_remove (priv->session_path);
		g_free (priv->session_path);
		priv->session_path = NULL;
	}

	brasero_session_settings_clean (priv->settings);

	G_OBJECT_CLASS (parent_class)->finalize (object);
}

static void
brasero_burn_session_init (BraseroBurnSession *obj)
{ }

static void
brasero_burn_session_class_init (BraseroBurnSessionClass *klass)
{
	GObjectClass *object_class = G_OBJECT_CLASS (klass);

	g_type_class_add_private (klass, sizeof (BraseroBurnSessionPrivate));

	parent_class = g_type_class_peek_parent(klass);
	object_class->finalize = brasero_burn_session_finalize;

	/* This is to delay the setting of track source until we know all settings */
	brasero_burn_session_signals [OUTPUT_CHANGED_SIGNAL] =
	    g_signal_new ("output_changed",
			  BRASERO_TYPE_BURN_SESSION,
			  G_SIGNAL_RUN_FIRST,
			  G_STRUCT_OFFSET (BraseroBurnSessionClass, output_changed),
			  NULL,
			  NULL,
			  g_cclosure_marshal_VOID__VOID,
			  G_TYPE_NONE,
			  0);

	brasero_burn_session_signals [INPUT_CHANGED_SIGNAL] =
	    g_signal_new ("input_changed",
			  BRASERO_TYPE_BURN_SESSION,
			  G_SIGNAL_RUN_FIRST,
			  G_STRUCT_OFFSET (BraseroBurnSessionClass, input_changed),
			  NULL,
			  NULL,
			  g_cclosure_marshal_VOID__VOID,
			  G_TYPE_NONE,
			  0);
}

BraseroBurnSession *
brasero_burn_session_new ()
{
	BraseroBurnSession *obj;
	
	obj = BRASERO_BURN_SESSION (g_object_new (BRASERO_TYPE_BURN_SESSION, NULL));
	
	return obj;
}
