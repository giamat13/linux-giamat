/* fbdesktop -- Archive Manager: lists and extracts .tar/.tar.gz/.tgz members
 * through the system `tar` -- already part of any Debian base, so nothing
 * new needs installing into the rootfs. Every call runs it via fork+execvp
 * with an argv array, never a shell, so there is no command-injection
 * surface from an oddly-named archive.
 *
 * ponytail: .zip is classified FCAT_ARCHIVE but not opened here -- unzip
 * isn't part of the base image and tar can't read it. Add it (and the
 * package) if a .zip actually shows up. */
#include "fbdesktop.h"

#define AR_BG    0x1e1e2e
#define AR_ROW   0x232338
#define AR_ROWALT 0x1e1e2c
#define AR_TXT   0xcdd6f4
#define AR_DIM   0x6c7086
#define AR_TOOLH 32
#define AR_ROWH  (font_h + 8)

/* Run argv and capture its stdout. SIGCHLD is parked back at SIG_DFL for the
 * duration: the desktop runs with it SIG_IGN'd (so terminal/browser children
 * never zombie), but that also makes waitpid() here fail with ECHILD before
 * we can read tar's exit status, so it has to come off ignore just for this
 * synchronous child. */
static int run_capture(char *const argv[], char *outbuf, size_t outsz)
{
	int pfd[2];
	if (pipe(pfd) != 0)
		return -1;
	signal(SIGCHLD, SIG_DFL);
	pid_t pid = fork();
	if (pid < 0) {
		signal(SIGCHLD, SIG_IGN);
		close(pfd[0]);
		close(pfd[1]);
		return -1;
	}
	if (pid == 0) {
		dup2(pfd[1], STDOUT_FILENO);
		close(pfd[0]);
		close(pfd[1]);
		execvp(argv[0], argv);
		_exit(127);
	}
	close(pfd[1]);
	size_t total = 0;
	ssize_t r;
	while (total < outsz - 1 && (r = read(pfd[0], outbuf + total, outsz - 1 - total)) > 0)
		total += r;
	outbuf[total] = 0;
	close(pfd[0]);
	int status = 0;
	waitpid(pid, &status, 0);
	signal(SIGCHLD, SIG_IGN);
	return (WIFEXITED(status) && WEXITSTATUS(status) == 0) ? 0 : -1;
}

static int run_wait(char *const argv[])
{
	signal(SIGCHLD, SIG_DFL);
	pid_t pid = fork();
	if (pid < 0) {
		signal(SIGCHLD, SIG_IGN);
		return -1;
	}
	if (pid == 0) {
		execvp(argv[0], argv);
		_exit(127);
	}
	int status = 0;
	waitpid(pid, &status, 0);
	signal(SIGCHLD, SIG_IGN);
	return (WIFEXITED(status) && WEXITSTATUS(status) == 0) ? 0 : -1;
}

static void archive_list(struct archivestate *a)
{
	a->count = 0;
	a->scroll = 0;
	char *argv[] = { "tar", "tf", a->path, NULL };
	static char out[65536];
	if (run_capture(argv, out, sizeof(out)) != 0) {
		snprintf(a->status, sizeof(a->status), "cannot list archive (unsupported or corrupt)");
		return;
	}
	char *line = out, *nl;
	while ((nl = strchr(line, '\n')) && a->count < ARC_MAXENT) {
		*nl = 0;
		snprintf(a->ent[a->count++], ARC_NAMELEN, "%s", line);
		line = nl + 1;
	}
	snprintf(a->status, sizeof(a->status), "%d entr%s", a->count, a->count == 1 ? "y" : "ies");
}

/* Strip a known archive suffix to name the folder entries get extracted into. */
static void archive_destdir(const char *path, char *out, size_t outsz)
{
	snprintf(out, outsz, "%s", path);
	static const char *suf[] = { ".tar.gz", ".tgz", ".tar", ".gz" };
	size_t len = strlen(out);
	for (size_t i = 0; i < sizeof(suf) / sizeof(suf[0]); i++) {
		size_t sl = strlen(suf[i]);
		if (len > sl && !strcasecmp(out + len - sl, suf[i])) {
			out[len - sl] = 0;
			break;
		}
	}
}

static void archive_extract(struct archivestate *a)
{
	char dest[FM_FULLLEN];
	archive_destdir(a->path, dest, sizeof(dest));
	if (mkdir(dest, 0755) != 0 && errno != EEXIST) {
		snprintf(a->status, sizeof(a->status), "extract failed: %s", strerror(errno));
		return;
	}
	char *argv[] = { "tar", "xf", a->path, "-C", dest, NULL };
	if (run_wait(argv) == 0)
		snprintf(a->status, sizeof(a->status), "extracted to %s", dest);
	else
		snprintf(a->status, sizeof(a->status), "extract failed");
}

