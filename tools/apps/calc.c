/* fbdesktop -- calculator: a display panel and a real button grid, driven by
 * either the mouse or the keyboard. Immediate-execution semantics, like a
 * physical calculator: one accumulator, one pending operator, no precedence. */
#include "fbdesktop.h"

#define C_BG      0x1e1e2e
#define C_DISP    0x181826
#define C_TXT     0xcdd6f4
#define C_DIM     0x6c7086
#define C_KEY     0x2b2b3a
#define C_KEYTOP  0x33334a
#define C_FN      0x232338
#define C_OPC     0x89b4fa

/* Row-major, matching the grid drawn below. */
static const char *keys[CALC_ROWS][CALC_COLS] = {
	{ "C",  "+/-", "%",   "/" },
	{ "7",  "8",   "9",   "*" },
	{ "4",  "5",   "6",   "-" },
	{ "1",  "2",   "3",   "+" },
	{ "0",  ".",   "DEL", "=" },
};

static int is_digit_key(const char *k) { return (k[0] >= '0' && k[0] <= '9' && !k[1]); }
static int is_op_key(const char *k)
{
	return !k[1] && (k[0] == '/' || k[0] == '*' || k[0] == '-' || k[0] == '+');
}

/* ---- geometry, shared by the renderer and the hit-test -------------- */

static void calc_grid(struct window *w, int content_y, int content_h,
		      int *gx, int *gy, int *cw, int *ch)
{
	int pad = 8;
	*gx = w->x + pad;
	*gy = content_y + CALC_DISPH + pad;
	int gw = w->w - 2 * pad;
	int gh = content_y + content_h - *gy - pad;
	*cw = gw / CALC_COLS;
	*ch = gh / CALC_ROWS;
}

/* ---- arithmetic ----------------------------------------------------- */

/* Trim %g's output so the display never shows "5.0000000001" noise. */
static void calc_fmt(double v, char *out, size_t n)
{
	snprintf(out, n, "%.10g", v);
}

static void calc_apply(struct calcstate *c)
{
	double v = atof(c->entry);
	switch (c->op) {
	case '+': c->acc += v; break;
	case '-': c->acc -= v; break;
	case '*': c->acc *= v; break;
	case '/':
		if (v == 0) {
			c->err = 1;
			c->acc = 0;
		} else {
			c->acc /= v;
		}
		break;
	default: c->acc = v; break;
	}
}

static void calc_reset(struct calcstate *c)
{
	snprintf(c->entry, sizeof(c->entry), "0");
	c->acc = 0;
	c->op = 0;
	c->fresh = 1;
	c->err = 0;
	c->expr[0] = 0;
}

/* One key press, named by its label -- the mouse and the keyboard both land
 * here so the two can never drift apart. */
static void calc_press(struct calcstate *c, const char *k)
{
	/* An error locks everything but Clear: the accumulator is meaningless. */
	if (c->err && strcmp(k, "C"))
		return;

	int len = (int)strlen(c->entry);

	if (is_digit_key(k)) {
		if (c->fresh || !strcmp(c->entry, "0")) {
			snprintf(c->entry, sizeof(c->entry), "%s", k);
			c->fresh = 0;
		} else if (len < (int)sizeof(c->entry) - 1) {
			c->entry[len] = k[0];
			c->entry[len + 1] = 0;
		}
		return;
	}
	if (!strcmp(k, ".")) {
		if (c->fresh) {
			snprintf(c->entry, sizeof(c->entry), "0.");
			c->fresh = 0;
		} else if (!strchr(c->entry, '.') && len < (int)sizeof(c->entry) - 1) {
			c->entry[len] = '.';
			c->entry[len + 1] = 0;
		}
		return;
	}
	if (is_op_key(k)) {
		calc_apply(c);
		c->op = k[0];
		calc_fmt(c->acc, c->entry, sizeof(c->entry));
		snprintf(c->expr, sizeof(c->expr), "%s %c", c->entry, c->op);
		c->fresh = 1;
		return;
	}
	if (!strcmp(k, "=")) {
		calc_apply(c); /* no pending op folds to acc = entry */
		c->op = 0;
		calc_fmt(c->acc, c->entry, sizeof(c->entry));
		snprintf(c->expr, sizeof(c->expr), "= %s", c->entry);
		c->fresh = 1;
		return;
	}
	if (!strcmp(k, "C")) {
		calc_reset(c);
		return;
	}
	if (!strcmp(k, "+/-")) {
		if (c->entry[0] == '-')
			memmove(c->entry, c->entry + 1, strlen(c->entry));
		else if (strcmp(c->entry, "0") && len < (int)sizeof(c->entry) - 1) {
			memmove(c->entry + 1, c->entry, len + 1);
			c->entry[0] = '-';
		}
		return;
	}
	if (!strcmp(k, "%")) {
		calc_fmt(atof(c->entry) / 100.0, c->entry, sizeof(c->entry));
		c->fresh = 1;
		return;
	}
	if (!strcmp(k, "DEL")) {
		if (c->fresh)
			return;
		if (len > 1)
			c->entry[len - 1] = 0;
		else
			snprintf(c->entry, sizeof(c->entry), "0");
		return;
	}
}

