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
 * Initializes and renders the map drawing area. Handles map drawing area
 * events, notably the "click to look at" mouse event.
 */

#include "client.h"

#include <math.h>
#include <gtk/gtk.h>

#include "image.h"
#include "main.h"
#include "mapdata.h"
#include "gtk2proto.h"

/* Configuration (from config.c) */
extern int predict_alpha;
extern bool time_map_redraw;

int map_image_size = DEFAULT_IMAGE_SIZE;
static gboolean map_updated = FALSE;

GtkWidget *map_notebook;
static GtkWidget *map_drawing_area;
/* tile_surface holds tiles + labels without any sub-tile translation or darkness
 * baked in.  It is the surface that gets blit-shifted by display_mapscroll().
 * map_surface is the final composited output: tile_surface shifted by
 * global_offset_x/y, with the darkness overlay applied on top. */
static cairo_surface_t *tile_surface = NULL;
static cairo_surface_t *map_surface  = NULL;

/* Scroll-blit state set by display_mapscroll() and consumed by gtk_map_redraw(). */
static gboolean map_scrolled  = FALSE;
static int      map_scroll_dx = 0;
static int      map_scroll_dy = 0;

/* Cached light-map surface for draw_darkness().  Reallocated only when the
 * number of visible tiles changes; its pixel content is overwritten each frame
 * via direct buffer writes rather than per-pixel Cairo drawing calls. */
static cairo_surface_t *lm_surface = NULL;
static int              lm_last_nx = -1;
static int              lm_last_ny = -1;

/* Viewport state used by dirty-region tracking to detect when a full redraw
 * is required vs. when visible tiles are unchanged and the frame can be skipped. */
static int    last_mx_start = G_MININT;
static int    last_my_start = G_MININT;
static double last_ew = 0.0;
static double last_eh = 0.0;

// Forward declarations for events
static gboolean map_button_event(GtkWidget *widget,
        GdkEventButton *event, gpointer user_data);
static gboolean map_expose_event(GtkWidget *widget,
        cairo_t *cr, gpointer user_data);

// Font for drawing player labels
static cairo_font_face_t *font;

/**
 * Called before entering the sandbox to cache things like fonts.
 */
void map_pre_sandbox_init() {
    // TODO: This is technically missing a cairo_font_face_destroy()
    font = cairo_toy_font_face_create("", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);

    const char *test_text = "TEST TEXT";
    cairo_surface_t *cst = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 10, 10);
    cairo_t *cr = cairo_create(cst);
    cairo_set_font_face(cr, font);
    cairo_text_extents_t extents;
    cairo_text_extents(cr, test_text, &extents);
    cairo_show_text(cr, test_text);
    cairo_destroy(cr);
    cairo_surface_destroy(cst);
}

/**
 * Calculate and set desired map size based on map window size.
 */
void map_check_resize() {
    if (!GTK_IS_WIDGET(map_drawing_area)) {
        // Called by config_check(), but main window layout not yet loaded.
        return;
    }

    GtkAllocation size;
    gtk_widget_get_allocation(map_drawing_area, &size);
    int scaled_size = map_image_size * use_config[CONFIG_MAPSCALE] / 100;
    int w = size.width / scaled_size + 1;
    int h = size.height / scaled_size + 1;
    w = (w > MAP_MAX_SIZE) ? MAP_MAX_SIZE : w;
    h = (h > MAP_MAX_SIZE) ? MAP_MAX_SIZE : h;

    // If request would be even, make it odd so player is centered.
    if (w % 2 == 0) {
        w += 1;
    }

    if (h % 2 == 0) {
        h += 1;
    }

    if (w != want_config[CONFIG_MAPWIDTH] || h != want_config[CONFIG_MAPHEIGHT]) {
        want_config[CONFIG_MAPWIDTH] = w;
        want_config[CONFIG_MAPHEIGHT] = h;
        if (csocket.fd) {
            client_mapsize(w, h);
        }
    }
}

/**
 * This initializes the stuff we need for the map.
 *
 * @param window_root The client's main playing window.
 */
