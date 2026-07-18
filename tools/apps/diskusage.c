/* fbdesktop -- Folder Usage: a recursive per-directory size scan (like `du
 * --max-depth=1`), rendered as one bar per immediate child. Lives in its own
 * file because the scan is a distinct piece of work from the rest of the
 * task manager, but it draws into whatever rectangle the Task Manager's Disk
 * tab (taskmgr.c, view_disk) hands it -- there is no standalone window here
 * any more; the per-filesystem statvfs view already covered "how full is the
 * disk", so this only adds "which folder is using it", right below.
 *
 * ponytail: /proc, /sys, /dev, /run are reported as 0 rather than walked --
 * they're pseudo-filesystems, not real disk usage, and /proc in particular
 * can be slow to fully recurse. */
#include "fbdesktop.h"

#define DU_BAR   0x232338
#define DU_TXT   0xcdd6f4
#define DU_DIM   0x6c7086
#define DU_TOOLH 26
#define DU_ROWH  (font_h + 10)

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

void du_scan(struct dustate *s)
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

/* ---- input: (x,y,w,h) is the same rectangle draw_du_panel was given -------- */

int du_panel_click(struct dustate *s, int rx, int ry, int rw, int rh, int px, int py)
{
	if (px < rx || px >= rx + rw || py < ry || py >= ry + rh)
		return 0;

	if (py < ry + DU_TOOLH) {
		int btn_w = 50;
		if (px < rx + btn_w) {
			char *slash = strrchr(s->path, '/');
			if (slash && slash != s->path)
				*slash = 0;
			else
				strcpy(s->path, "/");
			du_scan(s);
		}
		return 1;
	}

	int row = (py - (ry + DU_TOOLH)) / DU_ROWH + s->scroll;
	if (row < 0 || row >= s->count)
		return 1;
	struct duent *e = &s->ents[row];
	if (!e->isdir)
		return 1;
	char child[FM_PATHLEN];
	snprintf(child, sizeof(child), "%s/%s", s->path, e->name);
	if (strlen(child) < sizeof(s->path)) {
		memcpy(s->path, child, strlen(child) + 1);
		du_scan(s);
	}
	return 1;
}

/* ---- renderer: draws only within (x,y,w,h), never touches the rest of the tab */

void draw_du_panel(struct dustate *s, uint32_t accent, int x, int y, int w, int h)
{
	draw_glyph(G_DISK, x + 14, y + 14, accent, mix(accent, 0x000000, 78));
	int btn_w = 50;
	fill_round_rect_grad(x + 32, y + 2, btn_w, DU_TOOLH - 4, 5, 0x33334a, 0x2b2b3a);
	int lw = (int)strlen("Up") * font_w;
	draw_text(x + 32 + (btn_w - lw) / 2, y + (DU_TOOLH - font_h) / 2, "Up", DU_TXT);
	draw_text_clip(x + 32 + btn_w + 10, y + (DU_TOOLH - font_h) / 2, s->path, DU_DIM, w - btn_w - 60);

	int list_y = y + DU_TOOLH;
	int list_h = h - DU_TOOLH;
	int visible = list_h > 0 ? list_h / DU_ROWH : 0;
	if (s->count == 0) {
		draw_text(x, list_y + 6, s->status, DU_DIM);
		return;
	}
	long long maxsz = s->ents[0].size ? s->ents[0].size : 1;

	for (int i = 0; i < visible && s->scroll + i < s->count; i++) {
		struct duent *e = &s->ents[s->scroll + i];
		int ry = list_y + i * DU_ROWH;
		char sz[16];
		human_size(e->size, sz, sizeof(sz));

		int namew = 150;
		int barx = x + namew, barmax = w - namew - 80;
		draw_text_clip(x, ry + (DU_ROWH - font_h) / 2, e->name,
			       e->isdir ? DU_TXT : DU_DIM, namew - 8);
		int bw = barmax > 0 ? (int)((double)e->size / maxsz * barmax) : 0;
		if (bw < 3)
			bw = 3;
		if (barmax > 0) {
			fill_round_rect(barx, ry + DU_ROWH / 2 - 5, barmax, 10, 3, DU_BAR);
			fill_round_rect(barx, ry + DU_ROWH / 2 - 5, bw, 10, 3,
					e->isdir ? accent : mix(accent, 0x000000, 100));
		}
		draw_text(x + w - 64, ry + (DU_ROWH - font_h) / 2, sz, DU_TXT);
	}
}
