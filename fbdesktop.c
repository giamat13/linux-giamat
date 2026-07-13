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
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <linux/fb.h>
#include <linux/kd.h>
#include <linux/input.h>

#define test_bit(nr, arr) (((arr)[(nr) / 8] >> ((nr) % 8)) & 1)

#define ICON_W 140
#define ICON_H 90
#define ICON_GAP 30
#define TITLE_H 24
#define TASK_H 32
#define MAX_WIN 8
#define WIN_MINW 240
#define WIN_MINH 150
#define GRID_MAXCOLS 220
#define GRID_MAXROWS 110
#define COL_FG_DEFAULT 0xcdd6f4
#define COL_BG_DEFAULT 0x1e1e2e

enum wintype { WIN_TERM, WIN_OUTPUT };

struct icon {
	const char *label;
	const char *cmd;
	uint32_t color;
	int action; /* 0=open output window, 1=reboot, 2=poweroff, 3=spawn terminal */
};

static struct icon icons[] = {
	{"SYSTEM",    "uname -a; echo; cat /proc/uptime; echo; cat /proc/version", 0x3b82f6, 0},
	{"PROCESSES", "ps aux", 0x22c55e, 0},
	{"DISK",      "df -h", 0xeab308, 0},
	{"MEMORY",    "free -m", 0xf97316, 0},
	{"DMESG",     "dmesg | tail -40", 0x14b8a6, 0},
	{"TERMINAL",  NULL, 0x9333ea, 3},
	{"REBOOT",    NULL, 0xef4444, 1},
	{"POWER OFF", NULL, 0xf43f5e, 2},
};
#define NUM_ICONS (int)(sizeof(icons)/sizeof(icons[0]))

struct window {
	int used;
	enum wintype type;
	int x, y, w, h;
	int rx, ry, rw, rh; /* saved geometry for un-maximize */
	int minimized, maximized;
	char title[32];

