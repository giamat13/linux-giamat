/* Minimal framebuffer desktop: every icon opens a draggable/resizable window
 * (a real pty-backed VT100-ish terminal, or a one-shot command-output view),
 * plus a taskbar. No X11, no browser -- draws directly to /dev/fb0, reads
 * mouse from /dev/input/mice and keyboard from stdin in raw mode.
 * Font is pulled live from the kernel's own VT console font (KDFONTOP). */
#define _XOPEN_SOURCE 700
#define _DEFAULT_SOURCE
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <linux/fb.h>
#include <linux/kd.h>
#include <linux/input.h>

#define test_bit(nr, arr) (((arr)[(nr) / 8] >> ((nr) % 8)) & 1)

#define ICON_W 104        /* cell width  */
#define ICON_H 116        /* cell height: tile + label */
#define ICON_GAP 18
#define TILE 72           /* the colored rounded square */
#define TITLE_H 30
#define TASK_H 38
#define MAX_WIN 8
#define WIN_MINW 240
#define WIN_MINH 150
#define GRID_MAXCOLS 220
#define GRID_MAXROWS 110
#define COL_FG_DEFAULT 0xcdd6f4
#define COL_BG_DEFAULT 0x1e1e2e

enum wintype { WIN_TERM, WIN_OUTPUT, WIN_FILES, WIN_TASKMGR, WIN_EDIT, WIN_SETTINGS };

enum glyph {
	G_GAUGE, G_FOLDER, G_TERM, G_REFRESH, G_POWER, G_GEAR, G_FILE,
	G_IMAGE, G_ARCHIVE, G_CODE, G_EXEC, G_GLOBE
};

/* File-type classification by extension -- drives the icon/tag/color shown
 * for a file, both on the desktop and in the File Manager listing.
 * FCAT_EXEC covers extensionless system binaries (busybox, /init, and the
 * whole /bin, /sbin symlink farm): without it they'd be indistinguishable
 * from any other extensionless file under FCAT_OTHER. */
enum fcat { FCAT_DIR, FCAT_IMAGE, FCAT_ARCHIVE, FCAT_CODE, FCAT_TEXT, FCAT_EXEC, FCAT_OTHER };

static enum fcat classify_file(const char *name, int isdir, int isexec)
{
	if (isdir)
		return FCAT_DIR;
	const char *dot = strrchr(name, '.');
	if (!dot || !dot[1])
		return isexec ? FCAT_EXEC : FCAT_OTHER;
	char ext[8];
	int i;
	for (i = 0; dot[1 + i] && i < 7; i++)
		ext[i] = (char)tolower((unsigned char)dot[1 + i]);
	ext[i] = 0;
	if (!strcmp(ext, "png") || !strcmp(ext, "jpg") || !strcmp(ext, "jpeg") ||
	    !strcmp(ext, "gif") || !strcmp(ext, "bmp") || !strcmp(ext, "svg"))
		return FCAT_IMAGE;
	if (!strcmp(ext, "zip") || !strcmp(ext, "tar") || !strcmp(ext, "gz") ||
	    !strcmp(ext, "xz") || !strcmp(ext, "bz2") || !strcmp(ext, "tgz"))
		return FCAT_ARCHIVE;
	if (!strcmp(ext, "sh") || !strcmp(ext, "c") || !strcmp(ext, "h") ||
	    !strcmp(ext, "py") || !strcmp(ext, "js") || !strcmp(ext, "pl"))
		return FCAT_CODE;
	if (!strcmp(ext, "txt") || !strcmp(ext, "md") || !strcmp(ext, "log") ||
	    !strcmp(ext, "conf") || !strcmp(ext, "cfg"))
		return FCAT_TEXT;
	return isexec ? FCAT_EXEC : FCAT_OTHER;
}

static uint32_t fcat_color(enum fcat c)
{
	switch (c) {
	case FCAT_DIR:     return 0x89b4fa;
	case FCAT_IMAGE:   return 0xf9a825;
	case FCAT_ARCHIVE: return 0xa0785a;
	case FCAT_CODE:    return 0x22c55e;
	case FCAT_EXEC:    return 0xf43f5e;
	default:           return 0x94a3b8; /* TEXT and OTHER: same neutral as before */
	}
}

static int fcat_glyph(enum fcat c)
{
	switch (c) {
	case FCAT_DIR:     return G_FOLDER;
	case FCAT_IMAGE:   return G_IMAGE;
	case FCAT_ARCHIVE: return G_ARCHIVE;
	case FCAT_CODE:    return G_CODE;
	case FCAT_EXEC:    return G_EXEC;
	default:           return G_FILE;
	}
}

/* 5-char tag shown in the File Manager listing, same width as "[DIR]". */
static const char *fcat_tag(enum fcat c)
{
	switch (c) {
	case FCAT_DIR:     return "[DIR]";
	case FCAT_IMAGE:   return "[IMG]";
	case FCAT_ARCHIVE: return "[ZIP]";
	case FCAT_CODE:    return "[SRC]";
	case FCAT_TEXT:    return "[TXT]";
	case FCAT_EXEC:    return "[BIN]";
	default:           return "     ";
	}
}

struct icon {
	const char *label;
	const char *cmd;
	uint32_t color;
	int action; /* 1=reboot,2=poweroff,3=terminal,4=files,5=task manager,6=settings,7=X app */
	int glyph;
	int x, y;   /* free position on the desktop -- icons are draggable */
};

static struct icon icons[] = {
	{"TASK MGR",  NULL, 0x3b82f6, 5, G_GAUGE},
	{"FILES",     NULL, 0x06b6d4, 4, G_FOLDER},
	{"TERMINAL",  NULL, 0x9333ea, 3, G_TERM},
	{"BROWSER",   "/bin/browser", 0x0ea5e9, 7, G_GLOBE},
	{"SETTINGS",  NULL, 0x64748b, 6, G_GEAR},
	{"REBOOT",    NULL, 0xef4444, 1, G_REFRESH},
	{"POWER OFF", NULL, 0xf43f5e, 2, G_POWER},
};
#define NUM_ICONS (int)(sizeof(icons)/sizeof(icons[0]))

/* Desktop themes -- the only setting that has any effect at runtime; the
 * framebuffer mode itself is fixed by GRUB's gfxpayload at boot. */
struct theme {
	const char *name;
	uint32_t dtop, dbot, accent;
};
static const struct theme themes[] = {
	{"Midnight", 0x232338, 0x14141f, 0x3b82f6},
	{"Forest",   0x1e2f24, 0x0d1712, 0x22c55e},
	{"Ember",    0x2f2420, 0x1a1210, 0xf97316},
	{"Solarized",0x002b36, 0x073642, 0xb58900},
	{"Dracula",  0x282a36, 0x21222c, 0xff79c6},
};
#define NUM_THEMES (int)(sizeof(themes)/sizeof(themes[0]))
static int theme_idx;
static int show_hidden;         /* show .* files in file manager */
static int dblclick_delay = 200; /* ms: threshold before drag icon/row becomes selection */

/* One-second samples of CPU / memory use, oldest first. */
#define HIST 60
static int cpu_hist[HIST], mem_hist[HIST];
#define TM_GRAPH_H 120

/* Task Manager tabs -- one window, Windows-style, auto-refreshing. */
struct tmtab {
	const char *label;
	const char *cmd;
};
static const struct tmtab tm_tabs[] = {
	{"Processes",   "ps"},
	{"Performance", "free -m; echo; head -8 /proc/meminfo; echo; uptime"},
	{"Disk",        "df -h"},
	{"System",      "uname -a; echo; uptime; echo; head -1 /proc/version"},
	{"Dmesg",       "dmesg | tail -40"},
};
#define TM_NTABS (int)(sizeof(tm_tabs)/sizeof(tm_tabs[0]))
#define TM_TABH 26

/* icon drag state: press selects, movement past a threshold turns it into a
 * drag, release without movement launches. */
static int icon_press = -1;
static int icon_dragged;
static int icon_grab_dx, icon_grab_dy;

/* File manager state, allocated only for WIN_FILES windows. */
#define FM_MAXENT 512
#define FM_NAMELEN 96
#define FM_PATHLEN 512
/* cwd + '/' + name + NUL: any child path is guaranteed to fit */
#define FM_FULLLEN (FM_PATHLEN + FM_NAMELEN + 2)

/* Right-click context menu: single global instance, opened either on a
 * FILES window's row (CTXMODE_FILEWIN) or on the desktop background
 * (CTXMODE_DESKTOP). Declared here (not lower, by fm_puts) because
 * close_window() needs it. */
enum { CTXMODE_NONE, CTXMODE_FILEWIN, CTXMODE_DESKTOP };
static int ctxmenu_mode;
static int ctxmenu_win = -1;    /* FILEWIN: owning window */
static int ctxmenu_entidx = -1; /* FILEWIN: index into that window's fm->ents */
static int ctxmenu_deskidx = -1;/* DESKTOP: index into desk_files[], -1 = empty area */
static int ctxmenu_x, ctxmenu_y;
#define CTX_W 130
#define CTX_ITEMH 24
#define CTX_NITEMS 5
static const char *ctx_items[CTX_NITEMS] = { "Copy", "Cut", "Paste", "Rename", "Delete" };

/* Clipboard: one path at a time. mode: 0 none, 1 copy, 2 cut.
 * ponytail: files only (fopen/fread copy); directories support Cut (rename)
 * but not Copy -- a recursive copy is a bigger feature than this needs yet. */
static char clip_path[FM_FULLLEN];
static int clip_mode;

/* The desktop is backed by a real directory, same as Windows: icons/*
 * files there are drawn as desktop icons after the fixed app icons, and
 * dropping a file manager row onto the desktop copies it in here. */
#define DESKTOP_DIR "/root/Desktop"
#define DESK_MAXFILES 64
struct deskfile {
	char name[FM_NAMELEN];
	int isdir;
	int isexec;
};
static struct deskfile desk_files[DESK_MAXFILES];
static int desk_count;

/* File-row drag (file manager listing): press arms a candidate; movement
 * past a threshold turns it into a drag; release either drops it (moves
 * into a folder row, copies onto the desktop) or, if it never moved,
 * behaves like a normal click (select, or open if already selected). */
static int fmdrag_win = -1;
static int fmdrag_entidx = -1;
static int fmdrag_active;
static int fmdrag_was_preselected;
static int fmdrag_grab_x, fmdrag_grab_y;

struct fent {
	char name[FM_NAMELEN];
	int isdir;
	int isreg;
	int isexec;
	long size;
};

struct fmstate {
	char cwd[FM_PATHLEN];
	struct fent ents[FM_MAXENT];
	int count;
	int scroll;
	int sel;          /* selected row (index into ents), -1 = none */
	int prompt;       /* 0 none, 1 new file, 2 new folder, 3 rename */
	char pbuf[FM_NAMELEN];
	int confirm_del;  /* Delete was armed: the next click actually deletes */
	char status[64];

	char search[FM_NAMELEN];
	int searching;    /* Ctrl+F is capturing keys into search[] */
	int view[FM_MAXENT]; /* indices into ents[] that pass the search filter */
	int vcount;
};

/* File-manager toolbar, drawn between titlebar and listing. */
#define FM_TOOLH 28
#define FM_NBTN 4
static const char *fm_btns[FM_NBTN] = { "New File", "New Dir", "Delete", "Refresh" };
#define FM_BTNW 96

/* Text editor state, allocated only for WIN_EDIT windows. */
#define ED_MAXLINES 1024
#define ED_MAXCOL 240

struct edstate {
	char path[FM_FULLLEN];
	char line[ED_MAXLINES][ED_MAXCOL];
	int nlines;
	int cy, cx;   /* caret in buffer coords */
	int scroll;
	int dirty;
	int truncated; /* file didn't fit: refuse to save over it */
	char status[64];
};

struct window {
	int used;
	enum wintype type;
	int x, y, w, h;
	int rx, ry, rw, rh; /* saved geometry for un-maximize */
	int minimized, maximized;
	char title[FM_PATHLEN]; /* holds a full cwd for file windows; drawn clipped */

	int pty_fd;
	pid_t pid;
	struct fmstate *fm; /* WIN_FILES only */
	struct edstate *ed; /* WIN_EDIT only */
	int tab;            /* WIN_TASKMGR: active tab */

	int cols, rows;
	unsigned char gch[GRID_MAXROWS][GRID_MAXCOLS];
	uint32_t gfg[GRID_MAXROWS][GRID_MAXCOLS];
	uint32_t gbg[GRID_MAXROWS][GRID_MAXCOLS];
	int cur_row, cur_col;
	uint32_t attr_fg, attr_bg;

	int esc_state; /* 0 normal,1 ESC,2 CSI,3 OSC,4 charset-select,5 OSC-ST-wait */
	int csi_params[8];
	int csi_nparams;
};

static struct window wins[MAX_WIN];
static int zorder[MAX_WIN], zcount;
static int focused = -1;
static int drag_mode; /* 0 none, 1 move, 2 resize */
static int drag_win = -1;
static int alt_held;
static int sd_active;      /* "show desktop" is on: everything was minimized */
static int sd_saved[MAX_WIN];

static uint8_t *fbp;      /* real framebuffer */
static uint8_t *backbuf;  /* offscreen: draw here, then flush in one memcpy */
static struct fb_var_screeninfo vinfo;
static struct fb_fix_screeninfo finfo;
static int xres, yres, bpp, line_length;
static unsigned char font[512 * 32 * 4];
static int have_font;
static int font_w = 8, font_h = 16, font_bpr = 1;
static int mx, my, prev_left, prev_right;
/* /dev/tty1, kept open so the console mode can be handed back and forth with X */
static int confd = -1;
/* absolute pointer (evdev tablet) + evdev keyboard for Alt+Tab */
static int absptr_fd = -1, kbd_evdev_fd = -1;
static int abs_minx, abs_maxx, abs_miny, abs_maxy;
static int abs_curx, abs_cury, abs_btn, abs_rbtn;
static FILE *dbg;
static struct termios orig_termios;
static int have_orig_termios;
#define DBG(...) do { if (dbg) fprintf(dbg, __VA_ARGS__); } while (0)

static void put_pixel(int x, int y, uint32_t color)
{
	if (x < 0 || y < 0 || x >= xres || y >= yres)
		return;
	uint8_t *buf = backbuf ? backbuf : fbp;
	long off = (long)y * line_length + (long)x * (bpp / 8);
	if (bpp == 32) {
		*(uint32_t *)(buf + off) = color;
	} else if (bpp == 16) {
		uint8_t r = (color >> 16) & 0xff, g = (color >> 8) & 0xff, b = color & 0xff;
		uint16_t c565 = ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3);
		*(uint16_t *)(buf + off) = c565;
	} else if (bpp == 24) {
		buf[off] = color & 0xff;
		buf[off + 1] = (color >> 8) & 0xff;
		buf[off + 2] = (color >> 16) & 0xff;
	}
}

static void fill_rect(int x, int y, int w, int h, uint32_t color)
{
	for (int j = 0; j < h; j++)
		for (int i = 0; i < w; i++)
			put_pixel(x + i, y + j, color);
}

static void draw_cursor(int x, int y)
{
	int size = 12;
	put_pixel(x, y, 0xffffff);
	put_pixel(x + 1, y, 0xffffff);
	put_pixel(x, y + 1, 0xffffff);
	put_pixel(x + 1, y + 1, 0xffffff);
	for (int i = 2; i < size; i++) {
		put_pixel(x, y + i, 0xffffff);
		put_pixel(x + i, y, 0xffffff);
	}
	put_pixel(x + 1, y + 2, 0x000000);
	put_pixel(x + 2, y + 1, 0x000000);
	for (int i = 2; i < size - 1; i++) {
		put_pixel(x + 1, y + i, 0x000000);
		put_pixel(x + i, y + 1, 0x000000);
	}
}

static void blit_char(int x, int y, unsigned char c, uint32_t fg)
{
	if (!have_font)
		return;
	unsigned char *glyph = font + (int)c * 32;
	for (int row = 0; row < font_h; row++) {
		unsigned char *rowbits = glyph + row * font_bpr;
		for (int col = 0; col < font_w; col++) {
			unsigned char byte = rowbits[col / 8];
			if (byte & (0x80 >> (col % 8)))
				put_pixel(x + col, y + row, fg);
		}
	}
}

