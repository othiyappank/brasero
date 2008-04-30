/* -*- Mode: C; indent-tabs-mode: t; c-basic-offset: 8; tab-width: 8 -*- */
/*
 * trunk
 * Copyright (C) Philippe Rouquier 2008 <bonfire-app@wanadoo.fr>
 * 
 * trunk is free software: you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 * 
 * trunk is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details.
 * 
 * You should have received a copy of the GNU General Public License along
 * with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#ifdef HAVE_CONFIG_H
#  include <config.h>
#endif

#include <string.h>

#include <glib.h>
#include <glib-object.h>
#include <glib/gi18n.h>

#include <gtk/gtk.h>

#include <gst/gst.h>

#include "brasero-multi-song-props.h"
#include "brasero-rename.h"
#include "brasero-utils.h"

typedef struct _BraseroMultiSongPropsPrivate BraseroMultiSongPropsPrivate;
struct _BraseroMultiSongPropsPrivate
{
	GtkWidget *title;
	GtkWidget *artist;
	GtkWidget *composer;
	GtkWidget *isrc;
	GtkWidget *gap;
};


#define BRASERO_MULTI_SONG_PROPS_PRIVATE(o)  (G_TYPE_INSTANCE_GET_PRIVATE ((o), BRASERO_TYPE_MULTI_SONG_PROPS, BraseroMultiSongPropsPrivate))


G_DEFINE_TYPE (BraseroMultiSongProps, brasero_multi_song_props, GTK_TYPE_DIALOG);

void
brasero_multi_song_props_set_rename_callback (BraseroMultiSongProps *self,
					      GtkTreeSelection *selection,
					      gint column_num,
					      BraseroRenameCallback callback)
{
	BraseroMultiSongPropsPrivate *priv;

	priv = BRASERO_MULTI_SONG_PROPS_PRIVATE (self);

	brasero_rename_do (BRASERO_RENAME (priv->title),
			   selection,
			   column_num,
			   callback);
}

void
brasero_multi_song_props_get_properties (BraseroMultiSongProps *self,
					 gchar **artist,
					 gchar **composer,
					 gint *isrc,
					 gint64 *gap)
{
	const gchar *text;
	BraseroMultiSongPropsPrivate *priv;

	priv = BRASERO_MULTI_SONG_PROPS_PRIVATE (self);
	if (artist) {
		text = gtk_entry_get_text (GTK_ENTRY (priv->artist));
		if (text && strcmp (text, _("<keep current values>")))
			*artist = g_strdup (text);
		else
			*artist = NULL;
	}

	if (composer) {
		text = gtk_entry_get_text (GTK_ENTRY (priv->composer));
		if (text && strcmp (text, _("<keep current values>")))
			*composer = g_strdup (text);
		else
			*composer = NULL;
	}

	if (isrc) {
		text = gtk_entry_get_text (GTK_ENTRY (priv->isrc));
		if (text && strcmp (text, _("<keep current values>")))
			*isrc = (gint) g_strtod (text, NULL);
		else
			*isrc = -1;
	}

	if (gap)
		*gap = gtk_spin_button_get_value (GTK_SPIN_BUTTON (priv->gap)) * GST_SECOND;
}

static gboolean
brasero_multi_song_props_gap_output_cb (GtkSpinButton *spin,
					BraseroMultiSongProps *self)
{
	if (gtk_spin_button_get_value (spin) > 0.0)
		return FALSE;

	if (gtk_spin_button_get_value (spin) == -1.0)
		gtk_entry_set_text (GTK_ENTRY (spin), _("<keep current values>"));

	if (gtk_spin_button_get_value (spin) == 0.0)
		gtk_entry_set_text (GTK_ENTRY (spin), _("Remove silences"));

	return TRUE;
}

static guint
brasero_multi_song_props_gap_input_cb (GtkSpinButton *spin,
				       gdouble *val,
				       BraseroMultiSongProps *self)
{
	const gchar *text;

	text = gtk_entry_get_text (GTK_ENTRY (spin));
	if (text && !strcmp (text, _("<keep current values>"))) {
		*val = -1.0;
		return TRUE;
	}
	else if (text && !strcmp (text, _("Remove silences"))) {
		*val = 0.0;
		return TRUE;
	}

	return FALSE;
}

static gboolean
brasero_multi_song_props_entry_focus_out (GtkEntry *entry,
					  GdkEventFocus *event,
					  gpointer NULL_data)
{
	const gchar *text;

	text = gtk_entry_get_text (entry);
	if (!text || text [0] == '\0')
		gtk_entry_set_text (entry, _("<keep current values>"));

	return FALSE;
}

static gboolean
brasero_multi_song_props_entry_focus_in (GtkEntry *entry,
					 GdkEventFocus *event,
					 gpointer NULL_data)
{
	const gchar *text;

	text = gtk_entry_get_text (entry);
	if (text && !strcmp (text, _("<keep current values>")))
		gtk_entry_set_text (entry, "");

	return FALSE;
}

static void
brasero_multi_song_props_init (BraseroMultiSongProps *object)
{
	GtkWidget *box;
	GtkWidget *label;
	GtkWidget *table;
	GtkWidget *frame;
	BraseroMultiSongPropsPrivate *priv;

	priv = BRASERO_MULTI_SONG_PROPS_PRIVATE (object);

	gtk_dialog_set_has_separator (GTK_DIALOG (object), FALSE);
	gtk_box_set_spacing (GTK_BOX (GTK_DIALOG (object)->vbox), 0);
	gtk_window_set_default_size (GTK_WINDOW (object), 400, 200);

	priv->title = brasero_rename_new ();
	gtk_widget_show (priv->title);
	gtk_widget_set_tooltip_text (priv->title,
				     _("This information will be written to the disc using CD-TEXT technology. It can be read and displayed by some audio CD players."));

	frame = brasero_utils_pack_properties (_("<b>Song titles</b>"), priv->title, NULL);
	gtk_widget_show (frame);
	gtk_container_set_border_width (GTK_CONTAINER (frame), 6);
	gtk_box_pack_start (GTK_BOX (GTK_DIALOG (object)->vbox),
						frame,
						FALSE,
						FALSE,
						0);

	table = gtk_table_new (3, 2, FALSE);
	gtk_widget_show (table);
	gtk_table_set_row_spacings (GTK_TABLE (table), 4);
	gtk_table_set_col_spacings (GTK_TABLE (table), 6);

	frame = brasero_utils_pack_properties (_("<b>Additional song information</b>"), table, NULL);
	gtk_widget_show (frame);
	gtk_container_set_border_width (GTK_CONTAINER (frame), 6);
	gtk_box_pack_start (GTK_BOX (GTK_DIALOG (object)->vbox),
						frame,
						FALSE,
						FALSE,
						0);

	label = gtk_label_new (_("Artist:"));
	gtk_widget_show (label);
	gtk_misc_set_alignment (GTK_MISC (label), 0.0, 0.5);
	gtk_table_attach (GTK_TABLE (table), label, 0, 1, 1, 2, GTK_FILL, GTK_FILL, 0, 0);

	priv->artist = gtk_entry_new ();
	gtk_widget_show (priv->artist);
	gtk_entry_set_text (GTK_ENTRY (priv->artist), _("<keep current values>"));
	gtk_table_attach_defaults (GTK_TABLE (table), priv->artist, 1, 2, 1, 2);
	gtk_widget_set_tooltip_text (priv->artist,
				     _("This information will be written to the disc using CD-TEXT technology. It can be read and displayed by some audio CD players."));
	g_signal_connect (priv->artist,
			  "focus-in-event",
			  G_CALLBACK (brasero_multi_song_props_entry_focus_in),
			  NULL);

	g_signal_connect (priv->artist,
			  "focus-out-event",
			  G_CALLBACK (brasero_multi_song_props_entry_focus_out),
			  NULL);

	label = gtk_label_new (_("Composer:\t"));
	gtk_widget_show (label);
	gtk_misc_set_alignment (GTK_MISC (label), 0.0, 0.5);
	gtk_table_attach (GTK_TABLE (table), label, 0, 1, 2, 3, GTK_FILL, GTK_FILL, 0, 0);

	priv->composer = gtk_entry_new ();
	gtk_widget_show (priv->composer);
	gtk_entry_set_text (GTK_ENTRY (priv->composer), _("<keep current values>"));
	gtk_table_attach_defaults (GTK_TABLE (table), priv->composer, 1, 2, 2, 3);
	gtk_widget_set_tooltip_text (priv->composer,
				     _("This information will be written to the disc using CD-TEXT technology. It can be read and displayed by some audio CD players."));
	g_signal_connect (priv->composer,
			  "focus-in-event",
			  G_CALLBACK (brasero_multi_song_props_entry_focus_in),
			  NULL);

	g_signal_connect (priv->composer,
			  "focus-out-event",
			  G_CALLBACK (brasero_multi_song_props_entry_focus_out),
			  NULL);

	label = gtk_label_new ("ISRC:");
	gtk_widget_show (label);
	gtk_misc_set_alignment (GTK_MISC (label), 0.0, 0.5);
	gtk_table_attach (GTK_TABLE (table), label, 0, 1, 3, 4, GTK_FILL, GTK_FILL, 0, 0);

	priv->isrc = gtk_entry_new ();
	gtk_widget_show (priv->isrc);
	gtk_entry_set_text (GTK_ENTRY (priv->isrc), _("<keep current values>"));
	gtk_table_attach_defaults (GTK_TABLE (table), priv->isrc, 1, 2, 3, 4);

	g_signal_connect (priv->isrc,
			  "focus-in-event",
			  G_CALLBACK (brasero_multi_song_props_entry_focus_in),
			  NULL);

	g_signal_connect (priv->isrc,
			  "focus-out-event",
			  G_CALLBACK (brasero_multi_song_props_entry_focus_out),
			  NULL);

	/* second part of the dialog */
	box = gtk_hbox_new (FALSE, 6);
	gtk_widget_show (box);

	frame = brasero_utils_pack_properties (_("<b>Options</b>"), box, NULL);
	gtk_widget_show (frame);
	gtk_container_set_border_width (GTK_CONTAINER (frame), 6);
	gtk_box_pack_start (GTK_BOX (GTK_DIALOG (object)->vbox), frame, FALSE, FALSE, 0);

	label = gtk_label_new (_("Pause length:\t"));
	gtk_widget_show (label);
	gtk_misc_set_alignment (GTK_MISC (label), 0.0, 0.5);

	priv->gap = gtk_spin_button_new_with_range (-1.0, 100.0, 1.0);
	gtk_widget_show (priv->gap);
	gtk_spin_button_set_numeric (GTK_SPIN_BUTTON (priv->gap), FALSE);

	gtk_box_pack_start (GTK_BOX (box), label, FALSE, FALSE, 0);
	gtk_box_pack_start (GTK_BOX (box), priv->gap, TRUE, TRUE, 0);
	gtk_widget_set_tooltip_text (priv->gap,
				     _("Gives the length of the pause that should follow the track"));

	g_signal_connect (priv->gap,
			  "output",
			  G_CALLBACK (brasero_multi_song_props_gap_output_cb),
			  object);
	g_signal_connect (priv->gap,
			  "input",
			  G_CALLBACK (brasero_multi_song_props_gap_input_cb),
			  object);

	/* buttons */
	gtk_dialog_add_buttons (GTK_DIALOG (object),
				GTK_STOCK_CANCEL, GTK_RESPONSE_CANCEL,
				GTK_STOCK_APPLY, GTK_RESPONSE_ACCEPT,
				NULL);

	gtk_window_set_title (GTK_WINDOW (object), _("Song information"));
}

static void
brasero_multi_song_props_finalize (GObject *object)
{
	G_OBJECT_CLASS (brasero_multi_song_props_parent_class)->finalize (object);
}

static void
brasero_multi_song_props_class_init (BraseroMultiSongPropsClass *klass)
{
	GObjectClass *object_class = G_OBJECT_CLASS (klass);

	g_type_class_add_private (klass, sizeof (BraseroMultiSongPropsPrivate));

	object_class->finalize = brasero_multi_song_props_finalize;
}

GtkWidget *
brasero_multi_song_props_new (void)
{
	return g_object_new (BRASERO_TYPE_MULTI_SONG_PROPS, NULL);
}