void map_init(GtkWidget *window_root) {
    /* Reset all rendering state so the first frame after (re)connect forces a
     * full redraw regardless of any stale last_* values. */
    if (tile_surface) { cairo_surface_destroy(tile_surface); tile_surface = NULL; }
    if (map_surface)  { cairo_surface_destroy(map_surface);  map_surface  = NULL; }
    if (lm_surface)   { cairo_surface_destroy(lm_surface);   lm_surface   = NULL; }
    lm_last_nx = -1;
    lm_last_ny = -1;
    map_scrolled  = FALSE;
    map_scroll_dx = 0;
    map_scroll_dy = 0;
    last_mx_start = G_MININT;
    last_my_start = G_MININT;
    last_ew = 0.0;
    last_eh = 0.0;
    map_updated = TRUE;

    static gulong map_button_handler = 0;
    map_drawing_area = GTK_WIDGET(gtk_builder_get_object(
                window_xml, "drawingarea_map"));
    map_notebook = GTK_WIDGET(gtk_builder_get_object(
                window_xml, "map_notebook"));

    g_signal_connect(map_drawing_area, "configure_event",
            G_CALLBACK(map_check_resize), NULL);
    g_signal_connect(map_drawing_area, "draw",
            G_CALLBACK(map_expose_event), NULL);

    // Enable event masks and set callbacks to handle mouse events.
    // If its already connected (e.g. on a second login), then skip
    // an additional association.
    if (!g_signal_handler_is_connected(map_drawing_area, map_button_handler)) {
        gtk_widget_add_events(map_drawing_area,
            GDK_BUTTON_PRESS_MASK | GDK_BUTTON_RELEASE_MASK);
        map_button_handler = g_signal_connect(map_drawing_area, "event",
            G_CALLBACK(map_button_event), NULL);
    }

    // Set our image sizes.
    // IIRC, atoi stops at the first nonnumeric char, so the x in the size will be the end.
    if (face_info.facesets[face_info.faceset].size != NULL) {
        image_size = atoi(face_info.facesets[face_info.faceset].size);
        map_image_size  = image_size; // These should be the same.
    } else {
        LOG(LOG_ERROR, "map_init", "Invalid faceset size from server");
    }
    // If we are not on the default size, we need to resize pixmaps[0].
    if (map_image_size != DEFAULT_IMAGE_SIZE) {
        int nx = map_image_size, ny = map_image_size;
        guint8 *png_tmp = rescale_rgba_data(pixmaps[0]->map_image, &nx, &ny, use_config[CONFIG_MAPSCALE]);
        // Try to affect pixmap[0] in-place, since it is referenced extensively.
        pixmaps[0]->icon_width = nx;
        pixmaps[0]->icon_height = ny;
        // Do not affect full_icon_width/height, since those are expected to be the unscaled size.
        do_new_image(png_tmp, pixmaps[0]);
    }

    // Set map size based on window size and show widget.
    map_check_resize();
    gtk_widget_show(map_drawing_area);
}

/**
 * Draw a pixmap to the given map tile on screen.
 * @param ax Map cell on-screen x-coordinate
 * @param ay Map cell on-screen y-coordinate
 */
static void draw_pixmap(cairo_t *cr, PixmapInfo *pixmap, int ax, int ay) {
    const int dest_x = ax * map_image_size;
    const int dest_y = ay * map_image_size;
    cairo_set_source_surface(cr, pixmap->map_image, dest_x, dest_y);
    cairo_paint(cr);
}

/**
 * Draw a tile-sized sub-region of a smooth pixmap at a destination map cell.
 * Used to composite edge-blending tiles on top of neighbouring cells.
 *
 * @param cr     Cairo context for the map drawing area.
 * @param pixmap Smooth pixmap to draw from.
 * @param sx     Source tile column in the pixmap sheet.
 * @param sy     Source tile row in the pixmap sheet.
 * @param dx     Destination tile column on screen.
 * @param dy     Destination tile row on screen.
 */
static void draw_smooth_pixmap(cairo_t* cr, PixmapInfo* pixmap,
        const int sx, const int sy, const int dx, const int dy) {
    const int src_x = map_image_size * sx;
    const int src_y = map_image_size * sy;
    const int dest_x = map_image_size * dx;
    const int dest_y = map_image_size * dy;
    cairo_set_source_surface(cr, pixmap->map_image, dest_x - src_x, dest_y - src_y);
    cairo_rectangle(cr, dest_x, dest_y, map_image_size, map_image_size);
    cairo_fill(cr);
}

/**
 * Blit-shift the tile surface by the scroll delta so that unchanged tiles do
 * not need to be redrawn.  Only the newly revealed edge strip (and any bigface
 * boundary tiles marked by mapdata_scroll) is re-rendered each scroll step.
 *
 * Falls back to returning 0 (full redraw) when:
 *   - tile_surface has not been allocated yet
 *   - smooth pixel-lighting modes are active (interpolated darkness across the
 *     full viewport cannot be applied correctly to a partial tile update)
 *   - the scroll is diagonal (two strips would need to be redrawn, adding
 *     complexity for limited benefit)
 *   - the scroll distance equals or exceeds the entire surface width/height
 *
 * When returning 1, global_offset_x/y and want_offset_x/y are adjusted to
 * maintain visual continuity: the blit shifts tile_surface by -pixel_dx so
 * the old tile content stays at the same screen position; adding pixel_dx to
 * global_offset_x exactly cancels that shift in the compose step, and
 * decrementing want_offset_x by dx removes the consumed prediction so the
 * animation target becomes correct.
 *
 * @param dx Horizontal scroll distance in tiles.
 * @param dy Vertical scroll distance in tiles.
 * @return 1 if the scroll blit was handled, 0 if the caller should full-redraw.
 */
