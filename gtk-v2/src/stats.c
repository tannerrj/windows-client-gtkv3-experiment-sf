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
 * Support for character statistics.
 */

#include "client.h"

#include <assert.h>
#include <gtk/gtk.h>

#include "main.h"
#include "image.h"
#include "gtk2proto.h"

#define STAT_BAR_HP     0
#define STAT_BAR_SP     1
#define STAT_BAR_GRACE  2
#define STAT_BAR_FOOD   3
#define STAT_BAR_EXP    4
#define MAX_STAT_BARS   5

static const char * const stat_bar_names[MAX_STAT_BARS] = {
    "hp", "sp", "grace", "food", "exp"
};

static GtkWidget *stat_bar[MAX_STAT_BARS];

#define STYLE_NORMAL        0
#define STYLE_LOW           1
#define STYLE_SUPER         2
#define STYLE_GRAD_NORMAL   3
#define STYLE_GRAD_LOW      4
#define STYLE_GRAD_SUPER    5
#define NUM_STYLES          6

/* The name of the symbolic widget names we try to look up the styles of
 * (these will be prefixed with hp_, sp_, etc).  This should always match
 * NUM_STYLES.
 */
static const char * const stat_style_names[NUM_STYLES] = {
    "bar_normal", "bar_low", "bar_super",
    "gradual_bar_normal", "gradual_bar_low", "gradual_bar_super"
};

/* We really only care about the colors, as there isn't anything else we can
 * change about the progressbar widget itself.
 */
GdkRGBA *bar_colors[MAX_STAT_BARS][NUM_STYLES];


/* The table for showing skill exp is an x & y grid.  Note for proper
 * formatting, SKILL_BOXES_X must be even.  Hmmm - perhaps these should
 * instead be dynamically generated?
 */
#define SKILL_BOXES_X   6
#define SKILL_BOXES_Y   17

#define PROTECTION_BOXES_X      6
#define PROTECTION_BOXES_Y      6

typedef struct {
    GtkWidget *playername;
    GtkWidget *Str;
    GtkWidget *Dex;
    GtkWidget *Con;
    GtkWidget *Int;
    GtkWidget *Wis;
    GtkWidget *Cha;
    GtkWidget *Pow;
    GtkWidget *wc;
    GtkWidget *dam;
    GtkWidget *ac;
    GtkWidget *armor;
    GtkWidget *speed;
    GtkWidget *weapon_speed;
    GtkWidget *range;
    GtkWidget *exp;
    GtkWidget *level;
    GtkWidget *table_skills_exp;
    GtkWidget *table_protections;
    GtkWidget *skill_exp[SKILL_BOXES_X * SKILL_BOXES_Y];
    GtkWidget *resists[PROTECTION_BOXES_X * PROTECTION_BOXES_Y];

} StatWindow;

static StatWindow statwindow;

static gboolean need_mapping_update;

static int lastval[MAX_STAT_BARS], lastmax[MAX_STAT_BARS];

/**
 * Initialize stat bar colors from the active CSS theme.  Falls back to
 * Standard theme colors if the CSS classes are not found.  Safe to call
 * multiple times; frees old allocations before re-initializing.
 */
