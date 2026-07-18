/* fbdesktop -- PDF Viewer: no PDF parser of our own. Each page is rasterized
 * on demand by the system `pdftoppm` (poppler-utils, added to the rootfs) into
 * a private /tmp dir, then decoded with imgview.c's PPM loader and blitted
 * fit-to-window -- the exact same display path the Image Viewer uses. Prev/Next
 * just re-render; a render that yields no file means "no such page", so the
 * total page count never has to be known up front. */
#include "fbdesktop.h"

#define PV_BG    0x14141f
#define PV_TOOLH 32
#define PV_TXT   0xcdd6f4
#define PV_DIM   0x6c7086

/* pdftoppm -f N -l N -r 100 -png? No: -ppm keeps us on the no-library PPM
 * path. It writes "<root>-<page>.ppm" (or a zero-padded variant); we render
 * one page at a time into a fixed prefix and take whatever single file lands. */
static int pdf_render(struct pdfstate *p)
{
	free(p->rgb);
	p->rgb = NULL;
	p->iw = p->ih = 0;

	char prefix[FM_FULLLEN];
	snprintf(prefix, sizeof(prefix), "%s/page", p->tmpdir);

	char fs[16], ls[16];
	snprintf(fs, sizeof(fs), "%d", p->page);
	snprintf(ls, sizeof(ls), "%d", p->page);
	char *argv[] = { "pdftoppm", "-r", "100", "-f", fs, "-l", ls, "-ppm",
			 p->path, prefix, NULL };
	if (run_argv_wait(argv) != 0) {
		snprintf(p->status, sizeof(p->status), "pdftoppm failed (is this a PDF?)");
		return -1;
	}

	/* pdftoppm zero-pads the page number to the width of the last page, so we
	 * don't know the exact suffix -- take the single .ppm it dropped in. */
	DIR *d = opendir(p->tmpdir);
	if (!d) {
		snprintf(p->status, sizeof(p->status), "no output dir");
		return -1;
	}
	char found[FM_FULLLEN] = "";
	struct dirent *de;
	while ((de = readdir(d))) {
		const char *dot = strrchr(de->d_name, '.');
		if (dot && !strcasecmp(dot, ".ppm")) {
			snprintf(found, sizeof(found), "%s/%s", p->tmpdir, de->d_name);
			break;
		}
	}
	closedir(d);
	if (!found[0]) {
		snprintf(p->status, sizeof(p->status), "no such page");
		return -1;
	}

	int ok = load_ppm(found, &p->rgb, &p->iw, &p->ih) == 0;
	unlink(found); /* one page in flight at a time; don't let /tmp grow */
	if (!ok) {
		snprintf(p->status, sizeof(p->status), "could not decode page");
		return -1;
	}
	const char *base = strrchr(p->path, '/');
	base = base ? base + 1 : p->path;
	snprintf(p->status, sizeof(p->status), "%s  page %d", base, p->page);
	return 0;
}

/* Try to move to page cur+delta; if nothing renders there, stay put. */
static void pdf_nav(struct pdfstate *p, int delta)
{
	int want = p->page + delta;
	if (want < 1)
		return;
	int saved = p->page;
	p->page = want;
	if (pdf_render(p) != 0) {
		p->page = saved;
		pdf_render(p); /* restore the page we were on */
	}
}

/* ---- input -------------------------------------------------------------- */

void pdfview_click(struct window *w, int px, int py)
{
	struct pdfstate *p = w->pdf;
	int content_y = w->y + TITLE_H;
	if (py < content_y || py >= content_y + PV_TOOLH || !p->path[0])
		return;
	int bw = 90;
	if (px < w->x + 8 + bw)
		pdf_nav(p, -1);
	else if (px >= w->x + w->w - 8 - bw)
		pdf_nav(p, 1);
}

/* ---- renderer ------------------------------------------------------------ */