int display_mapscroll(int dx, int dy) {
    if (tile_surface == NULL) return 0;
    /* Smooth pixel-lit modes build a full-viewport interpolated light map;
     * repainting only the edge strip would leave incorrect darkness values at
     * the boundary of the redrawn area. */
    if (use_config[CONFIG_LIGHTING] == CFG_LT_PIXEL ||
            use_config[CONFIG_LIGHTING] == CFG_LT_PIXEL_BEST)
        return 0;
    /* Diagonal scrolls require two separate strips — not worth the complexity. */
    if (dx != 0 && dy != 0) return 0;

    const int pixel_dx = dx * map_image_size;
    const int pixel_dy = dy * map_image_size;
    const int sw = cairo_image_surface_get_width(tile_surface);
    const int sh = cairo_image_surface_get_height(tile_surface);

    if (abs(pixel_dx) >= sw || abs(pixel_dy) >= sh) return 0;

    /* Blit-shift tile_surface via a temporary copy.  Cairo does not support
     * using the same surface as both source and destination. */
    cairo_surface_t *tmp = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, sw, sh);
    cairo_t *cr = cairo_create(tmp);
    cairo_set_source_surface(cr, tile_surface, 0, 0);
    cairo_paint(cr);
    cairo_destroy(cr);

    cr = cairo_create(tile_surface);
    cairo_set_source_rgb(cr, 0, 0, 0);
    cairo_paint(cr);  /* black-fill newly exposed strip */
    cairo_set_source_surface(cr, tmp, -pixel_dx, -pixel_dy);
    cairo_paint(cr);
    cairo_destroy(cr);
    cairo_surface_destroy(tmp);

    /* Maintain visual continuity: tile_surface content shifted by -pixel_dx,
     * compose adds global_offset_x — net shift on screen is unchanged when
     * global_offset_x is incremented by pixel_dx here.  Decrement want_offset
     * by dx to remove the consumed prediction tile so the animation settles
     * correctly toward the new resting position. */
    global_offset_x += pixel_dx;
    global_offset_y += pixel_dy;
    want_offset_x   -= dx;
    want_offset_y   -= dy;

    map_scroll_dx = dx;
    map_scroll_dy = dy;
    map_scrolled  = TRUE;
    map_updated   = TRUE;
    return 1;
}

/**
 * Draw anything in adjacent squares that could smooth on given square
 *
 * @param mx
 * @param my Square to smooth on.
 * You should not call this function to smooth on a 'completely black' square.
 * @param layer Layer to examine (we smooth only one layer at a time)
 * @param picx
 * @param picy Place on the map_drawing_area->window to draw
 */
static void drawsmooth(cairo_t *cr, int mx, int my, int layer, int picx, int picy) {
    static int dx[8]= {0,1,1,1,0,-1,-1,-1};
    static int dy[8]= {-1,-1,0,1,1,1,0,-1};
    static int bweights[8]= {2,0,4,0,8,0,1,0};
    static int cweights[8]= {0,2,0,4,0,8,0,1};
    static int bc_exclude[8]= {
        1+2,/*north exclude northwest (bit0) and northeast(bit1)*/
        0,
        2+4,/*east exclude northeast and southeast*/
        0,
        4+8,/*and so on*/
        0,
        8+1,
        0
    };
    int partdone[8]= {0,0,0,0,0,0,0,0};
    int slevels[8];
    int sfaces[8];
    int i,weight,weightC;
    int emx,emy;
    int smoothface;
    int hasFace = 0;
    for (i=0; i<=layer; i++) {
        hasFace |= mapdata_cell(mx, my)->heads[i].face;
    }
    if (!hasFace || !mapdata_can_smooth(mx, my, layer)) {
        return;
    }
    for (i=0; i<8; i++) {
        emx=mx+dx[i];
        emy=my+dy[i];
        if (!mapdata_contains(emx, emy)) {
            slevels[i]=0;
            sfaces[i]=0; /*black picture*/
        } else if (mapdata_cell(emx, emy)->smooth[layer] <= mapdata_cell(mx, my)->smooth[layer]) {
            slevels[i]=0;
            sfaces[i]=0; /*black picture*/
        } else {
            slevels[i]=mapdata_cell(emx, emy)->smooth[layer];
            sfaces[i]=pixmaps[mapdata_cell(emx, emy)->heads[layer].face]->smooth_face;
        }
    }
    /*
     * Now we have a list of smoothlevel higher than current square.  There are
     * at most 8 different levels. so... check 8 times for the lowest one (we
     * draw from bottom to top!).
     */
    while (1) {
        int lowest = -1;
        for (i=0; i<8; i++) {
            if ( (slevels[i]>0) && (!partdone[i]) &&
                    ((lowest<0) || (slevels[i]<slevels[lowest]))
               ) {
                lowest=i;
            }
        }
        if (lowest<0) {
            break;    /*no more smooth to do on this square*/
        }
        /*printf ("hey, must smooth something...%d\n",sfaces[lowest]);*/
        /* Here we know 'what' to smooth
         *
         * Calculate the weight for border and weight for corners.  Then
         * 'markdone' the corresponding squares
         *
         * First, the border, which may exclude some corners
         */
        weight=0;
        weightC=15; /*works in backward. remove where there is nothing*/
        /*for (i=0;i<8;i++)
            cornermask[i]=1;*/
        for (i=0; i<8; i++) { /*check all nearby squares*/
            if ( (slevels[i]==slevels[lowest]) &&
                    (sfaces[i]==sfaces[lowest])) {
                partdone[i]=1;
                weight=weight+bweights[i];
                weightC&=~bc_exclude[i];
            } else {
                /*must rmove the weight of a corner if not in smoothing*/
                weightC&=~cweights[i];
            }
        }
        /*We can't do this before since we need the partdone to be adjusted*/
        if (sfaces[lowest]<=0) {
            continue;    /*Can't smooth black*/
        }
        smoothface=sfaces[lowest];
        if (smoothface<=0) {
            continue;  /*picture for smoothing not yet available*/
        }
        /*
         * now, it's quite easy. We must draw using a 32x32 part of the picture
         * smoothface.  This part is located using the 2 weights calculated:
         * (32*weight,0) and (32*weightC,32)
         */
        if ( (!pixmaps[smoothface]->map_image) ||
                (pixmaps[smoothface] == pixmaps[0])) {
            continue;    /*don't have the picture associated*/
        }

        if (weight > 0) {
            draw_smooth_pixmap(cr, pixmaps[smoothface], weight, 0, picx, picy);
        }

        if (weightC > 0) {
            draw_smooth_pixmap(cr, pixmaps[smoothface], weightC, 1, picx, picy);
        }
    }
}

