/* fbdesktop -- Disk Usage: one bar per immediate child of the current path,
 * each sized by a full recursive walk (like `du --max-depth=1`). Clicking a
 * directory bar descends into it; the Up button in the toolbar climbs back.
 *
 * ponytail: /proc, /sys, /dev, /run are reported as 0 rather than walked --
 * they're pseudo-filesystems, not real disk usage, and /proc in particular
 * can be slow to fully recurse. */
#include "fbdesktop.h"

#define DU_BG    0x1e1e2e
#define DU_BAR   0x232338
#define DU_TXT   0xcdd6f4
#define DU_DIM   0x6c7086
#define DU_TOOLH 32
#define DU_ROWH  (font_h + 14)

static void human_size(long long b, char *out, size_t n)
{
	if (b < 1024)
		snprintf(out, n, "%lld B", b);
	else if (b < 1024LL * 1024)
		snprintf(out, n, "%.1f K", b / 1024.0);
	else if (b < 1024LL * 1024 * 1024)
		snprintf(out, n, "%.1f M", b / (1024.0 * 1024));
	else
		snprintf(out, n, "%.1f G", b / (1024.0 * 1024 * 1024));
}

static int is_pseudofs(const char *path)
{
	return !strcmp(path, "/proc") || !strcmp(path, "/sys") ||
	       !strcmp(path, "/dev") || !strcmp(path, "/run");
}

static long long du_size(const char *path, int depth)
{
	if (depth > 60 || is_pseudofs(path))
		return 0;
	struct stat st;
	if (lstat(path, &st) != 0 || S_ISLNK(st.st_mode))
		return 0;
	if (!S_ISDIR(st.st_mode))
		return st.st_size;
	DIR *d = opendir(path);
	if (!d)
		return 0;
	long long total = 0;
	struct dirent *de;
	while ((de = readdir(d))) {
		if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, ".."))
			continue;
		char child[FM_FULLLEN];
		snprintf(child, sizeof(child), "%s/%s", path, de->d_name);
		total += du_size(child, depth + 1);
	}
	closedir(d);
	return total;
}

static int cmp_duent(const void *a, const void *b)
{
	const struct duent *p = a, *q = b;
	return (q->size > p->size) - (q->size < p->size);
}

static void du_scan(struct dustate *s)
{
	s->count = 0;
	s->scroll = 0;
	s->total = 0;
	DIR *d = opendir(s->path);
	if (!d) {
		snprintf(s->status, sizeof(s->status), "cannot open: %s", strerror(errno));
		return;
	}
	struct dirent *de;
	while ((de = readdir(d)) && s->count < DU_MAXENT) {
		if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, ".."))
			continue;
		char child[FM_FULLLEN];
		snprintf(child, sizeof(child), "%s/%s", s->path, de->d_name);
		struct stat st;
		if (lstat(child, &st) != 0 || S_ISLNK(st.st_mode))
			continue;
		struct duent *e = &s->ents[s->count++];
		snprintf(e->name, sizeof(e->name), "%s", de->d_name);
		e->isdir = S_ISDIR(st.st_mode);
		e->size = du_size(child, 0);
		s->total += e->size;
	}
	closedir(d);
	qsort(s->ents, s->count, sizeof(s->ents[0]), cmp_duent);
	snprintf(s->status, sizeof(s->status), "%d item%s", s->count, s->count == 1 ? "" : "s");
}

/* ---- input -------------------------------------------------------------- */

void du_click(struct window *w, int px, int py)
{
	struct dustate *s = w->du;
	int content_y = w->y + TITLE_H;

	if (py >= content_y && py < content_y + DU_TOOLH) {
		int btn_w = 60;
		if (px < w->x + 8 + btn_w) {
			char *slash = strrchr(s->path, '/');
			if (slash && slash != s->path)
				*slash = 0;
			else
				strcpy(s->path, "/");
			du_scan(s);
		}
		return;
	}

	int row = (py - (content_y + DU_TOOLH)) / DU_ROWH + s->scroll;
	if (row < 0 || row >= s->count)
		return;
	struct duent *e = &s->ents[row];
	if (!e->isdir)
		return;
	char child[FM_PATHLEN];
	snprintf(child, sizeof(child), "%s/%s", s->path, e->name);
	if (strlen(child) >= sizeof(s->path))
		return;
	memcpy(s->path, child, strlen(child) + 1);
	du_scan(s);
}

