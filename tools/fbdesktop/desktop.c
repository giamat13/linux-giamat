/* fbdesktop -- desktop: file-type classification, icons, the desktop directory,
 * taskbar, context menu, and the compositor (redraw_all) */
#include "fbdesktop.h"

enum fcat classify_file(const char *name, int isdir, int isexec)
{
	if (isdir)
		return FCAT_DIR;
	const char *dot = strrchr(name, '.');
	if (!dot || !dot[1])
		return isexec ? FCAT_EXEC : FCAT_OTHER;
	char ext[8];
	int i;
	for (i = 0; dot[1 + i] && i < 7; i++)
		ext[i] = (char)tolower((unsigned char)dot[1 + i]);
	ext[i] = 0;
	if (!strcmp(ext, "png") || !strcmp(ext, "jpg") || !strcmp(ext, "jpeg") ||
	    !strcmp(ext, "gif") || !strcmp(ext, "bmp") || !strcmp(ext, "svg"))
		return FCAT_IMAGE;
	if (!strcmp(ext, "zip") || !strcmp(ext, "tar") || !strcmp(ext, "gz") ||
	    !strcmp(ext, "xz") || !strcmp(ext, "bz2") || !strcmp(ext, "tgz"))
		return FCAT_ARCHIVE;
	if (!strcmp(ext, "sh") || !strcmp(ext, "c") || !strcmp(ext, "h") ||
	    !strcmp(ext, "py") || !strcmp(ext, "js") || !strcmp(ext, "pl"))
		return FCAT_CODE;
	if (!strcmp(ext, "txt") || !strcmp(ext, "md") || !strcmp(ext, "log") ||
	    !strcmp(ext, "conf") || !strcmp(ext, "cfg"))
		return FCAT_TEXT;
	return isexec ? FCAT_EXEC : FCAT_OTHER;
}

uint32_t fcat_color(enum fcat c)
{
	switch (c) {
	case FCAT_DIR:     return 0x89b4fa;
	case FCAT_IMAGE:   return 0xf9a825;
	case FCAT_ARCHIVE: return 0xa0785a;
	case FCAT_CODE:    return 0x22c55e;
	case FCAT_EXEC:    return 0xf43f5e;
	default:           return 0x94a3b8; /* TEXT and OTHER: same neutral as before */
	}
}

static int fcat_glyph(enum fcat c)
{
	switch (c) {
	case FCAT_DIR:     return G_FOLDER;
	case FCAT_IMAGE:   return G_IMAGE;
	case FCAT_ARCHIVE: return G_ARCHIVE;
	case FCAT_CODE:    return G_CODE;
	case FCAT_EXEC:    return G_EXEC;
	default:           return G_FILE;
	}
}

/* 5-char tag shown in the File Manager listing, same width as "[DIR]". */
const char *fcat_tag(enum fcat c)
{
	switch (c) {
	case FCAT_DIR:     return "[DIR]";
	case FCAT_IMAGE:   return "[IMG]";
	case FCAT_ARCHIVE: return "[ZIP]";
	case FCAT_CODE:    return "[SRC]";
	case FCAT_TEXT:    return "[TXT]";
	case FCAT_EXEC:    return "[BIN]";
	default:           return "     ";
	}
}

void init_icon_positions(void)
{
	int cols = (xres - ICON_GAP) / (ICON_W + ICON_GAP);
	if (cols < 1)
		cols = 1;
	for (int i = 0; i < NUM_ICONS; i++) {
		icons[i].x = ICON_GAP + (i % cols) * (ICON_W + ICON_GAP);
		icons[i].y = ICON_GAP + (i / cols) * (ICON_H + ICON_GAP);
	}
}

void clamp_icon(struct icon *ic)
{
	if (ic->x < 0) ic->x = 0;
	if (ic->y < 0) ic->y = 0;
	if (ic->x > xres - ICON_W) ic->x = xres - ICON_W;
	if (ic->y > yres - TASK_H - ICON_H) ic->y = yres - TASK_H - ICON_H;
}

/* Desktop files sit in the same grid as the fixed app icons, continuing
 * right after them; unlike app icons their position is always computed
 * (not draggable-to-reposition), since it comes from a real directory
 * listing that can change size at any time. */
void desk_item_pos(int idx, int *ox, int *oy)
{
	int cols = (xres - ICON_GAP) / (ICON_W + ICON_GAP);
	if (cols < 1)
		cols = 1;
	*ox = ICON_GAP + (idx % cols) * (ICON_W + ICON_GAP);
	*oy = ICON_GAP + (idx / cols) * (ICON_H + ICON_GAP);
}

