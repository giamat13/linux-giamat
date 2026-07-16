/* fbdesktop -- paint: a fixed logical pixel canvas drawn at an integer zoom,
 * with a tool row and a palette strip. The pixels are the point -- there is no
 * antialiasing anywhere, and the canvas saves as a PPM onto the desktop. */
#include "fbdesktop.h"

#define P_BAR   0x181826
#define P_TXT   0xcdd6f4
#define P_DIM   0x6c7086
#define P_FAINT 0x45475a

int paint_win = -1;

/* Index 0 is the paper: the eraser writes it, and Clear fills with it. */
static const uint32_t pal[PT_NCOL] = {
	0xffffff, 0x000000, 0x9399b2, 0x45475a,
	0xf38ba8, 0xfab387, 0xf9e2af, 0xa6e3a1,
	0x94e2d5, 0x89dceb, 0x89b4fa, 0x5b6ee1,
	0xcba6f7, 0xf5c2e7, 0xeba0ac, 0x8b5a2b,
};

static const char *tools[PT_NTOOL] = { "Pencil", "Eraser", "Fill", "Clear", "Save" };

#define P_BTNW 58
#define P_BTNG 4
#define P_BRUSHW 22

/* ---- geometry, shared by the renderer and every hit-test ------------ */

/* Canvas origin and zoom: the largest integer scale that fits, centered. */
static void pt_canvas(struct window *w, int *ox, int *oy, int *scale)
{
	int ax = w->x + 6, ay = w->y + TITLE_H + PT_TOOLH + 6;
	int aw = w->w - 12;
	int ah = (w->h - TITLE_H) - PT_TOOLH - PT_PALH - 12;
	int s = aw / PT_W;
	if (ah / PT_H < s)
		s = ah / PT_H;
	if (s < 1)
		s = 1;
	*scale = s;
	*ox = ax + (aw - PT_W * s) / 2;
	*oy = ay + (ah - PT_H * s) / 2;
}

/* Map a screen point to a canvas cell; 0 if it is outside the canvas. */
static int pt_cell(struct window *w, int px, int py, int *cx, int *cy)
{
	int ox, oy, s;
	pt_canvas(w, &ox, &oy, &s);
	int x = (px - ox) / s, y = (py - oy) / s;
	if (px < ox || py < oy || x < 0 || y < 0 || x >= PT_W || y >= PT_H)
		return 0;
	*cx = x;
	*cy = y;
	return 1;
}

static int pt_pal_y(struct window *w) { return w->y + w->h - PT_PALH; }

/* ---- painting ------------------------------------------------------- */

static void pt_dot(struct paintstate *p, int x, int y)
{
	uint8_t col = (p->tool == 1) ? 0 : (uint8_t)p->color;
	int r = p->brush - 1;
	for (int j = -r; j <= r; j++)
		for (int i = -r; i <= r; i++) {
			int nx = x + i, ny = y + j;
			if (nx < 0 || ny < 0 || nx >= PT_W || ny >= PT_H)
				continue;
			if (i * i + j * j > r * r + r) /* round off the corners */
				continue;
			p->px[ny][nx] = col;
		}
}

/* Bresenham: pointer events are sparse, so a fast stroke must be joined up or
 * it lands as a dotted line. */
static void pt_line(struct paintstate *p, int x0, int y0, int x1, int y1)
{
	int dx = x1 - x0, dy = y1 - y0;
	int sx = dx < 0 ? -1 : 1, sy = dy < 0 ? -1 : 1;
	dx = dx < 0 ? -dx : dx;
	dy = dy < 0 ? -dy : dy;
	int err = dx - dy;
	for (;;) {
		pt_dot(p, x0, y0);
		if (x0 == x1 && y0 == y1)
			break;
		int e2 = 2 * err;
		if (e2 > -dy) { err -= dy; x0 += sx; }
		if (e2 < dx)  { err += dx; y0 += sy; }
	}
}

/* Four-way flood fill. Each cell is recoloured as it is queued, so it can be
 * enqueued at most once and PT_W*PT_H slots always suffice. */
static void pt_fill(struct paintstate *p, int x, int y)
{
	static int qx[PT_W * PT_H], qy[PT_W * PT_H];
	uint8_t col = (p->tool == 1) ? 0 : (uint8_t)p->color;
	uint8_t target = p->px[y][x];
	if (target == col)
		return;
	int head = 0, tail = 0;
	qx[tail] = x; qy[tail] = y; tail++;
	p->px[y][x] = col;
	while (head < tail) {
		int cx = qx[head], cy = qy[head];
		head++;
		static const int dxs[4] = { 1, -1, 0, 0 }, dys[4] = { 0, 0, 1, -1 };
		for (int k = 0; k < 4; k++) {
			int nx = cx + dxs[k], ny = cy + dys[k];
			if (nx < 0 || ny < 0 || nx >= PT_W || ny >= PT_H)
				continue;
			if (p->px[ny][nx] != target)
				continue;
			p->px[ny][nx] = col;
			qx[tail] = nx; qy[tail] = ny; tail++;
		}
	}
}