/* ---- shape primitives (integer-only, no libm) ---- */

/* Blend two colors: t=0 -> a, t=255 -> b. */
static uint32_t mix(uint32_t a, uint32_t b, int t)
{
	int ar = (a >> 16) & 0xff, ag = (a >> 8) & 0xff, ab = a & 0xff;
	int br = (b >> 16) & 0xff, bg = (b >> 8) & 0xff, bb = b & 0xff;
	int r = ar + (br - ar) * t / 255;
	int g = ag + (bg - ag) * t / 255;
	int bl = ab + (bb - ab) * t / 255;
	return ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)bl;
}

/* Rounded rect with a vertical gradient (top==bot gives a flat fill). */
static void fill_round_rect_grad(int x, int y, int w, int h, int r,
				 uint32_t top, uint32_t bot)
{
	if (r * 2 > w) r = w / 2;
	if (r * 2 > h) r = h / 2;
	if (r < 0) r = 0;
	for (int j = 0; j < h; j++) {
		uint32_t col = (top == bot) ? top
			: mix(top, bot, h > 1 ? j * 255 / (h - 1) : 0);
		for (int i = 0; i < w; i++) {
			int cx = -1, cy = -1;
			if (i < r && j < r) { cx = r; cy = r; }
			else if (i >= w - r && j < r) { cx = w - r - 1; cy = r; }
			else if (i < r && j >= h - r) { cx = r; cy = h - r - 1; }
			else if (i >= w - r && j >= h - r) { cx = w - r - 1; cy = h - r - 1; }
			if (cx >= 0) {
				int dx = i - cx, dy = j - cy;
				if (dx * dx + dy * dy > r * r)
					continue;
			}
			put_pixel(x + i, y + j, col);
		}
	}
}

static void fill_round_rect(int x, int y, int w, int h, int r, uint32_t col)
{
	fill_round_rect_grad(x, y, w, h, r, col, col);
}

static void fill_circle(int cx, int cy, int r, uint32_t col)
{
	for (int j = -r; j <= r; j++)
		for (int i = -r; i <= r; i++)
			if (i * i + j * j <= r * r)
				put_pixel(cx + i, cy + j, col);
}

/* Annulus: outer radius r, thickness t. */
static void fill_ring(int cx, int cy, int r, int t, uint32_t col)
{
	int inner = r - t;
	if (inner < 0) inner = 0;
	for (int j = -r; j <= r; j++)
		for (int i = -r; i <= r; i++) {
			int d = i * i + j * j;
			if (d <= r * r && d >= inner * inner)
				put_pixel(cx + i, cy + j, col);
		}
}

/* Triangle with its tip at distance s from (cx,cy). dir: 0=up 1=down 2=left 3=right */
static void fill_triangle(int cx, int cy, int s, int dir, uint32_t col)
{
	for (int j = 0; j <= s; j++) {
		for (int i = -j; i <= j; i++) {
			int px, py;
			if (dir == 0)      { px = cx + i;     py = cy - s + j; }
			else if (dir == 1) { px = cx + i;     py = cy + s - j; }
			else if (dir == 2) { px = cx - s + j; py = cy + i;     }
			else               { px = cx + s - j; py = cy + i;     }
			put_pixel(px, py, col);
		}
	}
}

static void fill_vgradient(int x, int y, int w, int h, uint32_t top, uint32_t bot)
{
	for (int j = 0; j < h; j++) {
		uint32_t c = mix(top, bot, h > 1 ? j * 255 / (h - 1) : 0);
		for (int i = 0; i < w; i++)
			put_pixel(x + i, y + j, c);
	}
}

static void draw_text(int x, int y, const char *s, uint32_t fg)
{
	int cx = x, cy = y;
	for (; *s; s++) {
		if (*s == '\n') {
			cx = x;
			cy += font_h;
			continue;
		}
		if (cx + font_w > xres) {
			cx = x;
			cy += font_h;
		}
		blit_char(cx, cy, (unsigned char)*s, fg);
		cx += font_w;
	}
}

static void draw_text_clip(int x, int y, const char *s, uint32_t fg, int maxw)
{
	char buf[64];
	int maxchars = maxw / font_w;
	if (maxchars < 0)
		maxchars = 0;
	if (maxchars >= (int)sizeof(buf))
		maxchars = sizeof(buf) - 1;
	int n = strlen(s);
	if (n > maxchars)
		n = maxchars;
	memcpy(buf, s, n);
	buf[n] = 0;
	draw_text(x, y, buf, fg);
}

/* Vector-style glyphs, drawn from primitives and centered on (cx,cy).
 * `fg` is the ink, `hole` is used to punch cutouts back out of the tile. */
static void draw_glyph(int g, int cx, int cy, uint32_t fg, uint32_t hole)
{
	switch (g) {
	case G_GAUGE:
		/* a bar chart -- reads as "activity / task manager" */
		fill_round_rect(cx - 20, cy - 18, 40, 36, 4, fg);
		fill_round_rect(cx - 14, cy + 4,  6, 9,  1, hole);
		fill_round_rect(cx - 5,  cy - 4,  6, 17, 1, hole);
		fill_round_rect(cx + 4,  cy - 12, 6, 25, 1, hole);
		break;
	case G_FOLDER:
		fill_round_rect(cx - 19, cy - 17, 17, 9, 3, fg);
		fill_round_rect(cx - 19, cy - 12, 38, 27, 4, fg);
		fill_round_rect(cx - 16, cy - 6, 32, 3, 1, mix(fg, hole, 120));
		break;
	case G_TERM:
		fill_round_rect(cx - 20, cy - 16, 40, 32, 4, fg);
		fill_round_rect(cx - 16, cy - 8, 32, 20, 2, hole);
		fill_circle(cx - 15, cy - 12, 2, hole);
		fill_circle(cx - 9,  cy - 12, 2, hole);
		fill_circle(cx - 3,  cy - 12, 2, hole);
		/* prompt chevron + cursor */
		fill_rect(cx - 12, cy - 2, 3, 3, fg);
		fill_rect(cx - 9,  cy + 1, 3, 3, fg);
		fill_rect(cx - 12, cy + 4, 3, 3, fg);
		fill_rect(cx - 3,  cy + 4, 9, 3, fg);
		break;
	case G_REFRESH:
		fill_ring(cx, cy + 2, 16, 5, fg);
		fill_rect(cx, cy - 22, 22, 13, hole);       /* open the top-right arc */
		fill_triangle(cx + 4, cy - 13, 10, 3, fg);  /* arrow head on the opening */
		break;
	case G_POWER:
		fill_ring(cx, cy + 3, 16, 6, fg);
		fill_rect(cx - 5, cy - 16, 10, 12, hole);  /* gap at the top */
		fill_round_rect(cx - 2, cy - 18, 5, 18, 2, fg);
		break;
	case G_GLOBE:
		/* meridian + equator inside a ring -- reads as "web" */
		fill_ring(cx, cy, 19, 4, fg);
		fill_ring(cx, cy, 9, 3, fg);         /* the meridian, seen edge-on */
		fill_rect(cx - 16, cy - 8, 32, 3, fg);
		fill_rect(cx - 18, cy - 1, 36, 3, fg);
		fill_rect(cx - 16, cy + 6, 32, 3, fg);
		break;
	case G_GEAR:
		/* four teeth + body + hub */
		fill_round_rect(cx - 4, cy - 20, 8, 40, 2, fg);
		fill_round_rect(cx - 20, cy - 4, 40, 8, 2, fg);
		fill_round_rect(cx - 14, cy - 16, 8, 8, 2, fg);
		fill_round_rect(cx + 6, cy - 16, 8, 8, 2, fg);
		fill_round_rect(cx - 14, cy + 8, 8, 8, 2, fg);
		fill_round_rect(cx + 6, cy + 8, 8, 8, 2, fg);
		fill_circle(cx, cy, 14, fg);
		fill_circle(cx, cy, 6, hole);
		break;
	case G_FILE:
		/* a page with a folded corner and a couple of text lines */
		fill_round_rect(cx - 16, cy - 20, 32, 40, 3, fg);
		fill_triangle(cx + 16, cy - 20, 9, 1, hole);
		fill_rect(cx - 9, cy - 4, 18, 3, hole);
		fill_rect(cx - 9, cy + 4, 18, 3, hole);
		fill_rect(cx - 9, cy + 12, 12, 3, hole);
		break;
	case G_IMAGE:
		/* a photo frame with a mountain scene and a sun */
		fill_round_rect(cx - 18, cy - 14, 36, 28, 3, fg);
		fill_round_rect(cx - 14, cy - 10, 28, 20, 1, hole);
		fill_circle(cx + 5, cy - 4, 3, fg);
		fill_triangle(cx - 8, cy + 6, 6, 0, fg);
		fill_triangle(cx + 1, cy + 6, 8, 0, fg);
		break;
	case G_ARCHIVE:
		/* a packed box with a carrying strap */
		fill_round_rect(cx - 16, cy - 14, 32, 28, 3, fg);
		fill_rect(cx - 16, cy - 4, 32, 4, hole);
		fill_round_rect(cx - 4, cy - 9, 8, 6, 1, hole);
		break;
	case G_CODE:
		/* a document with a "< >" mark carved out */
		fill_round_rect(cx - 16, cy - 20, 32, 40, 3, fg);
		fill_triangle(cx - 5, cy - 2, 5, 2, hole);
		fill_triangle(cx + 5, cy - 2, 5, 3, hole);
		fill_rect(cx - 9, cy + 10, 18, 3, hole);
		break;
	case G_EXEC:
		/* a rounded "chip" body with a play/run triangle at its center */
		fill_round_rect(cx - 18, cy - 16, 36, 32, 6, fg);
		fill_triangle(cx - 4, cy, 9, 3, hole);
		break;
	default:
		break;
	}
}

static void init_icon_positions(void)
{
	int cols = (xres - ICON_GAP) / (ICON_W + ICON_GAP);
	if (cols < 1)
		cols = 1;
	for (int i = 0; i < NUM_ICONS; i++) {
		icons[i].x = ICON_GAP + (i % cols) * (ICON_W + ICON_GAP);
		icons[i].y = ICON_GAP + (i / cols) * (ICON_H + ICON_GAP);
	}
}

static void clamp_icon(struct icon *ic)
{
	if (ic->x < 0) ic->x = 0;
	if (ic->y < 0) ic->y = 0;
	if (ic->x > xres - ICON_W) ic->x = xres - ICON_W;
	if (ic->y > yres - TASK_H - ICON_H) ic->y = yres - TASK_H - ICON_H;
}

/* Desktop files sit in the same grid as the fixed app icons, continuing
 * right after them; unlike app icons their position is always computed
 * (not draggable-to-reposition), since it comes from a real directory
 * listing that can change size at any time. */
static void desk_item_pos(int idx, int *ox, int *oy)
{
	int cols = (xres - ICON_GAP) / (ICON_W + ICON_GAP);
	if (cols < 1)
		cols = 1;
	*ox = ICON_GAP + (idx % cols) * (ICON_W + ICON_GAP);
	*oy = ICON_GAP + (idx / cols) * (ICON_H + ICON_GAP);
}

static void draw_icons(void)
{
	for (int i = 0; i < NUM_ICONS; i++) {
		struct icon *ic = &icons[i];
		int lifted = (icon_press == i && icon_dragged);
		int tx = ic->x + (ICON_W - TILE) / 2;
		int ty = ic->y;
		uint32_t c = ic->color;

		/* drop shadow (deeper while the icon is lifted by a drag) */
		fill_round_rect(tx + 2, ty + (lifted ? 7 : 4), TILE, TILE, 18, 0x0c0c14);
		fill_round_rect_grad(tx, ty, TILE, TILE, 18,
				     mix(c, 0xffffff, lifted ? 75 : 40),
				     mix(c, 0x000000, 55));
		draw_glyph(ic->glyph, tx + TILE / 2, ty + TILE / 2,
			   0xffffff, mix(c, 0x000000, 78));

		int len = strlen(ic->label);
		int lx = ic->x + (ICON_W - len * font_w) / 2;
		draw_text(lx, ty + TILE + 9, ic->label, 0xdfe4f2);
	}

	for (int i = 0; i < desk_count; i++) {
		struct deskfile *df = &desk_files[i];
		int x, y;
		desk_item_pos(NUM_ICONS + i, &x, &y);
		int combined = NUM_ICONS + i;
		int lifted = (icon_press == combined && icon_dragged);
		int tx = x + (ICON_W - TILE) / 2, ty = y;
		enum fcat cat = classify_file(df->name, df->isdir, df->isexec);
		uint32_t c = fcat_color(cat);

		fill_round_rect(tx + 2, ty + (lifted ? 7 : 4), TILE, TILE, 18, 0x0c0c14);
		fill_round_rect_grad(tx, ty, TILE, TILE, 18,
				     mix(c, 0xffffff, lifted ? 75 : 40),
				     mix(c, 0x000000, 55));
		draw_glyph(fcat_glyph(cat), tx + TILE / 2, ty + TILE / 2,
			   0xffffff, mix(c, 0x000000, 78));
		draw_text_clip(x, ty + TILE + 9, df->name, 0xdfe4f2, ICON_W);
	}
}

static int icon_at(int px, int py)
{
	for (int i = NUM_ICONS - 1; i >= 0; i--) {
		struct icon *ic = &icons[i];
		if (px >= ic->x && px < ic->x + ICON_W &&
		    py >= ic->y && py < ic->y + ICON_H)
			return i;
	}
	for (int i = desk_count - 1; i >= 0; i--) {
		int x, y;
		desk_item_pos(NUM_ICONS + i, &x, &y);
		if (px >= x && px < x + ICON_W && py >= y && py < y + ICON_H)
			return NUM_ICONS + i;
	}
	return -1;
}

/* Directories first, then alphabetical -- same ordering as the file manager. */
static int deskfile_cmp(const void *a, const void *b)
{
	const struct deskfile *x = a, *y = b;
	if (x->isdir != y->isdir)
		return y->isdir - x->isdir;
	return strcmp(x->name, y->name);
}

/* Load the desktop's real directory listing (DESKTOP_DIR) into desk_files. */
static void desk_scan(void)
{
	desk_count = 0;
	DIR *d = opendir(DESKTOP_DIR);
	if (!d)
		return;
	struct dirent *de;
	while ((de = readdir(d)) && desk_count < DESK_MAXFILES) {
		if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, ".."))
			continue;
		if (!show_hidden && de->d_name[0] == '.')
			continue;
		if (strlen(de->d_name) >= FM_NAMELEN)
			continue;
		struct deskfile *e = &desk_files[desk_count];
		snprintf(e->name, FM_NAMELEN, "%s", de->d_name);
		char path[FM_FULLLEN];
		snprintf(path, sizeof(path), "%s/%s", DESKTOP_DIR, e->name);
		struct stat st;
		if (stat(path, &st) == 0) {
			e->isdir = S_ISDIR(st.st_mode);
			e->isexec = (st.st_mode & (S_IXUSR | S_IXGRP | S_IXOTH)) != 0;
		} else {
			e->isdir = e->isexec = 0;
		}
		desk_count++;
	}
	closedir(d);
	qsort(desk_files, desk_count, sizeof(struct deskfile), deskfile_cmp);
}

/* ---- character-grid terminal model, shared by live terminals and
 * one-shot command-output windows ---- */

static void update_grid_dims(struct window *w)
{
	int content_h = w->h - TITLE_H;
	if (w->type == WIN_FILES)
		content_h -= FM_TOOLH;
	if (w->type == WIN_TASKMGR) {
		content_h -= TM_TABH;
		if (w->tab == 1)
			content_h -= TM_GRAPH_H; /* graphs sit above the text */
	}
	int newcols = (w->w - 8) / font_w;
	int newrows = content_h / font_h;
	if (newcols > GRID_MAXCOLS) newcols = GRID_MAXCOLS;
	if (newrows > GRID_MAXROWS) newrows = GRID_MAXROWS;
	if (newcols < 1) newcols = 1;
	if (newrows < 1) newrows = 1;
	w->cols = newcols;
	w->rows = newrows;
	if (w->cur_row >= w->rows) w->cur_row = w->rows - 1;
	if (w->cur_col >= w->cols) w->cur_col = w->cols - 1;
}

