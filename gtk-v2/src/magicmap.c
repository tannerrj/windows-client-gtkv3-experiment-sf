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
 * Request a redraw of the magic map and switch the notebook to the magic map
 * page. Sets cpl.showmagic and computes tile resolution from the current
 * widget allocation. Does nothing if cpl.magicmap is NULL.
 */
void draw_magic_map() {
    if (!cpl.magicmap) {
        return;
    } else {
        cpl.showmagic = 1;
    }

    gtk_notebook_set_current_page(GTK_NOTEBOOK(map_notebook), MAGIC_MAP_PAGE);

    int width = gtk_widget_get_allocated_width(magic_map);
    int height = gtk_widget_get_allocated_height(magic_map);

    cpl.mapxres = width / cpl.mmapx;
    cpl.mapyres = height / cpl.mmapy;
    if (cpl.mapxres < 1 || cpl.mapyres < 1) {
        LOG(LOG_WARNING, "draw_magic_map",
            "magic map resolution less than 1, map is %dx%d", cpl.mmapx,
            cpl.mmapy);
        return;
    }

    if (cpl.mapxres > cpl.mapyres) {
        cpl.mapxres = cpl.mapyres;
    } else {
        cpl.mapyres = cpl.mapxres;
    }

    gtk_widget_queue_draw(magic_map);
}

/**
 * Request a redraw of the player position flash on the magic map.
 */
void magic_map_flash_pos() {
    gtk_widget_queue_draw(magic_map);
}

/**
 * GTK "draw" signal handler for the magic map drawing area. Renders all magic
 * map tiles and the player position flash using the cairo context provided by
 * the signal. Returns FALSE to allow further signal propagation.
 *
 * @param widget The magic map drawing area.
 * @param cr     Cairo context clipped to the drawing area.
 */
gboolean on_drawingarea_magic_map_expose_event(GtkWidget *widget, cairo_t *cr) {
    if (!cpl.magicmap) {
        return FALSE;
    }

    int width = gtk_widget_get_allocated_width(widget);
    int height = gtk_widget_get_allocated_height(widget);

    cpl.mapxres = width / cpl.mmapx;
    cpl.mapyres = height / cpl.mmapy;
    if (cpl.mapxres < 1 || cpl.mapyres < 1) {
        return FALSE;
    }
    if (cpl.mapxres > cpl.mapyres) {
        cpl.mapxres = cpl.mapyres;
    } else {
        cpl.mapyres = cpl.mapxres;
    }

    for (int y = 0; y < cpl.mmapy; y++) {
        for (int x = 0; x < cpl.mmapx; x++) {
            guint8 val = cpl.magicmap[y * cpl.mmapx + x];
            gdk_cairo_set_source_rgba(cr, &root_color[val & FACE_COLOR_MASK]);
            cairo_rectangle(cr, cpl.mapxres * x, cpl.mapyres * y,
                            cpl.mapxres, cpl.mapyres);
            cairo_fill(cr);
        }
    }

    gdk_cairo_set_source_rgba(cr, &root_color[(cpl.showmagic & 2) ? 0 : 1]);
    cairo_rectangle(cr, cpl.mapxres * cpl.pmapx, cpl.mapyres * cpl.pmapy,
                    cpl.mapxres, cpl.mapyres);
    cairo_fill(cr);

    return FALSE;
}