void draw_pdfview(struct window *w, int content_y, int content_h)
{
	struct pdfstate *p = w->pdf;
	fill_rect(w->x, content_y, w->w, content_h, PV_BG);

	if (!p->path[0]) {
		draw_text(w->x + 14, content_y + 14, "Open a .pdf from Files", PV_DIM);
		return;
	}

	int bw = 90;
	fill_round_rect_grad(w->x + 8, content_y + 4, bw, PV_TOOLH - 8, 5, 0x33334a, 0x2b2b3a);
	draw_text(w->x + 8 + 14, content_y + (PV_TOOLH - font_h) / 2, "< Prev", PV_TXT);
	fill_round_rect_grad(w->x + w->w - 8 - bw, content_y + 4, bw, PV_TOOLH - 8, 5, 0x33334a, 0x2b2b3a);
	draw_text(w->x + w->w - 8 - bw + 10, content_y + (PV_TOOLH - font_h) / 2, "Next >", PV_TXT);
	draw_text_clip(w->x + 8 + bw + 10, content_y + (PV_TOOLH - font_h) / 2, p->status,
		       PV_DIM, w->w - 2 * (bw + 16));

	int avail_w = w->w - 16, avail_h = content_h - PV_TOOLH - 16;
	if (!p->rgb || avail_w <= 0 || avail_h <= 0 || p->iw <= 0 || p->ih <= 0)
		return;
	double scale = avail_w / (double)p->iw;
	double scale_h = avail_h / (double)p->ih;
	if (scale_h < scale)
		scale = scale_h;
	int dw = (int)(p->iw * scale), dh = (int)(p->ih * scale);
	if (dw < 1) dw = 1;
	if (dh < 1) dh = 1;
	int ox = w->x + 8 + (avail_w - dw) / 2;
	int oy = content_y + PV_TOOLH + 8 + (avail_h - dh) / 2;

	for (int y = 0; y < dh; y++) {
		int sy = (int)(y / scale);
		if (sy >= p->ih) sy = p->ih - 1;
		for (int x = 0; x < dw; x++) {
			int sx = (int)(x / scale);
			if (sx >= p->iw) sx = p->iw - 1;
			unsigned char *pix = &p->rgb[(sy * p->iw + sx) * 3];
			put_pixel(ox + x, oy + y, (uint32_t)pix[0] << 16 | (uint32_t)pix[1] << 8 | pix[2]);
		}
	}
}

int spawn_pdfview(const char *path)
{
	int slot = -1;
	for (int i = 0; i < MAX_WIN; i++) {
		if (wins[i].used && wins[i].type == WIN_PDFVIEW) {
			slot = i;
			break;
		}
	}
	if (slot < 0) {
		slot = alloc_window_slot();
		if (slot < 0)
			return -1;
		struct pdfstate *p = calloc(1, sizeof(struct pdfstate));
		if (!p)
			return -1;
		/* Per-window scratch dir. mkdtemp needs the trailing XXXXXX template. */
		snprintf(p->tmpdir, sizeof(p->tmpdir), "/tmp/pdfview-XXXXXX");
		if (!mkdtemp(p->tmpdir)) {
			free(p);
			return -1;
		}
		memset(&wins[slot], 0, sizeof(wins[slot]));
		wins[slot].used = 1;
		wins[slot].type = WIN_PDFVIEW;
		wins[slot].pty_fd = -1;
		wins[slot].pdf = p;
		wins[slot].x = 280;
		wins[slot].y = 80;
		wins[slot].w = 460;
		wins[slot].h = 460;
		wins[slot].attr_fg = COL_FG_DEFAULT;
		wins[slot].attr_bg = COL_BG_DEFAULT;
		snprintf(wins[slot].title, sizeof(wins[slot].title), "PDF Viewer");
		zorder[zcount++] = slot;
	}
	if (path) {
		snprintf(wins[slot].pdf->path, sizeof(wins[slot].pdf->path), "%s", path);
		wins[slot].pdf->page = 1;
		pdf_render(wins[slot].pdf);
	}
	wins[slot].minimized = 0;
	raise_window(slot);
	focused = slot;
	return slot;
}
