# =============================================================================
#  Game Boy / Game Boy Color emulator — build
#
#  Backends are auto-detected; nothing needs installing to get a working build.
#    SDL2 : used when `pkg-config --exists sdl2` succeeds  (best experience)
#    X11  : used when <X11/Xlib.h> is present              (always on Kali)
#    ALSA : loaded at runtime with dlopen(), no -dev package needed
#
#  Targets:
#    make            optimised build      -> bin/gameboy
#    make debug      -O0 -g               -> bin/gameboy-debug
#    make asan       AddressSanitizer+UBSan
#    make test       run the automated Blargg/acid2 test suite
#    make run ROM=x  build and launch
# =============================================================================

CC      ?= cc
BIN     := bin/gameboy
SRCDIR  := src
OBJDIR  := build
INCLUDE := -Iinclude

WARN := -Wall -Wextra -Wno-unused-parameter -Wshadow -Wpointer-arith \
        -Wcast-align -Wstrict-prototypes -Wmissing-prototypes -Wno-sign-compare
CFLAGS_BASE := -std=c11 -D_GNU_SOURCE $(WARN) $(INCLUDE)
OPT     := -O2 -fno-strict-aliasing
LDLIBS  := -lm -ldl -lpthread

# ---- backend detection ------------------------------------------------------
HAVE_SDL2 := $(shell pkg-config --exists sdl2 2>/dev/null && echo 1)
HAVE_X11  := $(shell echo 'int main(void){return 0;}' > .gbcfg.c 2>/dev/null && \
                     $(CC) -include X11/Xlib.h -include X11/extensions/XShm.h .gbcfg.c \
                           -o /dev/null -lX11 -lXext 2>/dev/null && echo 1; rm -f .gbcfg.c)

ifeq ($(HAVE_SDL2),1)
  CFLAGS_BASE += -DGB_HAVE_SDL2 $(shell pkg-config --cflags sdl2)
  LDLIBS      += $(shell pkg-config --libs sdl2)
  BACKENDS    += SDL2
endif
ifeq ($(HAVE_X11),1)
  CFLAGS_BASE += -DGB_HAVE_X11
  LDLIBS      += -lX11 -lXext
  BACKENDS    += X11
endif
ifeq ($(BACKENDS),)
  $(warning ***  No video backend found. Install libsdl2-dev or libx11-dev.)
  $(warning ***  Building headless-only (--headless still works).)
endif

SRCS := $(shell find $(SRCDIR) -name '*.c' | sort)
# drop backends that are not available
ifneq ($(HAVE_SDL2),1)
  SRCS := $(filter-out $(SRCDIR)/platform/plat_sdl.c,$(SRCS))
endif
ifneq ($(HAVE_X11),1)
  SRCS := $(filter-out $(SRCDIR)/platform/plat_x11.c,$(SRCS))
endif

OBJS := $(SRCS:$(SRCDIR)/%.c=$(OBJDIR)/%.o)
DEPS := $(OBJS:.o=.d)

CFLAGS ?= $(CFLAGS_BASE) $(OPT)

.PHONY: all debug asan clean run test info help
all: $(BIN)
	@echo "  built $(BIN)   backends:$(if $(BACKENDS), $(BACKENDS), none)"

$(BIN): $(OBJS)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(OBJS) -o $@ $(LDLIBS)

$(OBJDIR)/%.o: $(SRCDIR)/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -MMD -MP -c $< -o $@

-include $(DEPS)

debug:
	@$(MAKE) --no-print-directory OPT="-O0 -g3 -DGB_DEBUG" OBJDIR=build/debug BIN=bin/gameboy-debug

asan:
	@$(MAKE) --no-print-directory \
	  OPT="-O1 -g3 -fsanitize=address,undefined -fno-omit-frame-pointer" \
	  LDLIBS="-lm -ldl -lpthread -fsanitize=address,undefined $(shell pkg-config --libs sdl2 2>/dev/null) $(if $(HAVE_X11),-lX11 -lXext,)" \
	  OBJDIR=build/asan BIN=bin/gameboy-asan

clean:
	rm -rf build bin

info:
	@echo "CC        = $(CC)"
	@echo "SDL2      = $(if $(HAVE_SDL2),yes,no)"
	@echo "X11       = $(if $(HAVE_X11),yes,no)"
	@echo "sources   = $(words $(SRCS)) files"
	@echo "LDLIBS    = $(LDLIBS)"

run: $(BIN)
	./$(BIN) $(ROM)

test: $(BIN)
	@./tools/run_tests.sh

help:
	@sed -n '2,20p' Makefile
