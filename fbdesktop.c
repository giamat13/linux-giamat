/* Minimal framebuffer desktop: clickable icons that run shell commands.
 * No X11, no browser -- draws directly to /dev/fb0, reads /dev/input/mice.
 * Font is pulled live from the kernel's own VT console font (GIO_FONT),
 * so no font data is embedded here. */
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <linux/fb.h>
#include <linux/kd.h>

#define ICON_W 140
#define ICON_H 90
#define ICON_GAP 30

struct icon {
	const char *label;
	const char *cmd;
	uint32_t color;
	int action; /* 0=run+show output, 1=reboot, 2=poweroff */
};

static struct icon icons[] = {
	{"SYSTEM",    "uname -a; echo; cat /proc/uptime; echo; cat /proc/version", 0x3b82f6, 0},
	{"PROCESSES", "ps aux", 0x22c55e, 0},
	{"DISK",      "df -h", 0xeab308, 0},
	{"MEMORY",    "free -m", 0xf97316, 0},
	{"DMESG",     "dmesg | tail -30", 0x14b8a6, 0},
	{"REBOOT",    NULL, 0xef4444, 1},
	{"POWER OFF", NULL, 0xf43f5e, 2},
};
#define NUM_ICONS (int)(sizeof(icons)/sizeof(icons[0]))

static uint8_t *fbp;
static struct fb_var_screeninfo vinfo;
static struct fb_fix_screeninfo finfo;
static int xres, yres, bpp, line_length;
static unsigned char font[512 * 32 * 4];
static int have_font;
static int font_w = 8, font_h = 16, font_bpr = 1; /* bytes per row */

static void put_pixel(int x, int y, uint32_t color)
{
	if (x < 0 || y < 0 || x >= xres || y >= yres)
		return;
	long off = (long)y * line_length + (long)x * (bpp / 8);
	if (bpp == 32) {
		*(uint32_t *)(fbp + off) = color;
	} else if (bpp == 16) {
		uint8_t r = (color >> 16) & 0xff, g = (color >> 8) & 0xff, b = color & 0xff;
		uint16_t c565 = ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3);
		*(uint16_t *)(fbp + off) = c565;
	} else if (bpp == 24) {
		fbp[off] = color & 0xff;
		fbp[off + 1] = (color >> 8) & 0xff;
		fbp[off + 2] = (color >> 16) & 0xff;
	}
}

static void fill_rect(int x, int y, int w, int h, uint32_t color)
{
	for (int j = 0; j < h; j++)
		for (int i = 0; i < w; i++)
			put_pixel(x + i, y + j, color);
}

static void blit_char(int x, int y, unsigned char c, uint32_t fg)
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

static void draw_text(int x, int y, const char *s, uint32_t fg)
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

static void draw_desktop(void)
{
	fill_rect(0, 0, xres, yres, 0x181825);
	int cols = (xres - ICON_GAP) / (ICON_W + ICON_GAP);
	if (cols < 1)
		cols = 1;
	for (int i = 0; i < NUM_ICONS; i++) {
		int col = i % cols, row = i / cols;
		int x = ICON_GAP + col * (ICON_W + ICON_GAP);
		int y = ICON_GAP + row * (ICON_H + ICON_GAP);
		fill_rect(x, y, ICON_W, ICON_H, icons[i].color);
		draw_text(x + 8, y + ICON_H / 2 - font_h / 2, icons[i].label, 0xffffff);
	}
}

static int icon_at(int mx, int my)
{
	int cols = (xres - ICON_GAP) / (ICON_W + ICON_GAP);
	if (cols < 1)
		cols = 1;
	for (int i = 0; i < NUM_ICONS; i++) {
		int col = i % cols, row = i / cols;
		int x = ICON_GAP + col * (ICON_W + ICON_GAP);
		int y = ICON_GAP + row * (ICON_H + ICON_GAP);
		if (mx >= x && mx < x + ICON_W && my >= y && my < y + ICON_H)
			return i;
	}
	return -1;
}

static void draw_cursor(int x, int y)
{
	fill_rect(x - 1, y - 6, 2, 12, 0xffffff);
	fill_rect(x - 6, y - 1, 12, 2, 0xffffff);
}

