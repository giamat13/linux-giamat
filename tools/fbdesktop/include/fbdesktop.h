/* Minimal framebuffer desktop: every icon opens a draggable/resizable window
 * (a real pty-backed VT100-ish terminal, a one-shot command-output view, or a
 * browser), plus a taskbar. Draws directly to /dev/fb0, reads mouse from an
 * evdev absolute pointer (or /dev/input/mice) and keyboard from stdin in raw
 * mode. Font is pulled live from the kernel's own VT console font (KDFONTOP).
 *
 * The browser window is the one thing we do not draw ourselves: Firefox runs on
 * a headless Xvfb, and we act as its screen and its input device -- pulling the
 * X root image over shared memory each frame and blitting it into an ordinary
 * window, then feeding clicks and keys back with XTEST. That is what lets it
 * minimise, maximise, close and sit in the taskbar like everything else.
 *
 * Layout of the sources (all link into one binary, see the Makefile):
 *   globals.c    shared state -- everything declared extern below
 *   draw.c       framebuffer primitives: pixels, rects, text, vector glyphs
 *   window.c     window records: z-order, focus, move/resize, frame drawing
 *   term.c       the character grid and the VT100-ish parser behind it
 *   desktop.c    icons, the desktop directory, taskbar, context menu, compositor
 *   files.c      file manager window + clipboard + file operations
 *   editor.c     text editor window
 *   taskmgr.c    task manager window + the CPU/memory sampler and graphs
 *   settings.c   settings window
 *   browser.c    Xvfb + Firefox, MIT-SHM capture, XTEST input
 *   main.c       framebuffer/input/font setup, hit testing, the event loop
 */
#ifndef FBDESKTOP_H
#define FBDESKTOP_H

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
#include <sys/ipc.h>
#include <sys/shm.h>
#include <linux/fb.h>
#include <linux/kd.h>
#include <linux/input.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/XKBlib.h>
#include <X11/keysym.h>
#include <X11/extensions/XShm.h>
#include <X11/extensions/XTest.h>

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

enum wintype { WIN_TERM, WIN_OUTPUT, WIN_FILES, WIN_TASKMGR, WIN_EDIT, WIN_SETTINGS, WIN_BROWSER };

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

struct icon {
	const char *label;
	const char *cmd;
	uint32_t color;
	int action; /* 1=reboot,2=poweroff,3=terminal,4=files,5=task manager,6=settings,7=X app */
	int glyph;
	int x, y;   /* free position on the desktop -- icons are draggable */
};
#define NUM_ICONS 7

/* Desktop themes -- the only setting that has any effect at runtime; the
 * framebuffer mode itself is fixed by GRUB's gfxpayload at boot. */
struct theme {
	const char *name;
	uint32_t dtop, dbot, accent;
};
#define NUM_THEMES 5

/* One-second samples of CPU / memory use, oldest first. */
#define HIST 60

/* Task Manager sidebar views. */
struct tmtab {
	const char *label;
	const char *cmd;
};
#define TM_NTABS 4

/* File manager state, allocated only for WIN_FILES windows. */
#define FM_MAXENT 512
#define FM_NAMELEN 96
#define FM_PATHLEN 512
/* cwd + '/' + name + NUL: any child path is guaranteed to fit */
#define FM_FULLLEN (FM_PATHLEN + FM_NAMELEN + 2)

/* Right-click context menu: single global instance, opened either on a
 * FILES window's row (CTXMODE_FILEWIN) or on the desktop background
 * (CTXMODE_DESKTOP). */
enum { CTXMODE_NONE, CTXMODE_FILEWIN, CTXMODE_DESKTOP };
#define CTX_W 130
#define CTX_ITEMH 24
#define CTX_NITEMS 5

/* The desktop is backed by a real directory, same as Windows: files there are
 * drawn as desktop icons after the fixed app icons, and dropping a file
 * manager row onto the desktop copies it in here. */
#define DESKTOP_DIR "/root/Desktop"
#define DESK_MAXFILES 64
struct deskfile {
	char name[FM_NAMELEN];
	int isdir;
	int isexec;
};

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
#define FM_BTNW 96
/* listing: a column-header strip, then icon rows font_h+FM_ROWPAD tall */
#define FM_HEADH 22
#define FM_ROWPAD 10

/* "Show desktop" button: minimize everything, click again to bring it all
 * back. Sits at the far right of the taskbar, to the right of the clock. */
#define SD_W 34

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

/* ---- shared state (defined in globals.c) ---------------------------- */

extern struct icon icons[NUM_ICONS];
extern const struct theme themes[NUM_THEMES];
extern const struct tmtab tm_tabs[TM_NTABS];
extern const char *ctx_items[CTX_NITEMS];
extern const char *fm_btns[FM_NBTN];

extern int theme_idx;
extern int show_hidden;         /* show .* files in file manager */
extern int dblclick_delay;      /* ms: drag icon/row vs. selection threshold */

extern int cpu_hist[HIST], mem_hist[HIST];

/* icon drag state: press selects, movement past a threshold turns it into a
 * drag, release without movement launches. */
extern int icon_press;
extern int icon_dragged;
extern int icon_grab_dx, icon_grab_dy;

extern int ctxmenu_mode;
extern int ctxmenu_win;      /* FILEWIN: owning window */
extern int ctxmenu_entidx;   /* FILEWIN: index into that window's fm->ents */
extern int ctxmenu_deskidx;  /* DESKTOP: index into desk_files[], -1 = empty area */
extern int ctxmenu_x, ctxmenu_y;