static void fm_render(struct window *w);
static void ed_render(struct window *w);
static int spawn_editor(const char *path);

static void resize_notify(struct window *w)
{
	update_grid_dims(w);
	if (w->type == WIN_FILES && w->fm)
		fm_render(w);
	if (w->type == WIN_EDIT && w->ed)
		ed_render(w);
	if (w->type == WIN_TERM && w->pty_fd >= 0) {
		struct winsize ws;
		memset(&ws, 0, sizeof(ws));
		ws.ws_row = w->rows;
		ws.ws_col = w->cols;
		ioctl(w->pty_fd, TIOCSWINSZ, &ws);
		if (w->pid > 0)
			kill(w->pid, SIGWINCH);
	}
}

static void clear_row_range(struct window *w, int row, int from, int to)
{
	for (int c = from; c <= to && c < w->cols; c++) {
		w->gch[row][c] = ' ';
		w->gfg[row][c] = w->attr_fg;
		w->gbg[row][c] = w->attr_bg;
	}
}

static void scroll_up(struct window *w)
{
	for (int r = 0; r < w->rows - 1; r++) {
		memcpy(w->gch[r], w->gch[r + 1], sizeof(w->gch[r]));
		memcpy(w->gfg[r], w->gfg[r + 1], sizeof(w->gfg[r]));
		memcpy(w->gbg[r], w->gbg[r + 1], sizeof(w->gbg[r]));
	}
	clear_row_range(w, w->rows - 1, 0, w->cols - 1);
}

static void erase_line(struct window *w, int mode)
{
	int from = 0, to = w->cols - 1;
	if (mode == 0) from = w->cur_col;
	else if (mode == 1) to = w->cur_col;
	clear_row_range(w, w->cur_row, from, to);
}

static void erase_screen(struct window *w, int mode)
{
	int rfrom = 0, rto = w->rows - 1;
	if (mode == 0) {
		erase_line(w, 0);
		rfrom = w->cur_row + 1;
	} else if (mode == 1) {
		erase_line(w, 1);
		rto = w->cur_row - 1;
	}
	for (int r = rfrom; r <= rto && r >= 0 && r < w->rows; r++)
		clear_row_range(w, r, 0, w->cols - 1);
}

static void putch_grid(struct window *w, unsigned char c)
{
	if (w->cur_col >= w->cols) {
		w->cur_col = 0;
		w->cur_row++;
	}
	if (w->cur_row >= w->rows) {
		scroll_up(w);
		w->cur_row = w->rows - 1;
	}
	w->gch[w->cur_row][w->cur_col] = c;
	w->gfg[w->cur_row][w->cur_col] = w->attr_fg;
	w->gbg[w->cur_row][w->cur_col] = w->attr_bg;
	w->cur_col++;
}

static void apply_sgr(struct window *w, int *params, int n)
{
	static const uint32_t palette[8] = {
		0x11111b, 0xf38ba8, 0xa6e3a1, 0xf9e2af,
		0x89b4fa, 0xf5c2e7, 0x94e2d5, 0xcdd6f4,
	};
	if (n == 0) {
		w->attr_fg = COL_FG_DEFAULT;
		w->attr_bg = COL_BG_DEFAULT;
		return;
	}
	for (int i = 0; i < n; i++) {
		int p = params[i];
		if (p == 0) { w->attr_fg = COL_FG_DEFAULT; w->attr_bg = COL_BG_DEFAULT; }
		else if (p >= 30 && p <= 37) w->attr_fg = palette[p - 30];
		else if (p == 39) w->attr_fg = COL_FG_DEFAULT;
		else if (p >= 40 && p <= 47) w->attr_bg = palette[p - 40];
		else if (p == 49) w->attr_bg = COL_BG_DEFAULT;
		else if (p >= 90 && p <= 97) w->attr_fg = palette[p - 90];
		else if (p >= 100 && p <= 107) w->attr_bg = palette[p - 100];
		/* bold/underline/etc: not tracked -- known simplification */
	}
}

/* Handles a pragmatic subset of VT100/ANSI: cursor motion, absolute
 * positioning, erase line/screen, SGR colors, and OSC/charset escapes are
 * consumed harmlessly. No alternate screen buffer, no scrollback beyond the
 * grid -- full-screen apps that rely on those (vi, top) will be usable but
 * imperfect. That's the accepted ceiling for this size of program. */
static void process_bytes(struct window *w, unsigned char *buf, int n)
{
	for (int i = 0; i < n; i++) {
		unsigned char c = buf[i];
		if (w->esc_state == 1) {
			if (c == '[') { w->esc_state = 2; w->csi_nparams = 0; w->csi_params[0] = 0; }
			else if (c == ']') w->esc_state = 3;
			else if (c == '(' || c == ')') w->esc_state = 4;
			else w->esc_state = 0;
			continue;
		}
		if (w->esc_state == 2) {
			if (c == '?') continue;
			if (c >= '0' && c <= '9') {
				w->csi_params[w->csi_nparams] = w->csi_params[w->csi_nparams] * 10 + (c - '0');
				continue;
			}
			if (c == ';') {
				if (w->csi_nparams < 7) w->csi_nparams++;
				w->csi_params[w->csi_nparams] = 0;
				continue;
			}
			int nparams = w->csi_nparams + 1;
			int *p = w->csi_params;
			int p0 = p[0];
			switch (c) {
			case 'A': w->cur_row -= p0 ? p0 : 1; if (w->cur_row < 0) w->cur_row = 0; break;
			case 'B': w->cur_row += p0 ? p0 : 1; if (w->cur_row >= w->rows) w->cur_row = w->rows - 1; break;
			case 'C': w->cur_col += p0 ? p0 : 1; if (w->cur_col >= w->cols) w->cur_col = w->cols - 1; break;
			case 'D': w->cur_col -= p0 ? p0 : 1; if (w->cur_col < 0) w->cur_col = 0; break;
			case 'H': case 'f': {
				int row = (nparams > 0 && p[0]) ? p[0] : 1;
				int col = (nparams > 1 && p[1]) ? p[1] : 1;
				w->cur_row = row - 1;
				w->cur_col = col - 1;
				if (w->cur_row < 0) w->cur_row = 0;
				if (w->cur_row >= w->rows) w->cur_row = w->rows - 1;
				if (w->cur_col < 0) w->cur_col = 0;
				if (w->cur_col >= w->cols) w->cur_col = w->cols - 1;
				break;
			}
			case 'J': erase_screen(w, p0); break;
			case 'K': erase_line(w, p0); break;
			case 'm': apply_sgr(w, p, nparams); break;
			default: break;
			}
			w->esc_state = 0;
			continue;
		}
		if (w->esc_state == 3) {
			if (c == 0x07) w->esc_state = 0;
			else if (c == 0x1b) w->esc_state = 5;
			continue;
		}
		if (w->esc_state == 5) { w->esc_state = 0; continue; }
		if (w->esc_state == 4) { w->esc_state = 0; continue; }

		if (c == 0x1b) { w->esc_state = 1; continue; }
		if (c == '\r') { w->cur_col = 0; continue; }
		if (c == '\n') {
			w->cur_row++;
			if (w->cur_row >= w->rows) { scroll_up(w); w->cur_row = w->rows - 1; }
			continue;
		}
		if (c == '\b') { if (w->cur_col > 0) w->cur_col--; continue; }
		if (c == '\t') {
			w->cur_col = (w->cur_col / 8 + 1) * 8;
			if (w->cur_col >= w->cols) w->cur_col = w->cols - 1;
			continue;
		}
		if (c >= 0x20 && c < 0x7f) { putch_grid(w, c); continue; }
	}
}

static void run_and_show(const char *cmd)
{
	fill_rect(0, 0, xres, yres, 0x11111b);
	FILE *p = popen(cmd, "r");
	int y = 10, max_rows = (yres - 40) / font_h;
	int row = 0;
	if (p) {
		char line[512];
		while (row < max_rows && fgets(line, sizeof(line), p)) {
			line[strcspn(line, "\n")] = 0;
			draw_text(10, y, line, 0xcdd6f4);
			y += font_h;
			row++;
		}
		pclose(p);
	}
	if (backbuf)
		memcpy(fbp, backbuf, (size_t)line_length * yres);
}

static void raise_window(int i)
{
	sd_active = 0; /* touching a window means we're no longer showing the desktop */
	for (int zi = 0; zi < zcount; zi++) {
		if (zorder[zi] == i) {
			for (int k = zi; k < zcount - 1; k++)
				zorder[k] = zorder[k + 1];
			zcount--;
			break;
		}
	}
	zorder[zcount++] = i;
}

static void close_window(int i)
{
	if (!wins[i].used)
		return;
	if (wins[i].type == WIN_TERM) {
		close(wins[i].pty_fd);
		if (wins[i].pid > 0) {
			kill(wins[i].pid, SIGTERM);
			waitpid(wins[i].pid, NULL, 0);
		}
	}
	if (wins[i].fm) {
		free(wins[i].fm);
		wins[i].fm = NULL;
	}
	if (wins[i].ed) {
		free(wins[i].ed);
		wins[i].ed = NULL;
	}
	wins[i].used = 0;
	for (int zi = 0; zi < zcount; zi++) {
		if (zorder[zi] == i) {
			for (int k = zi; k < zcount - 1; k++)
				zorder[k] = zorder[k + 1];
			zcount--;
			break;
		}
	}
	if (focused == i)
		focused = zcount > 0 ? zorder[zcount - 1] : -1;
	if (drag_win == i) {
		drag_win = -1;
		drag_mode = 0;
	}
	if (ctxmenu_mode == CTXMODE_FILEWIN && ctxmenu_win == i) {
		ctxmenu_win = -1;
		ctxmenu_mode = CTXMODE_NONE;
	}
	if (fmdrag_win == i) {
		fmdrag_win = -1;
		fmdrag_entidx = -1;
		fmdrag_active = 0;
	}
}

static void toggle_maximize(int i)
{
	struct window *w = &wins[i];
	if (!w->maximized) {
		w->rx = w->x; w->ry = w->y; w->rw = w->w; w->rh = w->h;
		w->x = 0; w->y = 0; w->w = xres; w->h = yres - TASK_H;
		w->maximized = 1;
	} else {
		w->x = w->rx; w->y = w->ry; w->w = w->rw; w->h = w->rh;
		w->maximized = 0;
	}
	resize_notify(w);
}

static int alloc_window_slot(void)
{
	for (int i = 0; i < MAX_WIN; i++)
		if (!wins[i].used)
			return i;
	return -1;
}

static int spawn_terminal(void)
{
	int slot = alloc_window_slot();
	if (slot < 0)
		return -1;

	int master = posix_openpt(O_RDWR | O_NOCTTY);
	if (master < 0)
		return -1;
	if (grantpt(master) < 0 || unlockpt(master) < 0) {
		close(master);
		return -1;
	}
	char *slavename = ptsname(master);
	if (!slavename) {
		close(master);
		return -1;
	}
	char slavebuf[64];
	strncpy(slavebuf, slavename, sizeof(slavebuf) - 1);
	slavebuf[sizeof(slavebuf) - 1] = 0;

	pid_t pid = fork();
	if (pid == 0) {
		setsid();
		int slave = open(slavebuf, O_RDWR);
		if (slave < 0)
			_exit(1);
		ioctl(slave, TIOCSCTTY, 0);
		dup2(slave, 0);
		dup2(slave, 1);
		dup2(slave, 2);
		if (slave > 2)
			close(slave);
		close(master);
		for (int i = 0; i < MAX_WIN; i++)
			if (i != slot && wins[i].used && wins[i].type == WIN_TERM)
				close(wins[i].pty_fd);
		setenv("TERM", "linux", 1);
		execl("/bin/sh", "sh", NULL);
		_exit(1);
	} else if (pid < 0) {
		close(master);
		return -1;
	}

	memset(&wins[slot], 0, sizeof(wins[slot]));
	wins[slot].used = 1;
	wins[slot].type = WIN_TERM;
	wins[slot].pty_fd = master;
	wins[slot].pid = pid;
	wins[slot].x = 200 + slot * 24;
	wins[slot].y = 120 + slot * 24;
	wins[slot].w = 560;
	wins[slot].h = 360;
	wins[slot].attr_fg = COL_FG_DEFAULT;
	wins[slot].attr_bg = COL_BG_DEFAULT;
	snprintf(wins[slot].title, sizeof(wins[slot].title), "Terminal %d", slot + 1);
	update_grid_dims(&wins[slot]);
	for (int r = 0; r < wins[slot].rows; r++)
		clear_row_range(&wins[slot], r, 0, wins[slot].cols - 1);
	resize_notify(&wins[slot]);
	zorder[zcount++] = slot;
	focused = slot;
	return slot;
}

/* Run a command and paint its stdout into the grid (one-shot). */
static void fill_grid_from_cmd(struct window *w, const char *cmd)
{
	for (int r = 0; r < w->rows; r++)
		clear_row_range(w, r, 0, w->cols - 1);
	w->cur_row = 0;
	w->cur_col = 0;
	FILE *p = popen(cmd, "r");
	if (p) {
		int ch;
		while ((ch = fgetc(p)) != EOF) {
			unsigned char c = (unsigned char)ch;
			if (c == '\r') continue;
			if (c == '\n') {
				w->cur_row++;
				w->cur_col = 0;
				if (w->cur_row >= w->rows) { scroll_up(w); w->cur_row = w->rows - 1; }
				continue;
			}
			if (c == '\t') {
				w->cur_col = (w->cur_col / 8 + 1) * 8;
				if (w->cur_col >= w->cols) w->cur_col = w->cols - 1;
				continue;
			}
			if (c >= 0x20 && c < 0x7f)
				putch_grid(w, c);
		}
		pclose(p);
	}
	w->cur_row = 0;
	w->cur_col = 0;
}

/* ---- task manager: one window, tabs, auto-refresh ---- */

static void taskmgr_refresh(struct window *w)
{
	if (w->tab < 0 || w->tab >= TM_NTABS)
		w->tab = 0;
	update_grid_dims(w); /* the Performance tab gives up rows to the graphs */
	fill_grid_from_cmd(w, tm_tabs[w->tab].cmd);
	snprintf(w->title, sizeof(w->title), "Task Manager  -  %s", tm_tabs[w->tab].label);
}

static int spawn_taskmgr(void)
{
	/* single-instance: focus the existing one instead of opening a second */
	for (int i = 0; i < MAX_WIN; i++) {
		if (wins[i].used && wins[i].type == WIN_TASKMGR) {
			wins[i].minimized = 0;
			raise_window(i);
			focused = i;
			return i;
		}
	}
	int slot = alloc_window_slot();
	if (slot < 0)
		return -1;
	memset(&wins[slot], 0, sizeof(wins[slot]));
	wins[slot].used = 1;
	wins[slot].type = WIN_TASKMGR;
	wins[slot].pty_fd = -1;
	wins[slot].x = 180;
	wins[slot].y = 90;
	wins[slot].w = 700;
	wins[slot].h = 460;
	wins[slot].attr_fg = COL_FG_DEFAULT;
	wins[slot].attr_bg = COL_BG_DEFAULT;
	wins[slot].tab = 0;
	update_grid_dims(&wins[slot]);
	taskmgr_refresh(&wins[slot]);
	zorder[zcount++] = slot;
	focused = slot;
	return slot;
}

/* Tab strip hit-test: returns clicked tab index or -1. */
static int taskmgr_tab_at(struct window *w, int px, int py)
{
	int ty = w->y + TITLE_H;
	if (py < ty || py >= ty + TM_TABH)
		return -1;
	int tw = w->w / TM_NTABS;
	int idx = (px - w->x) / tw;
	if (idx < 0 || idx >= TM_NTABS)
		return -1;
	return idx;
}

/* ---- file manager ---- */

static void fm_puts(struct window *w, int row, int col, const char *s, uint32_t fg)
{
	for (int i = 0; s[i] && col + i < w->cols; i++) {
		w->gch[row][col + i] = (unsigned char)s[i];
		w->gfg[row][col + i] = fg;
		w->gbg[row][col + i] = COL_BG_DEFAULT;
	}
}

