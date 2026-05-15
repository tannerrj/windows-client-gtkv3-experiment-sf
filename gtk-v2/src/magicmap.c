/*
 * Crossfire -- cooperative multi-player graphical RPG and adventure game
 *
 * Copyright (c) 1999-2013 Mark Wedel and the Crossfire Development Team
 * Copyright (c) 1992 Frank Tore Johansen
 *
 * Crossfire is free software and comes with ABSOLUTELY NO WARRANTY. You are
 * welcome to redistribute it under certain conditions. For details, see the
 * 'LICENSE' and 'COPYING' files.
 *
 * The authors can be reached via e-mail to crossfire-devel@real-time.com
 */

/**
 * @file
 * Covers drawing the magic map.
 */

#include <gtk/gtk.h>

#include "client.h"
#include "main.h"

/**
 * Render the entire magic map into the magic_map drawing area. Switches the
 * map notebook to the magic map page. Each tile is drawn as a filled rectangle
 * in the color indicated by its magic map value. Does nothing if the player
 * has no magic map data (cpl.magicmap == NULL).
 */
void draw_magic_map() {
    if (!cpl.magicmap) {
        // Do nothing if player has no magic map data.
        return;
    } else {
        cpl.showmagic = 1;
    }

    /*
     * Have to set this so that the gtk_widget_show below actually creates the
     * widget.  Switch to this page when person actually casts magic map spell.
     */
    gtk_notebook_set_current_page(GTK_NOTEBOOK(map_notebook), MAGIC_MAP_PAGE);

    GdkWindow *window = gtk_widget_get_window(magic_map);

    cpl.mapxres = gdk_window_get_width(window) / cpl.mmapx;
    cpl.mapyres = gdk_window_get_height(window) / cpl.mmapy;
    if (cpl.mapxres < 1 || cpl.mapyres < 1) {
        LOG(LOG_WARNING, "draw_magic_map",
            "magic map resolution less than 1, map is %dx%d", cpl.mmapx,
            cpl.mmapy);
        return;
    }

    /*
     * In theory, cpl.mapxres and cpl.mapyres do not have to be the same.
     * However, it probably makes sense to keep them the same value.  Need to
     * take the smaller value.
     */
    if (cpl.mapxres > cpl.mapyres) {
        cpl.mapxres = cpl.mapyres;
    } else {
        cpl.mapyres = cpl.mapxres;
    }

    cairo_t *cr = gdk_cairo_create(window);
    for (int y = 0; y < cpl.mmapy; y++) {
        for (int x = 0; x < cpl.mmapx; x++) {
            guint8 val = cpl.magicmap[y * cpl.mmapx + x];
            gdk_cairo_set_source_color(cr, &root_color[val & FACE_COLOR_MASK]);
            cairo_rectangle(cr, cpl.mapxres * x, cpl.mapyres * y, cpl.mapxres,
                            cpl.mapyres);
            cairo_fill(cr);
        }
    }
    cairo_destroy(cr);
}

/**
 * Flash the player's position on the magic map.
 */
void magic_map_flash_pos() {
    GdkWindow *window = gtk_widget_get_window(magic_map);
    cairo_t *cr = gdk_cairo_create(window);
    gdk_cairo_set_source_color(cr, &root_color[(cpl.showmagic & 2) ? 0 : 1]);
    cairo_rectangle(cr, cpl.mapxres * cpl.pmapx, cpl.mapyres * cpl.pmapy,
                    cpl.mapxres, cpl.mapyres);
    cairo_fill(cr);
    cairo_destroy(cr);
}

/**
 * GTK "draw" signal handler for the magic map drawing area. Redraws the magic
 * map on every expose event and returns FALSE to allow further signal
 * propagation.
 */
gboolean on_drawingarea_magic_map_expose_event() {
    draw_magic_map();
    return FALSE;
}
