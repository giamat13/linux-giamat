/* fbdesktop -- Timer: a stopwatch and a countdown timer sharing one window.
 * "running" plus a monotonic start mark and a banked total means pause/resume
 * never drifts (no per-tick accumulation error), the same trick a real stopwatch
 * chip uses. */
#include "fbdesktop.h"

#define TM_BG    0x1e1e2e
#define TM_DISP  0x181826
#define TM_TXT   0xcdd6f4
#define TM_DIM   0x6c7086
#define TM_BTN   0x2b2b3a
#define TM_BTNTOP 0x33334a

#define TM_DISPH 90
#define TM_ROWH  40
#define TM_NMODE 2
#define TM_NADJ  3
static const char *mode_labels[TM_NMODE] = { "Stopwatch", "Countdown" };
static const char *adj_labels[TM_NADJ]   = { "-1m", "+1m", "+5m" };

static double now_mono(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec + ts.tv_nsec / 1e9;
}

static double timer_elapsed(struct timerstate *t)
{
	if (!t->running)
		return t->banked;
	return t->banked + (now_mono() - (t->started.tv_sec + t->started.tv_nsec / 1e9));
}

/* ---- geometry, shared by the renderer and the hit-test -------------- */

static void timer_layout(struct window *w, int content_y, int content_h,
			 int *mode_y, int *adj_y, int *big_y, int *bw, int *bh)
{
	int pad = 8;
	*bh = TM_ROWH - 6;
	*mode_y = content_y + TM_DISPH + pad;
	*adj_y = *mode_y + TM_ROWH;
	*big_y = *adj_y + TM_ROWH + pad;
	*bw = (w->w - 2 * pad) / 3;
	(void)content_h;
}

/* ---- input ------------------------------------------------------------ */

void timer_click(struct window *w, int px, int py)
{
	struct timerstate *t = w->timer;
	int content_y = w->y + TITLE_H;
	int mode_y, adj_y, big_y, bw, bh;
	timer_layout(w, content_y, w->h - TITLE_H, &mode_y, &adj_y, &big_y, &bw, &bh);
	int pad = 8;

	if (py >= mode_y && py < mode_y + bh) {
		int col = (px - (w->x + pad)) / ((w->w - 2 * pad) / TM_NMODE);
		if (col >= 0 && col < TM_NMODE && col != t->mode) {
			t->mode = col;
			t->running = 0;
			t->banked = 0;
		}
		return;
	}
	if (t->mode == 1 && py >= adj_y && py < adj_y + bh) {
		int col = (px - (w->x + pad)) / bw;
		if (col == 0)
			t->countdown_secs -= 60;
		else if (col == 1)
			t->countdown_secs += 60;
		else if (col == 2)
			t->countdown_secs += 300;
		if (t->countdown_secs < 0)
			t->countdown_secs = 0;
		return;
	}
	int halfw = (w->w - 2 * pad) / 2;
	if (py >= big_y && py < big_y + bh) {
		if (px < w->x + pad + halfw) {
			if (!t->running)
				clock_gettime(CLOCK_MONOTONIC, &t->started);
			t->running = !t->running;
			if (!t->running)
				t->banked = timer_elapsed(t);
		} else {
			t->running = 0;
			t->banked = 0;
		}
	}
}

/* ---- renderer ----------------------------------------------------------- */

