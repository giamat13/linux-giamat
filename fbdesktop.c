/* Minimal framebuffer desktop: every icon opens a draggable/resizable window
 * (a real pty-backed VT100-ish terminal, or a one-shot command-output view),
 * plus a taskbar. No X11, no browser -- draws directly to /dev/fb0, reads
 * mouse from /dev/input/mice and keyboard from stdin in raw mode.
 * Font is pulled live from the kernel's own VT console font (KDFONTOP). */
#define _XOPEN_SOURCE 700
#define _DEFAULT_SOURCE
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
	G_GAUGE, G_FOLDER, G_TERM, G_REFRESH, G_POWER, G_GEAR
};

struct icon {
	const char *label;
	const char *cmd;
	uint32_t color;
	int action; /* 1=reboot,2=poweroff,3=terminal,4=files,5=task manager,6=settings */
	int glyph;
	int x, y;   /* free position on the desktop -- icons are draggable */
};

static struct icon icons[] = {
	{"TASK MGR",  NULL, 0x3b82f6, 5, G_GAUGE},
	{"FILES",     NULL, 0x06b6d4, 4, G_FOLDER},
	{"TERMINAL",  NULL, 0x9333ea, 3, G_TERM},
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
};
#define NUM_THEMES (int)(sizeof(themes)/sizeof(themes[0]))
static int theme_idx;

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

struct fent {
	char name[FM_NAMELEN];
	int isdir;
	int isreg;
	long size;
};

struct fmstate {
	char cwd[FM_PATHLEN];
	struct fent ents[FM_MAXENT];
	int count;
	int scroll;
};

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
static int mx, my, prev_left;
/* absolute pointer (evdev tablet) + evdev keyboard for Alt+Tab */
static int absptr_fd = -1, kbd_evdev_fd = -1;
static int abs_minx, abs_maxx, abs_miny, abs_maxy;
static int abs_curx, abs_cury, abs_btn;
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
}

static int icon_at(int px, int py)
{
	for (int i = NUM_ICONS - 1; i >= 0; i--) {
		struct icon *ic = &icons[i];
		if (px >= ic->x && px < ic->x + ICON_W &&
		    py >= ic->y && py < ic->y + ICON_H)
			return i;
	}
	return -1;
}

/* ---- character-grid terminal model, shared by live terminals and
 * one-shot command-output windows ---- */

static void update_grid_dims(struct window *w)
{
	int content_h = w->h - TITLE_H;
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

static void fm_render(struct window *w)
{
	struct fmstate *fm = w->fm;
	int maxscroll = fm->count - w->rows;
	if (maxscroll < 0) maxscroll = 0;
	if (fm->scroll > maxscroll) fm->scroll = maxscroll;
	if (fm->scroll < 0) fm->scroll = 0;

	for (int r = 0; r < w->rows; r++)
		clear_row_range(w, r, 0, w->cols - 1);

	for (int r = 0; r < w->rows; r++) {
		int i = fm->scroll + r;
		if (i >= fm->count)
			break;
		struct fent *e = &fm->ents[i];
		char line[FM_NAMELEN + 16];
		snprintf(line, sizeof(line), "%s %s", e->isdir ? "[DIR]" : "     ", e->name);
		fm_puts(w, r, 0, line, e->isdir ? 0x89b4fa : COL_FG_DEFAULT);
		if (e->isreg) {
			char sz[24];
			snprintf(sz, sizeof(sz), "%ld", e->size);
			int col = w->cols - (int)strlen(sz) - 1;
			if (col > (int)strlen(line) + 1)
				fm_puts(w, r, col, sz, 0x6c7086);
		}
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
				e->size = (long)st.st_size;
			} else {
				e->isdir = e->isreg = 0;
				e->size = 0;
			}
			fm->count++;
		}
		closedir(d);
		/* sort everything after the ".." entry, which must stay first */
		qsort(fm->ents + start, fm->count - start, sizeof(struct fent), fent_cmp);
	}
	snprintf(w->title, sizeof(w->title), "%s", fm->cwd);
	fm_render(w);
}

static void fm_click(struct window *w, int y)
{
	struct fmstate *fm = w->fm;
	int row = (y - (w->y + TITLE_H)) / font_h;
	if (row < 0 || row >= w->rows)
		return;
	int i = fm->scroll + row;
	if (i < 0 || i >= fm->count)
		return;
	struct fent *e = &fm->ents[i];

	if (!strcmp(e->name, "..")) {
		char *slash = strrchr(fm->cwd, '/');
		if (slash && slash != fm->cwd)
			*slash = 0;
		else
			strcpy(fm->cwd, "/");
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
		fm_load(w);
	} else if (e->isreg) {
		/* Regular files only: opening a fifo or char device would block forever. */
		spawn_editor(path);
	}
}