/* Case-insensitive substring test. */
static int fm_match(const char *name, const char *needle)
{
	if (!needle[0])
		return 1;
	size_t nlen = strlen(needle);
	for (const char *p = name; *p; p++) {
		size_t i = 0;
		while (i < nlen && p[i] &&
		       tolower((unsigned char)p[i]) == tolower((unsigned char)needle[i]))
			i++;
		if (i == nlen)
			return 1;
	}
	return 0;
}

/* Rebuild fm->view[] from fm->ents[] against the current search filter.
 * ".." always stays visible so search never traps you in a directory. */
static void fm_apply_filter(struct fmstate *fm)
{
	fm->vcount = 0;
	for (int i = 0; i < fm->count; i++) {
		if (!strcmp(fm->ents[i].name, "..") || fm_match(fm->ents[i].name, fm->search))
			fm->view[fm->vcount++] = i;
	}
}

static void fm_render(struct window *w)
{
	struct fmstate *fm = w->fm;
	int maxscroll = fm->vcount - w->rows;
	if (maxscroll < 0) maxscroll = 0;
	if (fm->scroll > maxscroll) fm->scroll = maxscroll;
	if (fm->scroll < 0) fm->scroll = 0;

	for (int r = 0; r < w->rows; r++)
		clear_row_range(w, r, 0, w->cols - 1);

	for (int r = 0; r < w->rows; r++) {
		int vi = fm->scroll + r;
		if (vi >= fm->vcount)
			break;
		int i = fm->view[vi];
		struct fent *e = &fm->ents[i];
		enum fcat cat = classify_file(e->name, e->isdir, e->isexec);
		char line[FM_NAMELEN + 16];
		snprintf(line, sizeof(line), "%s %s", fcat_tag(cat), e->name);
		/* Hidden files appear dimmed (starts with .); otherwise images,
		 * archives, code, executables, and directories get their category
		 * color -- plain text/unrecognized files keep the normal foreground. */
		int is_hidden = (e->name[0] == '.');
		uint32_t color;
		if (is_hidden)
			color = 0x6c7086;
		else if (cat == FCAT_TEXT || cat == FCAT_OTHER)
			color = COL_FG_DEFAULT;
		else
			color = fcat_color(cat);
		fm_puts(w, r, 0, line, color);
		if (e->isreg) {
			char sz[24];
			snprintf(sz, sizeof(sz), "%ld", e->size);
			int col = w->cols - (int)strlen(sz) - 1;
			if (col > (int)strlen(line) + 1)
				fm_puts(w, r, col, sz, 0x6c7086);
		}
		if (i == fm->sel)
			for (int c = 0; c < w->cols; c++)
				w->gbg[r][c] = 0x313244;
	}
}

/* Directories first, then alphabetical. */
static int fent_cmp(const void *a, const void *b)
{
	const struct fent *x = a, *y = b;
	if (x->isdir != y->isdir)
		return y->isdir - x->isdir;
	return strcmp(x->name, y->name);
}

static void fm_path(struct fmstate *fm, const char *name, char *out, size_t n)
{
	snprintf(out, n, "%s%s%s", fm->cwd, strcmp(fm->cwd, "/") ? "/" : "", name);
}

static void fm_load(struct window *w)
{
	struct fmstate *fm = w->fm;
	fm->count = 0;
	fm->scroll = 0;
	fm->sel = -1;
	fm->confirm_del = 0;

	if (strcmp(fm->cwd, "/") != 0) {
		snprintf(fm->ents[0].name, FM_NAMELEN, "..");
		fm->ents[0].isdir = 1;
		fm->ents[0].isreg = 0;
		fm->ents[0].size = 0;
		fm->count = 1;
	}

	DIR *d = opendir(fm->cwd);
	if (d) {
		int start = fm->count;
		struct dirent *de;
		while ((de = readdir(d)) && fm->count < FM_MAXENT) {
			if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, ".."))
				continue;
			struct fent *e = &fm->ents[fm->count];
			/* Skip hidden files unless show_hidden is on. */
			if (!show_hidden && de->d_name[0] == '.')
				continue;
			/* A name too long to store is a name we could never open again. */
			if (strlen(de->d_name) >= FM_NAMELEN)
				continue;
			snprintf(e->name, FM_NAMELEN, "%s", de->d_name);
			char path[FM_FULLLEN];
			fm_path(fm, e->name, path, sizeof(path));
			struct stat st;
			if (stat(path, &st) == 0) {
				e->isdir = S_ISDIR(st.st_mode);
				e->isreg = S_ISREG(st.st_mode);
				e->isexec = (st.st_mode & (S_IXUSR | S_IXGRP | S_IXOTH)) != 0;
				e->size = (long)st.st_size;
			} else {
				e->isdir = e->isreg = e->isexec = 0;
				e->size = 0;
			}
			fm->count++;
		}
		closedir(d);
		/* sort everything after the ".." entry, which must stay first */
		qsort(fm->ents + start, fm->count - start, sizeof(struct fent), fent_cmp);
	}
	fm_apply_filter(fm);
	snprintf(w->title, sizeof(w->title), "%s", fm->cwd);
	fm_render(w);
}

/* Toolbar hit-test: returns button index or -1. */
static int fm_btn_at(struct window *w, int px, int py)
{
	int by = w->y + TITLE_H;
	if (py < by || py >= by + FM_TOOLH)
		return -1;
	int idx = (px - w->x - 6) / (FM_BTNW + 4);
	if (idx < 0 || idx >= FM_NBTN)
		return -1;
	return idx;
}

/* Delete the selected entry. Files are unlinked, directories must be empty. */
/* Copy/Cut the selected entry to the clipboard -- shared by the Ctrl+C/
 * Ctrl+X keyboard shortcuts and the right-click menu's Copy/Cut items. */
static void fm_copy_selected(struct window *w, int cut)
{
	struct fmstate *fm = w->fm;
	if (fm->sel < 0 || fm->sel >= fm->count) {
		snprintf(fm->status, sizeof(fm->status), "select something first");
		return;
	}
	struct fent *e = &fm->ents[fm->sel];
	if (!strcmp(e->name, "..")) {
		snprintf(fm->status, sizeof(fm->status), "cannot %s ..", cut ? "cut" : "copy");
		return;
	}
	if (!cut && !e->isreg) {
		snprintf(fm->status, sizeof(fm->status), "select a file to copy");
		return;
	}
	fm_path(fm, e->name, clip_path, sizeof(clip_path));
	clip_mode = cut ? 2 : 1;
	snprintf(fm->status, sizeof(fm->status), "%s %s", cut ? "cut" : "copied", e->name);
}

static void fm_delete(struct window *w)
{
	struct fmstate *fm = w->fm;
	if (fm->sel < 0 || fm->sel >= fm->count) {
		snprintf(fm->status, sizeof(fm->status), "select something first");
		return;
	}
	struct fent *e = &fm->ents[fm->sel];
	if (!strcmp(e->name, "..")) {
		snprintf(fm->status, sizeof(fm->status), "cannot delete ..");
		return;
	}
	char path[FM_FULLLEN];
	fm_path(fm, e->name, path, sizeof(path));
	int r = e->isdir ? rmdir(path) : unlink(path);
	if (r != 0)
		snprintf(fm->status, sizeof(fm->status), "delete failed: %s", strerror(errno));
	else
		snprintf(fm->status, sizeof(fm->status), "deleted %s", e->name);
	fm_load(w);
}

/* Create whatever the prompt was asking for, named by fm->pbuf. */
static void fm_create(struct window *w)
{
	struct fmstate *fm = w->fm;
	if (!fm->pbuf[0] || strchr(fm->pbuf, '/')) {
		snprintf(fm->status, sizeof(fm->status), "bad name");
		fm->prompt = 0;
		return;
	}
	/* Every file this app creates should carry an extension, so its type
	 * icon/tag is always known -- directories have no such concept. */
	if (fm->prompt == 1 && !strchr(fm->pbuf, '.')) {
		size_t len = strlen(fm->pbuf);
		if (len + 4 < sizeof(fm->pbuf))
			memcpy(fm->pbuf + len, ".txt", 5);
	}
	char path[FM_FULLLEN];
	fm_path(fm, fm->pbuf, path, sizeof(path));
	int ok;
	if (fm->prompt == 2) {
		ok = mkdir(path, 0755) == 0;
	} else {
		int fd = open(path, O_WRONLY | O_CREAT | O_EXCL, 0644);
		ok = fd >= 0;
		if (fd >= 0)
			close(fd);
	}
	if (!ok)
		snprintf(fm->status, sizeof(fm->status), "create failed: %s", strerror(errno));
	else
		snprintf(fm->status, sizeof(fm->status), "created %s", fm->pbuf);
	fm->prompt = 0;
	fm->pbuf[0] = 0;
	fm_load(w);
}

/* Rename fm->sel to fm->pbuf, both within the current directory. */
static void fm_rename(struct window *w)
{
	struct fmstate *fm = w->fm;
	if (!fm->pbuf[0] || strchr(fm->pbuf, '/')) {
		snprintf(fm->status, sizeof(fm->status), "bad name");
		fm->prompt = 0;
		return;
	}
	if (fm->sel < 0 || fm->sel >= fm->count) {
		fm->prompt = 0;
		return;
	}
	struct fent *e = &fm->ents[fm->sel];
	/* Renaming a file shouldn't be able to strip its extension away. */
	if (!e->isdir && !strchr(fm->pbuf, '.')) {
		size_t len = strlen(fm->pbuf);
		if (len + 4 < sizeof(fm->pbuf))
			memcpy(fm->pbuf + len, ".txt", 5);
	}
	char oldpath[FM_FULLLEN], newpath[FM_FULLLEN];
	fm_path(fm, e->name, oldpath, sizeof(oldpath));
	fm_path(fm, fm->pbuf, newpath, sizeof(newpath));
	if (rename(oldpath, newpath) != 0)
		snprintf(fm->status, sizeof(fm->status), "rename failed: %s", strerror(errno));
	else
		snprintf(fm->status, sizeof(fm->status), "renamed to %s", fm->pbuf);
	fm->prompt = 0;
	fm->pbuf[0] = 0;
	fm_load(w);
}

/* Paste the clipboard into the current directory. Cut = rename (same fs
 * only); Copy = plain byte-for-byte file copy, no directories. */
static void fm_paste(struct window *w)
{
	struct fmstate *fm = w->fm;
	if (!clip_mode) {
		snprintf(fm->status, sizeof(fm->status), "clipboard is empty");
		return;
	}
	const char *base = strrchr(clip_path, '/');
	base = base ? base + 1 : clip_path;
	char dest[FM_FULLLEN];
	fm_path(fm, base, dest, sizeof(dest));

	/* Copying onto its own path would truncate the source while reading
	 * it (open dest "wb" == open src "wb"). Cut is harmless here (rename
	 * to the same path is a no-op) but there's nothing useful to do either. */
	if (!strcmp(dest, clip_path)) {
		snprintf(fm->status, sizeof(fm->status), "already here");
		return;
	}

	if (clip_mode == 2) {
		if (rename(clip_path, dest) == 0) {
			snprintf(fm->status, sizeof(fm->status), "moved %s", base);
			clip_mode = 0;
		} else {
			snprintf(fm->status, sizeof(fm->status), "move failed: %s", strerror(errno));
		}
	} else {
		FILE *in = fopen(clip_path, "rb");
		FILE *out = in ? fopen(dest, "wb") : NULL;
		if (!in || !out) {
			snprintf(fm->status, sizeof(fm->status), "copy failed: %s", strerror(errno));
		} else {
			char buf[4096];
			size_t n;
			while ((n = fread(buf, 1, sizeof(buf), in)) > 0)
				fwrite(buf, 1, n, out);
			snprintf(fm->status, sizeof(fm->status), "pasted %s", base);
		}
		if (in) fclose(in);
		if (out) fclose(out);
	}
	fm_load(w);
}

/* Paste the clipboard onto the desktop (DESKTOP_DIR). Mirrors fm_paste. */
static void desk_paste(void)
{
	if (!clip_mode)
		return;
	const char *base = strrchr(clip_path, '/');
	base = base ? base + 1 : clip_path;
	char dest[FM_FULLLEN];
	snprintf(dest, sizeof(dest), "%s/%s", DESKTOP_DIR, base);

	if (!strcmp(dest, clip_path)) /* see fm_paste: copying onto itself corrupts it */
		return;

	if (clip_mode == 2) {
		if (rename(clip_path, dest) == 0)
			clip_mode = 0;
	} else {
		FILE *in = fopen(clip_path, "rb");
		FILE *out = in ? fopen(dest, "wb") : NULL;
		if (in && out) {
			char buf[4096];
			size_t n;
			while ((n = fread(buf, 1, sizeof(buf), in)) > 0)
				fwrite(buf, 1, n, out);
		}
		if (in) fclose(in);
		if (out) fclose(out);
	}
	desk_scan();
}

/* Right-click: open the context menu over whichever FILES window (and row,
 * if any) is under the cursor, or over the desktop background itself.
 * Closes any menu already open elsewhere. */
static void ctxmenu_open(int x, int y)
{
	ctxmenu_mode = CTXMODE_NONE;
	ctxmenu_win = -1;
	for (int zi = zcount - 1; zi >= 0; zi--) {
		int i = zorder[zi];
		if (!wins[i].used || wins[i].minimized || wins[i].type != WIN_FILES)
			continue;
		struct window *w = &wins[i];
		if (x < w->x || x >= w->x + w->w || y < w->y || y >= w->y + w->h)
			continue;
		raise_window(i);
		focused = i;

		struct fmstate *fm = w->fm;
		int content_top = w->y + TITLE_H + FM_TOOLH;
		int row = (y - content_top) / font_h;
		int entidx = -1;
		if (row >= 0 && row < w->rows) {
			int vi = fm->scroll + row;
			if (vi >= 0 && vi < fm->vcount)
				entidx = fm->view[vi];
		}
		if (entidx >= 0)
			fm->sel = entidx;
		ctxmenu_mode = CTXMODE_FILEWIN;
		ctxmenu_entidx = entidx;
		ctxmenu_win = i;
		ctxmenu_x = x;
		ctxmenu_y = y;
		int h = CTX_NITEMS * CTX_ITEMH;
		if (ctxmenu_x + CTX_W > xres) ctxmenu_x = xres - CTX_W;
		if (ctxmenu_y + h > yres - TASK_H) ctxmenu_y = yres - TASK_H - h;
		fm_render(w);
		return;
	}

	/* Not over any FILES window: the desktop background itself, unless
	 * the click landed on the taskbar. */
	if (y < yres - TASK_H) {
		ctxmenu_mode = CTXMODE_DESKTOP;
		ctxmenu_deskidx = icon_at(x, y);
		if (ctxmenu_deskidx < NUM_ICONS)
			ctxmenu_deskidx = -1; /* fixed app icons aren't file targets */
		else
			ctxmenu_deskidx -= NUM_ICONS;
		ctxmenu_x = x;
		ctxmenu_y = y;
		int h = CTX_NITEMS * CTX_ITEMH;
		if (ctxmenu_x + CTX_W > xres) ctxmenu_x = xres - CTX_W;
		if (ctxmenu_y + h > yres - TASK_H) ctxmenu_y = yres - TASK_H - h;
	}
}

/* Returns 1 if the click landed on the open menu (whether or not it hit an
 * item), 0 if the caller should still run the normal hit-test. Either way
 * the menu is closed by the caller right after. */
