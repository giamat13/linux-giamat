/* fbdesktop -- Settings: a sidebar of tabs (Appearance, Desktop, File
 * Manager, System) plus a search box that flattens every toggle across all
 * tabs when typed into. Every row here is wired to a real global the rest of
 * the desktop actually reads -- see the comments by each extern in
 * fbdesktop.h for where. No decorative toggles that don't do anything. */
#include "fbdesktop.h"
#include <sys/utsname.h>

#define SET_SB_W   150
#define SET_NAV_H  40
#define SET_SEARCHH 30
#define SET_ROWH   34
#define SET_BTNW   130
#define SET_BTNH   30
#define TXT      0xcdd6f4
#define TXT_DIM  0x9399b2
#define CARD_BG  0x232338

#define SET_NTABS 4
static const char *set_tabs[SET_NTABS] = { "Appearance", "Desktop", "File Manager", "System" };

/* Every plain on/off setting in one registry: label, which tab it lives
 * under, and the real variable it flips. Search flattens across all of them
 * regardless of tab; picking a tab with an empty search shows only its own. */
struct setrow { const char *label; int tab; int *var; };
static struct setrow rows[] = {
	{ "24-hour clock",              0, &clock_24h },
	{ "Show seconds in clock",      0, &clock_show_secs },
	{ "Show date in taskbar",       0, &clock_show_date },
	{ "Solid background (no gradient)", 0, &wallpaper_solid },
	{ "Show icon labels",           1, &show_icon_labels },
	{ "Show hidden files",          2, &show_hidden },
	{ "Confirm before delete",      2, &confirm_delete },
	{ "Sort files by size",         2, &fm_sort_by_size },
};
#define NROWS (int)(sizeof(rows) / sizeof(rows[0]))

/* Typing filters live, same pattern as the Start Menu and File Search. */
static char set_filter[32];
static int set_typing;

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

/* Rows visible right now, in order: every match while searching, else just
 * the current tab's own rows. */
static int visible_rows(const struct window *w, int *out)
{
	int n = 0;
	for (int i = 0; i < NROWS; i++) {
		if (set_filter[0] ? ci_contains(rows[i].label, set_filter) : rows[i].tab == w->tab)
			out[n++] = i;
	}
	return n;
}