void stats_get_styles(void)
{
    int i, j;

    /* Free previous allocations. */
    for (i = 0; i < MAX_STAT_BARS; i++) {
        for (j = 0; j < NUM_STYLES; j++) {
            g_free(bar_colors[i][j]);
            bar_colors[i][j] = NULL;
        }
    }

    /* Hardcoded fallback colors matching the Standard theme. */
    static const GdkRGBA d_normal  = {0.000, 0.812, 0.000, 1.0}; /* #00cf00 */
    static const GdkRGBA d_low     = {0.812, 0.000, 0.000, 1.0}; /* #cf0000 */
    static const GdkRGBA d_grad_l  = {1.000, 0.000, 0.000, 1.0}; /* red     */
    static const GdkRGBA d_grad_h  = {0.000, 0.502, 0.000, 1.0}; /* green   */
    static const GdkRGBA d_grad_s  = {0.000, 0.000, 1.000, 1.0}; /* blue    */

    GdkRGBA c_normal = d_normal, c_low = d_low, c_super = d_normal;
    GdkRGBA c_grad_l = d_grad_l, c_grad_n = d_grad_h, c_grad_s = d_grad_s;

    get_css_fg_color("cf-stat-normal",      &c_normal);
    get_css_fg_color("cf-stat-low",         &c_low);
    get_css_fg_color("cf-stat-super",       &c_super);
    get_css_fg_color("cf-stat-grad-low",    &c_grad_l);
    get_css_fg_color("cf-stat-grad-normal", &c_grad_n);
    get_css_fg_color("cf-stat-grad-super",  &c_grad_s);

    for (i = 0; i < MAX_STAT_BARS; i++) {
        bar_colors[i][STYLE_NORMAL]      = g_new(GdkRGBA, 1);
        bar_colors[i][STYLE_LOW]         = g_new(GdkRGBA, 1);
        bar_colors[i][STYLE_SUPER]       = g_new(GdkRGBA, 1);
        bar_colors[i][STYLE_GRAD_NORMAL] = g_new(GdkRGBA, 1);
        bar_colors[i][STYLE_GRAD_LOW]    = g_new(GdkRGBA, 1);
        bar_colors[i][STYLE_GRAD_SUPER]  = g_new(GdkRGBA, 1);

        *bar_colors[i][STYLE_NORMAL]      = c_normal;
        *bar_colors[i][STYLE_LOW]         = c_low;
        *bar_colors[i][STYLE_SUPER]       = c_super;
        *bar_colors[i][STYLE_GRAD_NORMAL] = c_grad_n;
        *bar_colors[i][STYLE_GRAD_LOW]    = c_grad_l;
        *bar_colors[i][STYLE_GRAD_SUPER]  = c_grad_s;
    }
}

/**
 * Initialize the stats window by looking up all GtkBuilder-defined widgets by
 * name and storing their pointers in statwindow. Also populates the skill
 * experience and protection grid labels. Must be called once after the UI is
 * loaded.
 *
 * @param window_root The root window widget (unused directly, kept for
 *                    signature consistency with other init functions).
 */
void stats_init(GtkWidget *window_root) {
    int i, x, y;
    char buf[MAX_BUF];

    for (i=0; i<MAX_STAT_BARS; i++) {
        snprintf(buf, sizeof(buf), "progressbar_%s", stat_bar_names[i]);
        stat_bar[i] = GTK_WIDGET(gtk_builder_get_object(window_xml, buf));

        lastval[i] = -1;
        lastmax[i] = -1;
    }

    statwindow.playername =
        GTK_WIDGET(gtk_builder_get_object(window_xml, "label_playername"));
    statwindow.Str =
        GTK_WIDGET(gtk_builder_get_object(window_xml, "label_str"));
    statwindow.Dex =
        GTK_WIDGET(gtk_builder_get_object(window_xml, "label_dex"));
    statwindow.Con =
        GTK_WIDGET(gtk_builder_get_object(window_xml, "label_con"));
    statwindow.Int =
        GTK_WIDGET(gtk_builder_get_object(window_xml, "label_int"));
    statwindow.Wis =
        GTK_WIDGET(gtk_builder_get_object(window_xml, "label_wis"));
    statwindow.Pow =
        GTK_WIDGET(gtk_builder_get_object(window_xml, "label_pow"));
    statwindow.Cha =
        GTK_WIDGET(gtk_builder_get_object(window_xml, "label_cha"));
    statwindow.wc =
        GTK_WIDGET(gtk_builder_get_object(window_xml, "label_wc"));
    statwindow.dam =
        GTK_WIDGET(gtk_builder_get_object(window_xml, "label_dam"));
    statwindow.ac =
        GTK_WIDGET(gtk_builder_get_object(window_xml, "label_ac"));
    statwindow.armor =
        GTK_WIDGET(gtk_builder_get_object(window_xml, "label_armor"));
    statwindow.speed =
        GTK_WIDGET(gtk_builder_get_object(window_xml, "label_speed"));
    statwindow.weapon_speed =
        GTK_WIDGET(gtk_builder_get_object(window_xml, "label_weapon_speed"));
    statwindow.range =
        GTK_WIDGET(gtk_builder_get_object(window_xml, "label_range"));
    statwindow.exp =
        GTK_WIDGET(gtk_builder_get_object(window_xml, "label_exp"));
    statwindow.level =
        GTK_WIDGET(gtk_builder_get_object(window_xml, "label_level"));

    /* Note that the order the labels are attached to the tables determines
     * the order of display.  The order as right now is left to right,
     * then top to bottom, which means that is the order if displaying
     * skills & protections.
     */

    statwindow.table_skills_exp =
        GTK_WIDGET(gtk_builder_get_object(window_xml,"table_skills_exp"));

    if (statwindow.table_skills_exp) {
        /* Do not attempt to set up the table_skills_exp widget if it was not
         * defined in the layout.
         */
        for (i=0, x=0, y=0; i < SKILL_BOXES_X * SKILL_BOXES_Y; i++) {
            statwindow.skill_exp[i] = gtk_label_new("");
            gtk_grid_attach(GTK_GRID(statwindow.table_skills_exp), statwindow.skill_exp[i],
                            x, y, 1, 1);
            gtk_widget_show(statwindow.skill_exp[i]);
            x++;
            if (x == SKILL_BOXES_X) {
                x=0;
                y++;
            }
        }
    }

    statwindow.table_protections =
        GTK_WIDGET(gtk_builder_get_object(window_xml, "table_protections"));

    for (i=0, x=0, y=0; i < PROTECTION_BOXES_X * PROTECTION_BOXES_Y; i++) {
        statwindow.resists[i] = gtk_label_new("");
        gtk_grid_attach(GTK_GRID(statwindow.table_protections), statwindow.resists[i],
                        x, y, 1, 1);
        gtk_widget_show(statwindow.resists[i]);
        x++;
        if (x == PROTECTION_BOXES_X) {
            x=0;
            y++;
        }
    }
    stats_get_styles();
}

