/*
 * Crossfire -- cooperative multi-player graphical RPG and adventure game
 *
 * Copyright (c) 1999-2013 Mark Wedel and the Crossfire Development Team
 * Copyright (c) 1992 Frank Tore Johansen
 *
 * Crossfire is free software and comes with ABSOLUTELY NO WARRANTY. You are
 * welcome to redistribute it under certain conditions. For details, please
 * see COPYING and LICENSE.
 *
 * The authors can be reached via e-mail at <crossfire@metalforge.org>.
 */

/**
 * @file
 * Implement client configuration dialog
 */

#include "client.h"

#include <ctype.h>
#include <gtk/gtk.h>

#include "image.h"
#include "main.h"
#include "mapdata.h"
#include "gtk2proto.h"

static GKeyFile *config;
static GString *config_path;
static GtkCssProvider *theme_provider = NULL;

GtkWidget *config_dialog, *config_button_echo, *config_button_fasttcp,
    *config_button_timestamp, *config_button_grad_color,
    *config_button_foodbeep, *config_button_sound, *config_button_cache,
    *config_button_download, *config_button_fog, *config_button_smoothing;

GtkFileChooser *ui_filechooser, *theme_filechooser;
GtkComboBoxText *config_combobox_faceset;
GtkComboBox *config_combobox_displaymode, *config_combobox_lighting;

#define THEME_DEFAULT CF_DATADIR "/themes/standard.css"

/* Configuration variables initialized to NULL, set by config_load() */
static char *theme;
char* last_server;

/*
 * This should really be one of the CONFIG values, or perhaps a checkbox
 * someplace that displays frame rate.
 */
bool time_map_redraw = false;

/** Speed of local map prediction scrolling, 0-100 (0 to disable). */
int predict_alpha = 10;

static void on_config_close(GtkButton *button, gpointer user_data);

/**
 * Return true if the user's desired setting for the given config option differs
 * from the currently active setting (i.e. a change is pending reconnection).
 *
 * @param type CONFIG_xxx index to check.
 * @return     true if want_config[type] != use_config[type].
 */
static bool IS_DIFFERENT(int type) {
    return want_config[type] != use_config[type];
}

/**
 * Return the basename of the current UI file.
 */
static char *ui_name() {
    return g_path_get_basename(window_xml_file);
}

/**
 * Load a CSS file and replace the current theme provider on the default screen.
 * Errors are logged but do not abort. Safe to call multiple times.
 */
static void apply_theme_css(const char *path) {
    GdkScreen *screen = gdk_screen_get_default();
    if (theme_provider) {
        gtk_style_context_remove_provider_for_screen(screen, GTK_STYLE_PROVIDER(theme_provider));
        g_object_unref(theme_provider);
        theme_provider = NULL;
    }
    theme_provider = gtk_css_provider_new();
    GError *error = NULL;
    if (!gtk_css_provider_load_from_path(theme_provider, path, &error)) {
        LOG(LOG_ERROR, "apply_theme_css", "Failed to load CSS '%s': %s\n", path, error->message);
        g_error_free(error);
    } else {
        LOG(LOG_DEBUG, "apply_theme_css", "Loaded CSS from '%s'", path);
    }
    gtk_style_context_add_provider_for_screen(screen, GTK_STYLE_PROVIDER(theme_provider),
                                              GTK_STYLE_PROVIDER_PRIORITY_USER);
}

/**
 * Load the default CSS theme at startup.
 */
void init_theme() {
#ifdef WIN32
    /* On Windows, CF_DATADIR is relative; use the runtime absolute path instead. */
    gchar *default_theme = g_build_filename(CF_DATADIR_RT, "themes", "standard.css", NULL);
    apply_theme_css(default_theme);
    g_free(default_theme);
#else
    apply_theme_css(THEME_DEFAULT);
#endif
}

/**
 * Reload theme-controlled widget styles and force a full UI redraw. Called
 * after the user selects a new theme in the configuration dialog, and once
 * at startup to apply the initial theme.
 *
 * @param reload Non-zero if this is a reload (unused, kept for calling
 *               convention compatibility).
 */
