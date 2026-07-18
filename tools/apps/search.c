/* fbdesktop -- File Search: a plain recursive filename search under one root.
 * ponytail: root is fixed to /root (the only writable area a normal session
 * touches); make it editable if searching elsewhere is ever needed. */
#include "fbdesktop.h"

#define SR_BG    0x1e1e2e
#define SR_FIELD 0x181826
#define SR_TXT   0xcdd6f4
#define SR_DIM   0x6c7086
#define SR_ROW   0x232338
#define SR_ROWALT 0x1e1e2c

#define SR_TOOLH 36
#define SR_ROWH  (font_h + 10)

static int ci_contains(const char *hay, const char *needle)
{
	size_t nl = strlen(needle);
	if (!nl)
		return 1;
	for (const char *p = hay; *p; p++) {
		size_t i = 0;
		while (i < nl && p[i] && tolower((unsigned char)p[i]) == tolower((unsigned char)needle[i]))
			i++;
		if (i == nl)
			return 1;
	}
	return 0;
}

/* Recurse depth-first, skipping symlinks (no cycle risk) and stopping once
 * the result cap is hit. */
static void search_walk(struct searchstate *s, const char *dir)
{
	if (s->nres >= SEARCH_MAXRES)
		return;
	DIR *d = opendir(dir);
	if (!d)
		return;
	struct dirent *de;
	while ((de = readdir(d)) && s->nres < SEARCH_MAXRES) {
		if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, ".."))
			continue;
		char path[FM_FULLLEN];
		snprintf(path, sizeof(path), "%s/%s", dir, de->d_name);
		struct stat st;
		if (lstat(path, &st) != 0 || S_ISLNK(st.st_mode))
			continue;
		if (ci_contains(de->d_name, s->pattern)) {
			struct searchresult *r = &s->res[s->nres++];
			snprintf(r->path, sizeof(r->path), "%s", path);
			r->isdir = S_ISDIR(st.st_mode);
		}
		if (S_ISDIR(st.st_mode))
			search_walk(s, path);
	}
	closedir(d);
}

static void search_run(struct searchstate *s)
{
	s->nres = 0;
	s->scroll = 0;
	if (!s->pattern[0]) {
		snprintf(s->status, sizeof(s->status), "type a name to search for");
		return;
	}
	search_walk(s, s->root);
	if (s->nres >= SEARCH_MAXRES)
		snprintf(s->status, sizeof(s->status), "%d+ matches (capped)", s->nres);
	else
		snprintf(s->status, sizeof(s->status), "%d match%s", s->nres, s->nres == 1 ? "" : "es");
}

/* ---- geometry --------------------------------------------------------- */

static int search_visible_rows(struct window *w, int content_h)
{
	int h = content_h - SR_TOOLH;
	return h > 0 ? h / SR_ROWH : 0;
}

/* ---- input -------------------------------------------------------------- */

void search_click(struct window *w, int px, int py)
{
	struct searchstate *s = w->search;
	int content_y = w->y + TITLE_H;

	if (py >= content_y && py < content_y + SR_TOOLH) {
		int btn_w = 80;
		if (px >= w->x + w->w - btn_w) {
			search_run(s);
		} else {
			s->typing = 1;
		}
		return;
	}
	s->typing = 0;

	int row = (py - (content_y + SR_TOOLH)) / SR_ROWH + s->scroll;
	if (row < 0 || row >= s->nres)
		return;
	struct searchresult *r = &s->res[row];

	char dir[FM_PATHLEN];
	if (r->isdir) {
		snprintf(dir, sizeof(dir), "%s", r->path);
	} else {
		snprintf(dir, sizeof(dir), "%s", r->path);
		char *slash = strrchr(dir, '/');
		if (slash && slash != dir)
			*slash = 0;
		else
			strcpy(dir, "/");
	}
	int slot = spawn_file_window();
	if (slot >= 0 && wins[slot].fm) {
		snprintf(wins[slot].fm->cwd, sizeof(wins[slot].fm->cwd), "%s", dir);
		fm_load(&wins[slot]);
	}
}

void search_scroll(struct window *w, int value)
{
	struct searchstate *s = w->search;
	int content_h = w->h - TITLE_H;
	int maxscroll = s->nres - search_visible_rows(w, content_h);
	if (maxscroll < 0)
		maxscroll = 0;
	s->scroll -= value;
	if (s->scroll < 0) s->scroll = 0;
	if (s->scroll > maxscroll) s->scroll = maxscroll;
}