/* Compress a file or directory into "<name>.tar.gz" next to it. Runs with
 * -C into the parent dir so members are stored relative (plain "name", not
 * "/root/Desktop/name") -- the same reason the command-line `tar` recipe
 * always does this. */
int archive_create(const char *srcpath)
{
	char parent[FM_FULLLEN], base[FM_NAMELEN];
	snprintf(parent, sizeof(parent), "%s", srcpath);
	char *slash = strrchr(parent, '/');
	if (!slash || slash == parent)
		return -1;
	snprintf(base, sizeof(base), "%s", slash + 1);
	*slash = 0;

	char dest[FM_FULLLEN];
	snprintf(dest, sizeof(dest), "%s/%s.tar.gz", parent, base);
	char *argv[] = { "tar", "czf", dest, "-C", parent, base, NULL };
	return run_wait(argv);
}

/* ---- input -------------------------------------------------------------- */

void archive_click(struct window *w, int px, int py)
{
	struct archivestate *a = w->arc;
	int content_y = w->y + TITLE_H;
	if (!a->path[0])
		return;
	if (py >= content_y && py < content_y + AR_TOOLH) {
		int bw = 110;
		if (px >= w->x + w->w - 8 - bw)
			archive_extract(a);
		return;
	}
}

void archive_scroll(struct window *w, int value)
{
	struct archivestate *a = w->arc;
	int content_h = w->h - TITLE_H;
	int visible = (content_h - AR_TOOLH) > 0 ? (content_h - AR_TOOLH) / AR_ROWH : 0;
	int maxscroll = a->count - visible;
	if (maxscroll < 0)
		maxscroll = 0;
	a->scroll -= value;
	if (a->scroll < 0) a->scroll = 0;
	if (a->scroll > maxscroll) a->scroll = maxscroll;
}

/* ---- renderer ------------------------------------------------------------ */

void draw_archive(struct window *w, int content_y, int content_h)
{
	struct archivestate *a = w->arc;
	uint32_t accent = win_accent(w);
	fill_rect(w->x, content_y, w->w, content_h, AR_BG);

	if (!a->path[0]) {
		draw_text(w->x + 14, content_y + 14, "Open a .tar/.tar.gz/.tgz from Files", AR_DIM);
		return;
	}

	const char *base = strrchr(a->path, '/');
	base = base ? base + 1 : a->path;
	draw_text_clip(w->x + 10, content_y + (AR_TOOLH - font_h) / 2, base, AR_TXT, w->w - 240);

	int bw = 110;
	fill_round_rect_grad(w->x + w->w - 8 - bw, content_y + 4, bw, AR_TOOLH - 8, 5,
			     mix(accent, 0xffffff, 40), accent);
	int lw = (int)strlen("Extract All") * font_w;
	draw_text(w->x + w->w - 8 - bw + (bw - lw) / 2, content_y + (AR_TOOLH - font_h) / 2,
		  "Extract All", 0x11111c);

	int list_y = content_y + AR_TOOLH;
	if (a->count == 0) {
		draw_text(w->x + 14, list_y + 6, a->status, AR_DIM);
		return;
	}
	int visible = (content_h - AR_TOOLH) > 0 ? (content_h - AR_TOOLH) / AR_ROWH : 0;
	for (int i = 0; i < visible && a->scroll + i < a->count; i++) {
		int ry = list_y + i * AR_ROWH;
		fill_rect(w->x, ry, w->w, AR_ROWH, (i % 2) ? AR_ROWALT : AR_ROW);
		draw_text_clip(w->x + 10, ry + (AR_ROWH - font_h) / 2, a->ent[a->scroll + i],
			       AR_TXT, w->w - 20);
	}
}

int spawn_archive(const char *path)
{
	int slot = -1;
	for (int i = 0; i < MAX_WIN; i++) {
		if (wins[i].used && wins[i].type == WIN_ARCHIVE) {
			slot = i;
			break;
		}
	}
	if (slot < 0) {
		slot = alloc_window_slot();
		if (slot < 0)
			return -1;
		struct archivestate *a = calloc(1, sizeof(struct archivestate));
		if (!a)
			return -1;
		memset(&wins[slot], 0, sizeof(wins[slot]));
		wins[slot].used = 1;
		wins[slot].type = WIN_ARCHIVE;
		wins[slot].pty_fd = -1;
		wins[slot].arc = a;
		wins[slot].x = 300;
		wins[slot].y = 110;
		wins[slot].w = 420;
		wins[slot].h = 340;
		wins[slot].attr_fg = COL_FG_DEFAULT;
		wins[slot].attr_bg = COL_BG_DEFAULT;
		snprintf(wins[slot].title, sizeof(wins[slot].title), "Archive Manager");
		zorder[zcount++] = slot;
	}
	if (path) {
		snprintf(wins[slot].arc->path, sizeof(wins[slot].arc->path), "%s", path);
		archive_list(wins[slot].arc);
	}
	wins[slot].minimized = 0;
	raise_window(slot);
	focused = slot;
	return slot;
}