/* Arrow / PageUp / PageDown scroll the listing. */
static int fm_keys(struct window *w, const char *buf, int n)
{
	struct fmstate *fm = w->fm;
	int changed = 0;
	for (int i = 0; i + 2 < n; i++) {
		if (buf[i] != 0x1b || buf[i + 1] != '[')
			continue;
		int delta = 0;
		switch (buf[i + 2]) {
		case 'A': delta = -1; break;
		case 'B': delta = 1; break;
		case '5': delta = -(w->rows - 1); break;
		case '6': delta = w->rows - 1; break;
		default: break;
		}
		if (delta) {
			fm->scroll += delta;
			fm_render(w); /* clamps scroll */
			changed = 1;
		}
		i += 2;
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
		int bx = w->x + 16 + t * (SET_BTNW + 10);
		int by = content_y + 38;
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

	char info[256];
	snprintf(info, sizeof(info),
		 "Display\n  %dx%d  %d bpp\n  font %dx%d (kernel VT)\n"
		 "\nMode is fixed by GRUB gfxpayload at boot.",
		 xres, yres, bpp, font_w, font_h);
	draw_text(w->x + 16, content_y + 90, info, 0x9399b2);
}

/* Theme button hit-test; returns the clicked theme or -1. */
static int settings_click(struct window *w, int px, int py)
{
	int by = w->y + TITLE_H + 38;
	if (py < by || py >= by + SET_BTNH)
		return -1;
	for (int t = 0; t < NUM_THEMES; t++) {
		int bx = w->x + 16 + t * (SET_BTNW + 10);
		if (px >= bx && px < bx + SET_BTNW)
			return t;
	}
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
 * Sits at the right end of the taskbar, just left of the clock. */
#define SD_W 34

static int sd_x(void)
{
	int clockw = 8 * font_w; /* "hh:mm:ss" */
	return xres - clockw - 26 - SD_W - 10;
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
		if (bx + bw > sx - 8)
			break; /* out of room before the show-desktop button */
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
	fill_rect(xres - tw - 26, ty + 9, 1, TASK_H - 18, 0x3a3a4d);
	draw_text(xres - tw - 14, ty + (TASK_H - font_h) / 2, timebuf, 0xcdd6f4);
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
			if (bx + bw > sd_x() - 8)
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
					fm_click(w, y);
				} else if (w->type == WIN_EDIT) {
					ed_click(w, x, y);
				} else if (w->type == WIN_SETTINGS) {
					int t = settings_click(w, x, y);
					if (t >= 0)
						theme_idx = t;
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
	 * pointer never moved; otherwise the motion turns into a drag. */
	int idx = icon_at(x, y);
	if (idx >= 0) {
		icon_press = idx;
		icon_dragged = 0;
		icon_grab_dx = x - icons[idx].x;
		icon_grab_dy = y - icons[idx].y;
	}
}

static void launch_icon(int idx)
{
	struct icon *ic = &icons[idx];
	if (ic->action == 1) {
		run_and_show("echo Rebooting...");
		usleep(500000);
		system("reboot");
	} else if (ic->action == 2) {
		run_and_show("echo Powering off...");
		usleep(500000);
		system("poweroff -f");
	} else if (ic->action == 3) {
		spawn_terminal();
	} else if (ic->action == 4) {
		spawn_file_window();
	} else if (ic->action == 5) {
		spawn_taskmgr();
	} else if (ic->action == 6) {
		spawn_settings();
	}
}

/* Shared pointer handler: nx,ny = new absolute cursor position, left = button.
 * Works for both absolute (evdev tablet) and relative (PS/2 mouse) sources. */
static int process_pointer(int nx, int ny, int left)
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
		/* Past a few pixels of travel this is a drag, not a click. */
		if (!icon_dragged && (dx * dx + dy * dy) > 0) {
			int tx = mx - icon_grab_dx - icons[icon_press].x;
			int ty = my - icon_grab_dy - icons[icon_press].y;
			if (tx * tx + ty * ty > 9)
				icon_dragged = 1;
		}
		if (icon_dragged) {
			icons[icon_press].x = mx - icon_grab_dx;
			icons[icon_press].y = my - icon_grab_dy;
			clamp_icon(&icons[icon_press]);
			changed = 1;
		}
	} else if (left && !prev_left) {
		do_hit_test(mx, my);
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
			if (!was_drag)
				launch_icon(idx); /* a click that never moved */
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
	return changed;
}

/* PS/2 relative fallback (real mouse / touchpad, no absolute device). */
static int handle_mouse_packet(unsigned char *pkt)
{
	int left = pkt[0] & 0x1;
	int dx = pkt[1];
	int dy = pkt[2];
	if (pkt[0] & 0x10) dx -= 256;
	if (pkt[0] & 0x20) dy -= 256;
	dy = -dy;
	return process_pointer(mx + dx, my + dy, left);
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
		} else if (ev.type == EV_SYN && ev.code == SYN_REPORT) {
			int rx = abs_maxx - abs_minx; if (rx <= 0) rx = 1;
			int ry = abs_maxy - abs_miny; if (ry <= 0) ry = 1;
			int nx = (int)((long)(abs_curx - abs_minx) * (xres - 1) / rx);
			int ny = (int)((long)(abs_cury - abs_miny) * (yres - 1) / ry);
			if (process_pointer(nx, ny, abs_btn))
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

	int confd = open("/dev/tty1", O_RDWR);
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