/* Clipboard: one path at a time. mode: 0 none, 1 copy, 2 cut. */
extern char clip_path[FM_FULLLEN];
extern int clip_mode;

extern struct deskfile desk_files[DESK_MAXFILES];
extern int desk_count;

/* File-row drag (file manager listing): press arms a candidate; movement past
 * a threshold turns it into a drag; release either drops it (moves into a
 * folder row, copies onto the desktop) or, if it never moved, behaves like a
 * normal click (select, or open if already selected). */
extern int fmdrag_win;
extern int fmdrag_entidx;
extern int fmdrag_active;
extern int fmdrag_was_preselected;
extern int fmdrag_grab_x, fmdrag_grab_y;

extern struct window wins[MAX_WIN];
extern int zorder[MAX_WIN], zcount;
extern int focused;
extern int drag_mode; /* 0 none, 1 move, 2 resize */
extern int drag_win;
extern int alt_held;
extern int sd_active;      /* "show desktop" is on: everything was minimized */
extern int sd_saved[MAX_WIN];

extern uint8_t *fbp;      /* real framebuffer */
extern uint8_t *backbuf;  /* offscreen: draw here, then flush in one memcpy */
extern struct fb_var_screeninfo vinfo;
extern struct fb_fix_screeninfo finfo;
extern int xres, yres, bpp, line_length;
extern unsigned char font[512 * 32 * 4];
extern int have_font;
extern int font_w, font_h, font_bpr;
extern int mx, my, prev_left, prev_right;
/* /dev/tty1, kept open so the console mode can be handed back and forth with X */
extern int confd;
/* absolute pointer (evdev tablet) + evdev keyboard for Alt+Tab */
extern int absptr_fd, kbd_evdev_fd;
extern int abs_minx, abs_maxx, abs_miny, abs_maxy;
extern int abs_curx, abs_cury, abs_btn, abs_rbtn;
extern FILE *dbg;
extern struct termios orig_termios;
extern int have_orig_termios;

#define DBG(...) do { if (dbg) fprintf(dbg, __VA_ARGS__); } while (0)

/* ---- cross-module functions ----------------------------------------- */
/* draw.c */
void blit_char(int x, int y, unsigned char c, uint32_t fg);
void draw_cursor(int x, int y);
void draw_glyph(int g, int cx, int cy, uint32_t fg, uint32_t hole);
void draw_text(int x, int y, const char *s, uint32_t fg);
void draw_text_clip(int x, int y, const char *s, uint32_t fg, int maxw);
void fill_circle(int cx, int cy, int r, uint32_t col);
void fill_rect(int x, int y, int w, int h, uint32_t color);
void fill_round_rect(int x, int y, int w, int h, int r, uint32_t col);
void fill_round_rect_grad(int x, int y, int w, int h, int r,
			  uint32_t top, uint32_t bot);
void fill_vgradient(int x, int y, int w, int h, uint32_t top, uint32_t bot);
uint32_t mix(uint32_t a, uint32_t b, int t);
void put_pixel(int x, int y, uint32_t color);

/* desktop.c */
void clamp_icon(struct icon *ic);
enum fcat classify_file(const char *name, int isdir, int isexec);
void cycle_window_focus(void);
void desk_item_pos(int idx, int *ox, int *oy);
void desk_scan(void);
uint32_t fcat_color(enum fcat c);
const char *fcat_tag(enum fcat c);
int icon_at(int px, int py);
void init_icon_positions(void);
void redraw_all(void);
int sd_x(void);
int task_limit(void);
void toggle_show_desktop(void);

/* term.c */
void clear_row_range(struct window *w, int row, int from, int to);
void fill_grid_from_cmd(struct window *w, const char *cmd);
void process_bytes(struct window *w, unsigned char *buf, int n);
void resize_notify(struct window *w);
void run_and_show(const char *cmd);
int spawn_terminal(void);
void update_grid_dims(struct window *w);

/* window.c */
int alloc_window_slot(void);
void clamp_window(struct window *w);
void close_window(int i);
void draw_window(struct window *w);
void raise_window(int i);
void toggle_maximize(int i);
uint32_t win_accent(const struct window *w);

/* browser.c */
extern int browser_win;   /* window slot showing Firefox, -1 = not running */
void browser_keys(const char *buf, int n);
void browser_pointer(int lx, int ly, int left, int right);
void browser_teardown(void);
void draw_browser(struct window *w);
int spawn_browser(void);

/* taskmgr.c */
void draw_graph(int x, int y, int w, int h, const int *hist,
		uint32_t col, const char *label);
void draw_taskmgr(struct window *w, int content_y, int content_h);
void sample_stats(void);
int spawn_taskmgr(void);
void taskmgr_refresh(struct window *w);
void taskmgr_click(struct window *w, int px, int py);

/* files.c */
int ctxmenu_click(int x, int y);
void ctxmenu_open(int x, int y);
void draw_files(struct window *w, int content_y, int content_h);
void fm_click(struct window *w, int x, int y);
void fm_drop(int x, int y);
int fm_keys(struct window *w, const char *buf, int n);
void fm_load(struct window *w);
void fm_open_selected(int winidx, int entidx);
void fm_render(struct window *w);
int spawn_file_window(void);

/* editor.c */
void ed_click(struct window *w, int x, int y);
int ed_keys(struct window *w, const char *buf, int n);
void ed_render(struct window *w);
int spawn_editor(const char *path);

/* settings.c */
void draw_settings(struct window *w, int content_y);
int settings_click(struct window *w, int px, int py);
int spawn_settings(void);

/* main.c */
int process_pointer(int nx, int ny, int left, int right);

#endif /* FBDESKTOP_H */