static void draw_icons(void)
{
	for (int i = 0; i < NUM_ICONS; i++) {
		struct icon *ic = &icons[i];
		int lifted = (icon_press == i && icon_dragged);
		int tx = ic->x + (ICON_W - TILE) / 2;
		int ty = ic->y;
		uint32_t c = ic->color;

		/* drop shadow (deeper while the icon is lifted by a drag) */
		fill_round_rect(tx + 2, ty + (lifted ? 7 : 4), TILE, TILE, 18, 0x0c0c14);
		fill_round_rect_grad(tx, ty, TILE, TILE, 18,
				     mix(c, 0xffffff, lifted ? 75 : 40),
				     mix(c, 0x000000, 55));
		draw_glyph(ic->glyph, tx + TILE / 2, ty + TILE / 2,
			   0xffffff, mix(c, 0x000000, 78));

		int len = strlen(ic->label);
		int lx = ic->x + (ICON_W - len * font_w) / 2;
		draw_text(lx, ty + TILE + 9, ic->label, 0xdfe4f2);
	}

	for (int i = 0; i < desk_count; i++) {
		struct deskfile *df = &desk_files[i];
		int x, y;
		desk_item_pos(NUM_ICONS + i, &x, &y);
		int combined = NUM_ICONS + i;
		int lifted = (icon_press == combined && icon_dragged);
		int tx = x + (ICON_W - TILE) / 2, ty = y;
		enum fcat cat = classify_file(df->name, df->isdir, df->isexec);
		uint32_t c = fcat_color(cat);

		fill_round_rect(tx + 2, ty + (lifted ? 7 : 4), TILE, TILE, 18, 0x0c0c14);
		fill_round_rect_grad(tx, ty, TILE, TILE, 18,
				     mix(c, 0xffffff, lifted ? 75 : 40),
				     mix(c, 0x000000, 55));
		draw_glyph(fcat_glyph(cat), tx + TILE / 2, ty + TILE / 2,
			   0xffffff, mix(c, 0x000000, 78));
		draw_text_clip(x, ty + TILE + 9, df->name, 0xdfe4f2, ICON_W);
	}
}

int icon_at(int px, int py)
{
	for (int i = NUM_ICONS - 1; i >= 0; i--) {
		struct icon *ic = &icons[i];
		if (px >= ic->x && px < ic->x + ICON_W &&
		    py >= ic->y && py < ic->y + ICON_H)
			return i;
	}
	for (int i = desk_count - 1; i >= 0; i--) {
		int x, y;
		desk_item_pos(NUM_ICONS + i, &x, &y);
		if (px >= x && px < x + ICON_W && py >= y && py < y + ICON_H)
			return NUM_ICONS + i;
	}
	return -1;
}

/* Directories first, then alphabetical -- same ordering as the file manager. */
static int deskfile_cmp(const void *a, const void *b)
{
	const struct deskfile *x = a, *y = b;
	if (x->isdir != y->isdir)
		return y->isdir - x->isdir;
	return strcmp(x->name, y->name);
}

/* Load the desktop's real directory listing (DESKTOP_DIR) into desk_files. */
void desk_scan(void)
{
	desk_count = 0;
	DIR *d = opendir(DESKTOP_DIR);
	if (!d)
		return;
	struct dirent *de;
	while ((de = readdir(d)) && desk_count < DESK_MAXFILES) {
		if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, ".."))
			continue;
		if (!show_hidden && de->d_name[0] == '.')
			continue;
		if (strlen(de->d_name) >= FM_NAMELEN)
			continue;
		struct deskfile *e = &desk_files[desk_count];
		snprintf(e->name, FM_NAMELEN, "%s", de->d_name);
		char path[FM_FULLLEN];
		snprintf(path, sizeof(path), "%s/%s", DESKTOP_DIR, e->name);
		struct stat st;
		if (stat(path, &st) == 0) {
			e->isdir = S_ISDIR(st.st_mode);
			e->isexec = (st.st_mode & (S_IXUSR | S_IXGRP | S_IXOTH)) != 0;
		} else {
			e->isdir = e->isexec = 0;
		}
		desk_count++;
	}
	closedir(d);
	qsort(desk_files, desk_count, sizeof(struct deskfile), deskfile_cmp);
}

/* Start Menu: a button at the far left of the taskbar that pops up every
 * icon (apps, reboot, power off) as a plain list -- the same launch_icon()
 * a desktop double-click uses, so opening from the menu behaves identically. */
int start_x(void) { return 6; }

static int start_menu_h(void) { return NUM_ICONS * SM_ROWH + 8; }
static int start_menu_y(void) { return yres - TASK_H - start_menu_h() - 6; }

int start_hit(int px, int py)
{
	int ty = yres - TASK_H;
	return px >= start_x() && px < start_x() + START_W &&
	       py >= ty + 5 && py < ty + TASK_H - 5;
}