/**
 * Format an integer using an appropriate SI prefix.
 *
 * @param number Number to format
 * @return A newly-allocated string with the formatted number
 */
static char *format_si_number(gint64 number) {
    /* List of SI prefixes and corresponding values, least to greatest. */
    const char SI_SUFFIX[] = {'\0', 'k', 'M', 'G'};
    const float SI_VALUE[] = {1, 1000, 1000000, 1000000000};

    int suffix = 0, i;
    float value;

    /* Determine the most appropriate SI prefix to use. */
    for (i = sizeof(SI_SUFFIX) / sizeof(char) - 1; i > 0; i--) {
        if (number / SI_VALUE[i] >= 10) {
            suffix = i;
            break;
        }
    }

    value = number / SI_VALUE[suffix];

    /* If possible, trim the trailing zero decimal. */
    if (value - (int)value == 0) {
        return g_strdup_printf("%.0f%c", value, SI_SUFFIX[suffix]);
    } else {
        return g_strdup_printf("%.1f%c", value, SI_SUFFIX[suffix]);
    }
}

/**
 * Updates the stat bar and text display as it pertains to a specific stat.
 *
 * @param stat_no
 * The stat number to update.
 * @param max_stat
 * The normal maximum value this stat can have.  Note that within game terms,
 * the actual value can go above this via supercharging stats.
 * @param current_stat
 * current value of the stat.
 * @param statbar_max
 * @param statbar_stat
 * this is the stat value to use for drawing the statbar.  For most
 * stats, this is same as current stat, but for the exp bar,
 * we basically want it to be a graph relative to amount for next level.
 * @param can_alert
 * Whether this stat can go on alert when it gets low.  It doesn't make
 * sense for this to happen on exp (not really an alert if you gain a
 * level). Note: This is no longer used with the new style code - if
 * a stat shouldn't ever change color when it is low, the style should
 * dictate that.
 */
/**
 * Apply a GdkRGBA background color to a progress bar widget via a CSS provider.
 * Passing NULL removes any previously applied custom color.
 *
 * @param bar   The GtkProgressBar widget to style.
 * @param color RGBA color to apply, or NULL to clear.
 */
