/* fbdesktop -- task manager: a real drawn UI (sidebar + panels), not a text dump.
 *
 * Four views selected from a left sidebar:
 *   Processes   live sortable table sampled straight from /proc, with End Task
 *   Performance big filled CPU + memory area graphs and stat chips
 *   Disk        one usage bar per mounted filesystem (statvfs)
 *   System      kernel / host / cpu / memory key-value card
 *
 * Nothing here goes through the character grid -- every pixel is drawn from the
 * framebuffer primitives, same as the file manager and settings windows.
 */
#include "fbdesktop.h"
#include <sys/utsname.h>
#include <sys/statvfs.h>

#define SB_W      132       /* sidebar width */
#define NAV_H     46        /* height of one sidebar entry */
#define CARD_BG   0x232338
#define CARD_BG2  0x181826
#define TXT       0xcdd6f4
#define TXT_DIM   0x9399b2
#define TXT_FAINT 0x6c7086

/* ---- per-process sampling ------------------------------------------- */

#define MAXPROC 384
struct pinfo {
	int pid;
	char name[24];
	unsigned long jf;   /* utime+stime, cumulative */
	long rss_kb;
	int cpu_x10;        /* 0..1000, share of total CPU */
};
static struct pinfo cur[MAXPROC], prv[MAXPROC];
static int ncur, nprv;
static unsigned long tot_jf, prv_tot_jf;
static int proc_sel = -1;    /* selected pid, -1 = none */

/* layout captured during draw so the click handler hits the same pixels */
static int lay_px, lay_pw, lay_rows_y, lay_rowh, lay_visible;
static int btn_x, btn_y, btn_w, btn_h;

static unsigned long read_total_jiffies(void)
{
	FILE *f = fopen("/proc/stat", "r");
	if (!f)
		return 0;
	unsigned long v[8] = {0}, t = 0;
	if (fscanf(f, "cpu %lu %lu %lu %lu %lu %lu %lu %lu",
		   &v[0], &v[1], &v[2], &v[3], &v[4], &v[5], &v[6], &v[7]) >= 4)
		for (int i = 0; i < 8; i++)
			t += v[i];
	fclose(f);
	return t;
}

static int cmp_cpu(const void *a, const void *b)
{
	const struct pinfo *p = a, *q = b;
	if (p->cpu_x10 != q->cpu_x10)
		return q->cpu_x10 - p->cpu_x10;
	return (q->rss_kb > p->rss_kb) - (q->rss_kb < p->rss_kb);
}

static void sample_procs(void)
{
	static long pg;
	if (!pg)
		pg = sysconf(_SC_PAGESIZE) / 1024;

	memcpy(prv, cur, sizeof(cur));
	nprv = ncur;
	prv_tot_jf = tot_jf;
	tot_jf = read_total_jiffies();
	long dtot = (long)(tot_jf - prv_tot_jf);

	DIR *d = opendir("/proc");
	if (!d)
		return;
	ncur = 0;
	struct dirent *e;
	while ((e = readdir(d)) && ncur < MAXPROC) {
		if (e->d_name[0] < '0' || e->d_name[0] > '9')
			continue;
		char path[64];
		snprintf(path, sizeof(path), "/proc/%s/stat", e->d_name);
		FILE *f = fopen(path, "r");
		if (!f)
			continue;
		char buf[512];
		char *ok = fgets(buf, sizeof(buf), f);
		fclose(f);
		if (!ok)
			continue;
		char *lp = strchr(buf, '('), *rp = strrchr(buf, ')');
		if (!lp || !rp || rp < lp)
			continue;

		struct pinfo *p = &cur[ncur];
		p->pid = atoi(e->d_name);
		int nlen = rp - lp - 1;
		if (nlen > 23) nlen = 23;
		if (nlen < 0) nlen = 0;
		memcpy(p->name, lp + 1, nlen);
		p->name[nlen] = 0;

		unsigned long ut = 0, st = 0;
		long rss = 0;
		sscanf(rp + 1, " %*c %*d %*d %*d %*d %*d %*u %*u %*u %*u %*u %lu %lu"
			       " %*d %*d %*d %*d %*d %*d %*u %*u %ld",
		       &ut, &st, &rss);
		p->jf = ut + st;
		p->rss_kb = rss * pg;

		/* match by pid in the previous snapshot for the CPU delta */
		p->cpu_x10 = 0;
		if (dtot > 0) {
			for (int j = 0; j < nprv; j++)       /* ponytail: O(n*m) scan, fine at n<400 */
				if (prv[j].pid == p->pid) {
					long dj = (long)(p->jf - prv[j].jf);
					if (dj > 0) {
						p->cpu_x10 = (int)(dj * 1000 / dtot);
						if (p->cpu_x10 > 1000)
							p->cpu_x10 = 1000;
					}
					break;
				}
		}
		ncur++;
	}
	closedir(d);
	qsort(cur, ncur, sizeof(cur[0]), cmp_cpu);
}