void load_theme(int reload) {
    if (theme) {
        apply_theme_css(theme);
    }
    info_get_styles();
    inventory_get_styles();
    stats_get_styles();
    spell_get_styles();
    update_spell_information();
    /*
     * Set inv_updated to force a redraw - otherwise it will not
     * necessarily bind the lists with the new widgets.
     */
    cpl.below->inv_updated = 1;
    cpl.ob->inv_updated = 1;
    draw_lists();
    draw_stats(TRUE);
    draw_message_window(TRUE);
}

/**
 * Load settings from the legacy file format.
 */
static void config_load_legacy() {
    char path[MAX_BUF], inbuf[MAX_BUF], *cp;
    FILE *fp;
    int i, val;

    LOG(LOG_INFO, "config_load_legacy",
        "Configuration not found; trying old configuration files.");
    LOG(LOG_INFO, "config_load_legacy",
        "You will need to move your keybindings to the new location.");

    snprintf(path, sizeof(path), "%s/.crossfire/gdefaults2", g_getenv("HOME"));
    if ((fp = fopen(path, "r")) == NULL) {
        return;
    }
    while (fgets(inbuf, MAX_BUF - 1, fp)) {
        inbuf[MAX_BUF - 1] = '\0';
        inbuf[strlen(inbuf) - 1] = '\0'; /* kill newline */

        if (inbuf[0] == '#') {
            continue;
        }
        /* Skip any setting line that does not contain a colon character */
        if (!(cp = strchr(inbuf, ':'))) {
            continue;
        }
        *cp = '\0';
        cp += 2;    /* colon, space, then value */

        val = -1;
        if (isdigit(*cp)) {
            val = atoi(cp);
        } else if (!strcmp(cp, "True")) {
            val = TRUE;
        } else if (!strcmp(cp, "False")) {
            val = FALSE;
        }

        for (i = 1; i < CONFIG_NUMS; i++) {
            if (!strcmp(config_names[i], inbuf)) {
                if (val == -1) {
                    LOG(LOG_WARNING, "config.c::load_defaults",
                        "Invalid value/line: %s: %s", inbuf, cp);
                } else {
                    want_config[i] = val;
                }
                break;  /* Found a match - won't find another */
            }
        }
        /* We found a match in the loop above, so do not do anything more */
        if (i < CONFIG_NUMS) {
            continue;
        }

        /*
         * Legacy - now use the map_width and map_height values Don't do sanity
         * checking - that will be done below
         */
        if (!strcmp(inbuf, "mapsize")) {
            if (sscanf(cp, "%hdx%hd", &want_config[CONFIG_MAPWIDTH],
                       &want_config[CONFIG_MAPHEIGHT]) != 2) {
                LOG(LOG_WARNING, "config.c::load_defaults",
                    "Malformed mapsize option in gdefaults2.  Ignoring");
            }
        } else if (!strcmp(inbuf, "theme")) {
            if (theme != NULL) {
                g_free(theme);
            }
            theme = g_strdup(cp);
            continue;
        } else if (!strcmp(inbuf, "window_layout")) {
            strncpy(window_xml_file, cp, MAX_BUF - 1);
            continue;
        } else if (!strcmp(inbuf, "nopopups")) {
            /* Changed name from nopopups to popups, so inverse value */
            want_config[CONFIG_POPUPS] = !val;
            continue;
        } else if (!strcmp(inbuf, "nosplash")) {
            want_config[CONFIG_SPLASH] = !val;
            continue;
        } else if (!strcmp(inbuf, "splash")) {
            want_config[CONFIG_SPLASH] = val;
            continue;
        } else if (!strcmp(inbuf, "faceset")) {
            face_info.want_faceset = g_strdup(cp);  /* memory leak ! */
            continue;
        }
        /* legacy, as this is now just saved as 'lighting' */
        else if (!strcmp(inbuf, "per_tile_lighting")) {
            if (val) {
                want_config[CONFIG_LIGHTING] = CFG_LT_TILE;
            }
        } else if (!strcmp(inbuf, "per_pixel_lighting")) {
            if (val) {
                want_config[CONFIG_LIGHTING] = CFG_LT_PIXEL;
            }
        } else if (!strcmp(inbuf, "resists")) {
            if (val) {
                want_config[CONFIG_RESISTS] = val;
            }
        } else if (!strcmp(inbuf, "sdl")) {
            if (val) {
                want_config[CONFIG_DISPLAYMODE] = CFG_DM_SDL;
            }
        } else LOG(LOG_WARNING, "config.c::load_defaults",
                       "Unknown line in gdefaults2: %s %s", inbuf, cp);
    }
    fclose(fp);
}

