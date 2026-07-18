/* fbdesktop -- Image Viewer: fit-to-window display for the two formats this
 * desktop itself produces -- PPM (paint.c's save format) and plain 24-bit BMP
 * -- decoded with no library at all, so there's nothing to link and nothing
 * that can fail to find a codec. Prev/Next cycle through the same directory.
 *
 * ponytail: PNG/JPEG need a real decoder (zlib/DCT); out of scope until
 * something in this tree actually produces or needs one. */
#include "fbdesktop.h"

#define IV_BG    0x14141f
#define IV_TOOLH 32
#define IV_TXT   0xcdd6f4
#define IV_DIM   0x6c7086

static int read_ppm_int(FILE *f, int *out)
{
	int c;
	for (;;) {
		c = fgetc(f);
		if (c == '#') {
			while ((c = fgetc(f)) != '\n' && c != EOF)
				;
			continue;
		}
		if (c == EOF)
			return -1;
		if (!isspace(c))
			break;
	}
	int v = 0, got = 0;
	while (c != EOF && isdigit(c)) {
		v = v * 10 + (c - '0');
		c = fgetc(f);
		got = 1;
	}
	if (!got)
		return -1;
	ungetc(c, f);
	*out = v;
	return 0;
}

static int load_ppm(const char *path, unsigned char **out_rgb, int *ow, int *oh)
{
	FILE *f = fopen(path, "rb");
	if (!f)
		return -1;
	char magic[3] = { 0 };
	int w, h, maxval;
	if (fscanf(f, "%2s", magic) != 1 || strcmp(magic, "P6") ||
	    read_ppm_int(f, &w) || read_ppm_int(f, &h) || read_ppm_int(f, &maxval) ||
	    fgetc(f) == EOF) {
		fclose(f);
		return -1;
	}
	if (w <= 0 || h <= 0 || w > 8192 || h > 8192 || maxval != 255) {
		fclose(f);
		return -1;
	}
	unsigned char *rgb = malloc((size_t)w * h * 3);
	if (!rgb || fread(rgb, 1, (size_t)w * h * 3, f) != (size_t)w * h * 3) {
		free(rgb);
		fclose(f);
		return -1;
	}
	fclose(f);
	*out_rgb = rgb;
	*ow = w;
	*oh = h;
	return 0;
}