/**
 * Draw a single map layer to the given cairo context.
 */
static void map_draw_layer(cairo_t *cr, int layer, int mx_start, int nx, int my_start, int ny) {
    for (int x = 0; x <= nx; x++) {
        for (int y = 0; y <= ny; y++) {
            // Translate on-screen coordinates to virtual map coordinates.
            const int mx = mx_start + x;
            const int my = my_start + y;

            // Skip current cell if not visible and not using fog of war.
            if (!use_config[CONFIG_FOGWAR] && mapdata_cell(mx, my)->state == FOG) {
                continue;
            }

            // Draw pixmaps
            int dx, dy, face = mapdata_face_info(mx, my, layer, &dx, &dy);
            if (face > 0 && pixmaps[face]->map_image != NULL) {
                draw_pixmap(cr, pixmaps[face], x + dx, y + dy);
            }

            // Draw smoothing
            if (use_config[CONFIG_SMOOTH]) {
                drawsmooth(cr, mx, my, layer, x, y);
            }
        }
    }
}

/**
 * Draw player and DM name labels on top of all map tiles. Labels are
 * centered horizontally over their tile and stacked vertically when multiple
 * labels occupy the same cell. DM labels are drawn red; party members in
 * light blue; all others in white.
 *
 * @param cr       Cairo context for the map drawing area.
 * @param mx_start Virtual map x coordinate of the top-left tile.
 * @param nx       Number of tiles to draw in the x direction.
 * @param my_start Virtual map y coordinate of the top-left tile.
 * @param ny       Number of tiles to draw in the y direction.
 */
static void map_draw_labels(cairo_t *cr, int mx_start, int nx, int my_start, int ny) {
    cairo_set_font_face(cr, font);
    for (int x = 0; x <= nx; x++) {
        for (int y = 0; y <= ny; y++) {
            // Translate on-screen coordinates to virtual map coordinates.
            const int mx = mx_start + x;
            const int my = my_start + y;

            struct MapLabel *next = mapdata_cell(mx, my)->label;
            double off_y = 0;
            while (next != NULL) {
                // calculate extents to center text above square
                cairo_text_extents_t extents;
                cairo_text_extents(cr, next->label, &extents);
                double off_x = map_image_size/2 - extents.width/2;
                double sx = (mx-mx_start)*map_image_size + off_x;
                double sy = (my-my_start)*map_image_size + off_y;

                // background shading
                int bx = 3;
                double lineheight = extents.height+2*bx;
                cairo_set_source_rgba(cr, 0.3, 0.3, 0.3, 0.5);
                cairo_rectangle(cr, sx - bx, sy-extents.height-bx, extents.width+2*bx, lineheight);
                cairo_fill(cr);

                // render text
                cairo_move_to(cr, sx, sy);
                switch (next->subtype) {
                case MAP2_LABEL_DM:
                    cairo_set_source_rgb(cr, 1, 0, 0);
                    break;
                case MAP2_LABEL_PLAYER_PARTY:
                    cairo_set_source_rgb(cr, 177.0/255, 225.0/255, 255.0/255);
                    break;
                default:
                    cairo_set_source_rgb(cr, 1, 1, 1);
                }
                cairo_show_text(cr, next->label);

                next = next->next;
                off_y += lineheight;
            }
        }
    }
}

/**
 * Return the opacity (0.0–1.0) that should be used when darkening a map cell.
 * Fog-of-war cells receive an additional 0.2 opacity boost on top of the
 * server-reported darkness value.
 *
 * @param mx Virtual map x coordinate of the cell.
 * @param my Virtual map y coordinate of the cell.
 * @return   Alpha opacity for the darkness overlay.
 */
static double mapcell_darkness(int mx, int my) {
    double opacity = mapdata_cell(mx, my)->darkness / 192.0 * 0.6;
    if (use_config[CONFIG_FOGWAR] && mapdata_cell(mx, my)->state == FOG) {
        opacity += 0.2;
    }
    return opacity;
}