/**
 * Check that want_config is valid, copy the new configuration to use_config,
 * and apply the new configuration. Call this after changing want_config,
 * either through config_load() or on_config_close().
 */
void config_check() {
    if (want_config[CONFIG_ICONSCALE] < 25 ||
            want_config[CONFIG_ICONSCALE] > 200) {
        LOG(LOG_WARNING, "config_check",
                "Ignoring invalid 'iconscale' value '%d'; "
                "must be between 25 and 200.\n",
                want_config[CONFIG_ICONSCALE]);
        want_config[CONFIG_ICONSCALE] = use_config[CONFIG_ICONSCALE];
    }

    if (want_config[CONFIG_MAPSCALE] < 10 || want_config[CONFIG_MAPSCALE] > 200) {
        LOG(LOG_WARNING, "config_check",
                "Ignoring invalid 'mapscale' value '%d'; "
                "must be between 10 and 200.\n",
                want_config[CONFIG_MAPSCALE]);
        want_config[CONFIG_MAPSCALE] = use_config[CONFIG_MAPSCALE];
    }

    if (!want_config[CONFIG_LIGHTING]) {
        LOG(LOG_WARNING, "config_check",
            "No lighting mechanism selected - will not use darkness code");
        want_config[CONFIG_DARKNESS] = FALSE;
    }

    if (want_config[CONFIG_RESISTS] > 2) {
        LOG(LOG_WARNING, "config_check",
                "Ignoring invalid 'resists' value '%d'; "
                "must be either 0, 1, or 2.\n",
                want_config[CONFIG_RESISTS]);
        want_config[CONFIG_RESISTS] = 0;
    }

    /* Make sure the map size os OK */
    if (want_config[CONFIG_MAPWIDTH] < 9 ||
            want_config[CONFIG_MAPWIDTH] > MAP_MAX_SIZE) {
        LOG(LOG_WARNING, "config_check", "Invalid map width (%d) "
            "option in gdefaults2. Valid range is 9 to %d",
            want_config[CONFIG_MAPWIDTH], MAP_MAX_SIZE);
        want_config[CONFIG_MAPWIDTH] = use_config[CONFIG_MAPWIDTH];
    }

    if (want_config[CONFIG_MAPHEIGHT] < 9 ||
            want_config[CONFIG_MAPHEIGHT] > MAP_MAX_SIZE) {
        LOG(LOG_WARNING, "config_check", "Invalid map height (%d) "
            "option in gdefaults2. Valid range is 9 to %d",
            want_config[CONFIG_MAPHEIGHT], MAP_MAX_SIZE);
        want_config[CONFIG_MAPHEIGHT] = use_config[CONFIG_MAPHEIGHT];
    }

#if !defined(HAVE_OPENGL)
    if (want_config[CONFIG_DISPLAYMODE] == CFG_DM_OPENGL) {
        want_config[CONFIG_DISPLAYMODE] = CFG_DM_PIXMAP;
        LOG(LOG_ERROR, "config_check",
            "Display mode is set to OpenGL, but client "
            "is not compiled with OpenGL support.  Reverting to pixmap mode.");
    }
#endif

#if !defined(HAVE_SDL)
    if (want_config[CONFIG_DISPLAYMODE] == CFG_DM_SDL) {
        want_config[CONFIG_DISPLAYMODE] = CFG_DM_PIXMAP;
        LOG(LOG_ERROR, "config_check",
            "Display mode is set to SDL, but client "
            "is not compiled with SDL support.  Reverting to pixmap mode.");
    }
#endif

    if (want_config[CONFIG_CACHE]) {
        LOG(LOG_ERROR, "config_check",
                "Image caching is not currently supported in this client. Running without image caching.");
        want_config[CONFIG_CACHE] = 0;
    }

    // Enable darkness if lighting is not 'None'.
    if (want_config[CONFIG_LIGHTING] != CFG_LT_NONE) {
        want_config[CONFIG_DARKNESS] = 1;
    }

    if (want_config[CONFIG_SOUND] && init_sounds()) {
        use_config[CONFIG_SOUND] = 1;
    } else {
        use_config[CONFIG_SOUND] = 0;
    }
    if (csocket.fd) {
        cs_print_string(csocket.fd, "setup sound %d", use_config[CONFIG_SOUND]);
    }

#ifdef TCP_NODELAY
#ifndef WIN32
    // TODO: Merge with setsockopt code from client.c
    int q = want_config[CONFIG_FASTTCP];

    if (csocket.fd && setsockopt(csocket.fd, SOL_TCP, TCP_NODELAY, &q, sizeof(q)) == -1) {
        perror("TCP_NODELAY");
    }
#endif
#endif

    /* Copy sanitized user settings to current settings. */
    memcpy(use_config, want_config, sizeof(use_config));

    map_check_resize();
    image_size = DEFAULT_IMAGE_SIZE * use_config[CONFIG_ICONSCALE] / 100;
    if (!use_config[CONFIG_CACHE]) {
        use_config[CONFIG_DOWNLOAD] = FALSE;
    }
}

