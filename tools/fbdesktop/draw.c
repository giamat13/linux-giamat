/* fbdesktop -- framebuffer primitives: pixels, rects, text and the vector glyphs */
#include "fbdesktop.h"

void put_pixel(int x, int y, uint32_t color)
{
	if (x < 0 || y < 0 || x >= xres || y >= yres)
		return;
	uint8_t *buf = backbuf ? backbuf : fbp;
	long off = (long)y * line_length + (long)x * (bpp / 8);
	if (bpp == 32) {
		*(uint32_t *)(buf + off) = color;
	} else if (bpp == 16) {
		uint8_t r = (color >> 16) & 0xff, g = (color >> 8) & 0xff, b = color & 0xff;
		uint16_t c565 = ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3);
		*(uint16_t *)(buf + off) = c565;
	} else if (bpp == 24) {
		buf[off] = color & 0xff;
		buf[off + 1] = (color >> 8) & 0xff;
		buf[off + 2] = (color >> 16) & 0xff;
	}
}

void fill_rect(int x, int y, int w, int h, uint32_t color)
{
	for (int j = 0; j < h; j++)
		for (int i = 0; i < w; i++)
			put_pixel(x + i, y + j, color);
}

void draw_cursor(int x, int y)
{
	int size = 12;
	put_pixel(x, y, 0xffffff);
	put_pixel(x + 1, y, 0xffffff);
	put_pixel(x, y + 1, 0xffffff);
	put_pixel(x + 1, y + 1, 0xffffff);
	for (int i = 2; i < size; i++) {
		put_pixel(x, y + i, 0xffffff);
		put_pixel(x + i, y, 0xffffff);
	}
	put_pixel(x + 1, y + 2, 0x000000);
	put_pixel(x + 2, y + 1, 0x000000);
	for (int i = 2; i < size - 1; i++) {
		put_pixel(x + 1, y + i, 0x000000);
		put_pixel(x + i, y + 1, 0x000000);
	}
}

void blit_char(int x, int y, unsigned char c, uint32_t fg)
{
	if (!have_font)
		return;
	unsigned char *glyph = font + (int)c * 32;
	for (int row = 0; row < font_h; row++) {
		unsigned char *rowbits = glyph + row * font_bpr;
		for (int col = 0; col < font_w; col++) {
			unsigned char byte = rowbits[col / 8];
			if (byte & (0x80 >> (col % 8)))
				put_pixel(x + col, y + row, fg);
		}
	}
}

/* ---- shape primitives (integer-only, no libm) ---- */

/* Blend two colors: t=0 -> a, t=255 -> b. */
uint32_t mix(uint32_t a, uint32_t b, int t)
{
	int ar = (a >> 16) & 0xff, ag = (a >> 8) & 0xff, ab = a & 0xff;
	int br = (b >> 16) & 0xff, bg = (b >> 8) & 0xff, bb = b & 0xff;
	int r = ar + (br - ar) * t / 255;
	int g = ag + (bg - ag) * t / 255;
	int bl = ab + (bb - ab) * t / 255;
	return ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)bl;
}

/* Rounded rect with a vertical gradient (top==bot gives a flat fill). */
void fill_round_rect_grad(int x, int y, int w, int h, int r,
				 uint32_t top, uint32_t bot)
{
	if (r * 2 > w) r = w / 2;
	if (r * 2 > h) r = h / 2;
	if (r < 0) r = 0;
	for (int j = 0; j < h; j++) {
		uint32_t col = (top == bot) ? top
			: mix(top, bot, h > 1 ? j * 255 / (h - 1) : 0);
		for (int i = 0; i < w; i++) {
			int cx = -1, cy = -1;
			if (i < r && j < r) { cx = r; cy = r; }
			else if (i >= w - r && j < r) { cx = w - r - 1; cy = r; }
			else if (i < r && j >= h - r) { cx = r; cy = h - r - 1; }
			else if (i >= w - r && j >= h - r) { cx = w - r - 1; cy = h - r - 1; }
			if (cx >= 0) {
				int dx = i - cx, dy = j - cy;
				if (dx * dx + dy * dy > r * r)
					continue;
			}
			put_pixel(x + i, y + j, col);
		}
	}
}