/**
 * Composite a darkness overlay on top of the already-drawn map. Builds a
 * small grayscale light map (one pixel per tile plus a 1-pixel border to
 * avoid edge artefacts), then scales it up and blits it over the map using
 * the filter selected by CONFIG_LIGHTING (nearest, good, or best quality).
 *
 * The light-map surface (lm_surface) is cached and reallocated only when the
 * number of visible tiles changes.  Its pixel content is written directly into
 * the surface buffer each frame, replacing the previous per-pixel
 * cairo_rectangle/fill loop (which issued hundreds of drawing calls per frame).
 *
 * @param cr       Cairo context for the map drawing area.
 * @param nx       Number of tiles visible in the x direction.
 * @param ny       Number of tiles visible in the y direction.
 * @param mx_start Virtual map x coordinate of the top-left tile.
 * @param my_start Virtual map y coordinate of the top-left tile.
 */
static void draw_darkness(cairo_t *cr, int nx, int ny, int mx_start, int my_start) {
    /* Reallocate the cached surface only when the tile count changes. */
    if (lm_surface == NULL || nx != lm_last_nx || ny != lm_last_ny) {
        if (lm_surface) cairo_surface_destroy(lm_surface);
        lm_surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, nx + 2, ny + 2);
        lm_last_nx = nx;
        lm_last_ny = ny;
    }

    /* Write darkness values directly into the pixel buffer.
     *
     * The surface is (nx+2) × (ny+2): one pixel per visible tile plus a
     * one-pixel border on all sides (sampled from the edge tiles) to prevent
     * interpolation artefacts when the light map is scaled up.
     *
     * ARGB32 is premultiplied.  For a black pixel with opacity A the
     * premultiplied value is (A<<24)|0 — only the alpha channel is non-zero.
     *
     * Loop bounds: x from -1..nx (not nx+1) and y from -1..ny produce
     * exactly (nx+2)×(ny+2) pixels, matching the surface dimensions. */
    cairo_surface_flush(lm_surface);
    uint32_t *pixels = (uint32_t *)cairo_image_surface_get_data(lm_surface);
    const int stride = cairo_image_surface_get_stride(lm_surface) / (int)sizeof(uint32_t);

    for (int y = -1; y <= ny; y++) {
        for (int x = -1; x <= nx; x++) {
            const int cx = MIN(MAX(0, x), nx);
            const int cy = MIN(MAX(0, y), ny);
            const double opacity = mapcell_darkness(mx_start + cx, my_start + cy);
            const uint32_t alpha = (uint32_t)(opacity * 255.0 + 0.5);
            pixels[(y + 1) * stride + (x + 1)] = alpha << 24;
        }
    }
    cairo_surface_mark_dirty(lm_surface);

    /* Scale the light map up to tile resolution and composite over the map. */
    cairo_scale(cr, map_image_size, map_image_size);
    cairo_translate(cr, -1, -1);
    cairo_set_source_surface(cr, lm_surface, 0, 0);
    switch (use_config[CONFIG_LIGHTING]) {
        case CFG_LT_TILE:
            cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_NEAREST);
            break;
        case CFG_LT_PIXEL:
            cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_GOOD);
            break;
        case CFG_LT_PIXEL_BEST:
            cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_BEST);
            break;
    }
    cairo_paint(cr);
}

/**
 * Draw an outline around the move-to destination tile, if any. A yellow
 * outline indicates a walk destination and a red outline indicates an attack
 * destination. The outline is not drawn once the player reaches the target.
 *
 * @param cr       Cairo context for the map drawing area.
 * @param mx_start Virtual map x coordinate of the top-left visible tile.
 * @param my_start Virtual map y coordinate of the top-left visible tile.
 */
static void draw_move_to(cairo_t *cr, int mx_start, int my_start) {
    int mx = move_to_x;
    int my = move_to_y;
    int x = mx - mx_start;
    int y = my - my_start;
    if (!is_at_moveto()) {
        int sx = x * map_image_size;
        int sy = y * map_image_size;
        cairo_rectangle(cr, sx, sy, map_image_size, map_image_size);
        if (move_to_attack) {
            cairo_set_source_rgb(cr, 1, 0, 0); // red to attack
        } else {
            cairo_set_source_rgb(cr, 1, 1, 0); // yellow to walk
        }
        cairo_set_line_width(cr, 2);
        cairo_stroke(cr);
        cairo_rectangle(cr, sx + 2, sy + 2, map_image_size - 4, map_image_size - 4);
        cairo_set_source_rgb(cr, 0, 0, 0); // black
        cairo_set_line_width(cr, 1);
        cairo_stroke(cr);
    }
}