static uint32_t rd_le32(const unsigned char *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
static uint16_t rd_le16(const unsigned char *p) { return (uint16_t)(p[0] | p[1] << 8); }

static int load_bmp(const char *path, unsigned char **out_rgb, int *ow, int *oh)
{
	FILE *f = fopen(path, "rb");
	if (!f)
		return -1;
	unsigned char hdr[54];
	if (fread(hdr, 1, 54, f) != 54 || hdr[0] != 'B' || hdr[1] != 'M') {
		fclose(f);
		return -1;
	}
	uint32_t offbits = rd_le32(hdr + 10);
	int32_t w = (int32_t)rd_le32(hdr + 18);
	int32_t h = (int32_t)rd_le32(hdr + 22);
	uint16_t bpp = rd_le16(hdr + 28);
	uint32_t comp = rd_le32(hdr + 30);
	if (bpp != 24 || comp != 0) {
		fclose(f);
		return -1;
	}
	int flip = h > 0; /* positive height means bottom-up rows */
	int height = h > 0 ? h : -h;
	if (w <= 0 || height <= 0 || w > 8192 || height > 8192) {
		fclose(f);
		return -1;
	}
	int rowsz = ((w * 3 + 3) / 4) * 4;
	unsigned char *rgb = malloc((size_t)w * height * 3);
	unsigned char *row = malloc(rowsz);
	if (!rgb || !row || fseek(f, offbits, SEEK_SET) != 0) {
		free(rgb);
		free(row);
		fclose(f);
		return -1;
	}
	for (int y = 0; y < height; y++) {
		if (fread(row, 1, rowsz, f) != (size_t)rowsz) {
			free(rgb);
			free(row);
			fclose(f);
			return -1;
		}
		int dy = flip ? (height - 1 - y) : y;
		for (int x = 0; x < w; x++) {
			unsigned char b = row[x * 3 + 0], g = row[x * 3 + 1], r = row[x * 3 + 2];
			unsigned char *px = &rgb[(dy * w + x) * 3];
			px[0] = r;
			px[1] = g;
			px[2] = b;
		}
	}
	free(row);
	fclose(f);
	*out_rgb = rgb;
	*ow = w;
	*oh = height;
	return 0;
}

static void imgview_load(struct imgstate *im, const char *path)
{
	free(im->rgb);
	im->rgb = NULL;
	im->iw = im->ih = 0;
	snprintf(im->path, sizeof(im->path), "%s", path);

	const char *dot = strrchr(path, '.');
	int ok;
	if (dot && !strcasecmp(dot, ".bmp"))
		ok = load_bmp(path, &im->rgb, &im->iw, &im->ih) == 0;
	else
		ok = load_ppm(path, &im->rgb, &im->iw, &im->ih) == 0;

	const char *base = strrchr(path, '/');
	base = base ? base + 1 : path;
	if (ok)
		snprintf(im->status, sizeof(im->status), "%s  (%dx%d)", base, im->iw, im->ih);
	else
		snprintf(im->status, sizeof(im->status), "%s: unreadable image", base);
}

static int cmp_name(const void *a, const void *b)
{
	return strcmp((const char *)a, (const char *)b);
}

/* Sibling .ppm/.bmp files in the same directory, alphabetical; dir is +1/-1. */
static void imgview_nav(struct imgstate *im, int dir)
{
	char dirpath[FM_FULLLEN], curbase[FM_NAMELEN];
	snprintf(dirpath, sizeof(dirpath), "%s", im->path);
	char *slash = strrchr(dirpath, '/');
	if (!slash)
		return;
	snprintf(curbase, sizeof(curbase), "%s", slash + 1);
	*slash = 0;

	DIR *d = opendir(dirpath);
	if (!d)
		return;
	char names[256][FM_NAMELEN];
	int n = 0;
	struct dirent *de;
	while ((de = readdir(d)) && n < 256) {
		const char *dot = strrchr(de->d_name, '.');
		if (dot && (!strcasecmp(dot, ".ppm") || !strcasecmp(dot, ".bmp")))
			snprintf(names[n++], FM_NAMELEN, "%s", de->d_name);
	}
	closedir(d);
	if (n == 0)
		return;
	qsort(names, n, FM_NAMELEN, cmp_name);

	int idx = -1;
	for (int i = 0; i < n; i++)
		if (!strcmp(names[i], curbase))
			idx = i;
	int next = (idx < 0) ? 0 : (idx + dir + n) % n;

	char path[FM_FULLLEN];
	snprintf(path, sizeof(path), "%s/%s", dirpath, names[next]);
	imgview_load(im, path);
}

/* ---- input -------------------------------------------------------------- */

void imgview_click(struct window *w, int px, int py)
{
	struct imgstate *im = w->img;
	int content_y = w->y + TITLE_H;
	if (py < content_y || py >= content_y + IV_TOOLH || !im->path[0])
		return;
	int bw = 90;
	if (px < w->x + 8 + bw)
		imgview_nav(im, -1);
	else if (px >= w->x + w->w - 8 - bw)
		imgview_nav(im, 1);
}

/* ---- renderer ------------------------------------------------------------ */

void draw_imgview(struct window *w, int content_y, int content_h)
{
	struct imgstate *im = w->img;
	fill_rect(w->x, content_y, w->w, content_h, IV_BG);

	if (!im->path[0]) {
		draw_text(w->x + 14, content_y + 14, "Open an image (.ppm/.bmp) from Files", IV_DIM);
		return;
	}

	int bw = 90;
	fill_round_rect_grad(w->x + 8, content_y + 4, bw, IV_TOOLH - 8, 5, 0x33334a, 0x2b2b3a);
	draw_text(w->x + 8 + 14, content_y + (IV_TOOLH - font_h) / 2, "< Prev", IV_TXT);
	fill_round_rect_grad(w->x + w->w - 8 - bw, content_y + 4, bw, IV_TOOLH - 8, 5, 0x33334a, 0x2b2b3a);
	draw_text(w->x + w->w - 8 - bw + 10, content_y + (IV_TOOLH - font_h) / 2, "Next >", IV_TXT);
	draw_text_clip(w->x + 8 + bw + 10, content_y + (IV_TOOLH - font_h) / 2, im->status,
		       IV_DIM, w->w - 2 * (bw + 16));

	int avail_w = w->w - 16, avail_h = content_h - IV_TOOLH - 16;
	if (!im->rgb || avail_w <= 0 || avail_h <= 0 || im->iw <= 0 || im->ih <= 0)
		return;
	double scale = avail_w / (double)im->iw;
	double scale_h = avail_h / (double)im->ih;
	if (scale_h < scale)
		scale = scale_h;
	int dw = (int)(im->iw * scale), dh = (int)(im->ih * scale);
	if (dw < 1) dw = 1;
	if (dh < 1) dh = 1;
	int ox = w->x + 8 + (avail_w - dw) / 2;
	int oy = content_y + IV_TOOLH + 8 + (avail_h - dh) / 2;

	for (int y = 0; y < dh; y++) {
		int sy = (int)(y / scale);
		if (sy >= im->ih) sy = im->ih - 1;
		for (int x = 0; x < dw; x++) {
			int sx = (int)(x / scale);
			if (sx >= im->iw) sx = im->iw - 1;
			unsigned char *p = &im->rgb[(sy * im->iw + sx) * 3];
			put_pixel(ox + x, oy + y, (uint32_t)p[0] << 16 | (uint32_t)p[1] << 8 | p[2]);
		}
	}
}

int spawn_imgview(const char *path)
{
	int slot = -1;
	for (int i = 0; i < MAX_WIN; i++) {
		if (wins[i].used && wins[i].type == WIN_IMGVIEW) {
			slot = i;
			break;
		}
	}
	if (slot < 0) {
		slot = alloc_window_slot();
		if (slot < 0)
			return -1;
		struct imgstate *im = calloc(1, sizeof(struct imgstate));
		if (!im)
			return -1;
		memset(&wins[slot], 0, sizeof(wins[slot]));
		wins[slot].used = 1;
		wins[slot].type = WIN_IMGVIEW;
		wins[slot].pty_fd = -1;
		wins[slot].img = im;
		wins[slot].x = 280;
		wins[slot].y = 90;
		wins[slot].w = 420;
		wins[slot].h = 340;
		wins[slot].attr_fg = COL_FG_DEFAULT;
		wins[slot].attr_bg = COL_BG_DEFAULT;
		snprintf(wins[slot].title, sizeof(wins[slot].title), "Image Viewer");
		zorder[zcount++] = slot;
	}
	if (path)
		imgview_load(wins[slot].img, path);
	wins[slot].minimized = 0;
	raise_window(slot);
	focused = slot;
	return slot;
}
