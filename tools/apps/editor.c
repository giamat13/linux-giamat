/* fbdesktop -- text editor window */
#include "fbdesktop.h"

/* ---- text editor ---- */

void ed_render(struct window *w)
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

int ed_keys(struct window *w, const char *buf, int n)
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

void ed_click(struct window *w, int x, int y)
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
