/* ui_util.c
 * UI utility routines
 *
 * $Id: ui_util.c,v 1.5 2001/12/12 21:38:59 gerald Exp $
 *
 * Ethereal - Network traffic analyzer
 * By Gerald Combs <gerald@zing.org>
 * Copyright 1998 Gerald Combs
 *
 * 
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 * 
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 * 
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.
 */

#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include <glib.h>

#include <gtk/gtk.h>

#include "gtkglobals.h"
#include "ui_util.h"
#include "../ui_util.h"
#include "image/eicon3d16.xpm"


/* Set the name of the top-level window and its icon.
   XXX - for some reason, KWM insists on making the icon name be just
   the window name, in parentheses; perhaps it's trying to imitate
   Windows here, or perhaps it's not the icon name that appears in
   the taskbar.  The KWM_WIN_TITLE string overrides that, but I
   don't know how that gets set - it's set on "xterm"s, but they
   aren't KWM-aware, as far as I know. */
void
set_main_window_name(gchar *icon_name)
{
  gtk_window_set_title(GTK_WINDOW(top_level), icon_name);
  gdk_window_set_icon_name(top_level->window, icon_name);
}

/* Given a pointer to a GtkWidget for a top-level window, raise it and
   de-iconify it.  This routine is used if the user has done something to
   ask that a window of a certain type be popped up when there can be only
   one such window and such a window has already been popped up - we
   pop up the existing one rather than creating a new one.

   XXX - we should request that it be given the input focus, too.  Alas,
   GDK has nothing to do that, e.g. by calling "XSetInputFocus()" in a
   window in X.

   XXX - will this do the right thing on window systems other than X? */
void
reactivate_window(GtkWidget *win)
{
  gdk_window_show(win->window);
  gdk_window_raise(win->window);
}

/* Set our window icon.  The GDK documentation doesn't provide any
   actual documentation for gdk_window_set_icon(), so we'll steal
   libgimp/gimpdialog.c:gimp_dialog_realize_callback() from the Gimp
   sources and assume it's safe. 
   
   XXX - The current icon size is fixed at 16x16 pixels, which looks fine
   in KDE and GNOME.  Some windowing environments (e.g. CDE) have larger
   icon sizes, so we need to find a way to size our icon appropriately.
   
   */
void
window_icon_realize_cb (GtkWidget *win, gpointer data) 
{
#ifndef WIN32
  static GdkPixmap *icon_pmap = NULL;
  static GdkBitmap *icon_mask = NULL;
  GtkStyle         *style;

  style = gtk_widget_get_style (win);

  if (icon_pmap == NULL) {
    icon_pmap = gdk_pixmap_create_from_xpm_d (win->window,
		&icon_mask, &style->bg[GTK_STATE_NORMAL], eicon3d16_xpm);
  }

  gdk_window_set_icon (win->window, NULL, icon_pmap, icon_mask);
#endif
}
