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

enum wintype { WIN_TERM, WIN_OUTPUT, WIN_FILES, WIN_TASKMGR, WIN_EDIT, WIN_SETTINGS,
	       WIN_BROWSER, WIN_CALC, WIN_PAINT, WIN_CAL, WIN_TIMER, WIN_SEARCH,
	       WIN_IMGVIEW, WIN_ARCHIVE, WIN_SHOT };

enum glyph {
	G_GAUGE, G_FOLDER, G_TERM, G_REFRESH, G_POWER, G_GEAR, G_FILE,
	G_IMAGE, G_ARCHIVE, G_CODE, G_EXEC, G_GLOBE, G_CALC, G_PAINT, G_CAL,
	G_TIMER, G_SEARCH, G_DISK, G_SNAP
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
	int action; /* 1=reboot,2=poweroff,3=terminal,4=files,5=task manager,
		     * 6=settings,7=X app,8=calculator,9=paint,10=calendar,11=timer,
		     * 12=search,14=image viewer,15=archive manager,16=screenshot
		     * (13 was disk usage; merged into the task manager's Disk tab) */
	int glyph;
	int x, y;   /* free position on the desktop -- icons are draggable */
};
#define NUM_ICONS 15

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
#define CTX_NITEMS 6

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

/* Start Menu button + popup, far left of the taskbar. */
#define START_W  44
#define SM_W     200
#define SM_ROWH  26

/* Text editor state, allocated only for WIN_EDIT windows. */
#define ED_MAXLINES 1024
#define ED_MAXCOL 240
/* toolbar (Save/Undo/Redo) above the text, status bar (Ln/Col) below it */
#define ED_TOOLH 28
#define ED_STATH 22
#define ED_NBTN 3
#define ED_BTNW 76
#define ED_UNDO 32

/* One undo/redo step: a copy of the whole buffer plus the caret it had.
 * ponytail: full-buffer snapshots (lines are malloc'd to the used size, so a
 * small file costs little). Move to per-op deltas only if big files hurt. */
struct edsnap {
	char (*line)[ED_MAXCOL];
	int nlines, cy, cx;
};

/* Consecutive same-kind edits on one line coalesce into a single undo step --
 * without it a 32-deep stack would only reach back 32 keystrokes. */
enum edop { OP_NONE, OP_INSERT, OP_DELETE, OP_OTHER };

struct edstate {
	char path[FM_FULLLEN];
	char line[ED_MAXLINES][ED_MAXCOL];
	int nlines;
	int cy, cx;   /* caret in buffer coords */
	int scroll;
	int dirty;
	int truncated; /* file didn't fit: refuse to save over it */
	char status[64];

	struct edsnap undo[ED_UNDO], redo[ED_UNDO];
	int nundo, nredo;
	int last_op, last_cy; /* coalescing state, see enum edop */
};

/* Calculator state, allocated only for WIN_CALC windows. An immediate-execution
 * calculator (what a physical one does), not an expression parser: there is one
 * accumulator and one pending operator. */
#define CALC_COLS 4
#define CALC_ROWS 5
#define CALC_DISPH 64

struct calcstate {
	char entry[32];  /* the number currently being typed */
	double acc;      /* accumulator, folded in on every operator */
	char op;         /* pending operator, 0 = none */
	int fresh;       /* next digit replaces entry rather than appending */
	int err;         /* division by zero: display locks until C */
	char expr[48];   /* the dim history line above the entry */
};

/* Paint state, allocated only for WIN_PAINT windows. A fixed logical pixel
 * canvas drawn at an integer zoom -- the pixels are the point. */
#define PT_W 128
#define PT_H 96
#define PT_NCOL 16
#define PT_TOOLH 30
#define PT_PALH 30
#define PT_NTOOL 5      /* pencil, eraser, fill, clear, save */

struct paintstate {
	uint8_t px[PT_H][PT_W]; /* palette indices; 0 is the empty/white paper */
	int color;              /* active palette index */
	int tool;               /* 0 pencil, 1 eraser, 2 fill */
	int brush;              /* radius knob, 1..3 */
	int last_x, last_y;     /* previous canvas cell, for stroke interpolation */
	char status[48];
};

