/* fbdesktop -- file manager: listing, search, clipboard, drag-and-drop, context menu
 * actions and the file operations behind them */
#include "fbdesktop.h"

/* ---- file manager ---- */

/* Listing geometry, shared by the renderer and every hit-test below so a click
 * lands on the row it visually points at. */
static int fm_rowh(void)          { return font_h + FM_ROWPAD; }
static int fm_list_top(struct window *w) { return w->y + TITLE_H + FM_TOOLH + FM_HEADH; }
static int fm_visible_rows(struct window *w)
{
	int h = w->h - TITLE_H - FM_TOOLH - FM_HEADH;
	return h > 0 ? h / fm_rowh() : 0;
}
/* Map a pixel y inside a FILES window to a listing row index, or -1. */
static int fm_row_at(struct window *w, int y)
{
	int r = (y - fm_list_top(w)) / fm_rowh();
	if (r < 0 || r >= fm_visible_rows(w))
		return -1;
	return r;
}

/* Case-insensitive substring test. */
static int fm_match(const char *name, const char *needle)
{
	if (!needle[0])
		return 1;
	size_t nlen = strlen(needle);
	for (const char *p = name; *p; p++) {
		size_t i = 0;
		while (i < nlen && p[i] &&
		       tolower((unsigned char)p[i]) == tolower((unsigned char)needle[i]))
			i++;
		if (i == nlen)
			return 1;
	}
	return 0;
}

/* Rebuild fm->view[] from fm->ents[] against the current search filter.
 * ".." always stays visible so search never traps you in a directory. */
static void fm_apply_filter(struct fmstate *fm)
{
	fm->vcount = 0;
	for (int i = 0; i < fm->count; i++) {
		if (!strcmp(fm->ents[i].name, "..") || fm_match(fm->ents[i].name, fm->search))
			fm->view[fm->vcount++] = i;
	}
}

/* Kept as the "the listing state changed, reclamp scroll" hook every caller
 * already invokes; the pixels are drawn live by draw_files each frame. */
void fm_render(struct window *w)
{
	struct fmstate *fm = w->fm;
	int maxscroll = fm->vcount - fm_visible_rows(w);
	if (maxscroll < 0) maxscroll = 0;
	if (fm->scroll > maxscroll) fm->scroll = maxscroll;
	if (fm->scroll < 0) fm->scroll = 0;
}

/* Directories first, then by name or (if fm_sort_by_size) by size descending. */
static int fent_cmp(const void *a, const void *b)
{
	const struct fent *x = a, *y = b;
	if (x->isdir != y->isdir)
		return y->isdir - x->isdir;
	if (fm_sort_by_size && x->size != y->size)
		return (y->size > x->size) - (y->size < x->size);
	return strcmp(x->name, y->name);
}

static void fm_path(struct fmstate *fm, const char *name, char *out, size_t n)
{
	snprintf(out, n, "%s%s%s", fm->cwd, strcmp(fm->cwd, "/") ? "/" : "", name);
}

void fm_load(struct window *w)
{
	struct fmstate *fm = w->fm;
	fm->count = 0;
	fm->scroll = 0;
	fm->sel = -1;
	fm->confirm_del = 0;

	if (strcmp(fm->cwd, "/") != 0) {
		snprintf(fm->ents[0].name, FM_NAMELEN, "..");
		fm->ents[0].isdir = 1;
		fm->ents[0].isreg = 0;
		fm->ents[0].size = 0;
		fm->count = 1;
	}

	DIR *d = opendir(fm->cwd);
	if (d) {
		int start = fm->count;
		struct dirent *de;
		while ((de = readdir(d)) && fm->count < FM_MAXENT) {
			if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, ".."))
				continue;
			struct fent *e = &fm->ents[fm->count];
			/* Skip hidden files unless show_hidden is on. */
			if (!show_hidden && de->d_name[0] == '.')
				continue;
			/* A name too long to store is a name we could never open again. */
			if (strlen(de->d_name) >= FM_NAMELEN)
				continue;
			snprintf(e->name, FM_NAMELEN, "%s", de->d_name);
			char path[FM_FULLLEN];
			fm_path(fm, e->name, path, sizeof(path));
			struct stat st;
			if (stat(path, &st) == 0) {
				e->isdir = S_ISDIR(st.st_mode);
				e->isreg = S_ISREG(st.st_mode);
				e->isexec = (st.st_mode & (S_IXUSR | S_IXGRP | S_IXOTH)) != 0;
				e->size = (long)st.st_size;
			} else {
				e->isdir = e->isreg = e->isexec = 0;
				e->size = 0;
			}
			fm->count++;
		}
		closedir(d);
		/* sort everything after the ".." entry, which must stay first */
		qsort(fm->ents + start, fm->count - start, sizeof(struct fent), fent_cmp);
	}
	fm_apply_filter(fm);
	snprintf(w->title, sizeof(w->title), "%s", fm->cwd);
	fm_render(w);
}

/* Toolbar hit-test: returns button index or -1. */
static int fm_btn_at(struct window *w, int px, int py)
{
	int by = w->y + TITLE_H;
	if (py < by || py >= by + FM_TOOLH)
		return -1;
	int idx = (px - w->x - 6) / (FM_BTNW + 4);
	if (idx < 0 || idx >= FM_NBTN)
		return -1;
	return idx;
}

