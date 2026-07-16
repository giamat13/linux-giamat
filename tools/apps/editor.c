/* fbdesktop -- text editor window: toolbar, line-number gutter, caret, status
 * bar, and a snapshot undo/redo stack. Draws its own pixels; the character grid
 * is not involved. */
#include "fbdesktop.h"

#define ED_GUTTER_BG 0x181826
#define ED_TXT       0xcdd6f4
#define ED_DIM       0x6c7086
#define ED_FAINT     0x45475a
#define ED_CURLINE   0x26263a

/* ---- geometry, shared by the renderer and the hit-tests ------------- */

static int ed_text_top(struct window *w) { return w->y + TITLE_H + ED_TOOLH; }

static int ed_visible_rows(struct window *w)
{
	int h = w->h - TITLE_H - ED_TOOLH - ED_STATH;
	return h > 0 ? h / font_h : 0;
}

/* Gutter is as wide as the largest line number it must hold. */
static int ed_gutter_w(struct window *w)
{
	int digits = 1;
	for (int n = w->ed->nlines; n >= 10; n /= 10)
		digits++;
	if (digits < 3)
		digits = 3;
	return digits * font_w + 14;
}

static int ed_text_x(struct window *w) { return w->x + ed_gutter_w(w) + 6; }

/* ---- undo / redo ---------------------------------------------------- */

static void snap_free(struct edsnap *s)
{
	free(s->line);
	s->line = NULL;
}

/* Push a copy of the current buffer onto `st`, dropping the oldest step when
 * full. Returns 0 if the copy could not be allocated (the step is skipped
 * rather than half-recorded). */
static int snap_push(struct edsnap *st, int *n, struct edstate *e)
{
	char (*copy)[ED_MAXCOL] = malloc((size_t)e->nlines * ED_MAXCOL);
	if (!copy)
		return 0;
	memcpy(copy, e->line, (size_t)e->nlines * ED_MAXCOL);
	if (*n == ED_UNDO) {
		snap_free(&st[0]);
		memmove(st, st + 1, (ED_UNDO - 1) * sizeof(st[0]));
		(*n)--;
	}
	st[*n].line = copy;
	st[*n].nlines = e->nlines;
	st[*n].cy = e->cy;
	st[*n].cx = e->cx;
	(*n)++;
	return 1;
}

static void snap_clear(struct edsnap *st, int *n)
{
	while (*n > 0)
		snap_free(&st[--(*n)]);
}

/* Pop the top of `st` into the buffer. */
static void snap_pop_into(struct edsnap *st, int *n, struct edstate *e)
{
	struct edsnap *s = &st[--(*n)];
	memcpy(e->line, s->line, (size_t)s->nlines * ED_MAXCOL);
	e->nlines = s->nlines;
	e->cy = s->cy;
	e->cx = s->cx;
	snap_free(s);
}

/* Called before every mutation. Consecutive edits of the same kind on the same
 * line fold into the step already recorded, so undo moves in words, not keys. */
static void ed_touch(struct edstate *e, int op)
{
	if (op == e->last_op && (op == OP_INSERT || op == OP_DELETE) &&
	    e->cy == e->last_cy)
		return;
	if (snap_push(e->undo, &e->nundo, e))
		snap_clear(e->redo, &e->nredo);
	e->last_op = op;
	e->last_cy = e->cy;
}

/* Any caret move or command ends the current coalescing run. */
static void ed_break(struct edstate *e) { e->last_op = OP_NONE; }

static void ed_undo(struct window *w)
{
	struct edstate *e = w->ed;
	if (!e->nundo) {
		snprintf(e->status, sizeof(e->status), "nothing to undo");
		return;
	}
	snap_push(e->redo, &e->nredo, e);
	snap_pop_into(e->undo, &e->nundo, e);
	e->dirty = 1;
	ed_break(e);
	snprintf(e->status, sizeof(e->status), "undo");
}

static void ed_redo(struct window *w)
{
	struct edstate *e = w->ed;
	if (!e->nredo) {
		snprintf(e->status, sizeof(e->status), "nothing to redo");
		return;
	}
	snap_push(e->undo, &e->nundo, e);
	snap_pop_into(e->redo, &e->nredo, e);
	e->dirty = 1;
	ed_break(e);
	snprintf(e->status, sizeof(e->status), "redo");
}