/* Calendar state, allocated only for WIN_CAL windows. A month grid plus the
 * events of the selected day, persisted as one line per event in CAL_FILE. */
#define CAL_HEADH 36
#define CAL_WDH 22
#define CAL_PANW 224
#define CAL_MAXEV 256
#define CAL_TEXTLEN 64
#define CAL_FILE "/root/.calendar"

struct calevent {
	int y, m, d;              /* m is 1..12 */
	char text[CAL_TEXTLEN];
};

struct calstate {
	int year, month;          /* the month on display, month 1..12 */
	int sy, sm, sd;           /* the selected day */
	struct calevent ev[CAL_MAXEV];
	int nev;
	int sel_ev;               /* index into the selected day's list, -1 = none */
	char input[CAL_TEXTLEN];
	int typing;               /* keys are going into input[] */
	char status[48];
};

/* Timer state, allocated only for WIN_TIMER windows. Stopwatch and countdown
 * share one struct: "running" plus a monotonic start mark and a banked total,
 * so pause/resume never drifts (no per-tick accumulation error). */
struct timerstate {
	int running;
	int mode;              /* 0 stopwatch, 1 countdown */
	struct timespec started;
	double banked;         /* seconds accumulated before the current run */
	double countdown_secs; /* configured duration, countdown mode only */
	char status[48];
};

/* File Search state, allocated only for WIN_SEARCH windows. A plain recursive
 * substring search under one root, capped at SEARCH_MAXRES hits. */
#define SEARCH_MAXRES 256
struct searchresult {
	char path[FM_FULLLEN];
	int isdir;
};
struct searchstate {
	char root[FM_PATHLEN];
	char pattern[FM_NAMELEN];
	int typing;    /* Ctrl+F-style capture into pattern[] */
	struct searchresult res[SEARCH_MAXRES];
	int nres, scroll;
	char status[64];
};

/* Disk Usage state, allocated only for WIN_DISKUSAGE windows. One row per
 * immediate child of `path`, sized by a full recursive walk (like `du
 * --max-depth=1`); drilling into a row re-walks the new path. */
#define DU_MAXENT 256
struct duent {
	char name[FM_NAMELEN];
	long long size;
	int isdir;
};
struct dustate {
	char path[FM_PATHLEN];
	struct duent ents[DU_MAXENT];
	int count, scroll;
	long long total;
	char status[64];
};

/* Image Viewer state, allocated only for WIN_IMGVIEW windows. Only the two
 * formats this desktop itself produces -- PPM (paint.c's save format) and
 * plain 24-bit BMP -- decode with no library at all. */
struct imgstate {
	char path[FM_FULLLEN];
	unsigned char *rgb; /* iw*ih*3, top-to-bottom, malloc'd */
	int iw, ih;
	char status[64];
};

/* Archive Manager state, allocated only for WIN_ARCHIVE windows. Lists and
 * extracts via the system `tar`, already present for any Debian base --
 * gzip/xz/bzip2 members included, no new package required. */
#define ARC_MAXENT 512
#define ARC_NAMELEN 200
struct archivestate {
	char path[FM_FULLLEN];
	char ent[ARC_MAXENT][ARC_NAMELEN];
	int count, scroll;
	char status[96];
};

/* Screenshot Tool state, allocated only for WIN_SHOT windows. No captured
 * buffer is kept: the preview reads straight from the live backbuf, so all
 * that needs remembering is the last saved filename. */