/* Delete the selected entry. Files are unlinked, directories must be empty. */
/* Copy/Cut the selected entry to the clipboard -- shared by the Ctrl+C/
 * Ctrl+X keyboard shortcuts and the right-click menu's Copy/Cut items. */
static void fm_copy_selected(struct window *w, int cut)
{
	struct fmstate *fm = w->fm;
	if (fm->sel < 0 || fm->sel >= fm->count) {
		snprintf(fm->status, sizeof(fm->status), "select something first");
		return;
	}
	struct fent *e = &fm->ents[fm->sel];
	if (!strcmp(e->name, "..")) {
		snprintf(fm->status, sizeof(fm->status), "cannot %s ..", cut ? "cut" : "copy");
		return;
	}
	if (!cut && !e->isreg) {
		snprintf(fm->status, sizeof(fm->status), "select a file to copy");
		return;
	}
	fm_path(fm, e->name, clip_path, sizeof(clip_path));
	clip_mode = cut ? 2 : 1;
	snprintf(fm->status, sizeof(fm->status), "%s %s", cut ? "cut" : "copied", e->name);
}

static void fm_delete(struct window *w)
{
	struct fmstate *fm = w->fm;
	if (fm->sel < 0 || fm->sel >= fm->count) {
		snprintf(fm->status, sizeof(fm->status), "select something first");
		return;
	}
	struct fent *e = &fm->ents[fm->sel];
	if (!strcmp(e->name, "..")) {
		snprintf(fm->status, sizeof(fm->status), "cannot delete ..");
		return;
	}
	char path[FM_FULLLEN];
	fm_path(fm, e->name, path, sizeof(path));
	int r = e->isdir ? rmdir(path) : unlink(path);
	if (r != 0)
		snprintf(fm->status, sizeof(fm->status), "delete failed: %s", strerror(errno));
	else
		snprintf(fm->status, sizeof(fm->status), "deleted %s", e->name);
	fm_load(w);
}

/* Create whatever the prompt was asking for, named by fm->pbuf. */
static void fm_create(struct window *w)
{
	struct fmstate *fm = w->fm;
	if (!fm->pbuf[0] || strchr(fm->pbuf, '/')) {
		snprintf(fm->status, sizeof(fm->status), "bad name");
		fm->prompt = 0;
		return;
	}
	/* Every file this app creates should carry an extension, so its type
	 * icon/tag is always known -- directories have no such concept. */
	if (fm->prompt == 1 && !strchr(fm->pbuf, '.')) {
		size_t len = strlen(fm->pbuf);
		if (len + 4 < sizeof(fm->pbuf))
			memcpy(fm->pbuf + len, ".txt", 5);
	}
	char path[FM_FULLLEN];
	fm_path(fm, fm->pbuf, path, sizeof(path));
	int ok;
	if (fm->prompt == 2) {
		ok = mkdir(path, 0755) == 0;
	} else {
		int fd = open(path, O_WRONLY | O_CREAT | O_EXCL, 0644);
		ok = fd >= 0;
		if (fd >= 0)
			close(fd);
	}
	if (!ok)
		snprintf(fm->status, sizeof(fm->status), "create failed: %s", strerror(errno));
	else
		snprintf(fm->status, sizeof(fm->status), "created %s", fm->pbuf);
	fm->prompt = 0;
	fm->pbuf[0] = 0;
	fm_load(w);
}

/* Rename fm->sel to fm->pbuf, both within the current directory. */
static void fm_rename(struct window *w)
{
	struct fmstate *fm = w->fm;
	if (!fm->pbuf[0] || strchr(fm->pbuf, '/')) {
		snprintf(fm->status, sizeof(fm->status), "bad name");
		fm->prompt = 0;
		return;
	}
	if (fm->sel < 0 || fm->sel >= fm->count) {
		fm->prompt = 0;
		return;
	}
	struct fent *e = &fm->ents[fm->sel];
	/* Renaming a file shouldn't be able to strip its extension away. */
	if (!e->isdir && !strchr(fm->pbuf, '.')) {
		size_t len = strlen(fm->pbuf);
		if (len + 4 < sizeof(fm->pbuf))
			memcpy(fm->pbuf + len, ".txt", 5);
	}
	char oldpath[FM_FULLLEN], newpath[FM_FULLLEN];
	fm_path(fm, e->name, oldpath, sizeof(oldpath));
	fm_path(fm, fm->pbuf, newpath, sizeof(newpath));
	if (rename(oldpath, newpath) != 0)
		snprintf(fm->status, sizeof(fm->status), "rename failed: %s", strerror(errno));
	else
		snprintf(fm->status, sizeof(fm->status), "renamed to %s", fm->pbuf);
	fm->prompt = 0;
	fm->pbuf[0] = 0;
	fm_load(w);
}

/* Paste the clipboard into the current directory. Cut = rename (same fs
 * only); Copy = plain byte-for-byte file copy, no directories. */
