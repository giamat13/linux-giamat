/* fbdesktop -- window records: z-order, focus, open/close, maximize, clamping,
 * and the window frame itself */
#include "fbdesktop.h"

void raise_window(int i)
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

void close_window(int i)
{
	if (!wins[i].used)
		return;
	if (wins[i].type == WIN_BROWSER)
		browser_teardown();
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
		ed_free(wins[i].ed);
		free(wins[i].ed);
		wins[i].ed = NULL;
	}
	if (wins[i].calc) {
		free(wins[i].calc);
		wins[i].calc = NULL;
	}
	if (wins[i].paint) {
		if (paint_win == i)
			paint_win = -1; /* a stroke can't outlive its canvas */
		free(wins[i].paint);
		wins[i].paint = NULL;
	}
	if (wins[i].cal) {
		free(wins[i].cal);
		wins[i].cal = NULL;
	}
	if (wins[i].timer) {
		free(wins[i].timer);
		wins[i].timer = NULL;
	}
	if (wins[i].search) {
		free(wins[i].search);
		wins[i].search = NULL;
	}
	if (wins[i].img) {
		free(wins[i].img->rgb);
		free(wins[i].img);
		wins[i].img = NULL;
	}
	if (wins[i].arc) {
		free(wins[i].arc);
		wins[i].arc = NULL;
	}
	if (wins[i].shot) {
		free(wins[i].shot);
		wins[i].shot = NULL;
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
	if (ctxmenu_mode == CTXMODE_FILEWIN && ctxmenu_win == i) {
		ctxmenu_win = -1;
		ctxmenu_mode = CTXMODE_NONE;
	}
	if (fmdrag_win == i) {
		fmdrag_win = -1;
		fmdrag_entidx = -1;
		fmdrag_active = 0;
	}
}

void toggle_maximize(int i)
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

int alloc_window_slot(void)
{
	for (int i = 0; i < MAX_WIN; i++)
		if (!wins[i].used)
			return i;
	return -1;
}

/* Small colored dot per window type, drawn in the titlebar. */
uint32_t win_accent(const struct window *w)
{
	if (w->type == WIN_TERM)  return 0x9333ea;
	if (w->type == WIN_FILES) return 0x06b6d4;
	if (w->type == WIN_EDIT)  return 0xa6e3a1;
	return themes[theme_idx].accent;
}

void draw_window(struct window *w)
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

	if (w->type == WIN_BROWSER) {
		draw_browser(w);
		return; /* the X server owns these pixels; we only copy them */
	}

	if (w->type == WIN_SETTINGS) {
		fill_rect(w->x, content_y, w->w, content_h, COL_BG_DEFAULT);
		draw_settings(w, content_y);
		return; /* no grid, no resize grip -- settings is a fixed panel */
	}

	/* file manager draws its own toolbar + icon listing (no grid) */
	if (w->type == WIN_FILES && w->fm) {
		draw_files(w, content_y, content_h);
		return;
	}

	/* task manager draws its own sidebar + panels (no grid) */
	if (w->type == WIN_TASKMGR) {
		draw_taskmgr(w, content_y, content_h);
		return;
	}

	/* text editor draws its own toolbar + gutter + status bar (no grid) */
	if (w->type == WIN_EDIT && w->ed) {
		draw_editor(w, content_y, content_h);
		return;
	}

	if (w->type == WIN_CALC && w->calc) {
		draw_calc(w, content_y, content_h);
		return;
	}

	if (w->type == WIN_PAINT && w->paint) {
		draw_paint(w, content_y, content_h);
		return;
	}

	if (w->type == WIN_CAL && w->cal) {
		draw_cal(w, content_y, content_h);
		return;
	}

	if (w->type == WIN_TIMER && w->timer) {
		draw_timer(w, content_y, content_h);
		return;
	}

	if (w->type == WIN_SEARCH && w->search) {
		draw_search(w, content_y, content_h);
		return;
	}

	if (w->type == WIN_IMGVIEW && w->img) {
		draw_imgview(w, content_y, content_h);
		return;
	}

	if (w->type == WIN_ARCHIVE && w->arc) {
		draw_archive(w, content_y, content_h);
		return;
	}

	if (w->type == WIN_SHOT && w->shot) {
		draw_shot(w, content_y, content_h);
		return;
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
	if (w->type == WIN_TERM) {
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

void clamp_window(struct window *w)
{
	if (w->x < -w->w + 40) w->x = -w->w + 40;
	if (w->y < 0) w->y = 0;
	if (w->x > xres - 40) w->x = xres - 40;
	if (w->y > yres - TASK_H - TITLE_H) w->y = yres - TASK_H - TITLE_H;
}