static int ctxmenu_click(int x, int y)
{
	if (ctxmenu_mode == CTXMODE_NONE)
		return 0;
	int h = CTX_NITEMS * CTX_ITEMH;
	if (x < ctxmenu_x || x >= ctxmenu_x + CTX_W || y < ctxmenu_y || y >= ctxmenu_y + h)
		return 0;
	int idx = (y - ctxmenu_y) / CTX_ITEMH;

	if (ctxmenu_mode == CTXMODE_FILEWIN) {
		if (ctxmenu_win < 0 || !wins[ctxmenu_win].used)
			return 1;
		struct window *w = &wins[ctxmenu_win];
		struct fmstate *fm = w->fm;
		struct fent *e = NULL;
		if (ctxmenu_entidx >= 0 && ctxmenu_entidx < fm->count)
			e = &fm->ents[ctxmenu_entidx];
		int has_target = e && strcmp(e->name, "..");
		fm->status[0] = 0;

		switch (idx) {
		case 0: /* Copy */
			if (has_target) {
				fm->sel = ctxmenu_entidx;
				fm_copy_selected(w, 0);
			} else {
				snprintf(fm->status, sizeof(fm->status), "select a file to copy");
			}
			break;
		case 1: /* Cut */
			if (has_target) {
				fm->sel = ctxmenu_entidx;
				fm_copy_selected(w, 1);
			} else {
				snprintf(fm->status, sizeof(fm->status), "select something to cut");
			}
			break;
		case 2: /* Paste */
			fm_paste(w);
			break;
		case 3: /* Rename */
			if (has_target) {
				fm->sel = ctxmenu_entidx;
				fm->prompt = 3;
				snprintf(fm->pbuf, sizeof(fm->pbuf), "%s", e->name);
			} else {
				snprintf(fm->status, sizeof(fm->status), "select something to rename");
			}
			break;
		case 4: /* Delete */
			if (has_target) {
				fm->sel = ctxmenu_entidx;
				fm_delete(w);
			} else {
				snprintf(fm->status, sizeof(fm->status), "select something to delete");
			}
			break;
		}
		fm_render(w);
	} else { /* CTXMODE_DESKTOP */
		struct deskfile *df = NULL;
		if (ctxmenu_deskidx >= 0 && ctxmenu_deskidx < desk_count)
			df = &desk_files[ctxmenu_deskidx];

		switch (idx) {
		case 0: /* Copy -- files only, same restriction as the file manager */
			if (df && !df->isdir) {
				snprintf(clip_path, sizeof(clip_path), "%s/%s", DESKTOP_DIR, df->name);
				clip_mode = 1;
			}
			break;
		case 1: /* Cut */
			if (df) {
				snprintf(clip_path, sizeof(clip_path), "%s/%s", DESKTOP_DIR, df->name);
				clip_mode = 2;
			}
			break;
		case 2: /* Paste */
			desk_paste();
			break;
		case 3: /* Rename: not supported on the desktop -- ponytail, would
			 * need its own text-entry prompt outside any window. */
			break;
		case 4: /* Delete */
			if (df) {
				char path[FM_FULLLEN];
				snprintf(path, sizeof(path), "%s/%s", DESKTOP_DIR, df->name);
				if (df->isdir) rmdir(path); else unlink(path);
				desk_scan();
			}
			break;
		}
	}
	return 1;
}

static void fm_toolbar(struct window *w, int btn)
{
	struct fmstate *fm = w->fm;
	fm->status[0] = 0;
	if (btn != 2)
		fm->confirm_del = 0;
	switch (btn) {
	case 0:
	case 1:
		fm->prompt = btn == 0 ? 1 : 2;
		fm->pbuf[0] = 0;
		break;
	case 2:
		/* Deleting is irreversible: the first click only arms the button. */
		if (!fm->confirm_del) {
			fm->confirm_del = 1;
			snprintf(fm->status, sizeof(fm->status), "click Delete again to confirm");
		} else {
			fm->confirm_del = 0;
			fm_delete(w);
		}
		break;
	case 3:
		fm_load(w);
		break;
	}
}

/* Open the entry at fm->ents[entidx] in window winidx: ".." goes up, a
 * directory navigates into it, a regular file opens the text editor.
 * Split out of fm_click so a completed row press-without-drag and a
 * completed row press-then-drag-then-drop can share it. */
static void fm_open_selected(int winidx, int entidx)
{
	if (winidx < 0 || !wins[winidx].used)
		return;
	struct window *w = &wins[winidx];
	struct fmstate *fm = w->fm;
	if (entidx < 0 || entidx >= fm->count)
		return;
	struct fent *e = &fm->ents[entidx];

	if (!strcmp(e->name, "..")) {
		char *slash = strrchr(fm->cwd, '/');
		if (slash && slash != fm->cwd)
			*slash = 0;
		else
			strcpy(fm->cwd, "/");
		fm->search[0] = 0;
		fm_load(w);
		return;
	}

	char path[FM_FULLLEN];
	fm_path(fm, e->name, path, sizeof(path));

	if (e->isdir) {
		/* Refuse rather than truncate: a truncated cwd is a wrong directory. */
		if (strlen(path) >= sizeof(fm->cwd))
			return;
		memcpy(fm->cwd, path, strlen(path) + 1);
		fm->search[0] = 0;
		fm_load(w);
	} else if (e->isreg) {
		/* Regular files only: opening a fifo or char device would block forever. */
		spawn_editor(path);
	}
}

/* Drop the file that was being dragged (fmdrag_win/fmdrag_entidx) at (x,y):
 * onto a directory row in any FILES window -> move it there; onto the
 * desktop background -> copy it into DESKTOP_DIR; anywhere else -> no-op. */
static void fm_drop(int x, int y)
{
	if (fmdrag_win < 0 || !wins[fmdrag_win].used)
		return;
	struct window *sw = &wins[fmdrag_win];
	struct fmstate *sfm = sw->fm;
	if (fmdrag_entidx < 0 || fmdrag_entidx >= sfm->count)
		return;
	struct fent *se = &sfm->ents[fmdrag_entidx];
	if (!strcmp(se->name, ".."))
		return;
	char srcpath[FM_FULLLEN];
	fm_path(sfm, se->name, srcpath, sizeof(srcpath));

	/* Dropped on another (or the same) FILES window's directory row? Move. */
	for (int zi = zcount - 1; zi >= 0; zi--) {
		int i = zorder[zi];
		if (!wins[i].used || wins[i].minimized || wins[i].type != WIN_FILES)
			continue;
		struct window *tw = &wins[i];
		if (x < tw->x || x >= tw->x + tw->w || y < tw->y || y >= tw->y + tw->h)
			continue;

		struct fmstate *tfm = tw->fm;
		int content_top = tw->y + TITLE_H + FM_TOOLH;
		int row = (y - content_top) / font_h;
		if (row < 0 || row >= tw->rows)
			return; /* dropped on the titlebar/toolbar: no-op */
		int vi = tfm->scroll + row;
		if (vi < 0 || vi >= tfm->vcount)
			return; /* dropped past the end of the listing: no-op */
		int ti = tfm->view[vi];
		struct fent *te = &tfm->ents[ti];

		char destdir[FM_FULLLEN];
		if (!strcmp(te->name, "..")) {
			char *slash = strrchr(tfm->cwd, '/');
			if (slash && slash != tfm->cwd) {
				size_t n = (size_t)(slash - tfm->cwd);
				memcpy(destdir, tfm->cwd, n);
				destdir[n] = 0;
			} else {
				strcpy(destdir, "/");
			}
		} else if (te->isdir) {
			fm_path(tfm, te->name, destdir, sizeof(destdir));
		} else {
			return; /* dropped onto a file row: no-op */
		}

		char dest[FM_FULLLEN];
		snprintf(dest, sizeof(dest), "%s%s%s", destdir,
			 strcmp(destdir, "/") ? "/" : "", se->name);
		if (!strcmp(srcpath, dest))
			return; /* dropped onto its own folder */
		if (rename(srcpath, dest) == 0)
			snprintf(sfm->status, sizeof(sfm->status), "moved %s", se->name);
		else
			snprintf(sfm->status, sizeof(sfm->status), "move failed: %s", strerror(errno));
		fm_load(sw);
		if (tw != sw)
			fm_load(tw);
		return;
	}

	/* Not over any FILES window: the desktop, if it's not the taskbar.
	 * Files only -- same restriction as fm_paste's Copy. */
	if (y < yres - TASK_H) {
		if (se->isdir) {
			snprintf(sfm->status, sizeof(sfm->status), "cannot copy directories to desktop");
			fm_render(sw);
			return;
		}
		char dest[FM_FULLLEN];
		snprintf(dest, sizeof(dest), "%s/%s", DESKTOP_DIR, se->name);
		FILE *in = fopen(srcpath, "rb");
		FILE *out = in ? fopen(dest, "wb") : NULL;
		if (in && out) {
			char buf[4096];
			size_t n;
			while ((n = fread(buf, 1, sizeof(buf), in)) > 0)
				fwrite(buf, 1, n, out);
			snprintf(sfm->status, sizeof(sfm->status), "copied %s to desktop", se->name);
			desk_scan();
		} else {
			snprintf(sfm->status, sizeof(sfm->status), "copy failed: %s", strerror(errno));
		}
		if (in) fclose(in);
		if (out) fclose(out);
		fm_render(sw);
	}
}

static void fm_click(struct window *w, int x, int y)
{
	struct fmstate *fm = w->fm;
	int btn = fm_btn_at(w, x, y);
	if (btn >= 0) {
		fm_toolbar(w, btn);
		fm_render(w);
		return;
	}
	fm->status[0] = 0;
	fm->confirm_del = 0;

	int row = (y - (w->y + TITLE_H + FM_TOOLH)) / font_h;
	if (row < 0 || row >= w->rows)
		return;
	int vi = fm->scroll + row;
	if (vi < 0 || vi >= fm->vcount)
		return;
	int i = fm->view[vi];

	/* Arm this row as a drag candidate; motion past a threshold turns it
	 * into a drag (handled in process_pointer). If it never moved, the
	 * release replays today's click semantics: first click selects,
	 * a second click on an already-selected row opens it. */
	fmdrag_win = (int)(w - wins);
	fmdrag_entidx = i;
	fmdrag_active = 0;
	fmdrag_grab_x = x;
	fmdrag_grab_y = y;
	fmdrag_was_preselected = (fm->sel == i);
	fm->sel = i;
	fm_render(w);
}

/* While a New File / New Dir / Rename prompt is open, keys go into the name
 * field. While searching, keys go into the search filter. Otherwise arrows /
 * PageUp / PageDown scroll the listing and Ctrl+F starts a search. */
static int fm_keys(struct window *w, const char *buf, int n)
{
	struct fmstate *fm = w->fm;
	int changed = 0;

	if (fm->prompt) {
		for (int i = 0; i < n; i++) {
			unsigned char c = (unsigned char)buf[i];
			int len = (int)strlen(fm->pbuf);
			if (c == '\r' || c == '\n') {
				if (fm->prompt == 3)
					fm_rename(w);
				else
					fm_create(w);
			} else if (c == 0x1b) {
				fm->prompt = 0;
				fm->pbuf[0] = 0;
			} else if ((c == 0x7f || c == '\b') && len > 0) {
				fm->pbuf[len - 1] = 0;
			} else if (c >= 0x20 && c < 0x7f && len < FM_NAMELEN - 1) {
				fm->pbuf[len] = (char)c;
				fm->pbuf[len + 1] = 0;
			}
			changed = 1;
			if (!fm->prompt)
				break; /* Enter/Esc ended it; the rest isn't ours */
		}
		fm_render(w);
		return changed;
	}

	if (fm->searching) {
		for (int i = 0; i < n; i++) {
			unsigned char c = (unsigned char)buf[i];
			int len = (int)strlen(fm->search);
			if (c == '\r' || c == '\n') {
				fm->searching = 0;
			} else if (c == 0x1b) {
				fm->searching = 0;
				fm->search[0] = 0;
				fm->sel = -1;
			} else if ((c == 0x7f || c == '\b') && len > 0) {
				fm->search[len - 1] = 0;
			} else if (c >= 0x20 && c < 0x7f && len < FM_NAMELEN - 1) {
				fm->search[len] = (char)c;
				fm->search[len + 1] = 0;
			} else {
				continue;
			}
			fm_apply_filter(fm);
			changed = 1;
			if (!fm->searching)
				break; /* Enter/Esc ended input; the rest isn't ours */
		}
		fm_render(w);
		return changed;
	}

	for (int i = 0; i < n; i++) {
		unsigned char c = (unsigned char)buf[i];

		if (c == 0x1b && i + 1 < n && buf[i + 1] == '[') {
			if (i + 3 < n && buf[i + 2] == '3' && buf[i + 3] == '~') {
				/* Delete key: ESC [ 3 ~ -- same two-press confirm as
				 * the toolbar's Delete button. */
				fm->status[0] = 0;
				if (!fm->confirm_del) {
					fm->confirm_del = 1;
					snprintf(fm->status, sizeof(fm->status), "press Delete again to confirm");
				} else {
					fm->confirm_del = 0;
					fm_delete(w);
				}
				fm_render(w);
				changed = 1;
				i += 3;
				continue;
			}
			if (i + 2 < n) {
				int delta = 0;
				switch (buf[i + 2]) {
				case 'A': delta = -1; break;
				case 'B': delta = 1; break;
				case '5': delta = -(w->rows - 1); break;
				case '6': delta = w->rows - 1; break;
				default: break;
				}
				fm->confirm_del = 0;
				if (delta) {
					fm->scroll += delta;
					fm_render(w); /* clamps scroll */
					changed = 1;
				}
				i += 2;
				continue;
			}
		}

		if (c == 0x06) { /* Ctrl+F */
			fm->confirm_del = 0;
			fm->searching = 1;
			fm_render(w);
			return 1;
		}
		if (c == 0x03) { /* Ctrl+C */
			fm->confirm_del = 0;
			fm_copy_selected(w, 0);
			fm_render(w);
			changed = 1;
		} else if (c == 0x18) { /* Ctrl+X */
			fm->confirm_del = 0;
			fm_copy_selected(w, 1);
			fm_render(w);
			changed = 1;
		} else if (c == 0x16) { /* Ctrl+V */
			fm->confirm_del = 0;
			fm_paste(w);
			changed = 1;
		}
	}
	return changed;
}

static int spawn_file_window(void)
{
	int slot = alloc_window_slot();
	if (slot < 0)
		return -1;
	struct fmstate *fm = calloc(1, sizeof(struct fmstate));
	if (!fm)
		return -1;
	memset(&wins[slot], 0, sizeof(wins[slot]));
	wins[slot].used = 1;
	wins[slot].type = WIN_FILES;
	wins[slot].pty_fd = -1;
	wins[slot].fm = fm;
	wins[slot].x = 240 + slot * 24;
	wins[slot].y = 100 + slot * 24;
	wins[slot].w = 620;
	wins[slot].h = 440;
	wins[slot].attr_fg = COL_FG_DEFAULT;
	wins[slot].attr_bg = COL_BG_DEFAULT;
	snprintf(fm->cwd, sizeof(fm->cwd), "/");
	update_grid_dims(&wins[slot]);
	fm_load(&wins[slot]);
	zorder[zcount++] = slot;
	focused = slot;
	return slot;
}

/* ---- text editor ---- */

static void ed_render(struct window *w)
{
	struct edstate *e = w->ed;
	if (e->cy < 0) e->cy = 0;
	if (e->cy >= e->nlines) e->cy = e->nlines - 1;
	int len = (int)strlen(e->line[e->cy]);
	if (e->cx > len) e->cx = len;
	if (e->cx < 0) e->cx = 0;
	/* keep the caret on screen */
	if (e->cy < e->scroll) e->scroll = e->cy;
	if (e->cy >= e->scroll + w->rows) e->scroll = e->cy - w->rows + 1;
	if (e->scroll < 0) e->scroll = 0;

	for (int r = 0; r < w->rows; r++)
		clear_row_range(w, r, 0, w->cols - 1);
	for (int r = 0; r < w->rows; r++) {
		int i = e->scroll + r;
		if (i >= e->nlines)
			break;
		for (int c = 0; c < w->cols && e->line[i][c]; c++) {
			w->gch[r][c] = (unsigned char)e->line[i][c];
			w->gfg[r][c] = COL_FG_DEFAULT;
		}
	}
	w->cur_row = e->cy - e->scroll;
	w->cur_col = e->cx;
	if (w->cur_col >= w->cols) w->cur_col = w->cols - 1;

	snprintf(w->title, sizeof(w->title), "%s%.400s%s%s",
		 e->dirty ? "*" : "", e->path,
		 e->status[0] ? "  -  " : "", e->status);
}

static void ed_save(struct window *w)
{
	struct edstate *e = w->ed;
	if (e->truncated) {
		snprintf(e->status, sizeof(e->status), "REFUSED: file was truncated on load");
		return;
	}
	FILE *f = fopen(e->path, "w");
	if (!f) {
		snprintf(e->status, sizeof(e->status), "save failed: %s", strerror(errno));
		return;
	}
	for (int i = 0; i < e->nlines; i++)
		fprintf(f, "%s\n", e->line[i]);
	if (fclose(f) != 0) {
		snprintf(e->status, sizeof(e->status), "save failed: %s", strerror(errno));
		return;
	}
	e->dirty = 0;
	snprintf(e->status, sizeof(e->status), "saved %d lines", e->nlines);
}