static void run_and_show(const char *cmd)
{
	fill_rect(0, 0, xres, yres, 0x11111b);
	FILE *p = popen(cmd, "r");
	int y = 10, max_rows = (yres - 40) / font_h;
	int row = 0;
	if (p) {
		char line[512];
		while (row < max_rows && fgets(line, sizeof(line), p)) {
			line[strcspn(line, "\n")] = 0;
			draw_text(10, y, line, 0xcdd6f4);
			y += font_h;
			row++;
		}
		pclose(p);
	}
	draw_text(10, yres - font_h - 10, "-- click anywhere to go back --", 0x89b4fa);
}

int main(void)
{
	int fbfd = open("/dev/fb0", O_RDWR);
	if (fbfd < 0) {
		perror("open /dev/fb0");
		return 1;
	}
	ioctl(fbfd, FBIOGET_VSCREENINFO, &vinfo);
	ioctl(fbfd, FBIOGET_FSCREENINFO, &finfo);
	xres = vinfo.xres;
	yres = vinfo.yres;
	bpp = vinfo.bits_per_pixel;
	line_length = finfo.line_length;
	long screensize = (long)line_length * yres;
	fbp = mmap(0, screensize, PROT_READ | PROT_WRITE, MAP_SHARED, fbfd, 0);
	if (fbp == MAP_FAILED) {
		perror("mmap fb");
		return 1;
	}

	FILE *dbg = fopen("/dev/ttyS0", "w");
	if (dbg)
		setvbuf(dbg, NULL, _IONBF, 0);
#define DBG(...) do { if (dbg) fprintf(dbg, __VA_ARGS__); } while (0)

	int confd = open("/dev/tty1", O_RDWR);
	DBG("[fbdesktop] confd=%d errno=%d\n", confd, confd < 0 ? errno : 0);
	if (confd >= 0) {
		struct console_font_op op;
		memset(&op, 0, sizeof(op));
		op.op = KD_FONT_OP_GET;
		op.width = 32;
		op.height = 32;
		op.charcount = 512;
		op.data = font;
		int r = ioctl(confd, KDFONTOP, &op);
		DBG("[fbdesktop] KDFONTOP r=%d errno=%d w=%u h=%u count=%u\n",
			r, r < 0 ? errno : 0, op.width, op.height, op.charcount);
		have_font = r == 0;
		if (have_font) {
			font_w = op.width;
			font_h = op.height;
			font_bpr = (font_w + 7) / 8;
		}
		ioctl(confd, KDSETMODE, KD_GRAPHICS);
	}
	DBG("[fbdesktop] xres=%d yres=%d bpp=%d have_font=%d font_w=%d font_h=%d\n",
		xres, yres, bpp, have_font, font_w, font_h);

	int mousefd = open("/dev/input/mice", O_RDONLY);

	int mx = xres / 2, my = yres / 2;
	int prev_left = 0;
	int showing_output = 0;

	draw_desktop();
	draw_cursor(mx, my);

	if (mousefd >= 0) {
		unsigned char pkt[3];
		while (read(mousefd, pkt, 3) == 3) {
			int left = pkt[0] & 0x1;
			int dx = pkt[1];
			int dy = pkt[2];
			if (pkt[0] & 0x10)
				dx -= 256;
			if (pkt[0] & 0x20)
				dy -= 256;
			int old_mx = mx, old_my = my;
			mx += dx;
			my -= dy;
			if (mx < 0) mx = 0;
			if (my < 0) my = 0;
			if (mx >= xres) mx = xres - 1;
			if (my >= yres) my = yres - 1;

			if (left && !prev_left) {
				if (showing_output) {
					showing_output = 0;
					draw_desktop();
				} else {
					int idx = icon_at(mx, my);
					if (idx >= 0) {
						struct icon *ic = &icons[idx];
						if (ic->action == 1) {
							draw_text(10, 10, "Rebooting...", 0xffffff);
							usleep(500000);
							system("reboot");
						} else if (ic->action == 2) {
							draw_text(10, 10, "Powering off...", 0xffffff);
							usleep(500000);
							system("poweroff -f");
						} else {
							run_and_show(ic->cmd);
							showing_output = 1;
						}
					}
				}
			}
			prev_left = left;

			if (!showing_output && (old_mx != mx || old_my != my)) {
				draw_desktop();
				draw_cursor(mx, my);
			} else if (!showing_output) {
				draw_cursor(mx, my);
			}
		}
	}

	if (confd >= 0)
		ioctl(confd, KDSETMODE, KD_TEXT);
	return 0;
}