/**
 * Load settings from the user's configuration file into want_config.
 */
void config_load() {
    GError *error = NULL;

    /* Copy initial desired settings from current settings. */
    memcpy(want_config, use_config, sizeof(want_config));

    g_assert(g_file_test(config_dir, G_FILE_TEST_IS_DIR) == TRUE);

    /* Load existing or create new configuration file. */
    config = g_key_file_new();
    config_path = g_string_new(config_dir);
    g_string_append(config_path, "/client.ini");

    g_key_file_load_from_file(config, config_path->str, G_KEY_FILE_NONE, &error);

    /* Load configuration values into settings array. */
    if (error == NULL) {
        LOG(LOG_DEBUG, "config_load", "config_path='%s'", config_path->str);
        for (int i = 1; i < CONFIG_NUMS; i++) {
            GError *error = NULL;
            gint value = g_key_file_get_integer(config, "Client", config_names[i], &error);
            if (error == NULL) {
                want_config[i] = value;
            }
        }

        /* Load additional settings. */
        if (theme != NULL) {
            g_free(theme);
        }
        theme = g_key_file_get_string(config, "GTKv2", "theme", NULL);

        if (face_info.want_faceset != NULL) {
            g_free(face_info.want_faceset);
        }
        face_info.want_faceset = g_key_file_get_string(config, "GTKv2", "faceset", NULL);

        predict_alpha = g_key_file_get_integer(config, "GTKv2", "predict_alpha", NULL);

        if (last_server != NULL) {
            g_free(last_server);
        }
        last_server = g_key_file_get_string(config, "GTKv2", "last_server", NULL);

        char *layout = g_key_file_get_string(config, "GTKv2", "window_layout", NULL);
        g_strlcpy(window_xml_file, layout, sizeof(window_xml_file));
        free(layout);
    } else {
        g_error_free(error);

        /* Load legacy configuration file. */
        config_load_legacy();
    }

    if (theme == NULL) {
#ifdef WIN32
        theme = g_build_filename(CF_DATADIR_RT, "themes", "standard.css", NULL);
#else
        theme = g_strdup(THEME_DEFAULT);
#endif
    }

    if (face_info.want_faceset == NULL) {
        face_info.want_faceset = g_strdup("");
    }

    if (last_server == NULL) {
        last_server = g_strdup("");
    }
}