static void fm_paste(struct window *w)
{
	struct fmstate *fm = w->fm;
	if (!clip_mode) {
		snprintf(fm->status, sizeof(fm->status), "clipboard is empty");
		return;
	}
	const char *base = strrchr(clip_path, '/');
	base = base ? base + 1 : clip_path;
	char dest[FM_FULLLEN];
	fm_path(fm, base, dest, sizeof(dest));

	/* Copying onto its own path would truncate the source while reading
	 * it (open dest "wb" == open src "wb"). Cut is harmless here (rename
	 * to the same path is a no-op) but there's nothing useful to do either. */
	if (!strcmp(dest, clip_path)) {
		snprintf(fm->status, sizeof(fm->status), "already here");
		return;
	}

	if (clip_mode == 2) {
		if (rename(clip_path, dest) == 0) {
			snprintf(fm->status, sizeof(fm->status), "moved %s", base);
			clip_mode = 0;
		} else {
			snprintf(fm->status, sizeof(fm->status), "move failed: %s", strerror(errno));
		}
	} else {
		FILE *in = fopen(clip_path, "rb");
		FILE *out = in ? fopen(dest, "wb") : NULL;
		if (!in || !out) {
			snprintf(fm->status, sizeof(fm->status), "copy failed: %s", strerror(errno));
		} else {
			char buf[4096];
			size_t n;
			while ((n = fread(buf, 1, sizeof(buf), in)) > 0)
				fwrite(buf, 1, n, out);
			snprintf(fm->status, sizeof(fm->status), "pasted %s", base);
		}
		if (in) fclose(in);
		if (out) fclose(out);
	}
	fm_load(w);
}

/* Paste the clipboard onto the desktop (DESKTOP_DIR). Mirrors fm_paste. */
static void desk_paste(void)
{
	if (!clip_mode)
		return;
	const char *base = strrchr(clip_path, '/');
	base = base ? base + 1 : clip_path;
	char dest[FM_FULLLEN];
	snprintf(dest, sizeof(dest), "%s/%s", DESKTOP_DIR, base);

	if (!strcmp(dest, clip_path)) /* see fm_paste: copying onto itself corrupts it */
		return;

	if (clip_mode == 2) {
		if (rename(clip_path, dest) == 0)
			clip_mode = 0;
	} else {
		FILE *in = fopen(clip_path, "rb");
		FILE *out = in ? fopen(dest, "wb") : NULL;
		if (in && out) {
			char buf[4096];
			size_t n;
			while ((n = fread(buf, 1, sizeof(buf), in)) > 0)
				fwrite(buf, 1, n, out);
		}
		if (in) fclose(in);
		if (out) fclose(out);
	}
	desk_scan();
}

/* Right-click: open the context menu over whichever FILES window (and row,
 * if any) is under the cursor, or over the desktop background itself.
 * Closes any menu already open elsewhere. */
void ctxmenu_open(int x, int y)
{
	ctxmenu_mode = CTXMODE_NONE;
	ctxmenu_win = -1;
	for (int zi = zcount - 1; zi >= 0; zi--) {
		int i = zorder[zi];
		if (!wins[i].used || wins[i].minimized || wins[i].type != WIN_FILES)
			continue;
		struct window *w = &wins[i];
		if (x < w->x || x >= w->x + w->w || y < w->y || y >= w->y + w->h)
			continue;
		raise_window(i);
		focused = i;

		struct fmstate *fm = w->fm;
		int row = fm_row_at(w, y);
		int entidx = -1;
		if (row >= 0) {
			int vi = fm->scroll + row;
			if (vi >= 0 && vi < fm->vcount)
				entidx = fm->view[vi];
		}
		if (entidx >= 0)
			fm->sel = entidx;
		ctxmenu_mode = CTXMODE_FILEWIN;
		ctxmenu_entidx = entidx;
		ctxmenu_win = i;
		ctxmenu_x = x;
		ctxmenu_y = y;
		int h = CTX_NITEMS * CTX_ITEMH;
		if (ctxmenu_x + CTX_W > xres) ctxmenu_x = xres - CTX_W;
		if (ctxmenu_y + h > yres - TASK_H) ctxmenu_y = yres - TASK_H - h;
		fm_render(w);
		return;
	}

	/* Not over any FILES window: the desktop background itself, unless
	 * the click landed on the taskbar. */
	if (y < yres - TASK_H) {
		ctxmenu_mode = CTXMODE_DESKTOP;
		ctxmenu_deskidx = icon_at(x, y);
		if (ctxmenu_deskidx < NUM_ICONS)
			ctxmenu_deskidx = -1; /* fixed app icons aren't file targets */
		else
			ctxmenu_deskidx -= NUM_ICONS;
		ctxmenu_x = x;
		ctxmenu_y = y;
		int h = CTX_NITEMS * CTX_ITEMH;
		if (ctxmenu_x + CTX_W > xres) ctxmenu_x = xres - CTX_W;
		if (ctxmenu_y + h > yres - TASK_H) ctxmenu_y = yres - TASK_H - h;
	}
}

/* Returns 1 if the click landed on the open menu (whether or not it hit an
 * item), 0 if the caller should still run the normal hit-test. Either way
 * the menu is closed by the caller right after. */