/**
 * Redraw the map using a two-phase rendering approach.
 *
 * Phase 1 — tile_surface update:
 *   Tiles and labels are rendered into tile_surface without any sub-tile
 *   translation baked in.  This surface can be blit-shifted by
 *   display_mapscroll() so that most tiles survive unchanged across frames.
 *
 *   Three render paths:
 *     a) Partial (do_partial): tile_surface was already blit-shifted by
 *        display_mapscroll(); only the newly revealed edge strip is redrawn.
 *     b) Full: entire tile_surface is black-filled and all tiles redrawn.
 *     c) Skip: viewport stable, no dirty tiles, only animation running — the
 *        compose step handles the offset change without touching tile_surface.
 *
 * Phase 2 — compose into map_surface:
 *   tile_surface is blitted into map_surface with a cairo_translate() of
 *   global_offset_x/y (sub-tile smooth-scroll prediction) and the darkness
 *   overlay is applied on top.  This step always runs when we reach here,
 *   ensuring animation frames update the display even without tile changes.
 *
 * Dirty-region optimisations:
 *   1. map_updated cleared immediately so concurrent server packets reschedule.
 *   2. Surfaces reused; reallocated only on pixel-dimension change.
 *   3. When viewport is stable AND no animation is in progress, dirty flags are
 *      scanned.  If none are set the entire frame is skipped.
 *   4. When only animation is running (global_offset != 0, no tile changes),
 *      tile_surface is not re-rendered — the compose step alone is cheap.
 *   5. After each tile render the dirty flags are cleared.
 */
static void gtk_map_redraw() {
    if (!map_updated) {
        return;
    }
    /* Clear early so concurrent server packets reschedule correctly. */
    map_updated = FALSE;

    GtkAllocation size;
    gtk_widget_get_allocation(map_drawing_area, &size);

    float scale = use_config[CONFIG_MAPSCALE] / 100.0;
    const double ew = size.width / scale;
    const double eh = size.height / scale;
    const int nx = (int)ceilf(ew / map_image_size);
    const int ny = (int)ceilf(eh / map_image_size);
    const int vw = use_config[CONFIG_MAPWIDTH];
    const int vh = use_config[CONFIG_MAPHEIGHT];
    const int mx_start = (nx > vw) ? pl_pos.x - (nx - vw) / 2 : pl_pos.x;
    const int my_start = (ny > vh) ? pl_pos.y - (ny - vh) / 2 : pl_pos.y;

    const gboolean size_changed = (ew != last_ew || eh != last_eh);

    /* Consume the scroll-blit flag.  do_partial is only valid when surfaces
     * exist and pixel dimensions have not changed (which would invalidate the
     * blit anyway). */
    const gboolean do_partial = map_scrolled
                                && !size_changed
                                && tile_surface != NULL
                                && map_surface  != NULL;
    map_scrolled = FALSE;

    /* vp_stable: no blit pending, surfaces exist, viewport origin unchanged. */
    const gboolean surfaces_ok = tile_surface != NULL && map_surface != NULL && !size_changed;
    const gboolean vp_stable   = surfaces_ok && !do_partial
                                  && mx_start == last_mx_start
                                  && my_start == last_my_start;
    const gboolean animating   = global_offset_x != 0 || global_offset_y != 0;

    /* When the viewport is stable, scan dirty flags.
     * - No animation + no dirty tiles: skip the frame entirely.
     * - Animation + no dirty tiles: recompose only (tile_surface unchanged).
     * - Any dirty tiles: must re-render tile_surface regardless of animation. */
    gboolean any_dirty = FALSE;
    if (vp_stable) {
        for (int x = 0; x <= nx && !any_dirty; x++) {
            for (int y = 0; y <= ny && !any_dirty; y++) {
                const int mx = mx_start + x, my = my_start + y;
                if (mapdata_contains(mx, my) &&
                        (mapdata_cell(mx, my)->need_update ||
                         mapdata_cell(mx, my)->need_resmooth)) {
                    any_dirty = TRUE;
                }
            }
        }
        if (!animating && !any_dirty) return;
    }

    /* Recreate both surfaces when pixel dimensions change. */
    if (!surfaces_ok) {
        if (tile_surface) { cairo_surface_destroy(tile_surface); tile_surface = NULL; }
        if (map_surface)  { cairo_surface_destroy(map_surface);  map_surface  = NULL; }
        tile_surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, (int)ew, (int)eh);
        /* map_surface is the final composited output painted directly to the
         * screen widget.  It has a black background and is always fully opaque
         * so RGB24 suffices — avoiding an unnecessary alpha channel halves the
         * per-pixel blend cost in the final screen blit. */
        map_surface  = cairo_image_surface_create(CAIRO_FORMAT_RGB24,  (int)ew, (int)eh);
        last_ew = ew;
        last_eh = eh;
    }

    /* Phase 1: Update tile_surface.
     * Skip when the viewport is stable, animation is the only thing changing,
     * and no tile data has changed — the compose step alone handles it. */
    const gboolean skip_tile_update = vp_stable && animating && !any_dirty;
    if (!skip_tile_update) {
        cairo_t *cr = cairo_create(tile_surface);

        if (do_partial) {
            /* Clip to the newly revealed edge strip plus a one-tile inner
             * border so that adjacent smoothing tiles are also recalculated.
             * Tiles outside the clip retain their blit-shifted content. */
            int cx0 = 0, cy0 = 0, cx1 = nx, cy1 = ny;
            if (map_scroll_dx > 0) cx0 = MAX(0,  nx - map_scroll_dx - 1);
            if (map_scroll_dx < 0) cx1 = MIN(nx, -map_scroll_dx);
            if (map_scroll_dy > 0) cy0 = MAX(0,  ny - map_scroll_dy - 1);
            if (map_scroll_dy < 0) cy1 = MIN(ny, -map_scroll_dy);
            cairo_rectangle(cr,
                            (double)cx0 * map_image_size,
                            (double)cy0 * map_image_size,
                            (double)(cx1 - cx0 + 1) * map_image_size,
                            (double)(cy1 - cy0 + 1) * map_image_size);
            cairo_clip(cr);
        } else {
            cairo_set_source_rgb(cr, 0, 0, 0);
            cairo_paint(cr);
        }

        /* Draw layer-by-layer so big faces are correctly layered on top. */
        for (int layer = 0; layer < MAXLAYERS; layer++) {
            map_draw_layer(cr, layer, mx_start, nx, my_start, ny);
        }
        draw_move_to(cr, mx_start, my_start);
        map_draw_labels(cr, mx_start, nx, my_start, ny);
        cairo_destroy(cr);

        /* Clear dirty flags; subsequent frames are skipped until new data. */
        for (int x = 0; x <= nx; x++) {
            for (int y = 0; y <= ny; y++) {
                const int mx = mx_start + x, my = my_start + y;
                if (mapdata_contains(mx, my)) {
                    mapdata_cell(mx, my)->need_update   = 0;
                    mapdata_cell(mx, my)->need_resmooth = 0;
                }
            }
        }

        last_mx_start = mx_start;
        last_my_start = my_start;
    }

    /* Phase 2: Compose tile_surface into map_surface.
     * Apply the sub-tile smooth-scroll prediction offset and the darkness
     * overlay.  Both transforms use the same CTM so they stay aligned.
     *
     * The black fill uses OPERATOR_SOURCE: map_surface is RGB24 (fully
     * opaque), so SOURCE is equivalent to OVER for a solid colour but avoids
     * the multiply-by-alpha arithmetic in Cairo's software renderer — useful
     * on the GDK Win32 backend which has no GPU path.  OVER is restored before
     * blitting tile_surface so transparent sprite pixels blend correctly. */
    {
        cairo_t *cr = cairo_create(map_surface);
        cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
        cairo_set_source_rgb(cr, 0, 0, 0);
        cairo_paint(cr);
        cairo_set_operator(cr, CAIRO_OPERATOR_OVER);
        /* Sub-tile prediction offset — shifts both tiles and darkness. */
        cairo_translate(cr, global_offset_x, global_offset_y);
        cairo_set_source_surface(cr, tile_surface, 0, 0);
        cairo_paint(cr);
        if (use_config[CONFIG_LIGHTING] != 0) {
            draw_darkness(cr, nx, ny, mx_start, my_start);
        }
        cairo_destroy(cr);
    }

    gtk_widget_queue_draw(map_drawing_area);
}