/**
 * This function saves user settings chosen using the configuration popup
 * dialog.
 */
void save_defaults() {
    GError *error = NULL;

    /* Save GTKv2 specific client settings. */
    g_key_file_set_string(config, "GTKv2", "theme", theme);
    g_key_file_set_string(config, "GTKv2", "faceset", face_info.want_faceset);
    g_key_file_set_string(config, "GTKv2", "last_server", last_server);
    g_key_file_set_integer(config, "GTKv2", "predict_alpha", predict_alpha);
    g_key_file_set_string(config, "GTKv2", "window_layout", window_xml_file);

    /* Save the rest of the client settings. */
    for (int i = 1; i < CONFIG_NUMS; i++) {
        g_key_file_set_integer(config, "Client", config_names[i], want_config[i]);
    }

    g_file_set_contents(config_path->str,
            g_key_file_to_data(config, NULL, NULL), -1, &error);

    if (error != NULL) {
        draw_ext_info(NDI_RED, MSG_TYPE_CLIENT, MSG_TYPE_CLIENT_CONFIG,
                "Could not save settings!");
        g_warning("Could not save settings: %s", error->message);
        g_error_free(error);
    }
}

void config_init(GtkWidget *window_root) {
    config_dialog =
        GTK_WIDGET(gtk_builder_get_object(dialog_xml, "config_dialog"));

    // Initialize file choosers and set filename filters.
    ui_filechooser =
        GTK_FILE_CHOOSER(gtk_builder_get_object(dialog_xml, "ui_filechooser"));
    theme_filechooser = GTK_FILE_CHOOSER(
        gtk_builder_get_object(dialog_xml, "theme_filechooser"));

    GtkFileFilter *ui_filter = gtk_file_filter_new();
    gtk_file_filter_add_pattern(ui_filter, "*.ui");
    gtk_file_chooser_set_filter(ui_filechooser, ui_filter);

    config_button_echo =
        GTK_WIDGET(gtk_builder_get_object(dialog_xml, "config_button_echo"));
    config_button_fasttcp =
        GTK_WIDGET(gtk_builder_get_object(dialog_xml, "config_button_fasttcp"));
    config_button_timestamp =
        GTK_WIDGET(gtk_builder_get_object(dialog_xml, "config_button_timestamp"));
    config_button_grad_color =
        GTK_WIDGET(gtk_builder_get_object(dialog_xml, "config_button_grad_color"));
    config_button_foodbeep =
        GTK_WIDGET(gtk_builder_get_object(dialog_xml, "config_button_foodbeep"));
    config_button_sound =
        GTK_WIDGET(gtk_builder_get_object(dialog_xml, "config_button_sound"));
    config_button_cache =
        GTK_WIDGET(gtk_builder_get_object(dialog_xml, "config_button_cache"));
    config_button_download =
        GTK_WIDGET(gtk_builder_get_object(dialog_xml, "config_button_download"));
    config_button_fog =
        GTK_WIDGET(gtk_builder_get_object(dialog_xml, "config_button_fog"));
    config_button_smoothing =
        GTK_WIDGET(gtk_builder_get_object(dialog_xml, "config_button_smoothing"));

    config_combobox_displaymode = GTK_COMBO_BOX(
        gtk_builder_get_object(dialog_xml, "config_combobox_displaymode"));
    config_combobox_faceset = GTK_COMBO_BOX_TEXT(
        gtk_builder_get_object(dialog_xml, "config_combobox_faceset"));
    config_combobox_lighting = GTK_COMBO_BOX(
        gtk_builder_get_object(dialog_xml, "config_combobox_lighting"));

    GtkWidget *config_button_close =
        GTK_WIDGET(gtk_builder_get_object(dialog_xml, "config_button_close"));
    g_signal_connect(config_button_close, "clicked",
                     G_CALLBACK(on_config_close), NULL);
    g_signal_connect(config_dialog, "delete_event", G_CALLBACK(on_config_close),
                     NULL);

    // Initialize available rendering modes.
    GtkListStore *display_list =
        GTK_LIST_STORE(gtk_combo_box_get_model(config_combobox_displaymode));
    GtkTreeIter iter;
#ifdef HAVE_OPENGL
    gtk_list_store_append(display_list, &iter);
    gtk_list_store_set(display_list, &iter, 0, "OpenGL", 1, CFG_DM_OPENGL, -1);
#endif
#ifdef HAVE_SDL
    gtk_list_store_append(display_list, &iter);
    gtk_list_store_set(display_list, &iter, 0, "SDL", 1, CFG_DM_SDL, -1);
#endif
    gtk_list_store_append(display_list, &iter);
    gtk_list_store_set(display_list, &iter, 0, "Pixmap", 1, CFG_DM_PIXMAP, -1);
}