int spawn_settings(void)
{
	for (int i = 0; i < MAX_WIN; i++) {
		if (wins[i].used && wins[i].type == WIN_SETTINGS) {
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
	wins[slot].type = WIN_SETTINGS;
	wins[slot].pty_fd = -1;
	wins[slot].tab = 0;
	wins[slot].x = 260;
	wins[slot].y = 110;
	wins[slot].w = 560;
	wins[slot].h = 400;
	snprintf(wins[slot].title, sizeof(wins[slot].title), "Settings");
	zorder[zcount++] = slot;
	focused = slot;
	set_filter[0] = 0;
	set_typing = 0;
	return slot;
}

/* ---- Appearance tab's theme picker: the one row that isn't a plain toggle */

static void draw_theme_picker(int x, int y, int w)
{
	(void)w;
	draw_text(x, y, "Theme", 0xffffff);
	for (int t = 0; t < NUM_THEMES; t++) {
		int row = t / 3, col = t % 3;
		int bx = x + col * (SET_BTNW + 10);
		int by = y + 24 + row * (SET_BTNH + 8);
		int on = (t == theme_idx);
		fill_round_rect_grad(bx, by, SET_BTNW, SET_BTNH, 6,
				     mix(themes[t].dtop, 0xffffff, on ? 40 : 0),
				     themes[t].dbot);
		if (on)
			fill_round_rect(bx, by + SET_BTNH - 3, SET_BTNW, 3, 1, themes[t].accent);
		fill_circle(bx + 14, by + SET_BTNH / 2, 5, themes[t].accent);
		draw_text_clip(bx + 26, by + (SET_BTNH - font_h) / 2, themes[t].name,
			       on ? 0xffffff : 0xa6adc8, SET_BTNW - 32);
	}
}

static int theme_picker_click(int x, int y, int px, int py)
{
	for (int t = 0; t < NUM_THEMES; t++) {
		int row = t / 3, col = t % 3;
		int bx = x + col * (SET_BTNW + 10);
		int by = y + 24 + row * (SET_BTNH + 8);
		if (px >= bx && px < bx + SET_BTNW && py >= by && py < by + SET_BTNH) {
			theme_idx = t;
			return 1;
		}
	}
	return 0;
}

/* ---- System tab: read-only info, nothing to click ---- */

static void draw_system_info(int x, int y, int w)
{
	(void)w;
	struct utsname u;
	char host[64] = "?";
	gethostname(host, sizeof(host));
	double up = 0;
	FILE *f = fopen("/proc/uptime", "r");
	if (f) {
		if (fscanf(f, "%lf", &up) != 1)
			up = 0;
		fclose(f);
	}
	int s = (int)up;
	char upbuf[32];
	snprintf(upbuf, sizeof(upbuf), "%dh %02dm", s / 3600, (s % 3600) / 60);

	char line[128];
	int ly = y;
	draw_text(x, ly, "System", 0xffffff);
	ly += font_h + 14;
	if (uname(&u) == 0) {
		snprintf(line, sizeof(line), "%s  %s", u.sysname, u.release);
		draw_text(x, ly, line, TXT); ly += font_h + 8;
	}
	snprintf(line, sizeof(line), "host: %s", host);
	draw_text(x, ly, line, TXT_DIM); ly += font_h + 8;
	snprintf(line, sizeof(line), "uptime: %s", upbuf);
	draw_text(x, ly, line, TXT_DIM); ly += font_h + 8;
	snprintf(line, sizeof(line), "display: %dx%d, %d bpp", xres, yres, bpp);
	draw_text(x, ly, line, TXT_DIM); ly += font_h + 8;
	snprintf(line, sizeof(line), "font: %dx%d (from the running kernel's VT font)", font_w, font_h);
	draw_text(x, ly, line, TXT_DIM); ly += font_h + 14;
	draw_text(x, ly, "Resolution is fixed by GRUB's gfxpayload at boot.", TXT_DIM);
}

/* ---- toggle rows: shared by every tab and by the search results view ---- */

/* Flips the row's variable; show_hidden additionally needs every open File
 * Manager window (and the desktop icon listing) reloaded, or the toggle
 * would look broken until something else happened to trigger a reload. */
static void toggle_row(struct setrow *r)
{
	*r->var = !*r->var;
	if (r->var == &show_hidden) {
		for (int i = 0; i < MAX_WIN; i++)
			if (wins[i].used && wins[i].type == WIN_FILES && wins[i].fm)
				fm_load(&wins[i]);
		desk_scan();
	}
}

static void draw_toggle_row(int x, int y, int w, const char *label, int on)
{
	draw_text(x, y + (SET_ROWH - font_h) / 2, label, TXT);
	int sw2 = 32, sh2 = 16;
	int sx = x + w - sw2, sy = y + (SET_ROWH - sh2) / 2;
	fill_round_rect(sx, sy, sw2, sh2, 8, on ? 0x22c55e : 0x585b70);
	fill_circle(sx + (on ? sw2 - 8 : 8), sy + sh2 / 2, 6, 0xffffff);
}

/* ---- renderer ------------------------------------------------------------ */

void draw_settings(struct window *w, int content_y)
{
	int content_h = w->h - TITLE_H;
	fill_rect(w->x, content_y, w->w, content_h, COL_BG_DEFAULT);

	/* sidebar */
	fill_rect(w->x, content_y, SET_SB_W, content_h, CARD_BG);
	fill_rect(w->x + SET_SB_W, content_y, 1, content_h, 0x2a2a40);
	uint32_t accent = win_accent(w);
	for (int t = 0; t < SET_NTABS; t++) {
		int iy = content_y + 8 + t * SET_NAV_H;
		int on = (t == w->tab && !set_filter[0]);
		if (on) {
			fill_round_rect(w->x + 6, iy, SET_SB_W - 12, SET_NAV_H - 6, 6,
					mix(accent, CARD_BG, 170));
			fill_rect(w->x + 6, iy + 4, 3, SET_NAV_H - 14, accent);
		}
		draw_text(w->x + 18, iy + (SET_NAV_H - 6 - font_h) / 2, set_tabs[t],
			  on ? 0xffffff : TXT_DIM);
	}

	/* content pane */
	int px = w->x + SET_SB_W + 20;
	int pw = w->w - SET_SB_W - 40;

	fill_round_rect(px, content_y + 8, pw, SET_SEARCHH, 6,
			set_typing ? mix(CARD_BG, accent, 40) : CARD_BG);
	const char *shown = set_filter[0] ? set_filter : "Search settings...";
	draw_text_clip(px + 8, content_y + 8 + (SET_SEARCHH - font_h) / 2, shown,
		       set_filter[0] ? TXT : TXT_DIM, pw - 16);

	int y = content_y + 8 + SET_SEARCHH + 16;

	if (set_filter[0]) {
		int idxs[NROWS];
		int n = visible_rows(w, idxs);
		if (n == 0)
			draw_text(px, y, "no match", TXT_DIM);
		for (int i = 0; i < n; i++)
			draw_toggle_row(px, y + i * SET_ROWH, pw, rows[idxs[i]].label, *rows[idxs[i]].var);
		return;
	}

	if (w->tab == 0) {
		draw_theme_picker(px, y, pw);
		y += 24 + 2 * (SET_BTNH + 8) + 16;
	} else if (w->tab == 3) {
		draw_system_info(px, y, pw);
		return;
	}

	int idxs[NROWS];
	int n = visible_rows(w, idxs);
	for (int i = 0; i < n; i++)
		draw_toggle_row(px, y + i * SET_ROWH, pw, rows[idxs[i]].label, *rows[idxs[i]].var);
}

/* ---- input ---------------------------------------------------------- */

void settings_click(struct window *w, int px, int py)
{
	int content_y = w->y + TITLE_H;

	if (!set_filter[0] && px < w->x + SET_SB_W) {
		for (int t = 0; t < SET_NTABS; t++) {
			int iy = content_y + 8 + t * SET_NAV_H;
			if (py >= iy && py < iy + SET_NAV_H - 6) {
				w->tab = t;
				return;
			}
		}
		return;
	}

	int cx = w->x + SET_SB_W + 20;

	if (py >= content_y + 8 && py < content_y + 8 + SET_SEARCHH) {
		set_typing = 1;
		return;
	}
	set_typing = 0;

	int y = content_y + 8 + SET_SEARCHH + 16;

	if (set_filter[0]) {
		int idxs[NROWS];
		int n = visible_rows(w, idxs);
		int row = (py - y) / SET_ROWH;
		if (row >= 0 && row < n)
			toggle_row(&rows[idxs[row]]);
		return;
	}

	if (w->tab == 0) {
		if (theme_picker_click(cx, y, px, py))
			return;
		y += 24 + 2 * (SET_BTNH + 8) + 16;
	} else if (w->tab == 3) {
		return; /* read-only */
	}

	int idxs[NROWS];
	int n = visible_rows(w, idxs);
	int row = (py - y) / SET_ROWH;
	if (row >= 0 && row < n)
		toggle_row(&rows[idxs[row]]);
}

int settings_keys(struct window *w, const char *buf, int n)
{
	(void)w;
	if (!set_typing)
		return 0;
	int changed = 0;
	for (int i = 0; i < n; i++) {
		unsigned char ch = (unsigned char)buf[i];
		int len = (int)strlen(set_filter);
		if (ch == 0x1b) {
			set_filter[0] = 0;
			set_typing = 0;
		} else if (ch == 0x7f || ch == 0x08) {
			if (len > 0)
				set_filter[len - 1] = 0;
		} else if (ch >= 0x20 && ch < 0x7f && len < (int)sizeof(set_filter) - 1) {
			set_filter[len] = (char)ch;
			set_filter[len + 1] = 0;
		}
		changed = 1;
	}
	return changed;
}
