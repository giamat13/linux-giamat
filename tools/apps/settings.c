/* fbdesktop -- settings window */
#include "fbdesktop.h"

/* ---- settings ---- */

#define SET_BTNW 130
#define SET_BTNH 30

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
	wins[slot].x = 260;
	wins[slot].y = 140;
	wins[slot].w = 520;
	wins[slot].h = 300;
	snprintf(wins[slot].title, sizeof(wins[slot].title), "Settings");
	zorder[zcount++] = slot;
	focused = slot;
	return slot;
}

void draw_settings(struct window *w, int content_y)
{
	draw_text(w->x + 16, content_y + 14, "Theme", 0xffffff);
	for (int t = 0; t < NUM_THEMES; t++) {
		int row = t / 3;
		int col = t % 3;
		int bx = w->x + 16 + col * (SET_BTNW + 10);
		int by = content_y + 38 + row * (SET_BTNH + 8);
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

	/* Show hidden files toggle */
	draw_text(w->x + 16, content_y + 130, "Show hidden files", 0xffffff);
	int cx = w->x + 16 + 170, cy = content_y + 130;
	fill_round_rect(cx, cy, 32, 16, 8,
		        show_hidden ? 0x22c55e : 0x6c7086);
	fill_circle(cx + (show_hidden ? 24 : 8), cy + 8, 6, 0xffffff);

	/* Display info */
	char info[256];
	snprintf(info, sizeof(info),
		 "\nDisplay\n  %dx%d  %d bpp\n  font %dx%d (kernel VT)\n"
		 "\nMode is fixed by GRUB gfxpayload at boot.",
		 xres, yres, bpp, font_w, font_h);
	draw_text(w->x + 16, content_y + 160, info, 0x9399b2);
}

/* Settings hit-test: theme buttons (arranged 3+2), or hidden-files toggle.
 * Returns: 0-NUM_THEMES for themes, NUM_THEMES for toggle, -1 for none. */
int settings_click(struct window *w, int px, int py)
{
	for (int t = 0; t < NUM_THEMES; t++) {
		int row = t / 3;
		int col = t % 3;
		int bx = w->x + 16 + col * (SET_BTNW + 10);
		int by = w->y + TITLE_H + 38 + row * (SET_BTNH + 8);
		if (px >= bx && px < bx + SET_BTNW && py >= by && py < by + SET_BTNH)
			return t;
	}
	/* Hidden files toggle at offset 130 */
	int ty = w->y + TITLE_H + 130;
	int tx = w->x + 16 + 170;
	if (px >= tx && px < tx + 32 && py >= ty && py < ty + 16)
		return NUM_THEMES;
	return -1;
}