void ed_free(struct edstate *e)
{
	snap_clear(e->undo, &e->nundo);
	snap_clear(e->redo, &e->nredo);
}

/* ---- buffer state --------------------------------------------------- */

void ed_render(struct window *w)
{
	struct edstate *e = w->ed;
	int rows = ed_visible_rows(w);
	if (e->cy < 0) e->cy = 0;
	if (e->cy >= e->nlines) e->cy = e->nlines - 1;
	int len = (int)strlen(e->line[e->cy]);
	if (e->cx > len) e->cx = len;
	if (e->cx < 0) e->cx = 0;
	/* keep the caret on screen */
	if (e->cy < e->scroll) e->scroll = e->cy;
	if (rows > 0 && e->cy >= e->scroll + rows) e->scroll = e->cy - rows + 1;
	if (e->scroll < 0) e->scroll = 0;

	const char *base = strrchr(e->path, '/');
	base = base ? base + 1 : e->path;
	snprintf(w->title, sizeof(w->title), "%s%.400s", e->dirty ? "*" : "", base);
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
	ed_touch(e, OP_INSERT);
	memmove(l + e->cx + 1, l + e->cx, len - e->cx + 1);
	l[e->cx++] = c;
	e->dirty = 1;
}

static void ed_newline(struct edstate *e)
{
	if (e->nlines + 1 >= ED_MAXLINES)
		return;
	ed_touch(e, OP_OTHER);
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
	ed_break(e);
}

static void ed_backspace(struct edstate *e)
{
	char *l = e->line[e->cy];
	if (e->cx > 0) {
		ed_touch(e, OP_DELETE);
		int len = (int)strlen(l);
		memmove(l + e->cx - 1, l + e->cx, len - e->cx + 1);
		e->cx--;
		e->dirty = 1;
		return;
	}
	if (e->cy == 0)
		return;
	ed_touch(e, OP_OTHER);
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
	ed_break(e);
}

/* Forward delete: joins the next line up when the caret sits at end of line. */
static void ed_del_forward(struct edstate *e)
{
	char *l = e->line[e->cy];
	int len = (int)strlen(l);
	if (e->cx < len) {
		ed_touch(e, OP_DELETE);
		memmove(l + e->cx, l + e->cx + 1, len - e->cx);
		e->dirty = 1;
		return;
	}
	if (e->cy >= e->nlines - 1)
		return;
	e->cy++;
	e->cx = 0;
	ed_backspace(e); /* same join, from the other side */
}

/* Ctrl+K: delete the whole line. */
static void ed_del_line(struct edstate *e)
{
	ed_touch(e, OP_OTHER);
	if (e->nlines == 1) {
		e->line[0][0] = 0;
	} else {
		for (int i = e->cy; i < e->nlines - 1; i++)
			memcpy(e->line[i], e->line[i + 1], ED_MAXCOL);
		e->nlines--;
		if (e->cy >= e->nlines)
			e->cy = e->nlines - 1;
	}
	e->cx = 0;
	e->dirty = 1;
	ed_break(e);
}

/* Ctrl+D: duplicate the current line below it. */
static void ed_dup_line(struct edstate *e)
{
	if (e->nlines + 1 >= ED_MAXLINES)
		return;
	ed_touch(e, OP_OTHER);
	for (int i = e->nlines; i > e->cy + 1; i--)
		memcpy(e->line[i], e->line[i - 1], ED_MAXCOL);
	memcpy(e->line[e->cy + 1], e->line[e->cy], ED_MAXCOL);
	e->nlines++;
	e->cy++;
	e->dirty = 1;
	ed_break(e);
}

/* ---- input ---------------------------------------------------------- */