void draw_timer(struct window *w, int content_y, int content_h)
{
	struct timerstate *t = w->timer;
	uint32_t accent = win_accent(w);

	fill_rect(w->x, content_y, w->w, content_h, TM_BG);

	/* ---- big digital display ---- */
	fill_round_rect(w->x + 8, content_y + 6, w->w - 16, TM_DISPH - 10, 8, TM_DISP);
	double secs = timer_elapsed(t);
	if (t->mode == 1) {
		secs = t->countdown_secs - secs;
		if (secs <= 0) {
			secs = 0;
			t->running = 0;
		}
	}
	int isecs = (int)secs;
	char buf[32];
	if (isecs >= 3600)
		snprintf(buf, sizeof(buf), "%d:%02d:%02d", isecs / 3600, (isecs / 60) % 60, isecs % 60);
	else
		snprintf(buf, sizeof(buf), "%02d:%02d", isecs / 60, isecs % 60);
	int tw = (int)strlen(buf) * font_w * 2;
	int tx = w->x + (w->w - tw) / 2;
	int ty = content_y + (TM_DISPH - font_h) / 2;
	/* no font scaling available -- fake "big" by drawing each glyph twice, offset by 1px */
	draw_text(tx, ty, buf, t->running ? accent : TM_TXT);
	draw_text(tx + 1, ty, buf, t->running ? accent : TM_TXT);

	/* ---- mode toggle ---- */
	int mode_y, adj_y, big_y, bw, bh;
	timer_layout(w, content_y, content_h, &mode_y, &adj_y, &big_y, &bw, &bh);
	int pad = 8;
	int mw = (w->w - 2 * pad) / TM_NMODE;
	for (int i = 0; i < TM_NMODE; i++) {
		int bx = w->x + pad + i * mw;
		uint32_t top = (i == t->mode) ? mix(accent, 0xffffff, 40) : TM_BTNTOP;
		uint32_t bot = (i == t->mode) ? accent : TM_BTN;
		fill_round_rect_grad(bx, mode_y, mw - 4, bh, 6, top, bot);
		int lw = (int)strlen(mode_labels[i]) * font_w;
		draw_text(bx + (mw - 4 - lw) / 2, mode_y + (bh - font_h) / 2, mode_labels[i],
			  (i == t->mode) ? 0x11111c : TM_TXT);
	}

	/* ---- countdown adjust row (countdown mode only) ---- */
	if (t->mode == 1) {
		for (int i = 0; i < TM_NADJ; i++) {
			int bx = w->x + pad + i * bw;
			fill_round_rect_grad(bx, adj_y, bw - 4, bh, 6, TM_BTNTOP, TM_BTN);
			int lw = (int)strlen(adj_labels[i]) * font_w;
			draw_text(bx + (bw - 4 - lw) / 2, adj_y + (bh - font_h) / 2, adj_labels[i], TM_TXT);
		}
	}

	/* ---- start/stop + reset ---- */
	int halfw = (w->w - 2 * pad) / 2;
	const char *ss = t->running ? "Stop" : "Start";
	fill_round_rect_grad(w->x + pad, big_y, halfw - 4, bh, 6,
			     t->running ? mix(0xf38ba8, 0xffffff, 40) : mix(accent, 0xffffff, 40),
			     t->running ? 0xf38ba8 : accent);
	int sw = (int)strlen(ss) * font_w;
	draw_text(w->x + pad + (halfw - 4 - sw) / 2, big_y + (bh - font_h) / 2, ss, 0x11111c);

	fill_round_rect_grad(w->x + pad + halfw, big_y, halfw - 4, bh, 6, TM_BTNTOP, TM_BTN);
	int rw = (int)strlen("Reset") * font_w;
	draw_text(w->x + pad + halfw + (halfw - 4 - rw) / 2, big_y + (bh - font_h) / 2, "Reset", TM_TXT);
}

int spawn_timer(void)
{
	for (int i = 0; i < MAX_WIN; i++) {
		if (wins[i].used && wins[i].type == WIN_TIMER) {
			wins[i].minimized = 0;
			raise_window(i);
			focused = i;
			return i;
		}
	}
	int slot = alloc_window_slot();
	if (slot < 0)
		return -1;
	struct timerstate *t = calloc(1, sizeof(struct timerstate));
	if (!t)
		return -1;
	t->countdown_secs = 300;
	memset(&wins[slot], 0, sizeof(wins[slot]));
	wins[slot].used = 1;
	wins[slot].type = WIN_TIMER;
	wins[slot].pty_fd = -1;
	wins[slot].timer = t;
	wins[slot].x = 340;
	wins[slot].y = 130;
	wins[slot].w = 300;
	wins[slot].h = 260;
	wins[slot].attr_fg = COL_FG_DEFAULT;
	wins[slot].attr_bg = COL_BG_DEFAULT;
	snprintf(wins[slot].title, sizeof(wins[slot].title), "Timer");
	zorder[zcount++] = slot;
	focused = slot;
	return slot;
}