/**
 * Resize_map_window is a NOOP for the time being - not sure if it will in fact
 * need to do something, since there are scrollbars for the map window now.
 * Note - this is note a window resize request, but rather process the size
 * (in spaces) of the map - is received from server.
 */
void resize_map_window(int x, int y) {
}

/**
 * Incrementally interpolate the rendering offset toward the desired prediction
 * offset. The step size is controlled by CONFIG_MAPSCALE (predict_alpha),
 * producing a smooth scrolling effect when local movement prediction is active.
 */
static void update_global_offset() {
    int dx, dy;
    dx = ((want_offset_x*map_image_size) - global_offset_x)*predict_alpha/100.0;
    dy = ((want_offset_y*map_image_size) - global_offset_y)*predict_alpha/100.0;
    global_offset_x += dx;
    global_offset_y += dy;
}

/**
 * Draw the map window using the appropriate backend.
 */
void draw_map() {
    gint64 t_start, t_end;
    t_start = g_get_monotonic_time();

    /* If the local-prediction scroll offset is still animating toward zero,
     * a redraw is needed even when no tile data changed.  Detect that before
     * calling gtk_map_redraw() so the animation does not stall. */
    const int prev_offset_x = global_offset_x;
    const int prev_offset_y = global_offset_y;
    update_global_offset();
    if (global_offset_x != prev_offset_x || global_offset_y != prev_offset_y) {
        map_updated = TRUE;
    }

    gtk_map_redraw();

    t_end = g_get_monotonic_time();
    gint64 elapsed = t_end - t_start;
    if (time_map_redraw) {
        printf("profile/redraw,%"G_GINT64_FORMAT"\n", elapsed);
    }

    const unsigned int target_redraw = 100000;
    const int no_resize_above = 100; // don't resize above this value of mapscale
    if (elapsed > target_redraw && use_config[CONFIG_MAPSCALE] < no_resize_above) {
        use_config[CONFIG_MAPSCALE] = MIN(use_config[CONFIG_MAPSCALE] + 5, no_resize_above);
        LOG(LOG_DEBUG, "draw_map", "Increasing mapscale to %d to reduce draw time below %u us",
                use_config[CONFIG_MAPSCALE], target_redraw);
        map_check_resize();
    }
}