static void ed_insert(struct edstate *e, char c)
{
	char *l = e->line[e->cy];
	int len = (int)strlen(l);
	if (len + 1 >= ED_MAXCOL)
		return; /* ponytail: hard line-length cap, no wrapping */
	memmove(l + e->cx + 1, l + e->cx, len - e->cx + 1);
	l[e->cx++] = c;
	e->dirty = 1;
}

static void ed_newline(struct edstate *e)
{
	if (e->nlines + 1 >= ED_MAXLINES)
		return;
	for (int i = e->nlines; i > e->cy + 1; i--)
		memcpy(e->line[i], e->line[i - 1], ED_MAXCOL);
	char tail[ED_MAXCOL];
	char *cur = e->line[e->cy];
	snprintf(tail, sizeof(tail), "%s", cur + e->cx);
	cur[e->cx] = 0;
	memcpy(e->line[e->cy + 1], tail, sizeof(tail));
	e->nlines++;
	e->cy++;
	e->cx = 0;
	e->dirty = 1;
}

static void ed_backspace(struct edstate *e)
{
	char *l = e->line[e->cy];
	if (e->cx > 0) {
		int len = (int)strlen(l);
		memmove(l + e->cx - 1, l + e->cx, len - e->cx + 1);
		e->cx--;
		e->dirty = 1;
		return;
	}
	if (e->cy == 0)
		return;
	char *prev = e->line[e->cy - 1];
	int plen = (int)strlen(prev);
	int llen = (int)strlen(l);
	if (plen + llen < ED_MAXCOL)
		memcpy(prev + plen, l, llen + 1);
	for (int i = e->cy; i < e->nlines - 1; i++)
		memcpy(e->line[i], e->line[i + 1], ED_MAXCOL);
	e->nlines--;
	e->cy--;
	e->cx = plen;
	e->dirty = 1;
}

static int ed_keys(struct window *w, const char *buf, int n)
{
	struct edstate *e = w->ed;
	for (int i = 0; i < n; i++) {
		unsigned char c = (unsigned char)buf[i];
		if (c == 0x1b && i + 2 < n && buf[i + 1] == '[') {
			switch (buf[i + 2]) {
			case 'A': e->cy--; break;
			case 'B': e->cy++; break;
			case 'C': e->cx++; break;
			case 'D': e->cx--; break;
			case '5': e->cy -= w->rows - 1; break;
			case '6': e->cy += w->rows - 1; break;
			default: break;
			}
			if (e->cy < 0) e->cy = 0;
			if (e->cy >= e->nlines) e->cy = e->nlines - 1;
			i += 2;
			continue;
		}
		e->status[0] = 0;
		if (c == 0x13)          ed_save(w);           /* Ctrl+S */
		else if (c == '\r' || c == '\n') ed_newline(e);
		else if (c == 0x7f || c == '\b') ed_backspace(e);
		else if (c == '\t')     { for (int k = 0; k < 4; k++) ed_insert(e, ' '); }
		else if (c >= 0x20 && c < 0x7f) ed_insert(e, (char)c);
	}
	ed_render(w);
	return 1;
}

static void ed_click(struct window *w, int x, int y)
{
	struct edstate *e = w->ed;
	int row = (y - (w->y + TITLE_H)) / font_h;
	int col = (x - (w->x + 4)) / font_w;
	if (row < 0) row = 0;
	if (col < 0) col = 0;
	e->cy = e->scroll + row;
	e->cx = col;
	ed_render(w); /* clamps both */
}

static int spawn_editor(const char *path)
{
	int slot = alloc_window_slot();
	if (slot < 0)
		return -1;
	struct edstate *e = calloc(1, sizeof(struct edstate));
	if (!e)
		return -1;
	snprintf(e->path, sizeof(e->path), "%s", path);

	FILE *f = fopen(path, "r");
	if (f) {
		char raw[ED_MAXCOL * 4];
		while (e->nlines < ED_MAXLINES && fgets(raw, sizeof(raw), f)) {
			raw[strcspn(raw, "\n")] = 0;
			if (strlen(raw) >= ED_MAXCOL)
				e->truncated = 1;
			snprintf(e->line[e->nlines], ED_MAXCOL, "%s", raw);
			e->nlines++;
		}
		/* more lines than we can hold: saving would silently drop the rest */
		if (e->nlines >= ED_MAXLINES && fgetc(f) != EOF)
			e->truncated = 1;
		fclose(f);
	}
	if (e->nlines == 0)
		e->nlines = 1;
	if (e->truncated)
		snprintf(e->status, sizeof(e->status), "READ-ONLY: file too large");

	memset(&wins[slot], 0, sizeof(wins[slot]));
	wins[slot].used = 1;
	wins[slot].type = WIN_EDIT;
	wins[slot].pty_fd = -1;
	wins[slot].ed = e;
	wins[slot].x = 260 + slot * 24;
	wins[slot].y = 110 + slot * 24;
	wins[slot].w = 620;
	wins[slot].h = 420;
	wins[slot].attr_fg = COL_FG_DEFAULT;
	wins[slot].attr_bg = COL_BG_DEFAULT;
	update_grid_dims(&wins[slot]);
	ed_render(&wins[slot]);
	zorder[zcount++] = slot;
	focused = slot;
	return slot;
}

/* ---- settings ---- */

#define SET_BTNW 130
#define SET_BTNH 30

static int spawn_settings(void)
{
	for (int i = 0; i < MAX_WIN; i++) {
		if (wins[i].used && wins[i].type == WIN_SETTINGS) {
			wins[i].minimized = 0;
			raise_window(i);
			focused = i;
			return i;
		}
	}
	int slot = alloc_window_slot();
	if (slot < 0)
		return -1;
	memset(&wins[slot], 0, sizeof(wins[slot]));
	wins[slot].used = 1;
	wins[slot].type = WIN_SETTINGS;
	wins[slot].pty_fd = -1;
	wins[slot].x = 260;
	wins[slot].y = 140;
	wins[slot].w = 520;
	wins[slot].h = 300;
	snprintf(wins[slot].title, sizeof(wins[slot].title), "Settings");
	zorder[zcount++] = slot;
	focused = slot;
	return slot;
}

static void draw_settings(struct window *w, int content_y)
{
	draw_text(w->x + 16, content_y + 14, "Theme", 0xffffff);
	for (int t = 0; t < NUM_THEMES; t++) {
		int row = t / 3;
		int col = t % 3;
		int bx = w->x + 16 + col * (SET_BTNW + 10);
		int by = content_y + 38 + row * (SET_BTNH + 8);
		int on = (t == theme_idx);
		fill_round_rect_grad(bx, by, SET_BTNW, SET_BTNH, 6,
				     mix(themes[t].dtop, 0xffffff, on ? 40 : 0),
				     themes[t].dbot);
		if (on)
			fill_round_rect(bx, by + SET_BTNH - 3, SET_BTNW, 3, 1, themes[t].accent);
		fill_circle(bx + 14, by + SET_BTNH / 2, 5, themes[t].accent);
		draw_text_clip(bx + 26, by + (SET_BTNH - font_h) / 2, themes[t].name,
			       on ? 0xffffff : 0xa6adc8, SET_BTNW - 32);
	}

	/* Show hidden files toggle */
	draw_text(w->x + 16, content_y + 130, "Show hidden files", 0xffffff);
	int cx = w->x + 16 + 170, cy = content_y + 130;
	fill_round_rect(cx, cy, 32, 16, 8,
		        show_hidden ? 0x22c55e : 0x6c7086);
	fill_circle(cx + (show_hidden ? 24 : 8), cy + 8, 6, 0xffffff);

	/* Display info */
	char info[256];
	snprintf(info, sizeof(info),
		 "\nDisplay\n  %dx%d  %d bpp\n  font %dx%d (kernel VT)\n"
		 "\nMode is fixed by GRUB gfxpayload at boot.",
		 xres, yres, bpp, font_w, font_h);
	draw_text(w->x + 16, content_y + 160, info, 0x9399b2);
}

/* Settings hit-test: theme buttons (arranged 3+2), or hidden-files toggle.
 * Returns: 0-NUM_THEMES for themes, NUM_THEMES for toggle, -1 for none. */
static int settings_click(struct window *w, int px, int py)
{
	for (int t = 0; t < NUM_THEMES; t++) {
		int row = t / 3;
		int col = t % 3;
		int bx = w->x + 16 + col * (SET_BTNW + 10);
		int by = w->y + TITLE_H + 38 + row * (SET_BTNH + 8);
		if (px >= bx && px < bx + SET_BTNW && py >= by && py < by + SET_BTNH)
			return t;
	}
	/* Hidden files toggle at offset 130 */
	int ty = w->y + TITLE_H + 130;
	int tx = w->x + 16 + 170;
	if (px >= tx && px < tx + 32 && py >= ty && py < ty + 16)
		return NUM_THEMES;
	return -1;
}

/* ---- CPU / memory history ---- */

static long read_meminfo_kb(const char *key)
{
	FILE *f = fopen("/proc/meminfo", "r");
	if (!f)
		return 0;
	char line[128];
	long val = 0;
	size_t klen = strlen(key);
	while (fgets(line, sizeof(line), f)) {
		if (!strncmp(line, key, klen) && line[klen] == ':') {
			val = strtol(line + klen + 1, NULL, 10);
			break;
		}
	}
	fclose(f);
	return val;
}

/* Called once a second. Percentages, 0..100. */
static void sample_stats(void)
{
	static long prev_busy, prev_total;
	int cpu = 0, mem = 0;

	FILE *f = fopen("/proc/stat", "r");
	if (f) {
		long v[8] = {0};
		if (fscanf(f, "cpu %ld %ld %ld %ld %ld %ld %ld %ld",
			   &v[0], &v[1], &v[2], &v[3], &v[4], &v[5], &v[6], &v[7]) >= 4) {
			long total = 0;
			for (int i = 0; i < 8; i++)
				total += v[i];
			long busy = total - v[3] - v[4]; /* minus idle and iowait */
			long dt = total - prev_total, db = busy - prev_busy;
			if (prev_total && dt > 0)
				cpu = (int)(db * 100 / dt);
			prev_total = total;
			prev_busy = busy;
		}
		fclose(f);
	}

	long tot = read_meminfo_kb("MemTotal");
	long avail = read_meminfo_kb("MemAvailable");
	if (tot > 0 && avail > 0)
		mem = (int)((tot - avail) * 100 / tot);

	if (cpu < 0) cpu = 0;
	if (cpu > 100) cpu = 100;
	memmove(cpu_hist, cpu_hist + 1, sizeof(cpu_hist) - sizeof(cpu_hist[0]));
	memmove(mem_hist, mem_hist + 1, sizeof(mem_hist) - sizeof(mem_hist[0]));
	cpu_hist[HIST - 1] = cpu;
	mem_hist[HIST - 1] = mem;
}

static void draw_graph(int x, int y, int w, int h, const int *hist,
		       uint32_t col, const char *label)
{
	fill_round_rect(x, y, w, h, 4, 0x181826);
	for (int g = 1; g < 4; g++)
		fill_rect(x + 1, y + g * h / 4, w - 2, 1, 0x272740);

	int bw = w / HIST;
	if (bw < 1) bw = 1;
	for (int i = 0; i < HIST; i++) {
		int bh = hist[i] * (h - 2) / 100;
		int bx = x + 1 + i * (w - 2) / HIST;
		if (bh > 0)
			fill_rect(bx, y + h - 1 - bh, bw, bh, col);
	}

	char txt[48];
	snprintf(txt, sizeof(txt), "%s %d%%", label, hist[HIST - 1]);
	draw_text(x + 8, y + 6, txt, 0xffffff);
}

/* Small colored dot per window type, drawn in the titlebar. */
static uint32_t win_accent(const struct window *w)
{
	if (w->type == WIN_TERM)  return 0x9333ea;
	if (w->type == WIN_FILES) return 0x06b6d4;
	if (w->type == WIN_EDIT)  return 0xa6e3a1;
	return themes[theme_idx].accent;
}

static void draw_window(struct window *w)
{
	int i = (int)(w - wins);
	int act = (i == focused);

	/* drop shadow */
	fill_round_rect(w->x + 5, w->y + 6, w->w, w->h, 9, 0x0a0a11);
	/* frame: a 1px outline that brightens when focused */
	fill_round_rect(w->x - 1, w->y - 1, w->w + 2, w->h + 2, 9,
			act ? 0x6c7086 : 0x2a2a38);

	/* titlebar: rounded on top, squared where it meets the content */
	uint32_t t1 = act ? 0x494b61 : 0x2e2e3c;
	uint32_t t2 = act ? 0x33344a : 0x24242f;
	fill_round_rect_grad(w->x, w->y, w->w, TITLE_H, 8, t1, t2);
	fill_rect(w->x, w->y + TITLE_H - 8, w->w, 8, t2);

	/* type accent dot + title */
	fill_circle(w->x + 14, w->y + TITLE_H / 2, 4, act ? win_accent(w) : 0x585b70);
	draw_text_clip(w->x + 26, w->y + (TITLE_H - font_h) / 2, w->title,
		       act ? 0xffffff : 0x9399b2, w->w - 26 - 84);

	/* buttons: keep the three 24px bands the hit-test uses, draw them as dots */
	int cy = w->y + TITLE_H / 2;
	int closeX = w->x + w->w - 24, maxX = closeX - 24, minX = maxX - 24;
	fill_circle(minX + 12,   cy, 6, act ? 0xa6e3a1 : 0x585b70);
	fill_circle(maxX + 12,   cy, 6, act ? 0xf9e2af : 0x585b70);
	fill_circle(closeX + 12, cy, 6, act ? 0xf38ba8 : 0x585b70);

	int content_y = w->y + TITLE_H, content_h = w->h - TITLE_H;
	if (content_h < 0)
		content_h = 0;

	if (w->type == WIN_SETTINGS) {
		fill_rect(w->x, content_y, w->w, content_h, COL_BG_DEFAULT);
		draw_settings(w, content_y);
		return; /* no grid, no resize grip -- settings is a fixed panel */
	}

	/* file-manager toolbar sits between titlebar and listing */
	if (w->type == WIN_FILES && w->fm) {
		struct fmstate *fm = w->fm;
		fill_rect(w->x, content_y, w->w, FM_TOOLH, 0x181826);
		for (int b = 0; b < FM_NBTN; b++) {
			int bx = w->x + 6 + b * (FM_BTNW + 4);
			int armed = (b == 2 && fm->confirm_del);
			fill_round_rect_grad(bx, content_y + 3, FM_BTNW, FM_TOOLH - 6, 4,
					     armed ? 0xf38ba8 : 0x2b2b3a,
					     armed ? 0xc4506a : 0x22222e);
			int lw = (int)strlen(fm_btns[b]) * font_w;
			draw_text_clip(bx + (FM_BTNW - lw) / 2,
				       content_y + (FM_TOOLH - font_h) / 2,
				       fm_btns[b], 0xdfe4f2, FM_BTNW - 6);
		}
		/* name prompt / search / status share the strip right of the buttons */
		int tx = w->x + 6 + FM_NBTN * (FM_BTNW + 4) + 6;
		int ty2 = content_y + (FM_TOOLH - font_h) / 2;
		if (fm->prompt) {
			static const char *plabel[] = { "", "file", "dir", "rename" };
			char line[FM_NAMELEN + 16];
			snprintf(line, sizeof(line), "%s: %s_", plabel[fm->prompt], fm->pbuf);
			draw_text_clip(tx, ty2, line, 0xf9e2af, w->x + w->w - tx - 6);
		} else if (fm->searching) {
			char line[FM_NAMELEN + 16];
			snprintf(line, sizeof(line), "search: %s_", fm->search);
			draw_text_clip(tx, ty2, line, 0x94e2d5, w->x + w->w - tx - 6);
		} else if (fm->status[0]) {
			draw_text_clip(tx, ty2, fm->status, 0x9399b2, w->x + w->w - tx - 6);
		} else if (fm->search[0]) {
			char line[FM_NAMELEN + 16];
			snprintf(line, sizeof(line), "filter: %s", fm->search);
			draw_text_clip(tx, ty2, line, 0x6c7086, w->x + w->w - tx - 6);
		}
		content_y += FM_TOOLH;
		content_h -= FM_TOOLH;
	}

	/* task-manager tab strip sits between titlebar and content */
	if (w->type == WIN_TASKMGR) {
		int tw = w->w / TM_NTABS;
		fill_rect(w->x, content_y, w->w, TM_TABH, 0x181826);
		for (int t = 0; t < TM_NTABS; t++) {
			int tx = w->x + t * tw;
			int on = (t == w->tab);
			fill_rect(tx, content_y, tw - 1, TM_TABH,
				  on ? COL_BG_DEFAULT : 0x232338);
			if (on)
				fill_rect(tx, content_y, tw - 1, 2, win_accent(w));
			int lw = (int)strlen(tm_tabs[t].label) * font_w;
			draw_text_clip(tx + (tw - lw) / 2,
				       content_y + (TM_TABH - font_h) / 2,
				       tm_tabs[t].label,
				       on ? 0xffffff : 0x9399b2, tw - 6);
		}
		content_y += TM_TABH;
		content_h -= TM_TABH;

		if (w->tab == 1 && content_h > TM_GRAPH_H) {
			fill_rect(w->x, content_y, w->w, TM_GRAPH_H, COL_BG_DEFAULT);
			int gw = (w->w - 24) / 2, gh = TM_GRAPH_H - 16;
			draw_graph(w->x + 8, content_y + 8, gw, gh, cpu_hist, 0x89b4fa, "CPU");
			draw_graph(w->x + 16 + gw, content_y + 8, gw, gh, mem_hist, 0xa6e3a1, "MEM");
			content_y += TM_GRAPH_H;
			content_h -= TM_GRAPH_H;
		}
	}

	fill_rect(w->x, content_y, w->w, content_h, COL_BG_DEFAULT);

	for (int r = 0; r < w->rows; r++) {
		int ry = content_y + r * font_h;
		for (int c = 0; c < w->cols; c++) {
			uint32_t bg = w->gbg[r][c];
			if (bg != COL_BG_DEFAULT)
				fill_rect(w->x + 4 + c * font_w, ry, font_w, font_h, bg);
		}
	}
	for (int r = 0; r < w->rows; r++) {
		int ry = content_y + r * font_h;
		for (int c = 0; c < w->cols; c++) {
			unsigned char ch = w->gch[r][c];
			if (ch && ch != ' ')
				blit_char(w->x + 4 + c * font_w, ry, ch, w->gfg[r][c]);
		}
	}
	if (w->type == WIN_TERM || w->type == WIN_EDIT) {
		int bx = w->x + 4 + w->cur_col * font_w;
		int by = content_y + w->cur_row * font_h + font_h - 2;
		fill_rect(bx, by, font_w, 2, 0xf9e2af);
	}

	/* resize grip: three diagonal pips */
	if (!w->maximized) {
		for (int k = 0; k < 3; k++) {
			int gx = w->x + w->w - 5 - k * 4;
			int gy = w->y + w->h - 5;
			for (int m = 0; m <= k; m++)
				fill_rect(gx, gy - m * 4, 2, 2, 0x6c7086);
		}
	}
}