/* ---- input ---------------------------------------------------------- */

void calc_click(struct window *w, int px, int py)
{
	int gx, gy, cw, ch;
	calc_grid(w, w->y + TITLE_H, w->h - TITLE_H, &gx, &gy, &cw, &ch);
	if (cw <= 0 || ch <= 0)
		return;
	int col = (px - gx) / cw, row = (py - gy) / ch;
	if (col < 0 || col >= CALC_COLS || row < 0 || row >= CALC_ROWS)
		return;
	calc_press(w->calc, keys[row][col]);
}

int calc_keys(struct window *w, const char *buf, int n)
{
	struct calcstate *c = w->calc;
	int changed = 0;
	for (int i = 0; i < n; i++) {
		char k[4] = { buf[i], 0, 0, 0 };
		unsigned char ch = (unsigned char)buf[i];
		if (ch == '\r' || ch == '\n')
			strcpy(k, "=");
		else if (ch == 0x7f || ch == 0x08)
			strcpy(k, "DEL");
		else if (ch == 0x1b || ch == 'c' || ch == 'C')
			strcpy(k, "C");
		else if (!((ch >= '0' && ch <= '9') || ch == '.' || ch == '=' ||
			   ch == '+' || ch == '-' || ch == '*' || ch == '/' || ch == '%'))
			continue;
		calc_press(c, k);
		changed = 1;
	}
	return changed;
}

/* ---- renderer ------------------------------------------------------- */

void draw_calc(struct window *w, int content_y, int content_h)
{
	struct calcstate *c = w->calc;
	uint32_t accent = win_accent(w);

	fill_rect(w->x, content_y, w->w, content_h, C_BG);

	/* ---- display ---- */
	fill_round_rect(w->x + 8, content_y + 6, w->w - 16, CALC_DISPH - 10, 8, C_DISP);
	int right = w->x + w->w - 20;
	if (c->expr[0]) {
		int ew = (int)strlen(c->expr) * font_w;
		draw_text_clip(right - ew, content_y + 14, c->expr, C_DIM, w->w - 32);
	}
	const char *shown = c->err ? "cannot divide by zero" : c->entry;
	int sw = (int)strlen(shown) * font_w;
	draw_text_clip(right - sw, content_y + CALC_DISPH - 28, shown,
		       c->err ? 0xf38ba8 : 0xffffff, w->w - 32);

	/* ---- keypad ---- */
	int gx, gy, cw, ch;
	calc_grid(w, content_y, content_h, &gx, &gy, &cw, &ch);
	if (cw < 8 || ch < 8)
		return;
	for (int r = 0; r < CALC_ROWS; r++) {
		for (int col = 0; col < CALC_COLS; col++) {
			const char *k = keys[r][col];
			int bx = gx + col * cw, by = gy + r * ch;
			int bw = cw - 5, bh = ch - 5;

			uint32_t top = C_KEYTOP, bot = C_KEY, fg = C_TXT;
			if (!strcmp(k, "=")) {
				top = mix(accent, 0xffffff, 40);
				bot = accent;
				fg = 0x11111c;
			} else if (is_op_key(k)) {
				top = mix(C_OPC, C_KEY, 150);
				bot = mix(C_OPC, C_KEY, 190);
				fg = 0xffffff;
			} else if (!is_digit_key(k) && strcmp(k, ".")) {
				top = C_FN;
				bot = 0x1c1c28;
				fg = C_DIM;
			}
			fill_round_rect_grad(bx, by, bw, bh, 6, top, bot);
			int tw = (int)strlen(k) * font_w;
			draw_text(bx + (bw - tw) / 2, by + (bh - font_h) / 2, k, fg);
		}
	}

	/* resize grip */
	if (!w->maximized)
		for (int k = 0; k < 3; k++) {
			int gpx = w->x + w->w - 5 - k * 4;
			int gpy = w->y + w->h - 5;
			for (int m = 0; m <= k; m++)
				fill_rect(gpx, gpy - m * 4, 2, 2, C_DIM);
		}
}

int spawn_calc(void)
{
	/* single-instance: focus the existing one instead of opening a second */
	for (int i = 0; i < MAX_WIN; i++) {
		if (wins[i].used && wins[i].type == WIN_CALC) {
			wins[i].minimized = 0;
			raise_window(i);
			focused = i;
			return i;
		}
	}
	int slot = alloc_window_slot();
	if (slot < 0)
		return -1;
	struct calcstate *c = calloc(1, sizeof(struct calcstate));
	if (!c)
		return -1;
	calc_reset(c);
	memset(&wins[slot], 0, sizeof(wins[slot]));
	wins[slot].used = 1;
	wins[slot].type = WIN_CALC;
	wins[slot].pty_fd = -1;
	wins[slot].calc = c;
	wins[slot].x = 320;
	wins[slot].y = 120;
	wins[slot].w = 300;
	wins[slot].h = 400;
	wins[slot].attr_fg = COL_FG_DEFAULT;
	wins[slot].attr_bg = COL_BG_DEFAULT;
	snprintf(wins[slot].title, sizeof(wins[slot].title), "Calculator");
	zorder[zcount++] = slot;
	focused = slot;
	return slot;
}