/* ---- CPU / memory history (drives the Performance graphs) ----------- */

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
			long busy = total - v[3] - v[4];
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

/* ---- lifecycle ------------------------------------------------------ */

void taskmgr_refresh(struct window *w)
{
	if (w->tab < 0 || w->tab >= TM_NTABS)
		w->tab = 0;
	sample_procs();
	snprintf(w->title, sizeof(w->title), "Task Manager  -  %s", tm_tabs[w->tab].label);
}

int spawn_taskmgr(void)
{
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
	wins[slot].x = 170;
	wins[slot].y = 80;
	wins[slot].w = 720;
	wins[slot].h = 480;
	wins[slot].attr_fg = COL_FG_DEFAULT;
	wins[slot].attr_bg = COL_BG_DEFAULT;
	wins[slot].tab = 0;
	taskmgr_refresh(&wins[slot]);
	zorder[zcount++] = slot;
	focused = slot;
	return slot;
}

/* ---- small drawing helpers ------------------------------------------ */

static void draw_navicon(int tab, int cx, int cy, uint32_t col)
{
	switch (tab) {
	case 0: /* processes: three bars */
		fill_rect(cx - 6, cy - 2, 3, 8, col);
		fill_rect(cx - 1, cy - 6, 3, 12, col);
		fill_rect(cx + 4, cy - 5, 3, 11, col);
		break;
	case 1: /* performance: a zigzag pulse */
		fill_rect(cx - 7, cy + 2, 4, 3, col);
		fill_rect(cx - 3, cy - 4, 4, 3, col);
		fill_rect(cx + 1, cy + 1, 4, 3, col);
		fill_rect(cx + 5, cy - 3, 3, 3, col);
		break;
	case 2: /* disk: a cylinder */
		fill_round_rect(cx - 7, cy - 6, 14, 12, 3, col);
		fill_round_rect(cx - 7, cy - 6, 14, 4, 3, mix(col, 0xffffff, 60));
		break;
	default: /* system: a chip */
		fill_round_rect(cx - 6, cy - 6, 12, 12, 2, col);
		fill_rect(cx - 2, cy - 2, 4, 4, mix(col, CARD_BG2, 160));
		break;
	}
}

static void bar(int x, int y, int w, int h, int pct, uint32_t col)
{
	if (pct < 0) pct = 0;
	if (pct > 100) pct = 100;
	fill_round_rect(x, y, w, h, h / 2, 0x11111c);
	int fw = pct * w / 100;
	if (fw < h) fw = (pct > 0) ? h : 0;
	if (fw > 0)
		fill_round_rect(x, y, fw, h, h / 2, col);
}

static uint32_t load_col(int pct)
{
	return pct >= 80 ? 0xf38ba8 : pct >= 50 ? 0xf9e2af : 0xa6e3a1;
}