/* -1 if the click missed the popup entirely (caller still closes the menu). */
int start_menu_row_at(int px, int py)
{
	int mx0 = start_x(), my0 = start_menu_y();
	if (px < mx0 || px >= mx0 + SM_W || py < my0 || py >= my0 + start_menu_h())
		return -1;
	int row = (py - my0 - 4) / SM_ROWH;
	return (row >= 0 && row < NUM_ICONS) ? row : -1;
}

void draw_start_menu(void)
{
	if (!start_menu_open)
		return;
	int mx0 = start_x(), my0 = start_menu_y(), mh = start_menu_h();
	fill_round_rect(mx0 + 3, my0 + 4, SM_W, mh, 6, 0x0a0a11);
	fill_round_rect(mx0, my0, SM_W, mh, 6, 0x2e2e3c);
	for (int i = 0; i < NUM_ICONS; i++) {
		int ry = my0 + 4 + i * SM_ROWH;
		fill_circle(mx0 + 16, ry + SM_ROWH / 2, 6, icons[i].color);
		draw_text_clip(mx0 + 30, ry + (SM_ROWH - font_h) / 2, icons[i].label,
			       0xdfe4f2, SM_W - 38);
	}
}

/* "Show desktop": minimize everything, click again to bring it all back.
 * Sits at the far right of the taskbar, to the right of the clock. */

int sd_x(void)
{
	return xres - SD_W - 6;
}

/* Rightmost pixel the window buttons may use: clock and button come after. */
int task_limit(void)
{
	return sd_x() - 12 - 8 * font_w - 20;
}

void toggle_show_desktop(void)
{
	if (!sd_active) {
		for (int i = 0; i < MAX_WIN; i++) {
			sd_saved[i] = wins[i].used && !wins[i].minimized;
			if (sd_saved[i])
				wins[i].minimized = 1;
		}
		sd_active = 1;
	} else {
		for (int i = 0; i < MAX_WIN; i++)
			if (sd_saved[i] && wins[i].used)
				wins[i].minimized = 0;
		sd_active = 0;
	}
}

static void draw_taskbar(void)
{
	int ty = yres - TASK_H;
	fill_vgradient(0, ty, xres, TASK_H, 0x1c1c2a, 0x101018);
	fill_rect(0, ty, xres, 1, 0x3a3a4d);

	/* start button: a small 2x2 grid, like the show-desktop button's screen glyph */
	int stx = start_x(), sty2 = ty + 5, sth = TASK_H - 10;
	fill_round_rect_grad(stx, sty2, START_W, sth, 5,
			     start_menu_open ? 0x4a4c63 : 0x2b2b3a,
			     start_menu_open ? 0x393b52 : 0x22222e);
	uint32_t stc = start_menu_open ? themes[theme_idx].accent : 0x9399b2;
	fill_round_rect(stx + 9,  sty2 + 6,  8, 8, 2, stc);
	fill_round_rect(stx + 22, sty2 + 6,  8, 8, 2, stc);
	fill_round_rect(stx + 9,  sty2 + 17, 8, 8, 2, stc);
	fill_round_rect(stx + 22, sty2 + 17, 8, 8, 2, stc);

	/* show-desktop button: a small stylized screen */
	int sy = ty + 5, sh = TASK_H - 10, sx = sd_x();
	fill_round_rect_grad(sx, sy, SD_W, sh, 5,
			     sd_active ? 0x4a4c63 : 0x2b2b3a,
			     sd_active ? 0x393b52 : 0x22222e);
	fill_round_rect(sx + 8, sy + 7, SD_W - 16, sh - 16, 2,
			sd_active ? themes[theme_idx].accent : 0x9399b2);

	int bx = 8;
	for (int zi = 0; zi < zcount; zi++) {
		int i = zorder[zi];
		if (!wins[i].used)
			continue;
		int bw = 130, bh = TASK_H - 10;
		int by = ty + 5;
		if (bx + bw > task_limit())
			break; /* out of room before the clock */
		int act = (i == focused && !wins[i].minimized);
		fill_round_rect_grad(bx, by, bw, bh, 5,
				     act ? 0x4a4c63 : 0x2b2b3a,
				     act ? 0x393b52 : 0x22222e);
		if (act) /* accent underline on the focused task */
			fill_round_rect(bx + 6, by + bh - 3, bw - 12, 2, 1, win_accent(&wins[i]));
		fill_circle(bx + 11, by + bh / 2, 3,
			    wins[i].minimized ? 0x585b70 : win_accent(&wins[i]));
		draw_text_clip(bx + 20, by + (bh - font_h) / 2, wins[i].title,
			       act ? 0xffffff : 0xa6adc8, bw - 28);
		bx += bw + 6;
	}

	char timebuf[24];
	time_t t = time(NULL);
	struct tm *tm = localtime(&t);
	snprintf(timebuf, sizeof(timebuf), "%02d:%02d:%02d", tm->tm_hour, tm->tm_min, tm->tm_sec);
	int tw = (int)strlen(timebuf) * font_w;
	int cx = sx - 12 - tw; /* clock sits left of the show-desktop button */
	fill_rect(cx - 12, ty + 9, 1, TASK_H - 18, 0x3a3a4d);
	draw_text(cx, ty + (TASK_H - font_h) / 2, timebuf, 0xcdd6f4);
}