/**
 * GTK "draw" signal handler for the map drawing area. Blits the pre-rendered
 * map surface to the provided cairo context and returns FALSE to allow further
 * propagation of the event.
 *
 * @param widget    The map drawing area widget.
 * @param cr        Cairo context provided by the GTK3 draw signal.
 * @param user_data Unused.
 */
static gboolean map_expose_event(GtkWidget *widget, cairo_t *cr,
        gpointer user_data) {
    if (!map_surface) {
        return FALSE;
    }
    float scale = use_config[CONFIG_MAPSCALE] / 100.0;
    if (use_config[CONFIG_MAPSCALE] != 100) {
        cairo_scale(cr, scale, scale);
    }
    cairo_set_source_surface(cr, map_surface, 0, 0);
    if (use_config[CONFIG_MAPSCALE] % 100 == 0) {
        cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_NEAREST);
    }
    /* map_surface is RGB24 (fully opaque).  OPERATOR_SOURCE replaces the
     * destination directly without alpha blending, which is both correct and
     * faster than the default OVER on Cairo's software renderer. */
    cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
    cairo_paint(cr);
    return FALSE;
}

/**
 * Given a relative tile coordinate, determine its compass direction.
 * @param dx Relative 'x' coordinate
 * @param dy Relative 'y' coordinate
 * @return 0 if x and y are both zero, 1-8 for each compass direction
 */
int relative_direction(int dx, int dy) {
    if (dx == 0 && dy == 0) {
        return 0;
    } else if (dx == 0 && dy < 0) {
        return 1;
    } else if (dx > 0 && dy < 0) {
        return 2;
    } else if (dx > 0 && dy == 0) {
        return 3;
    } else if (dx > 0 && dy > 0) {
        return 4;
    } else if (dx == 0 && dy > 0) {
        return 5;
    } else if (dx < 0 && dy > 0) {
        return 6;
    } else if (dx < 0 && dy == 0) {
        return 7;
    } else if (dx < 0 && dy < 0) {
        return 8;
    } else {
        g_assert_not_reached();
    }
}

/**
 * Given x, y coordinates on the map drawing area, determine the tile that the
 * coordinates are on with respect to the player. Clicking on the tile with
 * the player returns (0, 0).
 */
static void coord_to_tile(int x, int y, int *dx, int *dy) {
    // Calculate tile below (x, y) coordinates. The top left corner is (0, 0).
    // This is easy, just floor divide the coordinates by tile size.
    const float tile_size = map_image_size * use_config[CONFIG_MAPSCALE]/100.0;
    int mx = x / tile_size;
    int my = y / tile_size;

    // Now calculate where the player is drawn. The code below is copied from
    // gtk_map_redraw() (see the comments there):
    GtkAllocation size;
    gtk_widget_get_allocation(map_drawing_area, &size);

    float scale = use_config[CONFIG_MAPSCALE]/100.0;
    const double ew = size.width / scale;
    const double eh = size.height / scale;

    const int nx = (int)ceilf(ew / map_image_size);
    const int ny = (int)ceilf(eh / map_image_size);

    const int vw = use_config[CONFIG_MAPWIDTH];
    const int vh = use_config[CONFIG_MAPHEIGHT];

    // Normally, the player is centered in the middle of the server viewport
    // (with floor division).
    int ox = vw / 2;
    int oy = vh / 2;

    // If mapscale is less than 100, what is shown in the client no longer
    // matches the viewport. Add the right offset.
    if (nx > vw) {
        ox += (nx - vw)/2;
    }

    if (ny > vh) {
        oy += (ny - vh)/2;
    }

    // Shift the clicked tile by the player's position.
    *dx = mx - ox;
    *dy = my - oy;
}

/**
 * Handle a mouse event in the drawing area.
 */
static gboolean map_button_event(GtkWidget *widget,
        GdkEventButton *event, gpointer user_data) {
    int dx, dy;
    coord_to_tile((int)event->x, (int)event->y, &dx, &dy);
    int dir = relative_direction(dx, dy);

    switch (event->button) {
        case 1:
            if (event->type == GDK_BUTTON_PRESS) {
                look_at(dx,dy);
            }
            break;
        case 2:
            if (event->type == GDK_BUTTON_RELEASE) {
                clear_fire();
            } else {
                fire_dir(dir);
            }
            break;
        case 3:
            if (event->type == GDK_BUTTON_PRESS) {
                set_move_to(dx, dy);
                run_move_to();
            }
            break;
    }

    return FALSE;
}

/**
 * This is called after the map has been all digested.  this should perhaps be
 * removed, and left to being done from from the main event loop.
 *
 * @param redraw If set, force redraw of all tiles.
 * @param notice If set, another call will follow soon.
 */
void display_map_doneupdate(int redraw, int notice) {
    map_updated |= redraw || !notice;
}