/* Filled area graph inside a rounded card. */
void draw_graph(int x, int y, int w, int h, const int *hist,
		uint32_t col, const char *label)
{
	fill_round_rect(x, y, w, h, 8, CARD_BG2);
	for (int g = 1; g < 4; g++)
		fill_rect(x + 8, y + g * h / 4, w - 16, 1, 0x2a2a40);

	int base = y + h - 8, span = h - 20;
	int cw = (w - 16) / HIST;
	if (cw < 1) cw = 1;
	for (int i = 0; i < HIST; i++) {
		int bh = hist[i] * span / 100;
		int bx = x + 8 + i * (w - 16) / HIST;
		if (bh > 0) {
			fill_vgradient(bx, base - bh, cw, bh, mix(col, CARD_BG2, 70), CARD_BG2);
			fill_rect(bx, base - bh, cw, 2, col);   /* crest line */
		}
	}
	char txt[48];
	int now = hist[HIST - 1];
	snprintf(txt, sizeof(txt), "%s", label);
	draw_text(x + 12, y + 10, txt, TXT_DIM);
	snprintf(txt, sizeof(txt), "%d%%", now);
	draw_text(x + w - 12 - (int)strlen(txt) * font_w, y + 10, txt, load_col(now));
}

/* ---- the four views ------------------------------------------------- */

static void view_processes(struct window *w, int px, int py, int pw, int ph)
{
	int pad = 14;
	int x = px + pad, top = py + pad, iw = pw - 2 * pad;

	draw_text(x, top, "Processes", TXT);
	char sub[48];
	snprintf(sub, sizeof(sub), "%d running", ncur);
	draw_text(x + 10 * font_w, top, sub, TXT_FAINT);

	/* End Task button, right aligned */
	int enabled = 0;
	for (int i = 0; i < ncur; i++)
		if (cur[i].pid == proc_sel) { enabled = 1; break; }
	btn_w = 11 * font_w;
	btn_h = font_h + 10;
	btn_x = px + pw - pad - btn_w;
	btn_y = top - 4;
	fill_round_rect_grad(btn_x, btn_y, btn_w, btn_h, 5,
			     enabled ? 0xf38ba8 : 0x2b2b3a,
			     enabled ? 0xc4506a : 0x232338);
	draw_text(btn_x + (btn_w - 8 * font_w) / 2, btn_y + 5,
		  "End Task", enabled ? 0x11111c : TXT_FAINT);

	/* column layout */
	int hy = top + font_h + 12;
	int c_pid = x;
	int c_cpu = px + pw - pad - 14 * font_w;
	int c_mem = px + pw - pad - 7 * font_w;
	int c_name = c_pid + 7 * font_w;
	fill_rect(x, hy + font_h + 4, iw, 1, 0x2a2a40);
	draw_text(c_pid, hy, "PID", TXT_FAINT);
	draw_text(c_name, hy, "NAME", TXT_FAINT);
	draw_text(c_cpu, hy, "CPU", TXT_FAINT);
	draw_text(c_mem, hy, "MEM", TXT_FAINT);

	lay_rowh = font_h + 8;
	lay_rows_y = hy + font_h + 8;
	lay_px = px;
	lay_pw = pw;
	int avail = py + ph - lay_rows_y - 4;
	lay_visible = avail / lay_rowh;
	if (lay_visible > ncur)
		lay_visible = ncur;

	for (int i = 0; i < lay_visible; i++) {
		struct pinfo *p = &cur[i];
		int ry = lay_rows_y + i * lay_rowh;
		int sel = (p->pid == proc_sel);
		if (sel)
			fill_round_rect(x - 4, ry - 2, iw + 8, lay_rowh, 4,
					mix(win_accent(w), CARD_BG2, 150));
		else if (i & 1)
			fill_rect(x - 4, ry - 2, iw + 8, lay_rowh, 0x20202f);

		int ty = ry + (lay_rowh - font_h) / 2 - 2;
		char t[32];
		snprintf(t, sizeof(t), "%d", p->pid);
		draw_text(c_pid, ty, t, TXT_DIM);
		draw_text_clip(c_name, ty, p->name, sel ? 0xffffff : TXT,
			       c_cpu - c_name - 8);

		int pct = p->cpu_x10 / 10;
		snprintf(t, sizeof(t), "%d.%d%%", p->cpu_x10 / 10, p->cpu_x10 % 10);
		draw_text(c_cpu, ty, t, pct >= 1 ? load_col(pct) : TXT_FAINT);

		if (p->rss_kb >= 1024)
			snprintf(t, sizeof(t), "%ldM", p->rss_kb / 1024);
		else
			snprintf(t, sizeof(t), "%ldK", p->rss_kb);
		draw_text(c_mem, ty, t, TXT_DIM);
	}
}