int search_keys(struct window *w, const char *buf, int n)
{
	struct searchstate *s = w->search;
	if (!s->typing)
		return 0;
	int changed = 0;
	for (int i = 0; i < n; i++) {
		unsigned char ch = (unsigned char)buf[i];
		int len = (int)strlen(s->pattern);
		if (ch == '\r' || ch == '\n') {
			search_run(s);
			changed = 1;
		} else if (ch == 0x7f || ch == 0x08) {
			if (len > 0) {
				s->pattern[len - 1] = 0;
				changed = 1;
			}
		} else if (ch >= 0x20 && ch < 0x7f && len < (int)sizeof(s->pattern) - 1) {
			s->pattern[len] = (char)ch;
			s->pattern[len + 1] = 0;
			changed = 1;
		}
	}
	return changed;
}

/* ---- renderer ------------------------------------------------------------ */

void draw_search(struct window *w, int content_y, int content_h)
{
	struct searchstate *s = w->search;
	uint32_t accent = win_accent(w);

	fill_rect(w->x, content_y, w->w, content_h, SR_BG);

	/* ---- toolbar: pattern field + Search button ---- */
	int btn_w = 80;
	fill_round_rect(w->x + 8, content_y + 5, w->w - btn_w - 16, SR_TOOLH - 10, 5,
			s->typing ? mix(SR_FIELD, accent, 40) : SR_FIELD);
	const char *shown = s->pattern[0] ? s->pattern : "search filename...";
	draw_text_clip(w->x + 14, content_y + (SR_TOOLH - font_h) / 2, shown,
		       s->pattern[0] ? SR_TXT : SR_DIM, w->w - btn_w - 28);

	fill_round_rect_grad(w->x + w->w - btn_w, content_y + 5, btn_w - 8, SR_TOOLH - 10, 5,
			     mix(accent, 0xffffff, 40), accent);
	int lw = (int)strlen("Search") * font_w;
	draw_text(w->x + w->w - btn_w + (btn_w - 8 - lw) / 2, content_y + (SR_TOOLH - font_h) / 2,
		  "Search", 0x11111c);

	/* ---- results ---- */
	int list_y = content_y + SR_TOOLH;
	int visible = search_visible_rows(w, content_h);
	if (s->nres == 0) {
		draw_text(w->x + 14, list_y + 6, s->status[0] ? s->status : "type a name and press Search",
			  SR_DIM);
		return;
	}
	for (int i = 0; i < visible && s->scroll + i < s->nres; i++) {
		struct searchresult *r = &s->res[s->scroll + i];
		int ry = list_y + i * SR_ROWH;
		fill_rect(w->x, ry, w->w, SR_ROWH, (i % 2) ? SR_ROWALT : SR_ROW);
		fill_circle(w->x + 14, ry + SR_ROWH / 2, 4, r->isdir ? 0x89b4fa : SR_DIM);
		draw_text_clip(w->x + 26, ry + (SR_ROWH - font_h) / 2, r->path, SR_TXT, w->w - 36);
	}
}

int spawn_search(void)
{
	for (int i = 0; i < MAX_WIN; i++) {
		if (wins[i].used && wins[i].type == WIN_SEARCH) {
			wins[i].minimized = 0;
			raise_window(i);
			focused = i;
			return i;
		}
	}
	int slot = alloc_window_slot();
	if (slot < 0)
		return -1;
	struct searchstate *s = calloc(1, sizeof(struct searchstate));
	if (!s)
		return -1;
	snprintf(s->root, sizeof(s->root), "/root");
	memset(&wins[slot], 0, sizeof(wins[slot]));
	wins[slot].used = 1;
	wins[slot].type = WIN_SEARCH;
	wins[slot].pty_fd = -1;
	wins[slot].search = s;
	wins[slot].x = 260;
	wins[slot].y = 100;
	wins[slot].w = 420;
	wins[slot].h = 340;
	wins[slot].attr_fg = COL_FG_DEFAULT;
	wins[slot].attr_bg = COL_BG_DEFAULT;
	snprintf(wins[slot].title, sizeof(wins[slot].title), "File Search");
	zorder[zcount++] = slot;
	focused = slot;
	return slot;
}