void fill_round_rect(int x, int y, int w, int h, int r, uint32_t col)
{
	fill_round_rect_grad(x, y, w, h, r, col, col);
}

void fill_circle(int cx, int cy, int r, uint32_t col)
{
	for (int j = -r; j <= r; j++)
		for (int i = -r; i <= r; i++)
			if (i * i + j * j <= r * r)
				put_pixel(cx + i, cy + j, col);
}

/* Annulus: outer radius r, thickness t. */
static void fill_ring(int cx, int cy, int r, int t, uint32_t col)
{
	int inner = r - t;
	if (inner < 0) inner = 0;
	for (int j = -r; j <= r; j++)
		for (int i = -r; i <= r; i++) {
			int d = i * i + j * j;
			if (d <= r * r && d >= inner * inner)
				put_pixel(cx + i, cy + j, col);
		}
}

/* Triangle with its tip at distance s from (cx,cy). dir: 0=up 1=down 2=left 3=right */
static void fill_triangle(int cx, int cy, int s, int dir, uint32_t col)
{
	for (int j = 0; j <= s; j++) {
		for (int i = -j; i <= j; i++) {
			int px, py;
			if (dir == 0)      { px = cx + i;     py = cy - s + j; }
			else if (dir == 1) { px = cx + i;     py = cy + s - j; }
			else if (dir == 2) { px = cx - s + j; py = cy + i;     }
			else               { px = cx + s - j; py = cy + i;     }
			put_pixel(px, py, col);
		}
	}
}

void fill_vgradient(int x, int y, int w, int h, uint32_t top, uint32_t bot)
{
	for (int j = 0; j < h; j++) {
		uint32_t c = mix(top, bot, h > 1 ? j * 255 / (h - 1) : 0);
		for (int i = 0; i < w; i++)
			put_pixel(x + i, y + j, c);
	}
}

void draw_text(int x, int y, const char *s, uint32_t fg)
{
	int cx = x, cy = y;
	for (; *s; s++) {
		if (*s == '\n') {
			cx = x;
			cy += font_h;
			continue;
		}
		if (cx + font_w > xres) {
			cx = x;
			cy += font_h;
		}
		blit_char(cx, cy, (unsigned char)*s, fg);
		cx += font_w;
	}
}

void draw_text_clip(int x, int y, const char *s, uint32_t fg, int maxw)
{
	char buf[64];
	int maxchars = maxw / font_w;
	if (maxchars < 0)
		maxchars = 0;
	if (maxchars >= (int)sizeof(buf))
		maxchars = sizeof(buf) - 1;
	int n = strlen(s);
	if (n > maxchars)
		n = maxchars;
	memcpy(buf, s, n);
	buf[n] = 0;
	draw_text(x, y, buf, fg);
}

/* Vector-style glyphs, drawn from primitives and centered on (cx,cy).
 * `fg` is the ink, `hole` is used to punch cutouts back out of the tile. */
