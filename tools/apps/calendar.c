/* fbdesktop -- calendar: a month grid on the left, the selected day's events on
 * the right. Events are typed into the panel and persisted one per line in
 * CAL_FILE, so they survive a reboot of the live image. */
#include "fbdesktop.h"

#define K_BAR   0x181826
#define K_CARD  0x232338
#define K_TXT   0xcdd6f4
#define K_DIM   0x9399b2
#define K_FAINT 0x6c7086
#define K_LINE  0x2a2a40

static const char *months[13] = { "",
	"January", "February", "March", "April", "May", "June",
	"July", "August", "September", "October", "November", "December" };
static const char *wdays[7] = { "Su", "Mo", "Tu", "We", "Th", "Fr", "Sa" };

/* ---- date helpers --------------------------------------------------- */

static int leap(int y) { return (y % 4 == 0 && y % 100 != 0) || y % 400 == 0; }

static int days_in(int y, int m)
{
	static const int d[13] = { 0, 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
	return (m == 2 && leap(y)) ? 29 : d[m];
}

/* Weekday of the 1st, 0 = Sunday. mktime normalises and fills tm_wday. */
static int first_wday(int y, int m)
{
	struct tm tm;
	memset(&tm, 0, sizeof(tm));
	tm.tm_year = y - 1900;
	tm.tm_mon = m - 1;
	tm.tm_mday = 1;
	tm.tm_hour = 12; /* noon: never straddles a DST boundary */
	tm.tm_isdst = -1;
	if (mktime(&tm) == (time_t)-1)
		return 0;
	return tm.tm_wday;
}

static void today(int *y, int *m, int *d)
{
	time_t t = time(NULL);
	struct tm *tm = localtime(&t);
	if (!tm) {
		*y = 2026; *m = 1; *d = 1;
		return;
	}
	*y = tm->tm_year + 1900;
	*m = tm->tm_mon + 1;
	*d = tm->tm_mday;
}

/* ---- event store ---------------------------------------------------- */

static void cal_load(struct calstate *c)
{
	c->nev = 0;
	FILE *f = fopen(CAL_FILE, "r");
	if (!f)
		return;
	char line[128];
	while (c->nev < CAL_MAXEV && fgets(line, sizeof(line), f)) {
		struct calevent *e = &c->ev[c->nev];
		char text[CAL_TEXTLEN];
		if (sscanf(line, "%d-%d-%d\t%63[^\n]", &e->y, &e->m, &e->d, text) != 4)
			continue;
		snprintf(e->text, sizeof(e->text), "%s", text);
		c->nev++;
	}
	fclose(f);
}

static void cal_save(struct calstate *c)
{
	FILE *f = fopen(CAL_FILE, "w");
	if (!f) {
		snprintf(c->status, sizeof(c->status), "save failed: %s", strerror(errno));
		return;
	}
	for (int i = 0; i < c->nev; i++)
		fprintf(f, "%04d-%02d-%02d\t%s\n", c->ev[i].y, c->ev[i].m,
			c->ev[i].d, c->ev[i].text);
	if (fclose(f) != 0)
		snprintf(c->status, sizeof(c->status), "save failed: %s", strerror(errno));
}

static int day_has_events(struct calstate *c, int y, int m, int d)
{
	for (int i = 0; i < c->nev; i++)
		if (c->ev[i].y == y && c->ev[i].m == m && c->ev[i].d == d)
			return 1;
	return 0;
}

/* Fill idx[] with the events of the selected day; returns how many. */
static int day_events(struct calstate *c, int *idx, int max)
{
	int n = 0;
	for (int i = 0; i < c->nev && n < max; i++)
		if (c->ev[i].y == c->sy && c->ev[i].m == c->sm && c->ev[i].d == c->sd)
			idx[n++] = i;
	return n;
}

static void cal_add(struct calstate *c)
{
	if (!c->input[0])
		return;
	if (c->nev >= CAL_MAXEV) {
		snprintf(c->status, sizeof(c->status), "calendar is full");
		return;
	}
	struct calevent *e = &c->ev[c->nev++];
	e->y = c->sy;
	e->m = c->sm;
	e->d = c->sd;
	snprintf(e->text, sizeof(e->text), "%s", c->input);
	c->input[0] = 0;
	c->typing = 0;
	cal_save(c);
	snprintf(c->status, sizeof(c->status), "added");
}

static void cal_del(struct calstate *c)
{
	int idx[CAL_MAXEV];
	int n = day_events(c, idx, CAL_MAXEV);
	if (c->sel_ev < 0 || c->sel_ev >= n)
		return;
	int victim = idx[c->sel_ev];
	memmove(&c->ev[victim], &c->ev[victim + 1],
		(size_t)(c->nev - victim - 1) * sizeof(c->ev[0]));
	c->nev--;
	c->sel_ev = -1;
	cal_save(c);
	snprintf(c->status, sizeof(c->status), "deleted");
}

/* ---- geometry, shared by the renderer and the hit-test -------------- */

static int grid_w(struct window *w) { return w->w - CAL_PANW; }

static void cell_size(struct window *w, int content_h, int *cw, int *chh)
{
	*cw = (grid_w(w) - 12) / 7;
	*chh = (content_h - CAL_HEADH - CAL_WDH - 12) / 6;
}

/* ---- input ---------------------------------------------------------- */

static void cal_goto_today(struct calstate *c)
{
	today(&c->sy, &c->sm, &c->sd);
	c->year = c->sy;
	c->month = c->sm;
	c->sel_ev = -1;
}

static void cal_step_month(struct calstate *c, int delta)
{
	c->month += delta;
	if (c->month < 1)  { c->month = 12; c->year--; }
	if (c->month > 12) { c->month = 1;  c->year++; }
	c->sel_ev = -1;
}

void cal_click(struct window *w, int px, int py)
{
	struct calstate *c = w->cal;
	int content_y = w->y + TITLE_H, content_h = w->h - TITLE_H;
	c->status[0] = 0;

	/* ---- header: prev / next / today (must match draw_cal's rects) ---- */
	if (py >= content_y && py < content_y + CAL_HEADH && px < w->x + grid_w(w)) {
		int tbw = 8 * font_w;
		if (px >= w->x + 6 && px < w->x + 32)
			cal_step_month(c, -1);
		else if (px >= w->x + 38 && px < w->x + 64)
			cal_step_month(c, 1);
		else if (px >= w->x + grid_w(w) - 6 - tbw && px < w->x + grid_w(w) - 6)
			cal_goto_today(c);
		return;
	}

	/* ---- day grid ---- */
	if (px < w->x + grid_w(w)) {
		int cw, chh;
		cell_size(w, content_h, &cw, &chh);
		if (cw <= 0 || chh <= 0)
			return;
		int gx = w->x + 6, gy = content_y + CAL_HEADH + CAL_WDH;
		int col = (px - gx) / cw, row = (py - gy) / chh;
		if (col < 0 || col >= 7 || row < 0 || row >= 6)
			return;
		int day = row * 7 + col - first_wday(c->year, c->month) + 1;
		if (day < 1 || day > days_in(c->year, c->month))
			return;
		c->sy = c->year;
		c->sm = c->month;
		c->sd = day;
		c->sel_ev = -1;
		c->typing = 0;
		c->input[0] = 0;
		return;
	}

	/* ---- events panel ---- */
	int panx = w->x + grid_w(w);
	int listy = content_y + CAL_HEADH + 8;
	int rowh = font_h + 12;
	int idx[CAL_MAXEV];
	int n = day_events(c, idx, CAL_MAXEV);

	/* the "+ add event" field sits under the list */
	int addy = content_y + content_h - rowh - 8;
	if (py >= addy && py < addy + rowh) {
		c->typing = 1;
		c->sel_ev = -1;
		return;
	}
	if (py >= listy) {
		int r = (py - listy) / rowh;
		if (r >= 0 && r < n) {
			/* clicking the armed row deletes it; the first click only selects */
			if (c->sel_ev == r && px >= panx + CAL_PANW - 24)
				cal_del(c);
			else
				c->sel_ev = r;
		}
	}
}

int cal_keys(struct window *w, const char *buf, int n)
{
	struct calstate *c = w->cal;
	int changed = 0;

	if (c->typing) {
		for (int i = 0; i < n; i++) {
			unsigned char ch = (unsigned char)buf[i];
			int len = (int)strlen(c->input);
			if (ch == '\r' || ch == '\n') {
				cal_add(c);
			} else if (ch == 0x1b) {
				c->typing = 0;
				c->input[0] = 0;
			} else if ((ch == 0x7f || ch == 0x08) && len > 0) {
				c->input[len - 1] = 0;
			} else if (ch >= 0x20 && ch < 0x7f && len < CAL_TEXTLEN - 1) {
				c->input[len] = (char)ch;
				c->input[len + 1] = 0;
			} else {
				continue;
			}
			changed = 1;
			if (!c->typing)
				break;
		}
		return changed;
	}

	for (int i = 0; i < n; i++) {
		unsigned char ch = (unsigned char)buf[i];
		c->status[0] = 0;

		if (ch == 0x1b && i + 2 < n && buf[i + 1] == '[') {
			if (i + 3 < n && buf[i + 3] == '~' && buf[i + 2] == '3') {
				cal_del(c);
				i += 3;
				changed = 1;
				continue;
			}
			int delta = 0;
			switch (buf[i + 2]) {
			case 'A': delta = -7; break;   /* up: a week back */
			case 'B': delta = 7; break;
			case 'C': delta = 1; break;
			case 'D': delta = -1; break;
			case '5': cal_step_month(c, -1); break;
			case '6': cal_step_month(c, 1); break;
			default: break;
			}
			if (delta) {
				/* walk the day across month/year edges */
				int d = c->sd + delta;
				while (d < 1) {
					cal_step_month(c, -1);
					c->sy = c->year; c->sm = c->month;
					d += days_in(c->year, c->month);
				}
				while (d > days_in(c->sy, c->sm)) {
					d -= days_in(c->sy, c->sm);
					cal_step_month(c, 1);
					c->sy = c->year; c->sm = c->month;
				}
				c->sd = d;
				c->year = c->sy;
				c->month = c->sm;
				c->sel_ev = -1;
			}
			i += 2;
			changed = 1;
			continue;
		}

		if (ch == 'n' || ch == '+') { c->typing = 1; c->input[0] = 0; changed = 1; }
		else if (ch == 't') { cal_goto_today(c); changed = 1; }
		else if (ch == 0x7f || ch == 0x08) { cal_del(c); changed = 1; }
	}
	return changed;
}

/* ---- renderer ------------------------------------------------------- */

void draw_cal(struct window *w, int content_y, int content_h)
{
	struct calstate *c = w->cal;
	uint32_t accent = win_accent(w);
	int ty, tm, td;
	today(&ty, &tm, &td);

	fill_rect(w->x, content_y, w->w, content_h, COL_BG_DEFAULT);

	/* ---- header ---- */
	int gw = grid_w(w);
	fill_rect(w->x, content_y, gw, CAL_HEADH, K_BAR);
	fill_round_rect(w->x + 6, content_y + 6, 26, CAL_HEADH - 12, 4, 0x2b2b3a);
	fill_round_rect(w->x + 38, content_y + 6, 26, CAL_HEADH - 12, 4, 0x2b2b3a);
	draw_text(w->x + 6 + 9, content_y + (CAL_HEADH - font_h) / 2, "<", K_TXT);
	draw_text(w->x + 38 + 9, content_y + (CAL_HEADH - font_h) / 2, ">", K_TXT);

	char hdr[48];
	snprintf(hdr, sizeof(hdr), "%s %d", months[c->month], c->year);
	draw_text(w->x + 76, content_y + (CAL_HEADH - font_h) / 2, hdr, 0xffffff);

	int tbw = 8 * font_w;
	fill_round_rect(w->x + gw - 6 - tbw, content_y + 6, tbw, CAL_HEADH - 12, 4, 0x2b2b3a);
	draw_text(w->x + gw - 6 - tbw + font_w, content_y + (CAL_HEADH - font_h) / 2,
		  "Today", K_DIM);

	/* ---- weekday strip ---- */
	int cw, chh;
	cell_size(w, content_h, &cw, &chh);
	int gx = w->x + 6, gy = content_y + CAL_HEADH + CAL_WDH;
	for (int i = 0; i < 7; i++)
		draw_text(gx + i * cw + (cw - 2 * font_w) / 2,
			  content_y + CAL_HEADH + (CAL_WDH - font_h) / 2,
			  wdays[i], i == 0 || i == 6 ? 0xf38ba8 : K_FAINT);
	fill_rect(gx, content_y + CAL_HEADH + CAL_WDH - 1, gw - 12, 1, K_LINE);

	/* ---- day cells ---- */
	int fw = first_wday(c->year, c->month), nd = days_in(c->year, c->month);
	if (cw > 0 && chh > 0)
		for (int i = 0; i < 42; i++) {
			int day = i - fw + 1;
			if (day < 1 || day > nd)
				continue;
			int cx = gx + (i % 7) * cw, cy = gy + (i / 7) * chh;
			int is_sel = (day == c->sd && c->month == c->sm && c->year == c->sy);
			int is_today = (day == td && c->month == tm && c->year == ty);

			if (is_sel)
				fill_round_rect(cx + 2, cy + 2, cw - 4, chh - 4, 5, accent);
			else if (is_today)
				fill_round_rect(cx + 2, cy + 2, cw - 4, chh - 4, 5,
						mix(accent, COL_BG_DEFAULT, 175));

			char ds[4];
			snprintf(ds, sizeof(ds), "%d", day);
			uint32_t dc = is_sel ? 0x11111c : is_today ? 0xffffff : K_TXT;
			draw_text(cx + (cw - (int)strlen(ds) * font_w) / 2,
				  cy + (chh - font_h) / 2 - 3, ds, dc);
			if (day_has_events(c, c->year, c->month, day))
				fill_circle(cx + cw / 2, cy + chh - 8, 2,
					    is_sel ? 0x11111c : accent);
		}

	/* ---- events panel ---- */
	int panx = w->x + gw;
	fill_rect(panx, content_y, CAL_PANW, content_h, K_BAR);
	fill_rect(panx, content_y, 1, content_h, K_LINE);

	char dh[48];
	snprintf(dh, sizeof(dh), "%s %d, %d", months[c->sm], c->sd, c->sy);
	draw_text_clip(panx + 12, content_y + (CAL_HEADH - font_h) / 2, dh, 0xffffff,
		       CAL_PANW - 24);

	int idx[CAL_MAXEV];
	int n = day_events(c, idx, CAL_MAXEV);
	int rowh = font_h + 12;
	int listy = content_y + CAL_HEADH + 8;
	int addy = content_y + content_h - rowh - 8;

	if (n == 0)
		draw_text(panx + 12, listy + 6, "no events", K_FAINT);
	for (int r = 0; r < n && listy + (r + 1) * rowh < addy; r++) {
		int ry = listy + r * rowh;
		int sel = (r == c->sel_ev);
		fill_round_rect(panx + 8, ry, CAL_PANW - 16, rowh - 4, 4,
				sel ? mix(accent, K_BAR, 140) : K_CARD);
		fill_rect(panx + 8, ry, 3, rowh - 4, accent);
		draw_text_clip(panx + 18, ry + 6, c->ev[idx[r]].text,
			       sel ? 0xffffff : K_TXT, CAL_PANW - 50);
		if (sel) /* armed: the little x deletes it */
			draw_text(panx + CAL_PANW - 20, ry + 6, "x", 0xf38ba8);
	}

	/* ---- add field ---- */
	fill_round_rect(panx + 8, addy, CAL_PANW - 16, rowh - 4, 4,
			c->typing ? 0x2b2b3a : K_CARD);
	if (c->typing) {
		char line[CAL_TEXTLEN + 2];
		snprintf(line, sizeof(line), "%s_", c->input);
		draw_text_clip(panx + 16, addy + 6, line, 0xf9e2af, CAL_PANW - 32);
	} else {
		draw_text(panx + 16, addy + 6, "+ add event", K_FAINT);
	}
	if (c->status[0])
		draw_text_clip(panx + 12, addy - font_h - 4, c->status, 0x94e2d5,
			       CAL_PANW - 24);

	/* resize grip */
	if (!w->maximized)
		for (int k = 0; k < 3; k++) {
			int px = w->x + w->w - 5 - k * 4;
			int py = w->y + w->h - 5;
			for (int m = 0; m <= k; m++)
				fill_rect(px, py - m * 4, 2, 2, K_FAINT);
		}
}

int spawn_cal(void)
{
	for (int i = 0; i < MAX_WIN; i++) {
		if (wins[i].used && wins[i].type == WIN_CAL) {
			wins[i].minimized = 0;
			raise_window(i);
			focused = i;
			return i;
		}
	}
	int slot = alloc_window_slot();
	if (slot < 0)
		return -1;
	struct calstate *c = calloc(1, sizeof(struct calstate));
	if (!c)
		return -1;
	cal_goto_today(c);
	c->sel_ev = -1;
	cal_load(c);
	memset(&wins[slot], 0, sizeof(wins[slot]));
	wins[slot].used = 1;
	wins[slot].type = WIN_CAL;
	wins[slot].pty_fd = -1;
	wins[slot].cal = c;
	wins[slot].x = 200;
	wins[slot].y = 90;
	wins[slot].w = 700;
	wins[slot].h = 460;
	wins[slot].attr_fg = COL_FG_DEFAULT;
	wins[slot].attr_bg = COL_BG_DEFAULT;
	snprintf(wins[slot].title, sizeof(wins[slot].title), "Calendar");
	zorder[zcount++] = slot;
	focused = slot;
	return slot;
}