int ctxmenu_click(int x, int y)
{
	if (ctxmenu_mode == CTXMODE_NONE)
		return 0;
	int h = CTX_NITEMS * CTX_ITEMH;
	if (x < ctxmenu_x || x >= ctxmenu_x + CTX_W || y < ctxmenu_y || y >= ctxmenu_y + h)
		return 0;
	int idx = (y - ctxmenu_y) / CTX_ITEMH;

	if (ctxmenu_mode == CTXMODE_FILEWIN) {
		if (ctxmenu_win < 0 || !wins[ctxmenu_win].used)
			return 1;
		struct window *w = &wins[ctxmenu_win];
		struct fmstate *fm = w->fm;
		struct fent *e = NULL;
		if (ctxmenu_entidx >= 0 && ctxmenu_entidx < fm->count)
			e = &fm->ents[ctxmenu_entidx];
		int has_target = e && strcmp(e->name, "..");
		fm->status[0] = 0;

		switch (idx) {
		case 0: /* Copy */
			if (has_target) {
				fm->sel = ctxmenu_entidx;
				fm_copy_selected(w, 0);
			} else {
				snprintf(fm->status, sizeof(fm->status), "select a file to copy");
			}
			break;
		case 1: /* Cut */
			if (has_target) {
				fm->sel = ctxmenu_entidx;
				fm_copy_selected(w, 1);
			} else {
				snprintf(fm->status, sizeof(fm->status), "select something to cut");
			}
			break;
		case 2: /* Paste */
			fm_paste(w);
			break;
		case 3: /* Rename */
			if (has_target) {
				fm->sel = ctxmenu_entidx;
				fm->prompt = 3;
				snprintf(fm->pbuf, sizeof(fm->pbuf), "%s", e->name);
			} else {
				snprintf(fm->status, sizeof(fm->status), "select something to rename");
			}
			break;
		case 4: /* Delete */
			if (has_target) {
				fm->sel = ctxmenu_entidx;
				fm_delete(w);
			} else {
				snprintf(fm->status, sizeof(fm->status), "select something to delete");
			}
			break;
		case 5: /* Compress */
			if (has_target) {
				char path[FM_FULLLEN];
				fm_path(fm, e->name, path, sizeof(path));
				if (archive_create(path) == 0)
					snprintf(fm->status, sizeof(fm->status), "compressed %s.tar.gz", e->name);
				else
					snprintf(fm->status, sizeof(fm->status), "compress failed");
				fm_load(w);
			} else {
				snprintf(fm->status, sizeof(fm->status), "select something to compress");
			}
			break;
		}
		fm_render(w);
	} else { /* CTXMODE_DESKTOP */
		struct deskfile *df = NULL;
		if (ctxmenu_deskidx >= 0 && ctxmenu_deskidx < desk_count)
			df = &desk_files[ctxmenu_deskidx];

		switch (idx) {
		case 0: /* Copy -- files only, same restriction as the file manager */
			if (df && !df->isdir) {
				snprintf(clip_path, sizeof(clip_path), "%s/%s", DESKTOP_DIR, df->name);
				clip_mode = 1;
			}
			break;
		case 1: /* Cut */
			if (df) {
				snprintf(clip_path, sizeof(clip_path), "%s/%s", DESKTOP_DIR, df->name);
				clip_mode = 2;
			}
			break;
		case 2: /* Paste */
			desk_paste();
			break;
		case 3: /* Rename: not supported on the desktop -- ponytail, would
			 * need its own text-entry prompt outside any window. */
			break;
		case 4: /* Delete */
			if (df) {
				char path[FM_FULLLEN];
				snprintf(path, sizeof(path), "%s/%s", DESKTOP_DIR, df->name);
				if (df->isdir) rmdir(path); else unlink(path);
				desk_scan();
			}
			break;
		case 5: /* Compress */
			if (df) {
				char path[FM_FULLLEN];
				snprintf(path, sizeof(path), "%s/%s", DESKTOP_DIR, df->name);
				archive_create(path);
				desk_scan();
			}
			break;
		}
	}
	return 1;
}

static void fm_toolbar(struct window *w, int btn)
{
	struct fmstate *fm = w->fm;
	fm->status[0] = 0;
	if (btn != 2)
		fm->confirm_del = 0;
	switch (btn) {
	case 0:
	case 1:
		fm->prompt = btn == 0 ? 1 : 2;
		fm->pbuf[0] = 0;
		break;
	case 2:
		/* Deleting is irreversible: the first click only arms the button --
		 * unless the "Confirm before delete" setting is off. */
		if (!confirm_delete) {
			fm_delete(w);
		} else if (!fm->confirm_del) {
			fm->confirm_del = 1;
			snprintf(fm->status, sizeof(fm->status), "click Delete again to confirm");
		} else {
			fm->confirm_del = 0;
			fm_delete(w);
		}
		break;
	case 3:
		fm_load(w);
		break;
	}
}

/* Open the entry at fm->ents[entidx] in window winidx: ".." goes up, a
 * directory navigates into it, a regular file opens the text editor.
 * Split out of fm_click so a completed row press-without-drag and a
 * completed row press-then-drag-then-drop can share it. */