static void pt_save(struct paintstate *p)
{
	char path[FM_FULLLEN];
	int n = 1;
	for (; n < 1000; n++) {
		snprintf(path, sizeof(path), "%s/paint-%d.ppm", DESKTOP_DIR, n);
		if (access(path, F_OK) != 0)
			break;
	}
	FILE *f = fopen(path, "wb");
	if (!f) {
		snprintf(p->status, sizeof(p->status), "save failed: %s", strerror(errno));
		return;
	}
	fprintf(f, "P6\n%d %d\n255\n", PT_W, PT_H);
	for (int y = 0; y < PT_H; y++)
		for (int x = 0; x < PT_W; x++) {
			uint32_t c = pal[p->px[y][x]];
			unsigned char rgb[3] = { (c >> 16) & 0xff, (c >> 8) & 0xff, c & 0xff };
			fwrite(rgb, 1, 3, f);
		}
	if (fclose(f) != 0)
		snprintf(p->status, sizeof(p->status), "save failed: %s", strerror(errno));
	else
		snprintf(p->status, sizeof(p->status), "saved paint-%d.ppm to Desktop", n);
	desk_scan(); /* it lands on the desktop, so show it there straight away */
}

/* ---- input ---------------------------------------------------------- */

void paint_click(struct window *w, int px, int py)
{
	struct paintstate *p = w->paint;
	int ty = w->y + TITLE_H;

	/* toolbar */
	if (py >= ty && py < ty + PT_TOOLH) {
		for (int i = 0; i < PT_NTOOL; i++) {
			int bx = w->x + 6 + i * (P_BTNW + P_BTNG);
			if (px >= bx && px < bx + P_BTNW) {
				p->status[0] = 0;
				if (i == 3)
					memset(p->px, 0, sizeof(p->px));
				else if (i == 4)
					pt_save(p);
				else
					p->tool = i;
				return;
			}
		}
		for (int b = 0; b < 3; b++) {
			int bx = w->x + w->w - 6 - (3 - b) * (P_BRUSHW + 3);
			if (px >= bx && px < bx + P_BRUSHW) {
				p->brush = b + 1;
				return;
			}
		}
		return;
	}

	/* palette */
	int py0 = pt_pal_y(w);
	if (py >= py0) {
		int sw = (w->w - 12) / PT_NCOL;
		int idx = (px - w->x - 6) / (sw > 0 ? sw : 1);
		if (idx >= 0 && idx < PT_NCOL) {
			p->color = idx;
			if (p->tool == 1)
				p->tool = 0; /* picking a colour means you want to draw */
		}
		return;
	}

	/* canvas: start a stroke and capture the pointer until release */
	int cx, cy;
	if (!pt_cell(w, px, py, &cx, &cy))
		return;
	p->status[0] = 0;
	if (p->tool == 2) {
		pt_fill(p, cx, cy);
		return;
	}
	pt_dot(p, cx, cy);
	p->last_x = cx;
	p->last_y = cy;
	paint_win = (int)(w - wins);
}

/* Pointer moved with the button still down after paint_click captured it. */
void paint_motion(int px, int py)
{
	if (paint_win < 0 || !wins[paint_win].used || !wins[paint_win].paint)
		return;
	struct window *w = &wins[paint_win];
	struct paintstate *p = w->paint;
	int cx, cy;
	if (!pt_cell(w, px, py, &cx, &cy))
		return;
	pt_line(p, p->last_x, p->last_y, cx, cy);
	p->last_x = cx;
	p->last_y = cy;
}

int paint_keys(struct window *w, const char *buf, int n)
{
	struct paintstate *p = w->paint;
	int changed = 0;
	for (int i = 0; i < n; i++) {
		switch (buf[i]) {
		case 'p': p->tool = 0; break;
		case 'e': p->tool = 1; break;
		case 'f': p->tool = 2; break;
		case 'c': memset(p->px, 0, sizeof(p->px)); break;
		case 0x13: pt_save(p); break;            /* Ctrl+S */
		case '1': p->brush = 1; break;
		case '2': p->brush = 2; break;
		case '3': p->brush = 3; break;
		case '[': if (p->color > 0) p->color--; break;
		case ']': if (p->color < PT_NCOL - 1) p->color++; break;
		default: continue;
		}
		changed = 1;
	}
	return changed;
}