/**
 * Removes all the text entries from the combo box. This function is not
 * available in GTK+2, so implement it ourselves.
 */
static void combo_box_text_remove_all(GtkComboBoxText *combo_box) {
    int count = gtk_tree_model_iter_n_children(
        gtk_combo_box_get_model(GTK_COMBO_BOX(combo_box)), NULL);
    for (int i = 0; i < count; i++) {
        gtk_combo_box_text_remove(combo_box, 0);
    }
}

/*
 * Setup config_dialog sets the buttons, combos, etc, to the state that matches
 * the want_config[] values.
 */
static void setup_config_dialog() {
    GtkTreeIter iter;

    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(config_button_echo),
                                 want_config[CONFIG_ECHO]);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(config_button_fasttcp),
                                 want_config[CONFIG_FASTTCP]);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(config_button_timestamp),
                                 want_config[CONFIG_TIMESTAMP]);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(config_button_grad_color),
                                 want_config[CONFIG_GRAD_COLOR]);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(config_button_foodbeep),
                                 want_config[CONFIG_FOODBEEP]);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(config_button_sound),
                                 want_config[CONFIG_SOUND]);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(config_button_cache),
                                 want_config[CONFIG_CACHE]);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(config_button_download),
                                 want_config[CONFIG_DOWNLOAD]);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(config_button_fog),
                                 want_config[CONFIG_FOGWAR]);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(config_button_smoothing),
                                 want_config[CONFIG_SMOOTH]);

    // Fill face set combo box with available face sets from the server.
    combo_box_text_remove_all(config_combobox_faceset);
    if (face_info.have_faceset_info) {
        for (int i = 0; i < MAX_FACE_SETS; i++) {
            const char *name = face_info.facesets[i].fullname;
            if (name != NULL) {
                gtk_combo_box_text_append_text(config_combobox_faceset, name);
                // g_ascii_strcasecmp expects both arguments to be non-null.
                // It appears to return 0 when one is null, confounding the result with
                // that of an actual match.
                if (face_info.want_faceset && !g_ascii_strcasecmp(face_info.want_faceset, name)) {
                    gtk_combo_box_set_active(GTK_COMBO_BOX(config_combobox_faceset), i);
                }
            } else {
                break;
            }
        }
    }

    // Set current display mode.
    GtkTreeModel *model;
    model = gtk_combo_box_get_model(config_combobox_displaymode);
    bool next = gtk_tree_model_get_iter_first(model, &iter);
    while (next) {
        int current;
        gtk_tree_model_get(model, &iter, 1, &current, -1);
        if (current == want_config[CONFIG_DISPLAYMODE]) {
            gtk_combo_box_set_active_iter(config_combobox_displaymode, &iter);
            break;
        }
        next = gtk_tree_model_iter_next(model, &iter);
    }

    // Lighting option indexes never change, so set option using index.
    gtk_combo_box_set_active(config_combobox_lighting,
                             want_config[CONFIG_LIGHTING]);

    gtk_file_chooser_set_filename(ui_filechooser, window_xml_file);
#ifdef WIN32
    /* On Windows, if set_filename didn't resolve (stale ini path), fall back to
     * navigating the chooser to the bundled ui directory so it opens usefully. */
    {
        gchar *check = gtk_file_chooser_get_filename(ui_filechooser);
        if (check == NULL) {
            gchar *abs_ui_dir = g_build_filename(CF_DATADIR_RT, "ui", NULL);
            gtk_file_chooser_set_current_folder(ui_filechooser, abs_ui_dir);
            g_free(abs_ui_dir);
        }
        g_free(check);
    }