void fm_open_selected(int winidx, int entidx)
{
	if (winidx < 0 || !wins[winidx].used)
		return;
	struct window *w = &wins[winidx];
	struct fmstate *fm = w->fm;
	if (entidx < 0 || entidx >= fm->count)
		return;
	struct fent *e = &fm->ents[entidx];

	if (!strcmp(e->name, "..")) {
		char *slash = strrchr(fm->cwd, '/');
		if (slash && slash != fm->cwd)
			*slash = 0;
		else
			strcpy(fm->cwd, "/");
		fm->search[0] = 0;
		fm_load(w);
		return;
	}

	char path[FM_FULLLEN];
	fm_path(fm, e->name, path, sizeof(path));

	if (e->isdir) {
		/* Refuse rather than truncate: a truncated cwd is a wrong directory. */
		if (strlen(path) >= sizeof(fm->cwd))
			return;
		memcpy(fm->cwd, path, strlen(path) + 1);
		fm->search[0] = 0;
		fm_load(w);
	} else if (e->isreg) {
		/* Regular files only: opening a fifo or char device would block forever. */
		open_regular_file(path, e->name, e->isexec);
	}
}

/* Open a regular file the way double-clicking it should: the viewer/manager
 * that matches its type, falling back to the text editor. Shared by the file
 * manager listing and the desktop icons, so the two can't drift apart. */
void open_regular_file(const char *path, const char *name, int isexec)
{
	enum fcat cat = classify_file(name, 0, isexec);
	const char *dot = strrchr(name, '.');
	if (cat == FCAT_IMAGE && dot && (!strcasecmp(dot, ".ppm") || !strcasecmp(dot, ".bmp")))
		spawn_imgview(path);
	else if (cat == FCAT_ARCHIVE && dot &&
		 (!strcasecmp(dot, ".tar") || !strcasecmp(dot, ".gz") || !strcasecmp(dot, ".tgz")))
		spawn_archive(path);
	else if (cat == FCAT_PDF)
		spawn_pdfview(path);
	else
		spawn_editor(path);
}

/* Drop the file that was being dragged (fmdrag_win/fmdrag_entidx) at (x,y):
 * onto a directory row in any FILES window -> move it there; onto the
 * desktop background -> copy it into DESKTOP_DIR; anywhere else -> no-op. */
void fm_drop(int x, int y)
{
	if (fmdrag_win < 0 || !wins[fmdrag_win].used)
		return;
	struct window *sw = &wins[fmdrag_win];
	struct fmstate *sfm = sw->fm;
	if (fmdrag_entidx < 0 || fmdrag_entidx >= sfm->count)
		return;
	struct fent *se = &sfm->ents[fmdrag_entidx];
	if (!strcmp(se->name, ".."))
		return;
	char srcpath[FM_FULLLEN];
	fm_path(sfm, se->name, srcpath, sizeof(srcpath));

	/* Dropped on another (or the same) FILES window's directory row? Move. */
	for (int zi = zcount - 1; zi >= 0; zi--) {
		int i = zorder[zi];
		if (!wins[i].used || wins[i].minimized || wins[i].type != WIN_FILES)
			continue;
		struct window *tw = &wins[i];
		if (x < tw->x || x >= tw->x + tw->w || y < tw->y || y >= tw->y + tw->h)
			continue;

		struct fmstate *tfm = tw->fm;
		int row = fm_row_at(tw, y);
		if (row < 0)
			return; /* dropped on the titlebar/toolbar/header: no-op */
		int vi = tfm->scroll + row;
		if (vi < 0 || vi >= tfm->vcount)
			return; /* dropped past the end of the listing: no-op */
		int ti = tfm->view[vi];
		struct fent *te = &tfm->ents[ti];

		char destdir[FM_FULLLEN];
		if (!strcmp(te->name, "..")) {
			char *slash = strrchr(tfm->cwd, '/');
			if (slash && slash != tfm->cwd) {
				size_t n = (size_t)(slash - tfm->cwd);
				memcpy(destdir, tfm->cwd, n);
				destdir[n] = 0;
			} else {
				strcpy(destdir, "/");
			}
		} else if (te->isdir) {
			fm_path(tfm, te->name, destdir, sizeof(destdir));
		} else {
			return; /* dropped onto a file row: no-op */
		}

		char dest[FM_FULLLEN];
		snprintf(dest, sizeof(dest), "%s%s%s", destdir,
			 strcmp(destdir, "/") ? "/" : "", se->name);
		if (!strcmp(srcpath, dest))
			return; /* dropped onto its own folder */
		if (rename(srcpath, dest) == 0)
			snprintf(sfm->status, sizeof(sfm->status), "moved %s", se->name);
		else
			snprintf(sfm->status, sizeof(sfm->status), "move failed: %s", strerror(errno));
		fm_load(sw);
		if (tw != sw)
			fm_load(tw);
		return;
	}

	/* Not over any FILES window: the desktop, if it's not the taskbar.
	 * Files only -- same restriction as fm_paste's Copy. */
	if (y < yres - TASK_H) {
		if (se->isdir) {
			snprintf(sfm->status, sizeof(sfm->status), "cannot copy directories to desktop");
			fm_render(sw);
			return;
		}
		char dest[FM_FULLLEN];
		snprintf(dest, sizeof(dest), "%s/%s", DESKTOP_DIR, se->name);
		FILE *in = fopen(srcpath, "rb");
		FILE *out = in ? fopen(dest, "wb") : NULL;
		if (in && out) {
			char buf[4096];
			size_t n;
			while ((n = fread(buf, 1, sizeof(buf), in)) > 0)
				fwrite(buf, 1, n, out);
			snprintf(sfm->status, sizeof(sfm->status), "copied %s to desktop", se->name);
			desk_scan();
		} else {
			snprintf(sfm->status, sizeof(sfm->status), "copy failed: %s", strerror(errno));
		}
		if (in) fclose(in);
		if (out) fclose(out);
		fm_render(sw);
	}
}

