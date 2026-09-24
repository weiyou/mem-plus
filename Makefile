# Build the helpers in this directory, and optionally install them beside mem-plus.
#
#   make
#   make install bindir=DIR
#
# bindir is the directory that receives mem-plus and the four helpers. It is
# required: there is no default. A leading ~ is expanded to the home directory.
# The script looks for helpers next to itself, so they are installed together.

# Make's built-in CC is cc. Use clang unless CC was set on the command line
# or in the environment.
ifeq ($(origin CC),default)
CC = clang
endif
CFLAGS ?= -O2

HELPERS = memfoot memsys membw memgpu
PROGS   = mem-plus $(HELPERS)

.PHONY: all install clean

all: $(HELPERS)

memfoot: memfoot.c
	$(CC) $(CFLAGS) -o $@ $<

memsys: memsys.c
	$(CC) $(CFLAGS) -o $@ $<

membw: membw.c
	$(CC) $(CFLAGS) -framework CoreFoundation -o $@ $<

memgpu: memgpu.c
	$(CC) $(CFLAGS) -framework CoreFoundation -o $@ $<

install: all
	@if [ -z "$(bindir)" ]; then \
		printf '%s\n' \
			'usage: make install bindir=DIR' \
			'DIR is required. mem-plus and the helpers are copied there.' >&2; \
		exit 1; \
	fi
	dest='$(bindir)'; \
	case "$$dest" in \
		"~") dest=$$HOME ;; \
		"~/"*) dest=$$HOME/$$(printf '%s\n' "$$dest" | sed 's|^~/||') ;; \
	esac; \
	mkdir -p "$$dest"; \
	install -m 755 $(PROGS) "$$dest/"

clean:
	rm -f $(HELPERS)