/* ---- renderer ------------------------------------------------------- */

void draw_paint(struct window *w, int content_y, int content_h)
{
	struct paintstate *p = w->paint;
	uint32_t accent = win_accent(w);

	fill_rect(w->x, content_y, w->w, content_h, COL_BG_DEFAULT);

	/* ---- toolbar ---- */
	fill_rect(w->x, content_y, w->w, PT_TOOLH, P_BAR);
	for (int i = 0; i < PT_NTOOL; i++) {
		int bx = w->x + 6 + i * (P_BTNW + P_BTNG);
		int on = (i < 3 && p->tool == i);
		fill_round_rect_grad(bx, content_y + 4, P_BTNW, PT_TOOLH - 8, 5,
				     on ? mix(accent, 0xffffff, 40) : 0x2b2b3a,
				     on ? accent : 0x22222e);
		int tw = (int)strlen(tools[i]) * font_w;
		draw_text_clip(bx + (P_BTNW - tw) / 2, content_y + (PT_TOOLH - font_h) / 2,
			       tools[i], on ? 0x11111c : 0xdfe4f2, P_BTNW - 4);
	}
	for (int b = 0; b < 3; b++) {
		int bx = w->x + w->w - 6 - (3 - b) * (P_BRUSHW + 3);
		int on = (p->brush == b + 1);
		fill_round_rect(bx, content_y + 4, P_BRUSHW, PT_TOOLH - 8, 4,
				on ? accent : 0x2b2b3a);
		fill_circle(bx + P_BRUSHW / 2, content_y + PT_TOOLH / 2, b + 1,
			    on ? 0x11111c : P_TXT);
	}

	/* ---- canvas ---- */
	int ox, oy, s;
	pt_canvas(w, &ox, &oy, &s);
	fill_rect(ox - 1, oy - 1, PT_W * s + 2, PT_H * s + 2, P_FAINT); /* border */
	fill_rect(ox, oy, PT_W * s, PT_H * s, pal[0]);                  /* paper */
	for (int y = 0; y < PT_H; y++)
		for (int x = 0; x < PT_W; x++) {
			uint8_t c = p->px[y][x];
			if (c) /* paper is already painted */
				fill_rect(ox + x * s, oy + y * s, s, s, pal[c]);
		}

	/* ---- palette + status ---- */
	int py0 = pt_pal_y(w);
	fill_rect(w->x, py0, w->w, PT_PALH, P_BAR);
	fill_rect(w->x, py0, w->w, 1, 0x2a2a40);
	int sw = (w->w - 12) / PT_NCOL;
	for (int i = 0; i < PT_NCOL; i++) {
		int bx = w->x + 6 + i * sw;
		int on = (i == p->color);
		if (on)
			fill_round_rect(bx - 1, py0 + 4, sw - 1, PT_PALH - 8, 4, accent);
		fill_round_rect(bx + 2, py0 + 7, sw - 7, PT_PALH - 14, 3, pal[i]);
	}
	if (p->status[0]) {
		int tw = (int)strlen(p->status) * font_w;
		draw_text_clip(w->x + w->w - 8 - tw, py0 - font_h - 4, p->status,
			       0x94e2d5, w->w - 16);
	}

	/* resize grip */
	if (!w->maximized)
		for (int k = 0; k < 3; k++) {
			int gx = w->x + w->w - 5 - k * 4;
			int gy = w->y + w->h - 5;
			for (int m = 0; m <= k; m++)
				fill_rect(gx, gy - m * 4, 2, 2, P_DIM);
		}
}

int spawn_paint(void)
{
	for (int i = 0; i < MAX_WIN; i++) {
		if (wins[i].used && wins[i].type == WIN_PAINT) {
			wins[i].minimized = 0;
			raise_window(i);
			focused = i;
			return i;
		}
	}
	int slot = alloc_window_slot();
	if (slot < 0)
		return -1;
	struct paintstate *p = calloc(1, sizeof(struct paintstate));
	if (!p)
		return -1;
	p->color = 1; /* black on white paper */
	p->brush = 1;
	memset(&wins[slot], 0, sizeof(wins[slot]));
	wins[slot].used = 1;
	wins[slot].type = WIN_PAINT;
	wins[slot].pty_fd = -1;
	wins[slot].paint = p;
	wins[slot].x = 200;
	wins[slot].y = 90;
	wins[slot].w = 560;
	wins[slot].h = 500;
	wins[slot].attr_fg = COL_FG_DEFAULT;
	wins[slot].attr_bg = COL_BG_DEFAULT;
	snprintf(wins[slot].title, sizeof(wins[slot].title), "Paint");
	zorder[zcount++] = slot;
	focused = slot;
	return slot;
}
