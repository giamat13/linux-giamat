/* fbdesktop -- the character grid and the VT100-ish parser behind it, shared by
 * every window type: live terminals, one-shot output, file manager, editor. */
#include "fbdesktop.h"

/* ---- character-grid terminal model, shared by live terminals and
 * one-shot command-output windows ---- */

void update_grid_dims(struct window *w)
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


void resize_notify(struct window *w)
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

void clear_row_range(struct window *w, int row, int from, int to)
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
void process_bytes(struct window *w, unsigned char *buf, int n)
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

/* Run a command and paint its stdout into the grid (one-shot). */
void fill_grid_from_cmd(struct window *w, const char *cmd)
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