static void set_bar_color(GtkWidget *bar, const GdkRGBA *color) {
    GtkStyleContext *ctx = gtk_widget_get_style_context(bar);
    static GtkCssProvider *providers[MAX_STAT_BARS] = {NULL};
    int idx = -1;
    for (int i = 0; i < MAX_STAT_BARS; i++) {
        if (stat_bar[i] == bar) { idx = i; break; }
    }
    if (idx < 0) return;
    if (providers[idx]) {
        gtk_style_context_remove_provider(ctx, GTK_STYLE_PROVIDER(providers[idx]));
        g_object_unref(providers[idx]);
        providers[idx] = NULL;
    }
    if (color) {
        char css[128];
        snprintf(css, sizeof(css),
                 "progressbar progress { background-color: rgba(%d,%d,%d,%.3f); }",
                 (int)(color->red * 255), (int)(color->green * 255),
                 (int)(color->blue * 255), color->alpha);
        providers[idx] = gtk_css_provider_new();
        gtk_css_provider_load_from_data(providers[idx], css, -1, NULL);
        gtk_style_context_add_provider(ctx, GTK_STYLE_PROVIDER(providers[idx]),
                                       GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    }
}

void update_stat(int stat_no, gint64 max_stat, gint64 current_stat,
                 gint64 statbar_max, gint64 statbar_stat, int can_alert)
{
    float bar;
    GdkRGBA ncolor, *set_color=NULL;

    /* If nothing changed, don't need to do anything */
    if (lastval[stat_no] == current_stat && lastmax[stat_no] == max_stat) {
        return;
    }

    lastval[stat_no] = current_stat;
    lastmax[stat_no] = max_stat;

    if (statbar_max > 0) {
        bar = (float) statbar_stat / (float) statbar_max;
    } else {
        bar = 0.0;
    }

    if (use_config[CONFIG_GRAD_COLOR]) {
        /* In this mode, the color of the stat bar were go between red and green
         * in a gradual style. Color is blended from low to high
         */

        GdkRGBA         *hcolor, *lcolor;
        float       diff;

        /* First thing we do is figure out current values, and thus what color
         * bases we use (based on super charged or normal value).  We also set
         * up diff as where we are between those two points.  In this way, the
         * blending logic below is the same regardless of actual value.
         */
        if (bar > 1.0) {
            if (bar>2.0) {
                bar=2.0;    /* Display unaffected; just calculations */
            }
            hcolor = bar_colors[stat_no][STYLE_GRAD_SUPER];
            lcolor = bar_colors[stat_no][STYLE_GRAD_NORMAL];
            diff = bar - 1.0;
        } else {
            if (bar < 0.0) {
                bar=0.0;    /* Like above, does not affect display */
            }
            hcolor = bar_colors[stat_no][STYLE_GRAD_NORMAL];
            lcolor = bar_colors[stat_no][STYLE_GRAD_LOW];
            diff = bar;
        }
        /* Now time to blend.  First, make sure colors are set.  then, we use
         * the lcolor as the base, making adjustments based on hcolor.  Values
         * in hcolor may be lower than lcolor, but that just means we
         * substract from lcolor, not add.
         */

        if (lcolor && hcolor) {
            ncolor = *lcolor;
            ncolor.red   += (hcolor->red   - lcolor->red)   * diff;
            ncolor.green += (hcolor->green - lcolor->green) * diff;
            ncolor.blue  += (hcolor->blue  - lcolor->blue)  * diff;
            set_color = &ncolor;
        }
    } else {
        if (statbar_stat * 4 < statbar_max) {
            set_color = bar_colors[stat_no][STYLE_LOW];
        } else if (statbar_stat > statbar_max) {
            set_color = bar_colors[stat_no][STYLE_SUPER];
        } else {
            set_color = bar_colors[stat_no][STYLE_NORMAL];
        }
    }
    if (bar > 1.0) {
        bar = 1.0;
    }
    if (bar < 0.0) {
        bar = 0.0;
    }

    GtkProgressBar *curr_bar = GTK_PROGRESS_BAR(stat_bar[stat_no]);

    set_bar_color(stat_bar[stat_no], set_color);

    char *label;

    // Display experience using SI prefixes; everything else normally.
    if (stat_no == STAT_BAR_EXP) {
        char *exp_curr = format_si_number(current_stat);
        char *exp_max = format_si_number(max_stat);
        label = g_strdup_printf("%s/%s", exp_curr, exp_max);
        g_free(exp_curr);
        g_free(exp_max);
    } else {
        label = g_strdup_printf("%" G_GINT64_FORMAT "/%" G_GINT64_FORMAT,
                current_stat, max_stat);
    }

    gtk_progress_bar_set_fraction(curr_bar, bar);
    gtk_progress_bar_set_text(curr_bar, label);
    g_free(label);
}

/**
 * Updates the stats pane - hp, sp, etc labels
 *
 * @param redraw
 */
void draw_message_window(int redraw)
{
    static int lastbeep=0;
    static gint64 level_diff;

    update_stat(STAT_BAR_HP, cpl.stats.maxhp, cpl.stats.hp,
                cpl.stats.maxhp, cpl.stats.hp, TRUE);
    update_stat(STAT_BAR_SP, cpl.stats.maxsp, cpl.stats.sp,
                cpl.stats.maxsp, cpl.stats.sp, TRUE);
    update_stat(STAT_BAR_GRACE, cpl.stats.maxgrace, cpl.stats.grace,
                cpl.stats.maxgrace, cpl.stats.grace, TRUE);
    update_stat(STAT_BAR_FOOD, 999, cpl.stats.food, 999, cpl.stats.food, TRUE);

    /* We may or may not have an exp table from the server.  If we don't, just
     * use current exp value so it will always appear maxed out.
     */
    /* We calculate level_diff here just so it makes the update_stat()
     * call below less messy.
     */
    if ((cpl.stats.level+1) < exp_table_max) {
        level_diff = exp_table[cpl.stats.level+1] - exp_table[cpl.stats.level];
    } else {
        level_diff=cpl.stats.exp;
    }

    update_stat(STAT_BAR_EXP,
                (cpl.stats.level+1) < exp_table_max ? exp_table[cpl.stats.level+1]:cpl.stats.exp,
                cpl.stats.exp,
                level_diff,
                (cpl.stats.level+1) < exp_table_max ?
                (cpl.stats.exp - exp_table[cpl.stats.level]):cpl.stats.exp, FALSE);

    if (use_config[CONFIG_FOODBEEP] && (cpl.stats.food%4==3) && (cpl.stats.food < 200)) {
        gdk_beep( );
    } else if (use_config[CONFIG_FOODBEEP] && cpl.stats.food == 0 && ++lastbeep == 5) {
        lastbeep = 0;
        gdk_beep( );
    }
}

/**
 * The mapping tables may not be completely full, so handle null values.
 * Always treat null values as later in the sort order.
 */
static int mapping_sort(NameMapping *a, NameMapping *b)
{
    if (!a->name && !b->name) {
        return 0;
    }
    if (!a->name) {
        return 1;
    }
    if (!b->name) {
        return -1;
    } else {
        return g_ascii_strcasecmp(a->name, b->name);
    }
}

/**
 *
 */
static void update_stat_mapping(void)
{
    int i;

    for (i=0; i < MAX_SKILL; i++) {
        skill_mapping[i].value=i;
        if (skill_names[i]) {
            skill_mapping[i].name = skill_names[i];
        } else {
            skill_mapping[i].name = NULL;
        }
    }
    qsort(skill_mapping, MAX_SKILL, sizeof(NameMapping),
          (int (*)(const void*, const void*))mapping_sort);

    for (i=0; i < NUM_RESISTS; i++) {
        resist_mapping[i].value=i;
        if (resists_name[i]) {
            resist_mapping[i].name = resists_name[i];
        } else {
            resist_mapping[i].name = NULL;
        }
    }
    qsort(resist_mapping, NUM_RESISTS, sizeof(NameMapping),
          (int (*)(const void*, const void*))mapping_sort);

    need_mapping_update = FALSE;
}

/**
 * Draws the stats window.  If redraw is true, it means we need to redraw the
 * entire thing, and not just do an updated
 */
void draw_stats(int redraw)
{
    static Stats last_stats;
    static char last_name[MAX_BUF]="", last_range[MAX_BUF]="";
    static int init_before=0, max_drawn_skill=0, max_drawn_resists=0;

    float weap_sp;
    char buff[MAX_BUF];
    int i, on_skill, sk;

    if (!init_before) {
        init_before=1;
        memset(&last_stats, 0, sizeof(Stats));
    }

    /* skill_names gets set as part of the initialization with the
     * client - however, right now, there is no callback when
     * it is set, so instead, just track that wee need to update
     * and see if it changes.
     */
    if (need_mapping_update && skill_names[1] != NULL) {
        update_stat_mapping();
    }

    if (strcmp(cpl.title, last_name) || redraw) {
        strcpy(last_name, cpl.title);
        gtk_label_set_text(GTK_LABEL(statwindow.playername), cpl.title);
    }

    if (redraw || cpl.stats.exp != last_stats.exp) {
        last_stats.exp = cpl.stats.exp;
        snprintf(buff, sizeof(buff), "Experience: %5" G_GINT64_FORMAT,
                 cpl.stats.exp);
        gtk_label_set_text(GTK_LABEL(statwindow.exp), buff);
    }

    if (redraw || cpl.stats.level != last_stats.level) {
        last_stats.level = cpl.stats.level;
        snprintf(buff, sizeof(buff), "Level: %d", cpl.stats.level);
        gtk_label_set_text(GTK_LABEL(statwindow.level), buff);
    }

    if (redraw || cpl.stats.Str != last_stats.Str) {
        last_stats.Str = cpl.stats.Str;
        snprintf(buff, sizeof(buff), "%2d", cpl.stats.Str);
        gtk_label_set_text(GTK_LABEL(statwindow.Str), buff);
    }

    if (redraw || cpl.stats.Dex != last_stats.Dex) {
        last_stats.Dex = cpl.stats.Dex;
        snprintf(buff, sizeof(buff), "%2d", cpl.stats.Dex);
        gtk_label_set_text(GTK_LABEL(statwindow.Dex), buff);
    }

    if (redraw || cpl.stats.Con != last_stats.Con) {
        last_stats.Con = cpl.stats.Con;
        snprintf(buff, sizeof(buff), "%2d", cpl.stats.Con);
        gtk_label_set_text(GTK_LABEL(statwindow.Con), buff);
    }

    if (redraw || cpl.stats.Int != last_stats.Int) {
        last_stats.Int = cpl.stats.Int;
        snprintf(buff, sizeof(buff), "%2d", cpl.stats.Int);
        gtk_label_set_text(GTK_LABEL(statwindow.Int), buff);
    }

    if (redraw || cpl.stats.Wis != last_stats.Wis) {
        last_stats.Wis = cpl.stats.Wis;
        snprintf(buff, sizeof(buff), "%2d", cpl.stats.Wis);
        gtk_label_set_text(GTK_LABEL(statwindow.Wis), buff);
    }

    if (redraw || cpl.stats.Pow != last_stats.Pow) {
        last_stats.Pow = cpl.stats.Pow;
        snprintf(buff, sizeof(buff), "%2d", cpl.stats.Pow);
        gtk_label_set_text(GTK_LABEL(statwindow.Pow), buff);
    }

    if (redraw || cpl.stats.Cha != last_stats.Cha) {
        last_stats.Cha = cpl.stats.Cha;
        snprintf(buff, sizeof(buff), "%2d", cpl.stats.Cha);
        gtk_label_set_text(GTK_LABEL(statwindow.Cha), buff);
    }

    if (redraw || cpl.stats.wc != last_stats.wc) {
        last_stats.wc = cpl.stats.wc;
        snprintf(buff, sizeof(buff), "%2d", cpl.stats.wc);
        gtk_label_set_text(GTK_LABEL(statwindow.wc), buff);
    }

    if (redraw || cpl.stats.dam != last_stats.dam) {
        last_stats.dam = cpl.stats.dam;
        snprintf(buff, sizeof(buff), "%d", cpl.stats.dam);
        gtk_label_set_text(GTK_LABEL(statwindow.dam), buff);
    }

    if (redraw || cpl.stats.ac != last_stats.ac) {
        last_stats.ac = cpl.stats.ac;
        snprintf(buff, sizeof(buff), "%d", cpl.stats.ac);
        gtk_label_set_text(GTK_LABEL(statwindow.ac), buff);
    }

    if (redraw || cpl.stats.resists[0] != last_stats.resists[0]) {
        last_stats.resists[0] = cpl.stats.resists[0];
        snprintf(buff, sizeof(buff), "%d", cpl.stats.resists[0]);
        gtk_label_set_text(GTK_LABEL(statwindow.armor), buff);
    }

    if (redraw || cpl.stats.speed != last_stats.speed) {
        last_stats.speed = cpl.stats.speed;
        snprintf(buff, sizeof(buff), "%3.2f",
                 (float)cpl.stats.speed / FLOAT_MULTF);
        gtk_label_set_text(GTK_LABEL(statwindow.speed), buff);
    }
    /* sc_version >= 1029 reports real value of weapon speed -
     * not as a factor of player speed.  Handle accordingly.
     */
    if (csocket.sc_version >= 1029) {
        weap_sp = (float)cpl.stats.weapon_sp / FLOAT_MULTF;
    } else {
        weap_sp = (float)cpl.stats.speed / ((float)cpl.stats.weapon_sp);
    }

    if (redraw || weap_sp != last_stats.weapon_sp) {
        last_stats.weapon_sp = weap_sp;
        snprintf(buff, sizeof(buff), "%3.2f", weap_sp);
        gtk_label_set_text(GTK_LABEL(statwindow.weapon_speed), buff);
    }

    if (redraw || strcmp(cpl.range, last_range)) {
        strcpy(last_range, cpl.range);
        snprintf(buff, sizeof(buff), "Range: %s", cpl.range);
        gtk_label_set_text(GTK_LABEL(statwindow.range), cpl.range);
    }

    update_skill_information();

    if (statwindow.table_skills_exp) {
        /* Do not attempt to set up the table_skills_exp widget if it was not
         * defined in the layout.
         */
        on_skill=0;
        assert(sizeof(statwindow.skill_exp)/sizeof(*statwindow.skill_exp) >= 2*MAX_SKILL);
        for (i=0; i<MAX_SKILL; i++) {
            /* Drawing a particular skill entry is tricky - only draw if
             * different, and only draw if we have a name for the skill and
             * the player has some exp in the skill - don't draw all 30 skills
             * for no reason.
             */
            sk = skill_mapping[i].value;

            if ((redraw || cpl.stats.skill_exp[sk] != last_stats.skill_exp[sk])
                    && skill_mapping[i].name && cpl.stats.skill_exp[sk]) {
                gtk_label_set_text(GTK_LABEL(statwindow.skill_exp[on_skill++]),
                              skill_mapping[i].name);
                snprintf(buff, sizeof(buff), "%" G_GINT64_FORMAT " (%d)",
                         cpl.stats.skill_exp[sk], cpl.stats.skill_level[sk]);
                gtk_label_set_text(
                    GTK_LABEL(statwindow.skill_exp[on_skill++]), buff);
                last_stats.skill_level[sk] = cpl.stats.skill_level[sk];
                last_stats.skill_exp[sk] = cpl.stats.skill_exp[sk];
            } else if (cpl.stats.skill_exp[sk]) {
                /* Don't need to draw the skill, but need to update the
                 * position of where to draw the next one.
                 */
                on_skill+=2;
            }
        }

        /* Since the number of skills we draw come and go, basically we want
         * to erase any extra.  This shows up when switching characters, eg,
         * character #1 knows 10 skills, #2 knows 5 - need to erase those 5
         * extra.
         */
        if (on_skill < max_drawn_skill) {
            for (int k = on_skill; k <= max_drawn_skill; k++) {
                gtk_label_set_text(GTK_LABEL(statwindow.skill_exp[k]), "");
            }
        }
        max_drawn_skill = on_skill;
    }

    /* Now do the resistance table */
    if (redraw || cpl.stats.resist_change) {
        int j = 0;

        cpl.stats.resist_change=0;
        for (int i = 0; i < NUM_RESISTS; i++) {
            sk = resist_mapping[i].value;
            if (cpl.stats.resists[sk]) {
                gtk_label_set_text(GTK_LABEL(statwindow.resists[j]),
                                   resist_mapping[i].name);
                j++;
                snprintf(buff, sizeof(buff), "%+4d", cpl.stats.resists[sk]);
                gtk_label_set_text(GTK_LABEL(statwindow.resists[j]), buff);
                j++;
                if (j >= PROTECTION_BOXES_X * PROTECTION_BOXES_Y) {
                    break;
                }
            }
        }
        /* Erase old/unused resistances */
        if (j < max_drawn_resists) {
            for (int i = j; i <= max_drawn_resists; i++) {
                gtk_label_set_text(GTK_LABEL(statwindow.resists[i]), "");
            }
        }
        max_drawn_resists = j;
    } /* if we draw the resists */


    /* Don't need to worry about hp, sp, grace, food - update_stat()
     * deals with that as part of the stat bar logic.
     */

}

void clear_stat_mapping() {
    need_mapping_update = TRUE;
}
