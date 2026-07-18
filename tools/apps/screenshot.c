/* fbdesktop -- Screenshot Tool: captures the current screen straight from the
 * backbuf (via get_pixel, draw.c) into a 24-bit BMP on the Desktop. No stored
 * snapshot: the live preview reads the backbuf every redraw, so a capture is
 * just "encode what's on screen right now" -- including this window itself,
 * same as any real screenshot tool. */
#include "fbdesktop.h"

#define SH_BG   0x14141f
#define SH_TXT  0xcdd6f4
#define SH_DIM  0x6c7086
#define SH_TOOLH 36
#define SH_DIR DESKTOP_DIR "/Screenshots"

static void wr_le16(unsigned char *p, uint16_t v) { p[0] = v & 0xff; p[1] = (v >> 8) & 0xff; }
static void wr_le32(unsigned char *p, uint32_t v)
{
	p[0] = v & 0xff; p[1] = (v >> 8) & 0xff; p[2] = (v >> 16) & 0xff; p[3] = (v >> 24) & 0xff;
}

static int save_bmp(const char *path)
{
	int w = xres, h = yres;
	int rowsz = ((w * 3 + 3) / 4) * 4;
	uint32_t datasize = (uint32_t)rowsz * h;
	FILE *f = fopen(path, "wb");
	if (!f)
		return -1;
	unsigned char hdr[54] = { 0 };
	hdr[0] = 'B'; hdr[1] = 'M';
	wr_le32(hdr + 2, 54 + datasize);
	wr_le32(hdr + 10, 54);
	wr_le32(hdr + 14, 40);
	wr_le32(hdr + 18, (uint32_t)w);
	wr_le32(hdr + 22, (uint32_t)h); /* positive: bottom-up rows */
	wr_le16(hdr + 26, 1);
	wr_le16(hdr + 28, 24);
	wr_le32(hdr + 34, datasize);
	fwrite(hdr, 1, 54, f);

	unsigned char *row = calloc(1, rowsz);
	if (!row) {
		fclose(f);
		return -1;
	}
	for (int y = h - 1; y >= 0; y--) {
		for (int x = 0; x < w; x++) {
			uint32_t c = get_pixel(x, y);
			row[x * 3 + 0] = c & 0xff;
			row[x * 3 + 1] = (c >> 8) & 0xff;
			row[x * 3 + 2] = (c >> 16) & 0xff;
		}
		fwrite(row, 1, rowsz, f);
	}
	free(row);
	fclose(f);
	return 0;
}

/* ---- input -------------------------------------------------------------- */

void shot_click(struct window *w, int px, int py)
{
	struct shotstate *s = w->shot;
	int content_y = w->y + TITLE_H;
	int bw = 140;
	if (py < content_y || py >= content_y + SH_TOOLH || px < w->x + 8 || px >= w->x + 8 + bw)
		return;

	mkdir(SH_DIR, 0755); /* ignore EEXIST -- it's fine if it's already there */
	char path[FM_FULLLEN];
	snprintf(path, sizeof(path), "%s/screenshot-%ld.bmp", SH_DIR, (long)time(NULL));
	if (save_bmp(path) == 0) {
		s->count++;
		snprintf(s->status, sizeof(s->status), "saved %s", strrchr(path, '/') + 1);
	} else {
		snprintf(s->status, sizeof(s->status), "save failed: %s", strerror(errno));
	}
}

/* ---- renderer ------------------------------------------------------------ */

void draw_shot(struct window *w, int content_y, int content_h)
{
	struct shotstate *s = w->shot;
	uint32_t accent = win_accent(w);
	fill_rect(w->x, content_y, w->w, content_h, SH_BG);

	int bw = 140;
	fill_round_rect_grad(w->x + 8, content_y + 4, bw, SH_TOOLH - 8, 5,
			     mix(accent, 0xffffff, 40), accent);
	int lw = (int)strlen("Capture Screen") * font_w;
	draw_text(w->x + 8 + (bw - lw) / 2, content_y + (SH_TOOLH - font_h) / 2, "Capture Screen", 0x11111c);
	draw_text_clip(w->x + 8 + bw + 12, content_y + (SH_TOOLH - font_h) / 2,
		       s->status[0] ? s->status : "captures the whole screen to Desktop",
		       SH_DIM, w->w - bw - 32);

	/* live shrunk preview of the current screen */
	int avail_w = w->w - 16, avail_h = content_h - SH_TOOLH - 16;
	if (avail_w <= 0 || avail_h <= 0)
		return;
	double scale = avail_w / (double)xres;
	double scale_h = avail_h / (double)yres;
	if (scale_h < scale)
		scale = scale_h;
	int dw = (int)(xres * scale), dh = (int)(yres * scale);
	int ox = w->x + 8 + (avail_w - dw) / 2;
	int oy = content_y + SH_TOOLH + 8 + (avail_h - dh) / 2;
	for (int y = 0; y < dh; y++) {
		int sy = (int)(y / scale);
		if (sy >= yres) sy = yres - 1;
		for (int x = 0; x < dw; x++) {
			int sx = (int)(x / scale);
			if (sx >= xres) sx = xres - 1;
			put_pixel(ox + x, oy + y, get_pixel(sx, sy));
		}
	}
}

int spawn_shot(void)
{
	for (int i = 0; i < MAX_WIN; i++) {
		if (wins[i].used && wins[i].type == WIN_SHOT) {
			wins[i].minimized = 0;
			raise_window(i);
			focused = i;
			return i;
		}
	}
	int slot = alloc_window_slot();
	if (slot < 0)
		return -1;
	struct shotstate *s = calloc(1, sizeof(struct shotstate));
	if (!s)
		return -1;
	memset(&wins[slot], 0, sizeof(wins[slot]));
	wins[slot].used = 1;
	wins[slot].type = WIN_SHOT;
	wins[slot].pty_fd = -1;
	wins[slot].shot = s;
	wins[slot].x = 260;
	wins[slot].y = 90;
	wins[slot].w = 380;
	wins[slot].h = 300;
	wins[slot].attr_fg = COL_FG_DEFAULT;
	wins[slot].attr_bg = COL_BG_DEFAULT;
	snprintf(wins[slot].title, sizeof(wins[slot].title), "Screenshot");
	zorder[zcount++] = slot;
	focused = slot;
	return slot;
}
