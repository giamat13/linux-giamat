# fbdesktop -- a userspace framebuffer desktop. Out-of-tree: nothing here is
# part of the kernel build, and Kbuild never descends into this directory.
#
#   make -C tools/fbdesktop        -> ~/build/fbdesktop
#
# Both source trees live under tools/, where this repo keeps its userspace
# programs (tools/perf, tools/bpf, ...):
#
#   tools/fbdesktop/   the desktop shell -- framebuffer, windows, the character
#                      grid, the event loop. This is the binary /sbin/init execs,
#                      and it is what links everything below into one program.
#   tools/apps/        one file per application window, each opened from a desktop
#                      icon. They are windows inside the shell, not separate
#                      binaries, so they link into the same executable.
#
# The Makefile and include/ sit in this directory rather than at the repo root
# because the root is a kernel tree and already owns those names.

BUILD  := $(HOME)/build
OBJDIR := $(BUILD)/fbdesktop-obj
BIN    := $(BUILD)/fbdesktop

APPDIR := ../apps

# -flto: the shell and the apps are separate translation units now, so without
# it the cross-module calls that the old single-file build inlined would stay
# real calls.
CFLAGS := -O2 -flto -Wall -Iinclude
LDLIBS := -lX11 -lXext -lXtst

SHELL_SRCS := main desktop window draw grid globals
APP_SRCS   := terminal files editor taskmgr settings browser calc paint calendar

OBJS := $(SHELL_SRCS:%=$(OBJDIR)/shell/%.o) $(APP_SRCS:%=$(OBJDIR)/apps/%.o)

$(BIN): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $(OBJS) $(LDLIBS)

$(OBJDIR)/shell/%.o: %.c include/fbdesktop.h | $(OBJDIR)
	$(CC) $(CFLAGS) -c -o $@ $<

$(OBJDIR)/apps/%.o: $(APPDIR)/%.c include/fbdesktop.h | $(OBJDIR)
	$(CC) $(CFLAGS) -c -o $@ $<

$(OBJDIR):
	mkdir -p $@/shell $@/apps

clean:
	rm -rf $(OBJDIR) $(BIN)

.PHONY: clean