void fm_click(struct window *w, int x, int y)
{
	struct fmstate *fm = w->fm;
	int btn = fm_btn_at(w, x, y);
	if (btn >= 0) {
		fm_toolbar(w, btn);
		fm_render(w);
		return;
	}
	fm->status[0] = 0;
	fm->confirm_del = 0;

	int row = fm_row_at(w, y);
	if (row < 0)
		return;
	int vi = fm->scroll + row;
	if (vi < 0 || vi >= fm->vcount)
		return;
	int i = fm->view[vi];

	/* Arm this row as a drag candidate; motion past a threshold turns it
	 * into a drag (handled in process_pointer). If it never moved, the
	 * release replays today's click semantics: first click selects,
	 * a second click on an already-selected row opens it. */
	fmdrag_win = (int)(w - wins);
	fmdrag_entidx = i;
	fmdrag_active = 0;
	fmdrag_grab_x = x;
	fmdrag_grab_y = y;
	fmdrag_was_preselected = (fm->sel == i);
	fm->sel = i;
	fm_render(w);
}

/* While a New File / New Dir / Rename prompt is open, keys go into the name
 * field. While searching, keys go into the search filter. Otherwise arrows /
 * PageUp / PageDown scroll the listing and Ctrl+F starts a search. */
int fm_keys(struct window *w, const char *buf, int n)
{
	struct fmstate *fm = w->fm;
	int changed = 0;

	if (fm->prompt) {
		for (int i = 0; i < n; i++) {
			unsigned char c = (unsigned char)buf[i];
			int len = (int)strlen(fm->pbuf);
			if (c == '\r' || c == '\n') {
				if (fm->prompt == 3)
					fm_rename(w);
				else
					fm_create(w);
			} else if (c == 0x1b) {
				fm->prompt = 0;
				fm->pbuf[0] = 0;
			} else if ((c == 0x7f || c == '\b') && len > 0) {
				fm->pbuf[len - 1] = 0;
			} else if (c >= 0x20 && c < 0x7f && len < FM_NAMELEN - 1) {
				fm->pbuf[len] = (char)c;
				fm->pbuf[len + 1] = 0;
			}
			changed = 1;
			if (!fm->prompt)
				break; /* Enter/Esc ended it; the rest isn't ours */
		}
		fm_render(w);
		return changed;
	}

	if (fm->searching) {
		for (int i = 0; i < n; i++) {
			unsigned char c = (unsigned char)buf[i];
			int len = (int)strlen(fm->search);
			if (c == '\r' || c == '\n') {
				fm->searching = 0;
			} else if (c == 0x1b) {
				fm->searching = 0;
				fm->search[0] = 0;
				fm->sel = -1;
			} else if ((c == 0x7f || c == '\b') && len > 0) {
				fm->search[len - 1] = 0;
			} else if (c >= 0x20 && c < 0x7f && len < FM_NAMELEN - 1) {
				fm->search[len] = (char)c;
				fm->search[len + 1] = 0;
			} else {
				continue;
			}
			fm_apply_filter(fm);
			changed = 1;
			if (!fm->searching)
				break; /* Enter/Esc ended input; the rest isn't ours */
		}
		fm_render(w);
		return changed;
	}

	for (int i = 0; i < n; i++) {
		unsigned char c = (unsigned char)buf[i];

		if (c == 0x1b && i + 1 < n && buf[i + 1] == '[') {
			if (i + 3 < n && buf[i + 2] == '3' && buf[i + 3] == '~') {
				/* Delete key: ESC [ 3 ~ -- same two-press confirm as
				 * the toolbar's Delete button. */
				fm->status[0] = 0;
				if (!fm->confirm_del) {
					fm->confirm_del = 1;
					snprintf(fm->status, sizeof(fm->status), "press Delete again to confirm");
				} else {
					fm->confirm_del = 0;
					fm_delete(w);
				}
				fm_render(w);
				changed = 1;
				i += 3;
				continue;
			}
			if (i + 2 < n) {
				int delta = 0;
				switch (buf[i + 2]) {
				case 'A': delta = -1; break;
				case 'B': delta = 1; break;
				case '5': delta = -(fm_visible_rows(w) - 1); break;
				case '6': delta = fm_visible_rows(w) - 1; break;
				default: break;
				}
				fm->confirm_del = 0;
				if (delta) {
					fm->scroll += delta;
					fm_render(w); /* clamps scroll */
					changed = 1;
				}
				i += 2;
				continue;
			}
		}

		if (c == 0x06) { /* Ctrl+F */
			fm->confirm_del = 0;
			fm->searching = 1;
			fm_render(w);
			return 1;
		}
		if (c == 0x03) { /* Ctrl+C */
			fm->confirm_del = 0;
			fm_copy_selected(w, 0);
			fm_render(w);
			changed = 1;
		} else if (c == 0x18) { /* Ctrl+X */
			fm->confirm_del = 0;
			fm_copy_selected(w, 1);
			fm_render(w);
			changed = 1;
		} else if (c == 0x16) { /* Ctrl+V */
			fm->confirm_del = 0;
			fm_paste(w);
			changed = 1;
		}
	}
	return changed;
}