void cycle_window_focus(void)
{
	if (zcount == 0)
		return;
	int next = -1;
	for (int i = 0; i < zcount; i++) {
		if (zorder[i] == focused) {
			next = zorder[(i + 1) % zcount];
			break;
		}
	}
	if (next == -1 && zcount > 0)
		next = zorder[0];
	if (next >= 0 && wins[next].used) {
		focused = next; /* raise_window alone only reorders z; focus must follow */
		if (wins[next].minimized) {
			wins[next].minimized = 0;
			if (wins[next].maximized) {
				wins[next].x = wins[next].rx;
				wins[next].y = wins[next].ry;
				wins[next].w = wins[next].rw;
				wins[next].h = wins[next].rh;
				wins[next].maximized = 0;
			}
		}
		raise_window(next);
	}
}

static void draw_ctxmenu(void)
{
	if (ctxmenu_mode == CTXMODE_NONE)
		return;

	int has_target, target_isreg;
	if (ctxmenu_mode == CTXMODE_FILEWIN) {
		if (ctxmenu_win < 0 || !wins[ctxmenu_win].used) {
			ctxmenu_mode = CTXMODE_NONE;
			return;
		}
		struct fmstate *fm = wins[ctxmenu_win].fm;
		struct fent *e = NULL;
		if (ctxmenu_entidx >= 0 && ctxmenu_entidx < fm->count)
			e = &fm->ents[ctxmenu_entidx];
		has_target = e && strcmp(e->name, "..");
		target_isreg = has_target && e->isreg;
	} else {
		struct deskfile *df = NULL;
		if (ctxmenu_deskidx >= 0 && ctxmenu_deskidx < desk_count)
			df = &desk_files[ctxmenu_deskidx];
		has_target = df != NULL;
		target_isreg = df && !df->isdir;
	}

	int h = CTX_NITEMS * CTX_ITEMH;
	fill_round_rect(ctxmenu_x + 3, ctxmenu_y + 4, CTX_W, h, 6, 0x0a0a11);
	fill_round_rect(ctxmenu_x, ctxmenu_y, CTX_W, h, 6, 0x2e2e3c);
	for (int i = 0; i < CTX_NITEMS; i++) {
		int iy = ctxmenu_y + i * CTX_ITEMH;
		int enabled;
		if (i == 2) enabled = clip_mode;                                /* Paste */
		else if (i == 0) enabled = target_isreg;                        /* Copy */
		else if (i == 3) enabled = has_target && ctxmenu_mode == CTXMODE_FILEWIN; /* Rename */
		else enabled = has_target;                                      /* Cut, Delete */
		draw_text(ctxmenu_x + 10, iy + (CTX_ITEMH - font_h) / 2,
			  ctx_items[i], enabled ? 0xdfe4f2 : 0x585b70);
	}
}

/* While a file manager row is being dragged, show its name near the cursor
 * so the user can see what they're carrying and where it'll land. */
static void draw_fmdrag(void)
{
	if (fmdrag_win < 0 || !fmdrag_active || !wins[fmdrag_win].used)
		return;
	struct fmstate *fm = wins[fmdrag_win].fm;
	if (fmdrag_entidx < 0 || fmdrag_entidx >= fm->count)
		return;
	const char *name = fm->ents[fmdrag_entidx].name;
	int w = (int)strlen(name) * font_w + 16;
	fill_round_rect(mx + 15, my + 15, w, font_h + 8, 4, 0x0a0a11);
	fill_round_rect(mx + 14, my + 14, w, font_h + 8, 4, 0x313244);
	draw_text(mx + 22, my + 18, name, 0xf9e2af);
}

void redraw_all(void)
{
	fill_vgradient(0, 0, xres, yres - TASK_H,
		       themes[theme_idx].dtop, themes[theme_idx].dbot);
	draw_icons();
	for (int zi = 0; zi < zcount; zi++) {
		int i = zorder[zi];
		if (wins[i].used && !wins[i].minimized)
			draw_window(&wins[i]);
	}
	draw_taskbar();
	draw_start_menu();
	draw_ctxmenu();
	draw_fmdrag();
	draw_cursor(mx, my);
	if (backbuf)
		memcpy(fbp, backbuf, (size_t)line_length * yres);
}