#endif
    gtk_file_chooser_set_filename(theme_filechooser, theme);
#ifdef WIN32
    /* Same fallback for the theme chooser. */
    {
        gchar *check = gtk_file_chooser_get_filename(theme_filechooser);
        if (check == NULL) {
            gchar *abs_theme_dir = g_build_filename(CF_DATADIR_RT, "themes", NULL);
            gtk_file_chooser_set_current_folder(theme_filechooser, abs_theme_dir);
            g_free(abs_theme_dir);
        }
        g_free(check);
    }
#endif
}

/**
 * Get an integer value from 'column' of the active field in 'combobox'.
 */
static int combobox_get_value(GtkComboBox *combobox, int column) {
    GtkTreeModel *model = gtk_combo_box_get_model(combobox);
    GtkTreeIter iter;
    int result;

    gtk_combo_box_get_active_iter(combobox, &iter);
    gtk_tree_model_get(model, &iter, column, &result, -1);
    return result;
}

/**
 * This is basically the opposite of setup_config_dialog() above - instead of
 * setting the display state appropriately, we read the display state and
 * update the want_config values.
 */
static void read_config_dialog(void) {
    want_config[CONFIG_ECHO] =
        gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(config_button_echo));
    want_config[CONFIG_FASTTCP] =
        gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(config_button_fasttcp));
    want_config[CONFIG_TIMESTAMP] = gtk_toggle_button_get_active(
        GTK_TOGGLE_BUTTON(config_button_timestamp));
    want_config[CONFIG_GRAD_COLOR] = gtk_toggle_button_get_active(
        GTK_TOGGLE_BUTTON(config_button_grad_color));
    want_config[CONFIG_FOODBEEP] =
        gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(config_button_foodbeep));
    want_config[CONFIG_SOUND] =
        gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(config_button_sound));
    want_config[CONFIG_CACHE] =
        gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(config_button_cache));
    want_config[CONFIG_DOWNLOAD] =
        gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(config_button_download));
    want_config[CONFIG_FOGWAR] =
        gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(config_button_fog));
    want_config[CONFIG_SMOOTH] = gtk_toggle_button_get_active(
        GTK_TOGGLE_BUTTON(config_button_smoothing));

    gchar *buf = 0;
    GtkTreeIter iter;
    /**
     * Since the combo box does not have the "has-entry" property set to TRUE, we cannot use
     * gtk_combo_box_text_get_active_text to get the currently selected option.
     * Since we really have no good reason to turn that on and open up the box for
     * arbitrary faceset strings, we can treat it more like a regular combo box.
     * We need to use an iterator retrieval and gtk_tree_model_get to fetch the text,
     * which is significantly more of a pain in the posterior.
     *
     * Daniel Hawkins -- 2020-11-21
     */
    if (gtk_combo_box_get_active_iter(GTK_COMBO_BOX(config_combobox_faceset), &iter)) {
        // We have an active selection in our iterator. Now we get the string from the tree model.
        GtkTreeModel *model = gtk_combo_box_get_model(GTK_COMBO_BOX(config_combobox_faceset));
        gtk_tree_model_get(model, &iter, 0, &buf, -1);
        if (buf) {
            free(face_info.want_faceset);
            face_info.want_faceset = g_strdup(buf);
            g_free(buf);
        }
        else {
            LOG(LOG_ERROR, "read_config_dialog", "Failed to get face set string from GTK Widget.");
        }
    }

    want_config[CONFIG_DISPLAYMODE] =
        combobox_get_value(config_combobox_displaymode, 1);

    // Lighting option indexes never change, so get option using index.
    want_config[CONFIG_LIGHTING] =
        gtk_combo_box_get_active(config_combobox_lighting);

    // Set UI file.
    buf = gtk_file_chooser_get_filename(ui_filechooser);
    if (buf != NULL) {
        gchar *abs_ui = g_canonicalize_filename(buf, NULL);
        g_free(buf);
        g_strlcpy(window_xml_file, abs_ui, sizeof(window_xml_file));
        g_free(abs_ui);
    }

    // Set and load theme file.
    buf = gtk_file_chooser_get_filename(theme_filechooser);
    if (buf != NULL) {
        gchar *abs_theme = g_canonicalize_filename(buf, NULL);
        g_free(buf);
        if (g_ascii_strcasecmp(abs_theme, theme) != 0) {
            g_free(theme);
            theme = abs_theme;
            load_theme(TRUE);
        } else {
            g_free(abs_theme);
        }
    }

    if (IS_DIFFERENT(CONFIG_GRAD_COLOR)) {
        draw_stats(TRUE);
    }
}