/* "Show desktop": minimize everything, click again to bring it all back.
 * Sits at the far right of the taskbar, to the right of the clock. */
#define SD_W 34

static int sd_x(void)
{
	return xres - SD_W - 6;
}

/* Rightmost pixel the window buttons may use: clock and button come after. */
static int task_limit(void)
{
	return sd_x() - 12 - 8 * font_w - 20;
}

static void toggle_show_desktop(void)
{
	if (!sd_active) {
		for (int i = 0; i < MAX_WIN; i++) {
			sd_saved[i] = wins[i].used && !wins[i].minimized;
			if (sd_saved[i])
				wins[i].minimized = 1;
		}
		sd_active = 1;
	} else {
		for (int i = 0; i < MAX_WIN; i++)
			if (sd_saved[i] && wins[i].used)
				wins[i].minimized = 0;
		sd_active = 0;
	}
}

static void draw_taskbar(void)
{
	int ty = yres - TASK_H;
	fill_vgradient(0, ty, xres, TASK_H, 0x1c1c2a, 0x101018);
	fill_rect(0, ty, xres, 1, 0x3a3a4d);

	/* show-desktop button: a small stylized screen */
	int sy = ty + 5, sh = TASK_H - 10, sx = sd_x();
	fill_round_rect_grad(sx, sy, SD_W, sh, 5,
			     sd_active ? 0x4a4c63 : 0x2b2b3a,
			     sd_active ? 0x393b52 : 0x22222e);
	fill_round_rect(sx + 8, sy + 7, SD_W - 16, sh - 16, 2,
			sd_active ? themes[theme_idx].accent : 0x9399b2);

	int bx = 8;
	for (int zi = 0; zi < zcount; zi++) {
		int i = zorder[zi];
		if (!wins[i].used)
			continue;
		int bw = 130, bh = TASK_H - 10;
		int by = ty + 5;
		if (bx + bw > task_limit())
			break; /* out of room before the clock */
		int act = (i == focused && !wins[i].minimized);
		fill_round_rect_grad(bx, by, bw, bh, 5,
				     act ? 0x4a4c63 : 0x2b2b3a,
				     act ? 0x393b52 : 0x22222e);
		if (act) /* accent underline on the focused task */
			fill_round_rect(bx + 6, by + bh - 3, bw - 12, 2, 1, win_accent(&wins[i]));
		fill_circle(bx + 11, by + bh / 2, 3,
			    wins[i].minimized ? 0x585b70 : win_accent(&wins[i]));
		draw_text_clip(bx + 20, by + (bh - font_h) / 2, wins[i].title,
			       act ? 0xffffff : 0xa6adc8, bw - 28);
		bx += bw + 6;
	}

	char timebuf[24];
	time_t t = time(NULL);
	struct tm *tm = localtime(&t);
	snprintf(timebuf, sizeof(timebuf), "%02d:%02d:%02d", tm->tm_hour, tm->tm_min, tm->tm_sec);
	int tw = (int)strlen(timebuf) * font_w;
	int cx = sx - 12 - tw; /* clock sits left of the show-desktop button */
	fill_rect(cx - 12, ty + 9, 1, TASK_H - 18, 0x3a3a4d);
	draw_text(cx, ty + (TASK_H - font_h) / 2, timebuf, 0xcdd6f4);
}

static void cycle_window_focus(void)
{
	if (zcount == 0)
		return;
	int next = -1;
	for (int i = 0; i < zcount; i++) {
		if (zorder[i] == focused) {
			next = zorder[(i + 1) % zcount];
			break;
		}
	}
	if (next == -1 && zcount > 0)
		next = zorder[0];
	if (next >= 0 && wins[next].used) {
		focused = next; /* raise_window alone only reorders z; focus must follow */
		if (wins[next].minimized) {
			wins[next].minimized = 0;
			if (wins[next].maximized) {
				wins[next].x = wins[next].rx;
				wins[next].y = wins[next].ry;
				wins[next].w = wins[next].rw;
				wins[next].h = wins[next].rh;
				wins[next].maximized = 0;
			}
		}
		raise_window(next);
	}
}

static void draw_ctxmenu(void)
{
	if (ctxmenu_mode == CTXMODE_NONE)
		return;

	int has_target, target_isreg;
	if (ctxmenu_mode == CTXMODE_FILEWIN) {
		if (ctxmenu_win < 0 || !wins[ctxmenu_win].used) {
			ctxmenu_mode = CTXMODE_NONE;
			return;
		}
		struct fmstate *fm = wins[ctxmenu_win].fm;
		struct fent *e = NULL;
		if (ctxmenu_entidx >= 0 && ctxmenu_entidx < fm->count)
			e = &fm->ents[ctxmenu_entidx];
		has_target = e && strcmp(e->name, "..");
		target_isreg = has_target && e->isreg;
	} else {
		struct deskfile *df = NULL;
		if (ctxmenu_deskidx >= 0 && ctxmenu_deskidx < desk_count)
			df = &desk_files[ctxmenu_deskidx];
		has_target = df != NULL;
		target_isreg = df && !df->isdir;
	}

	int h = CTX_NITEMS * CTX_ITEMH;
	fill_round_rect(ctxmenu_x + 3, ctxmenu_y + 4, CTX_W, h, 6, 0x0a0a11);
	fill_round_rect(ctxmenu_x, ctxmenu_y, CTX_W, h, 6, 0x2e2e3c);
	for (int i = 0; i < CTX_NITEMS; i++) {
		int iy = ctxmenu_y + i * CTX_ITEMH;
		int enabled;
		if (i == 2) enabled = clip_mode;                                /* Paste */
		else if (i == 0) enabled = target_isreg;                        /* Copy */
		else if (i == 3) enabled = has_target && ctxmenu_mode == CTXMODE_FILEWIN; /* Rename */
		else enabled = has_target;                                      /* Cut, Delete */
		draw_text(ctxmenu_x + 10, iy + (CTX_ITEMH - font_h) / 2,
			  ctx_items[i], enabled ? 0xdfe4f2 : 0x585b70);
	}
}

/* While a file manager row is being dragged, show its name near the cursor
 * so the user can see what they're carrying and where it'll land. */
static void draw_fmdrag(void)
{
	if (fmdrag_win < 0 || !fmdrag_active || !wins[fmdrag_win].used)
		return;
	struct fmstate *fm = wins[fmdrag_win].fm;
	if (fmdrag_entidx < 0 || fmdrag_entidx >= fm->count)
		return;
	const char *name = fm->ents[fmdrag_entidx].name;
	int w = (int)strlen(name) * font_w + 16;
	fill_round_rect(mx + 15, my + 15, w, font_h + 8, 4, 0x0a0a11);
	fill_round_rect(mx + 14, my + 14, w, font_h + 8, 4, 0x313244);
	draw_text(mx + 22, my + 18, name, 0xf9e2af);
}

static void redraw_all(void)
{
	fill_vgradient(0, 0, xres, yres - TASK_H,
		       themes[theme_idx].dtop, themes[theme_idx].dbot);
	draw_icons();
	for (int zi = 0; zi < zcount; zi++) {
		int i = zorder[zi];
		if (wins[i].used && !wins[i].minimized)
			draw_window(&wins[i]);
	}
	draw_taskbar();
	draw_ctxmenu();
	draw_fmdrag();
	draw_cursor(mx, my);
	if (backbuf)
		memcpy(fbp, backbuf, (size_t)line_length * yres);
}

static void clamp_window(struct window *w)
{
	if (w->x < -w->w + 40) w->x = -w->w + 40;
	if (w->y < 0) w->y = 0;
	if (w->x > xres - 40) w->x = xres - 40;
	if (w->y > yres - TASK_H - TITLE_H) w->y = yres - TASK_H - TITLE_H;
}

static void do_hit_test(int x, int y)
{
	if (y >= yres - TASK_H) {
		if (x >= sd_x() && x < sd_x() + SD_W) {
			toggle_show_desktop();
			return;
		}
		int bx = 8;
		for (int zi = 0; zi < zcount; zi++) {
			int i = zorder[zi];
			if (!wins[i].used)
				continue;
			int bw = 130;
			if (bx + bw > task_limit())
				break;
			if (x >= bx && x < bx + bw) {
				wins[i].minimized = 0;
				raise_window(i);
				focused = i;
				return;
			}
			bx += bw + 6;
		}
		return;
	}

	for (int zi = zcount - 1; zi >= 0; zi--) {
		int i = zorder[zi];
		if (!wins[i].used || wins[i].minimized)
			continue;
		struct window *w = &wins[i];
		if (x >= w->x && x < w->x + w->w && y >= w->y && y < w->y + w->h) {
			if (y < w->y + TITLE_H) {
				int closeX = w->x + w->w - 24;
				int maxX = closeX - 24;
				int minX = maxX - 24;
				if (x >= closeX) {
					close_window(i);
				} else if (x >= maxX) {
					raise_window(i);
					focused = i;
					toggle_maximize(i);
				} else if (x >= minX) {
					wins[i].minimized = 1;
				} else {
					raise_window(i);
					focused = i;
					drag_mode = 1;
					drag_win = i;
				}
				return;
			} else if (!w->maximized && x >= w->x + w->w - 10 && y >= w->y + w->h - 10) {
				raise_window(i);
				focused = i;
				drag_mode = 2;
				drag_win = i;
				return;
			} else {
				raise_window(i);
				focused = i;
				if (w->type == WIN_FILES) {
					fm_click(w, x, y);
				} else if (w->type == WIN_EDIT) {
					ed_click(w, x, y);
				} else if (w->type == WIN_SETTINGS) {
					int t = settings_click(w, x, y);
					if (t >= 0 && t < NUM_THEMES) {
						theme_idx = t;
					} else if (t == NUM_THEMES) {
						show_hidden = !show_hidden;
						/* Reload all open file windows and the desktop to show/hide .* files */
						for (int i = 0; i < MAX_WIN; i++)
							if (wins[i].used && wins[i].type == WIN_FILES && wins[i].fm)
								fm_load(&wins[i]);
						desk_scan();
					}
				} else if (w->type == WIN_TASKMGR) {
					int t = taskmgr_tab_at(w, x, y);
					if (t >= 0 && t != w->tab) {
						w->tab = t;
						taskmgr_refresh(w);
					}
				}
				return;
			}
		}
	}

	/* Press on an icon only *selects* it. It launches on release if the
	 * pointer never moved; otherwise the motion turns into a drag.
	 * Indices >= NUM_ICONS are desktop files: same press/drag/release
	 * state machine, but their position is fixed (grid-computed), not
	 * draggable, so a "drag" on one just cancels the open. */
	int idx = icon_at(x, y);
	if (idx >= 0) {
		icon_press = idx;
		icon_dragged = 0;
		int ix, iy;
		if (idx < NUM_ICONS) { ix = icons[idx].x; iy = icons[idx].y; }
		else desk_item_pos(idx, &ix, &iy);
		icon_grab_dx = x - ix;
		icon_grab_dy = y - iy;
	}
}

/* Hand the whole screen to an X client (the browser) and block until it quits.
 * X drives the same framebuffer we do, so the one thing we must not do while it
 * runs is keep drawing -- blocking here is what keeps the two off each other. */
static void run_x_app(const char *cmd)
{
	if (!cmd)
		return;
	/* The kernel refuses to switch away from a VT that is in KD_GRAPHICS, so
	 * leaving it set here hangs X forever inside VT_WAITACTIVE on its own vt. */
	if (confd >= 0)
		ioctl(confd, KDSETMODE, KD_TEXT);

	int rc = system(cmd);
	(void)rc;

	if (confd >= 0)
		ioctl(confd, KDSETMODE, KD_GRAPHICS);
	/* Keys pressed while X held the console are still queued on our stdin. */
	tcflush(STDIN_FILENO, TCIFLUSH);
}

static void launch_icon(int idx)
{
	struct icon *ic = &icons[idx];
	if (ic->action == 1) {
		run_and_show("echo Rebooting...");
		usleep(500000);
		system("reboot -f");
	} else if (ic->action == 2) {
		run_and_show("echo Powering off...");
		usleep(500000);
		system("poweroff -f");
	} else if (ic->action == 3) {
		spawn_terminal();
	} else if (ic->action == 7) {
		run_x_app(ic->cmd);
	} else if (ic->action == 4) {
		spawn_file_window();
	} else if (ic->action == 5) {
		spawn_taskmgr();
	} else if (ic->action == 6) {
		spawn_settings();
	}
}

/* Opening a desktop icon: a folder opens a File Manager rooted there, a
 * file opens the text editor -- same as double-clicking it in File Manager. */
static void launch_deskfile(int i)
{
	if (i < 0 || i >= desk_count)
		return;
	struct deskfile *df = &desk_files[i];
	char path[FM_FULLLEN];
	snprintf(path, sizeof(path), "%s/%s", DESKTOP_DIR, df->name);
	if (df->isdir) {
		int slot = spawn_file_window();
		if (slot >= 0) {
			snprintf(wins[slot].fm->cwd, sizeof(wins[slot].fm->cwd), "%s", path);
			fm_load(&wins[slot]);
		}
	} else {
		spawn_editor(path);
	}
}