/* ---- listing renderer ---------------------------------------------- */

static void human_size(long b, char *out, size_t n)
{
	if (b < 1024)
		snprintf(out, n, "%ld B", b);
	else if (b < 1024L * 1024)
		snprintf(out, n, "%.1f K", b / 1024.0);
	else if (b < 1024L * 1024 * 1024)
		snprintf(out, n, "%.1f M", b / (1024.0 * 1024));
	else
		snprintf(out, n, "%.1f G", b / (1024.0 * 1024 * 1024));
}

/* A ~16px file-type icon, composed from the exported primitives; `bg` is the
 * row colour, used to punch cutouts back out of the ink. */
static void fm_icon(enum fcat cat, int cx, int cy, uint32_t col, uint32_t bg)
{
	switch (cat) {
	case FCAT_DIR:
		fill_round_rect(cx - 8, cy - 7, 7, 5, 2, col);       /* tab */
		fill_round_rect(cx - 8, cy - 4, 16, 10, 2, col);     /* body */
		break;
	case FCAT_IMAGE:
		fill_round_rect(cx - 8, cy - 6, 16, 12, 2, col);
		fill_round_rect(cx - 6, cy - 4, 12, 8, 1, bg);
		fill_circle(cx + 3, cy - 1, 2, col);
		fill_rect(cx - 6, cy + 2, 12, 2, col);
		break;
	case FCAT_ARCHIVE:
		fill_round_rect(cx - 7, cy - 7, 14, 14, 2, col);
		fill_rect(cx - 7, cy - 1, 14, 3, bg);
		fill_round_rect(cx - 2, cy - 5, 4, 3, 1, bg);
		break;
	case FCAT_CODE:
		fill_round_rect(cx - 7, cy - 8, 14, 16, 2, col);
		fill_rect(cx - 4, cy - 1, 2, 2, bg);
		fill_rect(cx - 5, cy + 1, 2, 2, bg);
		fill_rect(cx - 4, cy + 3, 2, 2, bg);
		fill_rect(cx + 2, cy - 1, 2, 2, bg);
		fill_rect(cx + 3, cy + 1, 2, 2, bg);
		fill_rect(cx + 2, cy + 3, 2, 2, bg);
		break;
	case FCAT_EXEC:
		fill_round_rect(cx - 8, cy - 7, 16, 14, 4, col);
		fill_round_rect(cx - 3, cy - 3, 6, 6, 1, mix(col, bg, 150));
		break;
	default: /* TEXT, OTHER: a page with a couple of text lines */
		fill_round_rect(cx - 6, cy - 8, 12, 16, 2, col);
		fill_rect(cx - 3, cy - 3, 6, 1, bg);
		fill_rect(cx - 3, cy,     6, 1, bg);
		fill_rect(cx - 3, cy + 3, 5, 1, bg);
		break;
	}
}