void draw_glyph(int g, int cx, int cy, uint32_t fg, uint32_t hole)
{
	switch (g) {
	case G_GAUGE:
		/* a bar chart -- reads as "activity / task manager" */
		fill_round_rect(cx - 20, cy - 18, 40, 36, 4, fg);
		fill_round_rect(cx - 14, cy + 4,  6, 9,  1, hole);
		fill_round_rect(cx - 5,  cy - 4,  6, 17, 1, hole);
		fill_round_rect(cx + 4,  cy - 12, 6, 25, 1, hole);
		break;
	case G_FOLDER:
		fill_round_rect(cx - 19, cy - 17, 17, 9, 3, fg);
		fill_round_rect(cx - 19, cy - 12, 38, 27, 4, fg);
		fill_round_rect(cx - 16, cy - 6, 32, 3, 1, mix(fg, hole, 120));
		break;
	case G_TERM:
		fill_round_rect(cx - 20, cy - 16, 40, 32, 4, fg);
		fill_round_rect(cx - 16, cy - 8, 32, 20, 2, hole);
		fill_circle(cx - 15, cy - 12, 2, hole);
		fill_circle(cx - 9,  cy - 12, 2, hole);
		fill_circle(cx - 3,  cy - 12, 2, hole);
		/* prompt chevron + cursor */
		fill_rect(cx - 12, cy - 2, 3, 3, fg);
		fill_rect(cx - 9,  cy + 1, 3, 3, fg);
		fill_rect(cx - 12, cy + 4, 3, 3, fg);
		fill_rect(cx - 3,  cy + 4, 9, 3, fg);
		break;
	case G_REFRESH:
		fill_ring(cx, cy + 2, 16, 5, fg);
		fill_rect(cx, cy - 22, 22, 13, hole);       /* open the top-right arc */
		fill_triangle(cx + 4, cy - 13, 10, 3, fg);  /* arrow head on the opening */
		break;
	case G_POWER:
		fill_ring(cx, cy + 3, 16, 6, fg);
		fill_rect(cx - 5, cy - 16, 10, 12, hole);  /* gap at the top */
		fill_round_rect(cx - 2, cy - 18, 5, 18, 2, fg);
		break;
	case G_GLOBE:
		/* meridian + equator inside a ring -- reads as "web" */
		fill_ring(cx, cy, 19, 4, fg);
		fill_ring(cx, cy, 9, 3, fg);         /* the meridian, seen edge-on */
		fill_rect(cx - 16, cy - 8, 32, 3, fg);
		fill_rect(cx - 18, cy - 1, 36, 3, fg);
		fill_rect(cx - 16, cy + 6, 32, 3, fg);
		break;
	case G_GEAR:
		/* four teeth + body + hub */
		fill_round_rect(cx - 4, cy - 20, 8, 40, 2, fg);
		fill_round_rect(cx - 20, cy - 4, 40, 8, 2, fg);
		fill_round_rect(cx - 14, cy - 16, 8, 8, 2, fg);
		fill_round_rect(cx + 6, cy - 16, 8, 8, 2, fg);
		fill_round_rect(cx - 14, cy + 8, 8, 8, 2, fg);
		fill_round_rect(cx + 6, cy + 8, 8, 8, 2, fg);
		fill_circle(cx, cy, 14, fg);
		fill_circle(cx, cy, 6, hole);
		break;
	case G_FILE:
		/* a page with a folded corner and a couple of text lines */
		fill_round_rect(cx - 16, cy - 20, 32, 40, 3, fg);
		fill_triangle(cx + 16, cy - 20, 9, 1, hole);
		fill_rect(cx - 9, cy - 4, 18, 3, hole);
		fill_rect(cx - 9, cy + 4, 18, 3, hole);
		fill_rect(cx - 9, cy + 12, 12, 3, hole);
		break;
	case G_IMAGE:
		/* a photo frame with a mountain scene and a sun */
		fill_round_rect(cx - 18, cy - 14, 36, 28, 3, fg);
		fill_round_rect(cx - 14, cy - 10, 28, 20, 1, hole);
		fill_circle(cx + 5, cy - 4, 3, fg);
		fill_triangle(cx - 8, cy + 6, 6, 0, fg);
		fill_triangle(cx + 1, cy + 6, 8, 0, fg);
		break;
	case G_ARCHIVE:
		/* a packed box with a carrying strap */
		fill_round_rect(cx - 16, cy - 14, 32, 28, 3, fg);
		fill_rect(cx - 16, cy - 4, 32, 4, hole);
		fill_round_rect(cx - 4, cy - 9, 8, 6, 1, hole);
		break;
	case G_CODE:
		/* a document with a "< >" mark carved out */
		fill_round_rect(cx - 16, cy - 20, 32, 40, 3, fg);
		fill_triangle(cx - 5, cy - 2, 5, 2, hole);
		fill_triangle(cx + 5, cy - 2, 5, 3, hole);
		fill_rect(cx - 9, cy + 10, 18, 3, hole);
		break;
	case G_EXEC:
		/* a rounded "chip" body with a play/run triangle at its center */
		fill_round_rect(cx - 18, cy - 16, 36, 32, 6, fg);
		fill_triangle(cx - 4, cy, 9, 3, hole);
		break;
	default:
		break;
	}
}