static void fmt_uptime(char *out, size_t n)
{
	FILE *f = fopen("/proc/uptime", "r");
	double up = 0;
	if (f) { if (fscanf(f, "%lf", &up) != 1) up = 0; fclose(f); }
	int s = (int)up;
	snprintf(out, n, "%dh %02dm", s / 3600, (s % 3600) / 60);
}

static void view_performance(struct window *w, int px, int py, int pw, int ph)
{
	(void)w;
	int pad = 14;
	int x = px + pad, iw = pw - 2 * pad;
	int chips_h = 44;
	int gh = (ph - 3 * pad - chips_h) / 2;

	int y0 = py + pad;
	draw_graph(x, y0, iw, gh, cpu_hist, 0x89b4fa, "CPU");
	int y1 = y0 + gh + pad;
	draw_graph(x, y1, iw, gh, mem_hist, 0xa6e3a1, "Memory");

	/* used/total overlaid on the memory card */
	long tot = read_meminfo_kb("MemTotal"), avail = read_meminfo_kb("MemAvailable");
	if (tot > 0) {
		char m[48];
		snprintf(m, sizeof(m), "%ld / %ld MB", (tot - avail) / 1024, tot / 1024);
		draw_text(x + 12, y1 + gh - font_h - 8, m, TXT_FAINT);
	}

	/* stat chips row */
	int cy = y1 + gh + pad;
	int cw = (iw - 2 * 10) / 3;
	const char *lbl[3] = { "Cores", "Uptime", "Load" };
	char val[3][32];
	snprintf(val[0], 32, "%ld", sysconf(_SC_NPROCESSORS_ONLN));
	fmt_uptime(val[1], 32);
	FILE *f = fopen("/proc/loadavg", "r");
	double la = 0;
	if (f) { if (fscanf(f, "%lf", &la) != 1) la = 0; fclose(f); }
	snprintf(val[2], 32, "%.2f", la);
	for (int i = 0; i < 3; i++) {
		int cx = x + i * (cw + 10);
		fill_round_rect(cx, cy, cw, chips_h, 8, CARD_BG);
		draw_text(cx + 12, cy + 8, lbl[i], TXT_FAINT);
		draw_text(cx + 12, cy + 8 + font_h + 3, val[i], TXT);
	}
}

/* Backs the "Folder Usage" section below the filesystem cards -- a single
 * instance is enough since (like proc_sel and the CPU/mem history) there is
 * only ever one Task Manager worth looking at. */
static struct dustate du_state;

/* Shared by view_disk (draw) and taskmgr_click so the two can't drift apart:
 * the rectangle the folder-usage panel is drawn into and hit-tested against. */
static void disk_panel_rect(struct window *w, int *rx, int *ry, int *rw, int *rh)
{
	int content_y = w->y + TITLE_H, content_h = w->h - TITLE_H;
	int pad = 14, x = w->x + SB_W + 1 + pad, iw = w->w - SB_W - 1 - 2 * pad;
	int fs_top = content_y + pad + font_h + 10;
	int fs_area_h = (content_h - 2 * pad - font_h - 10) * 2 / 5;
	int fs_bottom = fs_top + fs_area_h;
	*rx = x;
	*ry = fs_bottom + 6 + font_h + 8;
	*rw = iw;
	*rh = w->y + w->h - pad - *ry;
}