void draw_files(struct window *w, int content_y, int content_h)
{
	struct fmstate *fm = w->fm;
	uint32_t accent = win_accent(w);
	int lx = w->x, lw = w->w, pad = 10;

	/* ---- toolbar ---- */
	fill_rect(lx, content_y, lw, FM_TOOLH, 0x181826);
	for (int b = 0; b < FM_NBTN; b++) {
		int bx = lx + 6 + b * (FM_BTNW + 4);
		int armed = (b == 2 && fm->confirm_del);
		fill_round_rect_grad(bx, content_y + 3, FM_BTNW, FM_TOOLH - 6, 4,
				     armed ? 0xf38ba8 : 0x2b2b3a,
				     armed ? 0xc4506a : 0x22222e);
		int blw = (int)strlen(fm_btns[b]) * font_w;
		draw_text_clip(bx + (FM_BTNW - blw) / 2,
			       content_y + (FM_TOOLH - font_h) / 2,
			       fm_btns[b], 0xdfe4f2, FM_BTNW - 6);
	}
	int tx = lx + 6 + FM_NBTN * (FM_BTNW + 4) + 6;
	int ty2 = content_y + (FM_TOOLH - font_h) / 2;
	if (fm->prompt) {
		static const char *plabel[] = { "", "file", "dir", "rename" };
		char line[FM_NAMELEN + 16];
		snprintf(line, sizeof(line), "%s: %s_", plabel[fm->prompt], fm->pbuf);
		draw_text_clip(tx, ty2, line, 0xf9e2af, lx + lw - tx - 6);
	} else if (fm->searching) {
		char line[FM_NAMELEN + 16];
		snprintf(line, sizeof(line), "search: %s_", fm->search);
		draw_text_clip(tx, ty2, line, 0x94e2d5, lx + lw - tx - 6);
	} else if (fm->status[0]) {
		draw_text_clip(tx, ty2, fm->status, 0x9399b2, lx + lw - tx - 6);
	} else if (fm->search[0]) {
		char line[FM_NAMELEN + 16];
		snprintf(line, sizeof(line), "filter: %s", fm->search);
		draw_text_clip(tx, ty2, line, 0x6c7086, lx + lw - tx - 6);
	}

	/* ---- column layout ---- */
	int type_w = 6 * font_w, size_w = 9 * font_w;
	int name_x = lx + pad + 22;
	int size_x = lx + lw - pad - type_w - size_w;
	int type_x = lx + lw - pad - type_w;

	/* ---- column header ---- */
	int hy = content_y + FM_TOOLH;
	fill_rect(lx, hy, lw, FM_HEADH, 0x1a1a28);
	fill_rect(lx, hy + FM_HEADH - 1, lw, 1, 0x2a2a40);
	int hty = hy + (FM_HEADH - font_h) / 2;
	draw_text(name_x, hty, "Name", 0x6c7086);
	draw_text(size_x, hty, "Size", 0x6c7086);
	draw_text(type_x, hty, "Type", 0x6c7086);

	/* ---- rows ---- */
	int list_top = content_y + FM_TOOLH + FM_HEADH;
	int list_bot = content_y + content_h;
	fill_rect(lx, list_top, lw, list_bot - list_top, COL_BG_DEFAULT);

	int vis = fm_visible_rows(w), rowh = fm_rowh();
	if (fm->vcount == 0) {
		const char *msg = "empty folder";
		draw_text(lx + (lw - (int)strlen(msg) * font_w) / 2,
			  list_top + rowh, msg, 0x6c7086);
	}
	for (int r = 0; r < vis; r++) {
		int vi = fm->scroll + r;
		if (vi >= fm->vcount)
			break;
		int i = fm->view[vi];
		struct fent *e = &fm->ents[i];
		int ry = list_top + r * rowh;
		int sel = (i == fm->sel);

		uint32_t rowbg = COL_BG_DEFAULT;
		if (sel) {
			rowbg = mix(accent, COL_BG_DEFAULT, 150);
			fill_round_rect(lx + 4, ry + 1, lw - 8, rowh - 2, 4, rowbg);
		} else if (r & 1) {
			rowbg = 0x20202f;
			fill_rect(lx + 4, ry + 1, lw - 8, rowh - 2, rowbg);
		}

		enum fcat cat = classify_file(e->name, e->isdir, e->isexec);
		int hidden = (e->name[0] == '.');
		uint32_t col = hidden ? 0x6c7086
			: (cat == FCAT_TEXT || cat == FCAT_OTHER) ? COL_FG_DEFAULT
			: fcat_color(cat);
		uint32_t iconc = hidden ? 0x585b70 : fcat_color(cat);

		fm_icon(cat, lx + pad + 8, ry + rowh / 2, iconc, rowbg);

		int ty = ry + (rowh - font_h) / 2;
		draw_text_clip(name_x, ty, e->name, sel ? 0xffffff : col,
			       size_x - name_x - 8);
		if (e->isreg) {
			char sz[24];
			human_size(e->size, sz, sizeof(sz));
			draw_text(size_x, ty, sz, 0x6c7086);
		} else if (e->isdir) {
			draw_text(size_x, ty, "--", 0x585b70);
		}
		draw_text_clip(type_x, ty, fcat_tag(cat), 0x6c7086, type_w);
	}

	/* ---- scroll position indicator (non-interactive) ---- */
	if (fm->vcount > vis && vis > 0) {
		int track_h = list_bot - list_top - 4;
		int th = track_h * vis / fm->vcount;
		if (th < 12) th = 12;
		int maxsc = fm->vcount - vis;
		int sy = list_top + 2 + (maxsc > 0 ? (track_h - th) * fm->scroll / maxsc : 0);
		fill_round_rect(lx + lw - 5, sy, 3, th, 1, 0x45475a);
	}

	/* resize grip */
	if (!w->maximized)
		for (int k = 0; k < 3; k++) {
			int gx = w->x + w->w - 5 - k * 4;
			int gy = w->y + w->h - 5;
			for (int m = 0; m <= k; m++)
				fill_rect(gx, gy - m * 4, 2, 2, 0x6c7086);
		}
}

int spawn_file_window(void)
{
	int slot = alloc_window_slot();
	if (slot < 0)
		return -1;
	struct fmstate *fm = calloc(1, sizeof(struct fmstate));
	if (!fm)
		return -1;
	memset(&wins[slot], 0, sizeof(wins[slot]));
	wins[slot].used = 1;
	wins[slot].type = WIN_FILES;
	wins[slot].pty_fd = -1;
	wins[slot].fm = fm;
	wins[slot].x = 240 + slot * 24;
	wins[slot].y = 100 + slot * 24;
	wins[slot].w = 620;
	wins[slot].h = 440;
	wins[slot].attr_fg = COL_FG_DEFAULT;
	wins[slot].attr_bg = COL_BG_DEFAULT;
	snprintf(fm->cwd, sizeof(fm->cwd), "/");
	update_grid_dims(&wins[slot]);
	fm_load(&wins[slot]);
	zorder[zcount++] = slot;
	focused = slot;
	return slot;
}