int ed_keys(struct window *w, const char *buf, int n)
{
	struct edstate *e = w->ed;
	int rows = ed_visible_rows(w);
	if (rows < 2)
		rows = 2;

	for (int i = 0; i < n; i++) {
		unsigned char c = (unsigned char)buf[i];

		if (c == 0x1b && i + 2 < n && buf[i + 1] == '[') {
			/* Delete / Home / End arrive as ESC [ <n> ~ */
			if (i + 3 < n && buf[i + 3] == '~') {
				switch (buf[i + 2]) {
				case '3': e->status[0] = 0; ed_del_forward(e); break;
				case '1':
				case '7': e->cx = 0; ed_break(e); break;
				case '4':
				case '8': e->cx = (int)strlen(e->line[e->cy]); ed_break(e); break;
				case '5': e->cy -= rows - 1; ed_break(e); break;
				case '6': e->cy += rows - 1; ed_break(e); break;
				default: break;
				}
				i += 3;
				if (e->cy < 0) e->cy = 0;
				if (e->cy >= e->nlines) e->cy = e->nlines - 1;
				continue;
			}
			switch (buf[i + 2]) {
			case 'A': e->cy--; break;
			case 'B': e->cy++; break;
			case 'C': e->cx++; break;
			case 'D': e->cx--; break;
			case 'H': e->cx = 0; break;                       /* Home */
			case 'F': e->cx = (int)strlen(e->line[e->cy]); break; /* End */
			default: break;
			}
			ed_break(e);
			if (e->cy < 0) e->cy = 0;
			if (e->cy >= e->nlines) e->cy = e->nlines - 1;
			i += 2;
			continue;
		}

		e->status[0] = 0;
		switch (c) {
		case 0x13: ed_save(w); break;                 /* Ctrl+S */
		case 0x1a: ed_undo(w); break;                 /* Ctrl+Z */
		case 0x19: ed_redo(w); break;                 /* Ctrl+Y */
		case 0x0b: ed_del_line(e); break;             /* Ctrl+K */
		case 0x04: ed_dup_line(e); break;             /* Ctrl+D */
		case 0x01: e->cx = 0; ed_break(e); break;     /* Ctrl+A: line start */
		case 0x05: e->cx = (int)strlen(e->line[e->cy]); ed_break(e); break; /* Ctrl+E */
		case '\r':
		case '\n': ed_newline(e); break;
		case 0x7f:
		case 0x08: ed_backspace(e); break;
		case '\t':
			for (int k = 0; k < 4; k++)
				ed_insert(e, ' ');
			break;
		default:
			if (c >= 0x20 && c < 0x7f)
				ed_insert(e, (char)c);
			break;
		}
	}
	ed_render(w);
	return 1;
}

/* Toolbar hit-test: returns button index or -1. */
static int ed_btn_at(struct window *w, int px, int py)
{
	int by = w->y + TITLE_H;
	if (py < by || py >= by + ED_TOOLH)
		return -1;
	int idx = (px - w->x - 6) / (ED_BTNW + 4);
	if (idx < 0 || idx >= ED_NBTN)
		return -1;
	return idx;
}

void ed_click(struct window *w, int x, int y)
{
	struct edstate *e = w->ed;
	int btn = ed_btn_at(w, x, y);
	if (btn >= 0) {
		e->status[0] = 0;
		if (btn == 0) ed_save(w);
		else if (btn == 1) ed_undo(w);
		else ed_redo(w);
		ed_render(w);
		return;
	}
	int row = (y - ed_text_top(w)) / font_h;
	int col = (x - ed_text_x(w)) / font_w;
	if (row < 0) row = 0;
	if (row >= ed_visible_rows(w)) row = ed_visible_rows(w) - 1;
	if (col < 0) col = 0;
	e->cy = e->scroll + row;
	e->cx = col;
	ed_break(e);
	ed_render(w); /* clamps both */
}

/* ---- renderer ------------------------------------------------------- */

