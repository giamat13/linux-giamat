/* fbdesktop -- task manager: tabs, auto-refresh, and the CPU/memory sampler + graphs */
#include "fbdesktop.h"

/* ---- task manager: one window, tabs, auto-refresh ---- */

void taskmgr_refresh(struct window *w)
{
	if (w->tab < 0 || w->tab >= TM_NTABS)
		w->tab = 0;
	update_grid_dims(w); /* the Performance tab gives up rows to the graphs */
	fill_grid_from_cmd(w, tm_tabs[w->tab].cmd);
	snprintf(w->title, sizeof(w->title), "Task Manager  -  %s", tm_tabs[w->tab].label);
}

int spawn_taskmgr(void)
{
	/* single-instance: focus the existing one instead of opening a second */
	for (int i = 0; i < MAX_WIN; i++) {
		if (wins[i].used && wins[i].type == WIN_TASKMGR) {
			wins[i].minimized = 0;
			raise_window(i);
			focused = i;
			return i;
		}
	}
	int slot = alloc_window_slot();
	if (slot < 0)
		return -1;
	memset(&wins[slot], 0, sizeof(wins[slot]));
	wins[slot].used = 1;
	wins[slot].type = WIN_TASKMGR;
	wins[slot].pty_fd = -1;
	wins[slot].x = 180;
	wins[slot].y = 90;
	wins[slot].w = 700;
	wins[slot].h = 460;
	wins[slot].attr_fg = COL_FG_DEFAULT;
	wins[slot].attr_bg = COL_BG_DEFAULT;
	wins[slot].tab = 0;
	update_grid_dims(&wins[slot]);
	taskmgr_refresh(&wins[slot]);
	zorder[zcount++] = slot;
	focused = slot;
	return slot;
}

/* Tab strip hit-test: returns clicked tab index or -1. */
int taskmgr_tab_at(struct window *w, int px, int py)
{
	int ty = w->y + TITLE_H;
	if (py < ty || py >= ty + TM_TABH)
		return -1;
	int tw = w->w / TM_NTABS;
	int idx = (px - w->x) / tw;
	if (idx < 0 || idx >= TM_NTABS)
		return -1;
	return idx;
}

/* ---- CPU / memory history ---- */

static long read_meminfo_kb(const char *key)
{
	FILE *f = fopen("/proc/meminfo", "r");
	if (!f)
		return 0;
	char line[128];
	long val = 0;
	size_t klen = strlen(key);
	while (fgets(line, sizeof(line), f)) {
		if (!strncmp(line, key, klen) && line[klen] == ':') {
			val = strtol(line + klen + 1, NULL, 10);
			break;
		}
	}
	fclose(f);
	return val;
}

/* Called once a second. Percentages, 0..100. */
void sample_stats(void)
{
	static long prev_busy, prev_total;
	int cpu = 0, mem = 0;

	FILE *f = fopen("/proc/stat", "r");
	if (f) {
		long v[8] = {0};
		if (fscanf(f, "cpu %ld %ld %ld %ld %ld %ld %ld %ld",
			   &v[0], &v[1], &v[2], &v[3], &v[4], &v[5], &v[6], &v[7]) >= 4) {
			long total = 0;
			for (int i = 0; i < 8; i++)
				total += v[i];
			long busy = total - v[3] - v[4]; /* minus idle and iowait */
			long dt = total - prev_total, db = busy - prev_busy;
			if (prev_total && dt > 0)
				cpu = (int)(db * 100 / dt);
			prev_total = total;
			prev_busy = busy;
		}
		fclose(f);
	}

	long tot = read_meminfo_kb("MemTotal");
	long avail = read_meminfo_kb("MemAvailable");
	if (tot > 0 && avail > 0)
		mem = (int)((tot - avail) * 100 / tot);

	if (cpu < 0) cpu = 0;
	if (cpu > 100) cpu = 100;
	memmove(cpu_hist, cpu_hist + 1, sizeof(cpu_hist) - sizeof(cpu_hist[0]));
	memmove(mem_hist, mem_hist + 1, sizeof(mem_hist) - sizeof(mem_hist[0]));
	cpu_hist[HIST - 1] = cpu;
	mem_hist[HIST - 1] = mem;
}

void draw_graph(int x, int y, int w, int h, const int *hist,
		       uint32_t col, const char *label)
{
	fill_round_rect(x, y, w, h, 4, 0x181826);
	for (int g = 1; g < 4; g++)
		fill_rect(x + 1, y + g * h / 4, w - 2, 1, 0x272740);

	int bw = w / HIST;
	if (bw < 1) bw = 1;
	for (int i = 0; i < HIST; i++) {
		int bh = hist[i] * (h - 2) / 100;
		int bx = x + 1 + i * (w - 2) / HIST;
		if (bh > 0)
			fill_rect(bx, y + h - 1 - bh, bw, bh, col);
	}

	char txt[48];
	snprintf(txt, sizeof(txt), "%s %d%%", label, hist[HIST - 1]);
	draw_text(x + 8, y + 6, txt, 0xffffff);
}