	int pty_fd;
	pid_t pid;

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

static int icon_grid_xy(int i, int *ox, int *oy)
{
	int cols = (xres - ICON_GAP) / (ICON_W + ICON_GAP);
	if (cols < 1)
		cols = 1;
	int col = i % cols, row = i / cols;
	*ox = ICON_GAP + col * (ICON_W + ICON_GAP);
	*oy = ICON_GAP + row * (ICON_H + ICON_GAP);
	return cols;
}

static void draw_icons(void)
{
	for (int i = 0; i < NUM_ICONS; i++) {
		int x, y;
		icon_grid_xy(i, &x, &y);
		fill_rect(x, y, ICON_W, ICON_H, icons[i].color);
		draw_text(x + 8, y + ICON_H / 2 - font_h / 2, icons[i].label, 0xffffff);
	}
}

static int icon_at(int px, int py)
{
	for (int i = 0; i < NUM_ICONS; i++) {
		int x, y;
		icon_grid_xy(i, &x, &y);
		if (px >= x && px < x + ICON_W && py >= y && py < y + ICON_H)
			return i;
	}
	return -1;
}

/* ---- character-grid terminal model, shared by live terminals and
 * one-shot command-output windows ---- */

static void update_grid_dims(struct window *w)
{
	int content_h = w->h - TITLE_H;
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

static void resize_notify(struct window *w)
{
	update_grid_dims(w);
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

static int spawn_output_window(const char *title, const char *cmd)
{
	int slot = alloc_window_slot();
	if (slot < 0)
		return -1;
	memset(&wins[slot], 0, sizeof(wins[slot]));
	wins[slot].used = 1;
	wins[slot].type = WIN_OUTPUT;
	wins[slot].pty_fd = -1;
	wins[slot].x = 220 + slot * 24;
	wins[slot].y = 140 + slot * 24;
	wins[slot].w = 560;
	wins[slot].h = 380;
	wins[slot].attr_fg = COL_FG_DEFAULT;
	wins[slot].attr_bg = COL_BG_DEFAULT;
	snprintf(wins[slot].title, sizeof(wins[slot].title), "%s", title);
	update_grid_dims(&wins[slot]);
	struct window *w = &wins[slot];
	for (int r = 0; r < w->rows; r++)
		clear_row_range(w, r, 0, w->cols - 1);

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
			putch_grid(w, c);
		}
		pclose(p);
	}
	w->cur_row = 0;
	w->cur_col = 0;
	zorder[zcount++] = slot;
	focused = slot;
	return slot;
}

static void draw_window(struct window *w)
{
	fill_rect(w->x, w->y, w->w, TITLE_H, 0x313244);
	draw_text_clip(w->x + 6, w->y + 4, w->title, 0xffffff, w->w - 80);

	int bx = w->x + w->w - 24;
	fill_rect(bx, w->y, 24, TITLE_H, 0xef4444);
	draw_text(bx + 8, w->y + 4, "X", 0xffffff);
	bx -= 24;
	fill_rect(bx, w->y, 24, TITLE_H, 0x45475a);
	draw_text(bx + 8, w->y + 4, "^", 0xffffff);
	bx -= 24;
	fill_rect(bx, w->y, 24, TITLE_H, 0x45475a);
	draw_text(bx + 8, w->y + 4, "_", 0xffffff);

	int content_y = w->y + TITLE_H, content_h = w->h - TITLE_H;
	if (content_h < 0)
		content_h = 0;
	fill_rect(w->x, content_y, w->w, content_h, COL_BG_DEFAULT);

	for (int r = 0; r < w->rows; r++) {
		int cy = content_y + r * font_h;
		for (int c = 0; c < w->cols; c++) {
			uint32_t bg = w->gbg[r][c];
			if (bg != COL_BG_DEFAULT)
				fill_rect(w->x + 4 + c * font_w, cy, font_w, font_h, bg);
		}
	}
	for (int r = 0; r < w->rows; r++) {
		int cy = content_y + r * font_h;
		for (int c = 0; c < w->cols; c++) {
			unsigned char ch = w->gch[r][c];
			if (ch && ch != ' ')
				blit_char(w->x + 4 + c * font_w, cy, ch, w->gfg[r][c]);
		}
	}
	if (w->type == WIN_TERM) {
		int cx = w->x + 4 + w->cur_col * font_w;
		int cy = content_y + w->cur_row * font_h + font_h - 2;
		fill_rect(cx, cy, font_w, 2, 0xf9e2af);
	}

	if (!w->maximized)
		fill_rect(w->x + w->w - 10, w->y + w->h - 10, 10, 10, 0x585b70);
}

static void draw_taskbar(void)
{
	fill_rect(0, yres - TASK_H, xres, TASK_H, 0x11111b);
	int bx = 4;
	for (int zi = 0; zi < zcount; zi++) {
		int i = zorder[zi];
		if (!wins[i].used)
			continue;
		int bw = 130;
		uint32_t bg = (i == focused && !wins[i].minimized) ? 0x45475a : 0x313244;
		fill_rect(bx, yres - TASK_H + 4, bw, TASK_H - 8, bg);
		draw_text_clip(bx + 6, yres - TASK_H + 4 + (TASK_H - 8 - font_h) / 2, wins[i].title, 0xffffff, bw - 12);
		bx += bw + 6;
	}
	char timebuf[16];
	time_t t = time(NULL);
	struct tm *tm = localtime(&t);
	snprintf(timebuf, sizeof(timebuf), "%02d:%02d:%02d", tm->tm_hour, tm->tm_min, tm->tm_sec);
	draw_text_clip(xres - 80, yres - TASK_H + 4 + (TASK_H - 8 - font_h) / 2, timebuf, 0xffffff, 70);
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
	fill_rect(0, 0, xres, yres - TASK_H, 0x181825);
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
		int bx = 4;
		for (int zi = 0; zi < zcount; zi++) {
			int i = zorder[zi];
			if (!wins[i].used)
				continue;
			int bw = 130;
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
				return;
			}
		}
	}

	int idx = icon_at(x, y);
	if (idx >= 0) {
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
		} else {
			spawn_output_window(ic->label, ic->cmd);
		}
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
	} else if (left && !prev_left) {
		do_hit_test(mx, my);
		changed = 1;
	}
	if (!left && prev_left) {
		if (drag_mode == 2 && drag_win >= 0 && wins[drag_win].used)
			resize_notify(&wins[drag_win]);
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

		int pr = poll(fds, n, -1);
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
			 * don't leak into the focused shell. */
			if (r > 0 && !alt_held && focused >= 0 && wins[focused].used &&
			    wins[focused].type == WIN_TERM)
				write(wins[focused].pty_fd, buf, r);
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