static void view_disk(struct window *w, int px, int py, int pw, int ph)
{
	int pad = 14, x = px + pad, iw = pw - 2 * pad;
	draw_text(x, py + pad, "Filesystems", TXT);

	/* Filesystem cards get a fixed top slice; the folder browser gets the
	 * rest, so it stays visible regardless of how many mounts there are. */
	int fs_top = py + pad + font_h + 10;
	int fs_area_h = (ph - 2 * pad - font_h - 10) * 2 / 5;
	int fs_bottom = fs_top + fs_area_h;

	FILE *f = fopen("/proc/mounts", "r");
	char dev[128], mnt[128], fs[64];
	int y = fs_top;
	int cardh = 3 * font_h + 6;
	while (f && y + cardh < fs_bottom &&
	       fscanf(f, "%127s %127s %63s %*s %*d %*d", dev, mnt, fs) == 3) {
		/* real block/image filesystems only -- skip proc/sysfs/cgroup/etc */
		if (strcmp(fs, "ext4") && strcmp(fs, "ext3") && strcmp(fs, "ext2") &&
		    strcmp(fs, "vfat") && strcmp(fs, "squashfs") && strcmp(fs, "overlay") &&
		    strcmp(fs, "iso9660"))
			continue;
		struct statvfs vfs;
		if (statvfs(mnt, &vfs) != 0 || vfs.f_blocks == 0)
			continue;
		unsigned long long totb = (unsigned long long)vfs.f_blocks * vfs.f_frsize;
		unsigned long long freeb = (unsigned long long)vfs.f_bfree * vfs.f_frsize;
		unsigned long long usedb = totb - freeb;
		int pct = totb ? (int)(usedb * 100 / totb) : 0;

		fill_round_rect(x, y, iw, cardh, 8, CARD_BG);
		draw_text(x + 12, y + 8, mnt, TXT);
		char r[64];
		snprintf(r, sizeof(r), "%s  -  %llu / %llu MB",
			 fs, usedb >> 20, totb >> 20);
		draw_text(x + 12, y + 8 + font_h + 2, r, TXT_FAINT);
		bar(x + 12, y + cardh - font_h + 2, iw - 24 - 6 * font_w, 8, pct, load_col(pct));
		char pc[8];
		snprintf(pc, sizeof(pc), "%d%%", pct);
		draw_text(x + iw - 12 - (int)strlen(pc) * font_w, y + cardh - font_h, pc, load_col(pct));
		y += cardh + 10;
	}
	if (f)
		fclose(f);

	/* ---- folder usage: merged in from the old standalone Disk Usage app ---- */
	draw_text(x, fs_bottom + 6, "Folder Usage", TXT);
	if (!du_state.path[0]) {
		snprintf(du_state.path, sizeof(du_state.path), "/root");
		du_scan(&du_state);
	}
	int rx, ry, rw, rh;
	disk_panel_rect(w, &rx, &ry, &rw, &rh);
	draw_du_panel(&du_state, win_accent(w), rx, ry, rw, rh);
}

static void kv(int x, int *y, int w, const char *k, const char *v)
{
	draw_text(x, *y, k, TXT_FAINT);
	draw_text_clip(x + 11 * font_w, *y, v, TXT, w - 11 * font_w);
	*y += font_h + 8;
}

static void view_system(struct window *w, int px, int py, int pw, int ph)
{
	(void)w; (void)ph;
	int pad = 14, x = px + pad, iw = pw - 2 * pad;
	draw_text(x, py + pad, "System", TXT);

	int cy = py + pad + font_h + 10;
	fill_round_rect(x, cy, iw, 8 * (font_h + 8) + 8, 8, CARD_BG);
	int y = cy + 14, tx = x + 14;

	struct utsname u;
	char cpu[128] = "unknown";
	FILE *f = fopen("/proc/cpuinfo", "r");
	if (f) {
		char line[256];
		while (fgets(line, sizeof(line), f))
			if (!strncmp(line, "model name", 10)) {
				char *c = strchr(line, ':');
				if (c) {
					c += 2;
					c[strcspn(c, "\n")] = 0;
					snprintf(cpu, sizeof(cpu), "%s", c);
				}
				break;
			}
		fclose(f);
	}
	char up[32], mem[32];
	fmt_uptime(up, sizeof(up));
	snprintf(mem, sizeof(mem), "%ld MB", read_meminfo_kb("MemTotal") / 1024);

	if (uname(&u) == 0) {
		kv(tx, &y, iw, "Kernel", u.release);
		kv(tx, &y, iw, "System", u.sysname);
		kv(tx, &y, iw, "Machine", u.machine);
		kv(tx, &y, iw, "Host", u.nodename);
	}
	kv(tx, &y, iw, "CPU", cpu);
	{
		char n[16];
		snprintf(n, sizeof(n), "%ld", sysconf(_SC_NPROCESSORS_ONLN));
		kv(tx, &y, iw, "Cores", n);
	}
	kv(tx, &y, iw, "Memory", mem);
	kv(tx, &y, iw, "Uptime", up);
}