void on_configure_activate(GtkWidget *menuitem, gpointer user_data) {
    gtk_widget_show(config_dialog);
    setup_config_dialog();
}

static void on_config_close(GtkButton *button, gpointer user_data) {
    read_config_dialog();
    config_check();
    save_defaults();
    gtk_widget_hide(config_dialog);
}

/**
 * Save client window positions to a file unique to each layout.
 */
void save_winpos() {
    GSList *pane_list, *list_loop;
    int x, y, w, h, wx, wy;

    /* Save window position and size. */
    get_window_coord(window_root, &x, &y, &wx, &wy, &w, &h);

    GString *window_root_info = g_string_new(NULL);
    g_string_printf(window_root_info, "+%d+%dx%dx%d", wx, wy, w, h);

    g_key_file_set_string(config, ui_name(),
            "window_root", window_root_info->str);
    g_string_free(window_root_info, TRUE);

    /* Save the positions of all the PANEDs. */
    pane_list = gtk_builder_get_objects(window_xml);

    for (list_loop = pane_list; list_loop != NULL; list_loop = list_loop->next) {
        GType type = G_OBJECT_TYPE(list_loop->data);

        if (type == GTK_TYPE_PANED) {
            g_key_file_set_integer(config, ui_name(),
                    gtk_buildable_get_name(list_loop->data),
                    gtk_paned_get_position(GTK_PANED(list_loop->data)));
        }
    }

    g_slist_free(pane_list);
    save_defaults();

    draw_ext_info(NDI_BLUE, MSG_TYPE_CLIENT, MSG_TYPE_CLIENT_CONFIG,
                  "Window positions saved!");
}

/**
 * Handles saving of the window positions when the Client | Save Window
 * Position menu item is activated.
 *
 * @param menuitem
 * @param user_data
 */
void on_save_window_position_activate(GtkMenuItem *menuitem,
        gpointer user_data) {
    save_winpos();
    /*
     * The following prevents multiple saves per menu activation.
     */
    g_signal_stop_emission_by_name(GTK_WIDGET(menuitem), "activate");
}

/**
 * Resize the client window and its panels using saved window positions.
 *
 * @param window_root The client's main window.
 */
void load_window_positions(GtkWidget *window_root) {
    GSList *pane_list, *list;
    pane_list = gtk_builder_get_objects(window_xml);

    // Load and set main window dimensions.
    gchar *root_size = g_key_file_get_string(config, ui_name(),
            "window_root", NULL);

    if (root_size != NULL) {
        int w, h;

        if (sscanf(root_size, "+%*d+%*dx%dx%d", &w, &h) == 2) {
            gtk_window_set_default_size(GTK_WINDOW(window_root), w, h);
        }

        g_free(root_size);
    }

    // Load and set panel positions.
    for (list = pane_list; list != NULL; list = list->next) {
        GType type = G_OBJECT_TYPE(list->data);

        if (type == GTK_TYPE_PANED) {
            int position = g_key_file_get_integer(config, ui_name(),
                    gtk_buildable_get_name(list->data), NULL);

            if (position != 0) {
                gtk_paned_set_position(GTK_PANED(list->data), position);
            }
        }
    }

    g_slist_free(pane_list);
}