/* Shared pointer handler: nx,ny = new absolute cursor position, left = button.
 * Works for both absolute (evdev tablet) and relative (PS/2 mouse) sources. */
static int process_pointer(int nx, int ny, int left, int right)
{
	if (nx < 0) nx = 0;
	if (ny < 0) ny = 0;
	if (nx >= xres) nx = xres - 1;
	if (ny >= yres) ny = yres - 1;
	int dx = nx - mx, dy = ny - my;
	mx = nx;
	my = ny;

	int changed = (dx || dy);
	if (left && drag_mode == 1 && drag_win >= 0 && wins[drag_win].used) {
		wins[drag_win].x += dx;
		wins[drag_win].y += dy;
		clamp_window(&wins[drag_win]);
		changed = 1;
	} else if (left && drag_mode == 2 && drag_win >= 0 && wins[drag_win].used) {
		wins[drag_win].w += dx;
		wins[drag_win].h += dy;
		if (wins[drag_win].w < WIN_MINW) wins[drag_win].w = WIN_MINW;
		if (wins[drag_win].h < WIN_MINH) wins[drag_win].h = WIN_MINH;
		resize_notify(&wins[drag_win]);
		changed = 1;
	} else if (left && prev_left && icon_press >= 0) {
		/* Past a few pixels of travel this is a drag, not a click.
		 * Desktop files (idx >= NUM_ICONS) have a fixed, grid-computed
		 * position -- dragging one just cancels the open, it doesn't move. */
		int ix, iy;
		if (icon_press < NUM_ICONS) { ix = icons[icon_press].x; iy = icons[icon_press].y; }
		else desk_item_pos(icon_press, &ix, &iy);
		if (!icon_dragged && (dx * dx + dy * dy) > 0) {
			int tx = mx - icon_grab_dx - ix;
			int ty = my - icon_grab_dy - iy;
			if (tx * tx + ty * ty > 9)
				icon_dragged = 1;
		}
		if (icon_dragged && icon_press < NUM_ICONS) {
			icons[icon_press].x = mx - icon_grab_dx;
			icons[icon_press].y = my - icon_grab_dy;
			clamp_icon(&icons[icon_press]);
			changed = 1;
		}
	} else if (left && prev_left && fmdrag_win >= 0 && !fmdrag_active) {
		/* Same drag-threshold pattern for a pressed file manager row. */
		if ((dx * dx + dy * dy) > 0) {
			int tx = mx - fmdrag_grab_x, ty = my - fmdrag_grab_y;
			if (tx * tx + ty * ty > 9)
				fmdrag_active = 1;
		}
		changed = 1;
	} else if (left && !prev_left) {
		/* An open context menu eats the next click: on it, run the item;
		 * off it, dismiss it and let the click through to the desktop. */
		if (ctxmenu_mode != CTXMODE_NONE) {
			int handled = ctxmenu_click(mx, my);
			ctxmenu_mode = CTXMODE_NONE;
			if (!handled)
				do_hit_test(mx, my);
		} else {
			do_hit_test(mx, my);
		}
		changed = 1;
	}

	if (!left && prev_left) {
		if (drag_mode == 2 && drag_win >= 0 && wins[drag_win].used)
			resize_notify(&wins[drag_win]);
		if (icon_press >= 0) {
			int idx = icon_press;
			int was_drag = icon_dragged;
			icon_press = -1;
			icon_dragged = 0;
			if (!was_drag) { /* a click that never moved */
				if (idx < NUM_ICONS)
					launch_icon(idx);
				else
					launch_deskfile(idx - NUM_ICONS);
			}
		}
		if (fmdrag_win >= 0) {
			if (fmdrag_active)
				fm_drop(mx, my);
			else if (fmdrag_was_preselected)
				fm_open_selected(fmdrag_win, fmdrag_entidx);
			fmdrag_win = -1;
			fmdrag_entidx = -1;
			fmdrag_active = 0;
		}
		changed = 1;
	}
	if (left != prev_left)
		changed = 1;
	if (!left) {
		drag_mode = 0;
		drag_win = -1;
	}
	prev_left = left;

	if (right && !prev_right) {
		ctxmenu_open(mx, my);
		changed = 1;
	}
	prev_right = right;

	return changed;
}

/* PS/2 relative fallback (real mouse / touchpad, no absolute device). */
static int handle_mouse_packet(unsigned char *pkt)
{
	int left = pkt[0] & 0x1;
	int right = pkt[0] & 0x2;
	int dx = pkt[1];
	int dy = pkt[2];
	if (pkt[0] & 0x10) dx -= 256;
	if (pkt[0] & 0x20) dy -= 256;
	dy = -dy;
	return process_pointer(mx + dx, my + dy, left, right);
}

/* Read all pending events from the absolute pointer; map to screen coords. */
static int read_abs_pointer(void)
{
	struct input_event ev;
	int changed = 0;
	while (read(absptr_fd, &ev, sizeof(ev)) == (int)sizeof(ev)) {
		if (ev.type == EV_ABS) {
			if (ev.code == ABS_X) abs_curx = ev.value;
			else if (ev.code == ABS_Y) abs_cury = ev.value;
		} else if (ev.type == EV_KEY) {
			if (ev.code == BTN_LEFT || ev.code == BTN_TOUCH)
				abs_btn = ev.value ? 1 : 0;
			else if (ev.code == BTN_RIGHT)
				abs_rbtn = ev.value ? 1 : 0;
		} else if (ev.type == EV_SYN && ev.code == SYN_REPORT) {
			int rx = abs_maxx - abs_minx; if (rx <= 0) rx = 1;
			int ry = abs_maxy - abs_miny; if (ry <= 0) ry = 1;
			int nx = (int)((long)(abs_curx - abs_minx) * (xres - 1) / rx);
			int ny = (int)((long)(abs_cury - abs_miny) * (yres - 1) / ry);
			if (process_pointer(nx, ny, abs_btn, abs_rbtn))
				changed = 1;
		}
	}
	return changed;
}

/* Read evdev keyboard just to catch Alt+Tab (keymap-independent). Text input
 * still flows through stdin. */
static int read_kbd_evdev(void)
{
	struct input_event ev;
	int changed = 0;
	while (read(kbd_evdev_fd, &ev, sizeof(ev)) == (int)sizeof(ev)) {
		if (ev.type != EV_KEY)
			continue;
		if (ev.code == KEY_LEFTALT || ev.code == KEY_RIGHTALT) {
			alt_held = (ev.value != 0);
		} else if (ev.code == KEY_TAB && ev.value == 1 && alt_held) {
			cycle_window_focus();
			changed = 1;
		}
	}
	return changed;
}

static void scan_input_devices(void)
{
	for (int i = 0; i < 32; i++) {
		char path[32];
		snprintf(path, sizeof(path), "/dev/input/event%d", i);
		int fd = open(path, O_RDONLY | O_NONBLOCK);
		if (fd < 0)
			continue;
		unsigned char evbit[(EV_MAX + 7) / 8] = {0};
		unsigned char absbit[(ABS_MAX + 7) / 8] = {0};
		unsigned char keybit[(KEY_MAX + 7) / 8] = {0};
		ioctl(fd, EVIOCGBIT(0, sizeof(evbit)), evbit);
		int is_abs = 0, is_kbd = 0;
		if (test_bit(EV_ABS, evbit)) {
			ioctl(fd, EVIOCGBIT(EV_ABS, sizeof(absbit)), absbit);
			if (test_bit(ABS_X, absbit) && test_bit(ABS_Y, absbit))
				is_abs = 1;
		}
		if (test_bit(EV_KEY, evbit)) {
			ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(keybit)), keybit);
			if (test_bit(KEY_A, keybit) && test_bit(KEY_ENTER, keybit))
				is_kbd = 1;
		}
		if (is_abs && absptr_fd < 0) {
			absptr_fd = fd;
			struct input_absinfo ai;
			if (ioctl(fd, EVIOCGABS(ABS_X), &ai) == 0) { abs_minx = ai.minimum; abs_maxx = ai.maximum; }
			if (ioctl(fd, EVIOCGABS(ABS_Y), &ai) == 0) { abs_miny = ai.minimum; abs_maxy = ai.maximum; }
			DBG("[input] abs pointer %s x[%d..%d] y[%d..%d]\n", path, abs_minx, abs_maxx, abs_miny, abs_maxy);
			continue;
		}
		if (is_kbd && kbd_evdev_fd < 0) {
			kbd_evdev_fd = fd;
			DBG("[input] keyboard %s\n", path);
			continue;
		}
		close(fd);
	}
}

/* USB tablets enumerate a few hundred ms after boot, often after we first run.
 * Retry briefly until an absolute pointer appears, then give up and let the
 * caller fall back to the relative PS/2 mouse.
 * ponytail: fixed ~2s cap; only real hardware with no tablet ever waits the
 * full time. Switch to a udev/inotify watch if that delay matters. */
static void open_input_devices(void)
{
	for (int attempt = 0; attempt < 20; attempt++) {
		scan_input_devices();
		if (absptr_fd >= 0)
			return;
		usleep(100000);
	}
}

static void setup_raw_stdin(void)
{
	if (tcgetattr(STDIN_FILENO, &orig_termios) == 0) {
		have_orig_termios = 1;
		struct termios raw = orig_termios;
		raw.c_lflag &= ~(ICANON | ECHO | ISIG);
		raw.c_iflag &= ~(IXON | ICRNL | BRKINT | INPCK | ISTRIP);
		raw.c_cc[VMIN] = 0;
		raw.c_cc[VTIME] = 0;
		tcsetattr(STDIN_FILENO, TCSANOW, &raw);
	}
}

static void restore_stdin(void)
{
	if (have_orig_termios)
		tcsetattr(STDIN_FILENO, TCSANOW, &orig_termios);
}

int main(void)
{
	int fbfd = open("/dev/fb0", O_RDWR);
	if (fbfd < 0) {
		perror("open /dev/fb0");
		return 1;
	}
	ioctl(fbfd, FBIOGET_VSCREENINFO, &vinfo);
	ioctl(fbfd, FBIOGET_FSCREENINFO, &finfo);
	xres = vinfo.xres;
	yres = vinfo.yres;
	bpp = vinfo.bits_per_pixel;
	line_length = finfo.line_length;
	long screensize = (long)line_length * yres;
	fbp = mmap(0, screensize, PROT_READ | PROT_WRITE, MAP_SHARED, fbfd, 0);
	if (fbp == MAP_FAILED) {
		perror("mmap fb");
		return 1;
	}
	backbuf = malloc(screensize);  /* NULL is fine: put_pixel falls back to fbp */

	dbg = fopen("/dev/ttyS0", "w");
	if (dbg)
		setvbuf(dbg, NULL, _IONBF, 0);

	confd = open("/dev/tty1", O_RDWR);
	if (confd >= 0) {
		struct console_font_op op;
		memset(&op, 0, sizeof(op));
		op.op = KD_FONT_OP_GET;
		op.width = 32;
		op.height = 32;
		op.charcount = 512;
		op.data = font;
		int r = ioctl(confd, KDFONTOP, &op);
		have_font = r == 0;
		if (have_font) {
			font_w = op.width;
			font_h = op.height;
			font_bpr = (font_w + 7) / 8;
		}
		ioctl(confd, KDSETMODE, KD_GRAPHICS);
	}
	setup_raw_stdin();
	DBG("[fbdesktop] xres=%d yres=%d bpp=%d have_font=%d font_w=%d font_h=%d\n",
		xres, yres, bpp, have_font, font_w, font_h);

	init_icon_positions(); /* needs xres/yres; without it every icon sits at 0,0 */
	mkdir(DESKTOP_DIR, 0755); /* ignore EEXIST -- it's fine if it's already there */
	desk_scan();

	open_input_devices();
	/* Only fall back to relative PS/2 mouse when no absolute tablet exists,
	 * otherwise mousedev would relay the tablet as relative and cause drift. */
	int mousefd = (absptr_fd < 0) ? open("/dev/input/mice", O_RDONLY) : -1;

	mx = xres / 2;
	my = yres / 2;

	signal(SIGCHLD, SIG_IGN);
	signal(SIGPIPE, SIG_IGN);

	redraw_all();

	for (;;) {
		struct pollfd fds[4 + MAX_WIN];
		int n = 0;
		int mouse_i = -1, abs_i = -1, kev_i = -1, kbd_i;
		if (mousefd >= 0) {
			mouse_i = n;
			fds[n].fd = mousefd;
			fds[n].events = POLLIN;
			n++;
		}
		if (absptr_fd >= 0) {
			abs_i = n;
			fds[n].fd = absptr_fd;
			fds[n].events = POLLIN;
			n++;
		}
		if (kbd_evdev_fd >= 0) {
			kev_i = n;
			fds[n].fd = kbd_evdev_fd;
			fds[n].events = POLLIN;
			n++;
		}
		kbd_i = n;
		fds[n].fd = STDIN_FILENO;
		fds[n].events = POLLIN;
		n++;
		int win_i[MAX_WIN];
		for (int i = 0; i < MAX_WIN; i++) {
			win_i[i] = -1;
			if (wins[i].used && wins[i].type == WIN_TERM) {
				win_i[i] = n;
				fds[n].fd = wins[i].pty_fd;
				fds[n].events = POLLIN;
				n++;
			}
		}

		/* Wake at least once a second so the taskbar clock ticks and any
		 * open Task Manager tab refreshes without needing input. */
		int pr = poll(fds, n, 1000);
		if (pr == 0) {
			sample_stats();
			for (int i = 0; i < MAX_WIN; i++)
				if (wins[i].used && wins[i].type == WIN_TASKMGR && !wins[i].minimized)
					taskmgr_refresh(&wins[i]);
			/* Pick up files added/removed in DESKTOP_DIR from outside this
			 * app (a terminal, another program) -- skip mid-interaction so
			 * indices an active press/menu is holding don't shift under it. */
			if (icon_press < 0 && ctxmenu_mode != CTXMODE_DESKTOP)
				desk_scan();
			redraw_all();
			continue;
		}
		if (pr < 0) {
			if (errno == EINTR)
				continue;
			break;
		}

		int need_redraw = 0;

		if (mouse_i >= 0 && (fds[mouse_i].revents & POLLIN)) {
			unsigned char pkt[3];
			if (read(mousefd, pkt, 3) == 3) {
				if (handle_mouse_packet(pkt))
					need_redraw = 1;
			}
		}
		if (abs_i >= 0 && (fds[abs_i].revents & POLLIN)) {
			if (read_abs_pointer())
				need_redraw = 1;
		}
		if (kev_i >= 0 && (fds[kev_i].revents & POLLIN)) {
			if (read_kbd_evdev())
				need_redraw = 1;
		}
		if (fds[kbd_i].revents & POLLIN) {
			char buf[64];
			int r = read(STDIN_FILENO, buf, sizeof(buf));
			/* Swallow keystrokes while Alt is held so Alt+Tab's ESC/Tab bytes
			 * don't leak into the focused window. */
			if (r > 0 && !alt_held && focused >= 0 && wins[focused].used) {
				if (wins[focused].type == WIN_TERM)
					write(wins[focused].pty_fd, buf, r);
				else if (wins[focused].type == WIN_EDIT &&
					 ed_keys(&wins[focused], buf, r))
					need_redraw = 1;
				else if (wins[focused].type == WIN_FILES &&
					 fm_keys(&wins[focused], buf, r))
					need_redraw = 1;
			}
		}
		for (int i = 0; i < MAX_WIN; i++) {
			if (win_i[i] >= 0 && (fds[win_i[i]].revents & (POLLIN | POLLHUP))) {
				char buf[1024];
				int r = read(wins[i].pty_fd, buf, sizeof(buf));
				if (r > 0) {
					process_bytes(&wins[i], (unsigned char *)buf, r);
					need_redraw = 1;
				} else {
					close_window(i);
					need_redraw = 1;
				}
			}
		}

		if (need_redraw)
			redraw_all();
	}

	restore_stdin();
	if (confd >= 0)
		ioctl(confd, KDSETMODE, KD_TEXT);
	return 0;
}