/* ---- compositor entry ----------------------------------------------- */

void draw_taskmgr(struct window *w, int content_y, int content_h)
{
	uint32_t accent = win_accent(w);

	/* sidebar */
	fill_rect(w->x, content_y, SB_W, content_h, CARD_BG2);
	fill_rect(w->x + SB_W, content_y, 1, content_h, 0x2a2a40);
	for (int t = 0; t < TM_NTABS; t++) {
		int iy = content_y + 10 + t * NAV_H;
		int on = (t == w->tab);
		if (on) {
			fill_round_rect(w->x + 8, iy, SB_W - 16, NAV_H - 6, 6,
					mix(accent, CARD_BG2, 170));
			fill_rect(w->x + 8, iy + 4, 3, NAV_H - 14, accent);
		}
		uint32_t ic = on ? accent : TXT_DIM;
		draw_navicon(t, w->x + 28, iy + (NAV_H - 6) / 2, ic);
		draw_text(w->x + 44, iy + (NAV_H - 6 - font_h) / 2,
			  tm_tabs[t].label, on ? 0xffffff : TXT_DIM);
	}

	/* content panel */
	int px = w->x + SB_W + 1;
	int pw = w->w - SB_W - 1;
	fill_rect(px, content_y, pw, content_h, COL_BG_DEFAULT);

	switch (w->tab) {
	case 0: view_processes(w, px, content_y, pw, content_h); break;
	case 1: view_performance(w, px, content_y, pw, content_h); break;
	case 2: view_disk(w, px, content_y, pw, content_h); break;
	default: view_system(w, px, content_y, pw, content_h); break;
	}

	/* resize grip */
	if (!w->maximized)
		for (int k = 0; k < 3; k++) {
			int gx = w->x + w->w - 5 - k * 4;
			int gy = w->y + w->h - 5;
			for (int m = 0; m <= k; m++)
				fill_rect(gx, gy - m * 4, 2, 2, TXT_FAINT);
		}
}

void taskmgr_disk_scroll(struct window *w, int value)
{
	int rx, ry, rw, rh;
	disk_panel_rect(w, &rx, &ry, &rw, &rh);
	du_panel_scroll(&du_state, rh, value);
}

/* Sidebar / row / button hit testing. */
void taskmgr_click(struct window *w, int px, int py)
{
	int content_y = w->y + TITLE_H;

	/* sidebar nav */
	if (px < w->x + SB_W) {
		for (int t = 0; t < TM_NTABS; t++) {
			int iy = content_y + 10 + t * NAV_H;
			if (py >= iy && py < iy + NAV_H - 6) {
				if (t != w->tab) {
					w->tab = t;
					proc_sel = -1;
					taskmgr_refresh(w);
				}
				return;
			}
		}
		return;
	}

	if (w->tab == 2) {
		int rx, ry, rw, rh;
		disk_panel_rect(w, &rx, &ry, &rw, &rh);
		du_panel_click(&du_state, rx, ry, rw, rh, px, py);
		return;
	}
	if (w->tab != 0)
		return;

	/* End Task */
	if (px >= btn_x && px < btn_x + btn_w && py >= btn_y && py < btn_y + btn_h) {
		if (proc_sel > 0) {
			kill(proc_sel, SIGTERM);
			proc_sel = -1;
		}
		return;
	}

	/* process row */
	if (py >= lay_rows_y && lay_rowh > 0) {
		int idx = (py - lay_rows_y) / lay_rowh;
		if (idx >= 0 && idx < lay_visible)
			proc_sel = (cur[idx].pid == proc_sel) ? -1 : cur[idx].pid;
	}
}