void draw_editor(struct window *w, int content_y, int content_h)
{
	struct edstate *e = w->ed;
	uint32_t accent = win_accent(w);
	int lx = w->x, lw = w->w;

	/* ---- toolbar ---- */
	static const char *btns[ED_NBTN] = { "Save", "Undo", "Redo" };
	fill_rect(lx, content_y, lw, ED_TOOLH, ED_GUTTER_BG);
	for (int b = 0; b < ED_NBTN; b++) {
		int bx = lx + 6 + b * (ED_BTNW + 4);
		int live = (b == 0) ? (e->dirty && !e->truncated)
			 : (b == 1) ? (e->nundo > 0) : (e->nredo > 0);
		fill_round_rect_grad(bx, content_y + 3, ED_BTNW, ED_TOOLH - 6, 4,
				     live ? 0x2b2b3a : 0x212130,
				     live ? 0x22222e : 0x1c1c28);
		if (b == 0 && e->dirty && !e->truncated)
			fill_circle(bx + 8, content_y + ED_TOOLH / 2, 3, 0xf9e2af);
		int tw = (int)strlen(btns[b]) * font_w;
		draw_text_clip(bx + (ED_BTNW - tw) / 2 + (b == 0 ? 5 : 0),
			       content_y + (ED_TOOLH - font_h) / 2,
			       btns[b], live ? 0xdfe4f2 : ED_FAINT, ED_BTNW - 6);
	}
	/* status message / read-only badge shares the strip right of the buttons */
	int tx = lx + 6 + ED_NBTN * (ED_BTNW + 4) + 8;
	int ty = content_y + (ED_TOOLH - font_h) / 2;
	if (e->truncated)
		draw_text_clip(tx, ty, "READ-ONLY: file too large", 0xf38ba8, lx + lw - tx - 6);
	else if (e->status[0])
		draw_text_clip(tx, ty, e->status, 0x94e2d5, lx + lw - tx - 6);

	/* ---- gutter + text ---- */
	int top = content_y + ED_TOOLH;
	int bot = content_y + content_h - ED_STATH;
	int gw = ed_gutter_w(w);
	int txt_x = ed_text_x(w);
	int rows = ed_visible_rows(w);

	fill_rect(lx, top, lw, bot - top, COL_BG_DEFAULT);
	fill_rect(lx, top, gw, bot - top, ED_GUTTER_BG);
	fill_rect(lx + gw, top, 1, bot - top, 0x2a2a40);

	for (int r = 0; r < rows; r++) {
		int i = e->scroll + r;
		if (i >= e->nlines)
			break;
		int ry = top + r * font_h;
		int cur = (i == e->cy);
		if (cur) {
			fill_rect(lx + gw + 1, ry, lw - gw - 1, font_h, ED_CURLINE);
			fill_rect(lx, ry, 3, font_h, accent);
		}
		char num[12];
		snprintf(num, sizeof(num), "%d", i + 1);
		int nx = lx + gw - 8 - (int)strlen(num) * font_w;
		draw_text(nx, ry, num, cur ? accent : ED_FAINT);
		draw_text_clip(txt_x, ry, e->line[i], ED_TXT, lx + lw - txt_x - 4);
	}

	/* caret */
	int crow = e->cy - e->scroll;
	if (crow >= 0 && crow < rows) {
		int cxp = txt_x + e->cx * font_w;
		if (cxp < lx + lw - 2)
			fill_rect(cxp, top + crow * font_h, 2, font_h, 0xf9e2af);
	}

	/* ---- status bar ---- */
	fill_rect(lx, bot, lw, ED_STATH, ED_GUTTER_BG);
	fill_rect(lx, bot, lw, 1, 0x2a2a40);
	int sty = bot + (ED_STATH - font_h) / 2;
	char left[64];
	snprintf(left, sizeof(left), "Ln %d, Col %d", e->cy + 1, e->cx + 1);
	draw_text(lx + 10, sty, left, ED_DIM);
	char right[80];
	snprintf(right, sizeof(right), "%d lines   %s", e->nlines,
		 e->truncated ? "read-only" : e->dirty ? "modified" : "saved");
	draw_text_clip(lx + lw - 10 - (int)strlen(right) * font_w, sty, right,
		       e->dirty && !e->truncated ? 0xf9e2af : ED_DIM, lw / 2);

	/* resize grip */
	if (!w->maximized)
		for (int k = 0; k < 3; k++) {
			int gx = w->x + w->w - 5 - k * 4;
			int gy = w->y + w->h - 5;
			for (int m = 0; m <= k; m++)
				fill_rect(gx, gy - m * 4, 2, 2, ED_DIM);
		}
}

int spawn_editor(const char *path)
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
	wins[slot].w = 640;
	wins[slot].h = 440;
	wins[slot].attr_fg = COL_FG_DEFAULT;
	wins[slot].attr_bg = COL_BG_DEFAULT;
	ed_render(&wins[slot]);
	zorder[zcount++] = slot;
	focused = slot;
	return slot;
}