struct shotstate {
	int count;
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
	struct fmstate *fm;       /* WIN_FILES only */
	struct edstate *ed;       /* WIN_EDIT only */
	struct calcstate *calc;   /* WIN_CALC only */
	struct paintstate *paint; /* WIN_PAINT only */
	struct calstate *cal;     /* WIN_CAL only */
	struct timerstate *timer;     /* WIN_TIMER only */
	struct searchstate *search;   /* WIN_SEARCH only */
	struct imgstate *img;         /* WIN_IMGVIEW only */
	struct archivestate *arc;     /* WIN_ARCHIVE only */
	struct shotstate *shot;       /* WIN_SHOT only */
	int tab;                  /* WIN_TASKMGR / WIN_SETTINGS: active tab */

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

/* Settings, all real switches read elsewhere -- see settings.c's row
 * registry for where each one is wired in. */
extern int clock_24h;           /* taskbar clock: 24h vs 12h */
extern int clock_show_secs;     /* taskbar clock: include seconds */
extern int clock_show_date;     /* taskbar clock: prefix with the date */
extern int wallpaper_solid;     /* desktop background: flat dtop vs the usual gradient */
extern int show_icon_labels;    /* draw the name under desktop/app icons */
extern int confirm_delete;      /* File Manager toolbar Delete arms before it fires */
extern int fm_sort_by_size;     /* File Manager listing: sort by size instead of name */

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
extern int start_menu_open;
extern char start_filter[32]; /* Start Menu's search-as-you-type app filter */

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
uint32_t get_pixel(int x, int y);
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
int start_x(void);
int start_hit(int px, int py);
int start_menu_row_at(int px, int py);
int start_menu_keys(const char *buf, int n);
void draw_start_menu(void);

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
void open_regular_file(const char *path, const char *name, int isexec);
int spawn_file_window(void);

/* editor.c */
void draw_editor(struct window *w, int content_y, int content_h);
void ed_click(struct window *w, int x, int y);
void ed_free(struct edstate *e);
int ed_keys(struct window *w, const char *buf, int n);
void ed_render(struct window *w);
int spawn_editor(const char *path);

/* settings.c */
void draw_settings(struct window *w, int content_y);
void settings_click(struct window *w, int px, int py);
int settings_keys(struct window *w, const char *buf, int n);
int spawn_settings(void);

/* calc.c */
void calc_click(struct window *w, int px, int py);
int calc_keys(struct window *w, const char *buf, int n);
void draw_calc(struct window *w, int content_y, int content_h);
int spawn_calc(void);

/* paint.c */
void draw_paint(struct window *w, int content_y, int content_h);
void paint_click(struct window *w, int px, int py);
int paint_keys(struct window *w, const char *buf, int n);
void paint_motion(int px, int py);
int spawn_paint(void);
/* Window slot whose canvas has the pointer captured for a stroke, -1 = none.
 * Same press/motion/release shape as the file-manager row drag. */
extern int paint_win;

/* calendar.c */
void cal_click(struct window *w, int px, int py);
int cal_keys(struct window *w, const char *buf, int n);
void draw_cal(struct window *w, int content_y, int content_h);
int spawn_cal(void);

/* timer.c */
void draw_timer(struct window *w, int content_y, int content_h);
void timer_click(struct window *w, int px, int py);
int spawn_timer(void);

/* search.c */
void draw_search(struct window *w, int content_y, int content_h);
void search_click(struct window *w, int px, int py);
int search_keys(struct window *w, const char *buf, int n);
int spawn_search(void);

/* diskusage.c -- Folder Usage panel embedded in the Task Manager's Disk tab */
void du_scan(struct dustate *s);
void draw_du_panel(struct dustate *s, uint32_t accent, int x, int y, int w, int h);
int du_panel_click(struct dustate *s, int rx, int ry, int rw, int rh, int px, int py);

/* imgview.c */
void draw_imgview(struct window *w, int content_y, int content_h);
void imgview_click(struct window *w, int px, int py);
int spawn_imgview(const char *path);

/* archive.c */
void draw_archive(struct window *w, int content_y, int content_h);
void archive_click(struct window *w, int px, int py);
int spawn_archive(const char *path);
int archive_create(const char *srcpath);

/* screenshot.c */
void draw_shot(struct window *w, int content_y, int content_h);
void shot_click(struct window *w, int px, int py);
int spawn_shot(void);

/* main.c */
int process_pointer(int nx, int ny, int left, int right);
void launch_icon(int idx);

#endif /* FBDESKTOP_H */
