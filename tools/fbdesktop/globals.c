/* Shared state for fbdesktop. Every symbol here is declared extern in
 * fbdesktop.h; nothing else in the program defines file-scope state that
 * more than one module needs. */
#include "fbdesktop.h"

struct icon icons[NUM_ICONS] = {
	{"TASK MGR",  NULL, 0x3b82f6, 5, G_GAUGE},
	{"FILES",     NULL, 0x06b6d4, 4, G_FOLDER},
	{"TERMINAL",  NULL, 0x9333ea, 3, G_TERM},
	{"BROWSER",   NULL, 0x0ea5e9, 7, G_GLOBE},
	{"CALCULATOR",NULL, 0x14b8a6, 8, G_CALC},
	{"PAINT",     NULL, 0xec4899, 9, G_PAINT},
	{"CALENDAR",  NULL, 0xf59e0b, 10, G_CAL},
	{"SETTINGS",  NULL, 0x64748b, 6, G_GEAR},
	{"TIMER",     NULL, 0xfbbf24, 11, G_TIMER},
	{"SEARCH",    NULL, 0x38bdf8, 12, G_SEARCH},
	{"IMAGES",    NULL, 0xf472b6, 14, G_IMAGE},
	{"ARCHIVE",   NULL, 0xa78bfa, 15, G_ARCHIVE},
	{"SNIPPING",  NULL, 0x34d399, 16, G_SNAP},
	{"REBOOT",    NULL, 0xef4444, 1, G_REFRESH},
	{"POWER OFF", NULL, 0xf43f5e, 2, G_POWER},
};

const struct theme themes[NUM_THEMES] = {
	{"Midnight", 0x232338, 0x14141f, 0x3b82f6},
	{"Forest",   0x1e2f24, 0x0d1712, 0x22c55e},
	{"Ember",    0x2f2420, 0x1a1210, 0xf97316},
	{"Solarized",0x002b36, 0x073642, 0xb58900},
	{"Dracula",  0x282a36, 0x21222c, 0xff79c6},
};

const struct tmtab tm_tabs[TM_NTABS] = {
	{"Processes",   NULL},
	{"Performance", NULL},
	{"Disk",        NULL},
	{"System",      NULL},
};

const char *ctx_items[CTX_NITEMS] = { "Copy", "Cut", "Paste", "Rename", "Delete", "Compress" };
const char *fm_btns[FM_NBTN] = { "New File", "New Dir", "Delete", "Refresh" };

int theme_idx;
int show_hidden;
int dblclick_delay = 200;

int clock_24h = 1;
int clock_show_secs = 1;
int clock_show_date;
int wallpaper_solid;
int show_icon_labels = 1;
int confirm_delete = 1;
int fm_sort_by_size;

int cpu_hist[HIST], mem_hist[HIST];

int icon_press = -1;
int icon_dragged;
int icon_grab_dx, icon_grab_dy;

int ctxmenu_mode;
int ctxmenu_win = -1;
int ctxmenu_entidx = -1;
int ctxmenu_deskidx = -1;
int ctxmenu_x, ctxmenu_y;

char clip_path[FM_FULLLEN];
int clip_mode;

struct deskfile desk_files[DESK_MAXFILES];
int desk_count;

int fmdrag_win = -1;
int fmdrag_entidx = -1;
int fmdrag_active;
int fmdrag_was_preselected;
int fmdrag_grab_x, fmdrag_grab_y;

struct window wins[MAX_WIN];
int zorder[MAX_WIN], zcount;
int focused = -1;
int drag_mode;
int drag_win = -1;
int alt_held;
int sd_active;
int sd_saved[MAX_WIN];
int start_menu_open;
char start_filter[32];

uint8_t *fbp;
uint8_t *backbuf;
struct fb_var_screeninfo vinfo;
struct fb_fix_screeninfo finfo;
int xres, yres, bpp, line_length;
unsigned char font[512 * 32 * 4];
int have_font;
int font_w = 8, font_h = 16, font_bpr = 1;
int mx, my, prev_left, prev_right;
int confd = -1;
int absptr_fd = -1, kbd_evdev_fd = -1, wheel_fd = -1;
int abs_minx, abs_maxx, abs_miny, abs_maxy;
int abs_curx, abs_cury, abs_btn, abs_rbtn;
FILE *dbg;
struct termios orig_termios;
int have_orig_termios;