/* ---- renderer ------------------------------------------------------------ */

void draw_diskusage(struct window *w, int content_y, int content_h)
{
	struct dustate *s = w->du;
	uint32_t accent = win_accent(w);

	fill_rect(w->x, content_y, w->w, content_h, DU_BG);

	/* ---- toolbar: Up button + breadcrumb ---- */
	int btn_w = 60;
	fill_round_rect_grad(w->x + 8, content_y + 4, btn_w, DU_TOOLH - 8, 5, 0x33334a, 0x2b2b3a);
	int lw = (int)strlen("Up") * font_w;
	draw_text(w->x + 8 + (btn_w - lw) / 2, content_y + (DU_TOOLH - font_h) / 2, "Up", DU_TXT);
	draw_text_clip(w->x + 8 + btn_w + 10, content_y + (DU_TOOLH - font_h) / 2, s->path,
		       DU_DIM, w->w - btn_w - 24);

	int list_y = content_y + DU_TOOLH;
	int visible = (content_h - DU_TOOLH) > 0 ? (content_h - DU_TOOLH) / DU_ROWH : 0;
	if (s->count == 0) {
		draw_text(w->x + 14, list_y + 6, s->status, DU_DIM);
		return;
	}
	long long maxsz = s->ents[0].size ? s->ents[0].size : 1;

	for (int i = 0; i < visible && s->scroll + i < s->count; i++) {
		struct duent *e = &s->ents[s->scroll + i];
		int ry = list_y + i * DU_ROWH;
		char sz[16];
		human_size(e->size, sz, sizeof(sz));

		int namew = 160;
		int barx = w->x + 12 + namew, barmax = w->w - namew - 90;
		draw_text_clip(w->x + 12, ry + (DU_ROWH - font_h) / 2, e->name,
			       e->isdir ? DU_TXT : DU_DIM, namew - 8);
		int bw = (int)((double)e->size / maxsz * barmax);
		if (bw < 3)
			bw = 3;
		fill_round_rect(barx, ry + DU_ROWH / 2 - 6, barmax, 12, 3, DU_BAR);
		fill_round_rect(barx, ry + DU_ROWH / 2 - 6, bw, 12, 3,
				e->isdir ? accent : mix(accent, 0x000000, 100));
		draw_text(w->x + w->w - 70, ry + (DU_ROWH - font_h) / 2, sz, DU_TXT);
	}
}

int spawn_diskusage(void)
{
	for (int i = 0; i < MAX_WIN; i++) {
		if (wins[i].used && wins[i].type == WIN_DISKUSAGE) {
			wins[i].minimized = 0;
			raise_window(i);
			focused = i;
			return i;
		}
	}
	int slot = alloc_window_slot();
	if (slot < 0)
		return -1;
	struct dustate *s = calloc(1, sizeof(struct dustate));
	if (!s)
		return -1;
	snprintf(s->path, sizeof(s->path), "/root");
	memset(&wins[slot], 0, sizeof(wins[slot]));
	wins[slot].used = 1;
	wins[slot].type = WIN_DISKUSAGE;
	wins[slot].pty_fd = -1;
	wins[slot].du = s;
	wins[slot].x = 300;
	wins[slot].y = 100;
	wins[slot].w = 460;
	wins[slot].h = 360;
	wins[slot].attr_fg = COL_FG_DEFAULT;
	wins[slot].attr_bg = COL_BG_DEFAULT;
	snprintf(wins[slot].title, sizeof(wins[slot].title), "Disk Usage");
	zorder[zcount++] = slot;
	focused = slot;
	du_scan(s);
	return slot;
}
