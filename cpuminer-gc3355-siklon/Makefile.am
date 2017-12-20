
if WANT_JANSSON
JANSSON_CPPFLAGS= -I$(top_srcdir)/compat/jansson
else
JANSSON_CPPFLAGS=
endif

EXTRA_DIST	= example-cfg.json nomacro.pl

SUBDIRS		= compat

minerd_CPPFLAGS	= $(PTHREAD_FLAGS) -fno-strict-aliasing $(JANSSON_CPPFLAGS)

bin_PROGRAMS	= minerd

dist_man_MANS	= minerd.1

minerd_SOURCES	= elist.h miner.h compat.h \
		  cpu-miner.c util.c \
		  gc3355.h \
		  sha2.c scrypt.c
if ARCH_x86
minerd_SOURCES += sha2-x86.S scrypt-x86.S
endif
if ARCH_x86_64
minerd_SOURCES += sha2-x64.S scrypt-x64.S
endif
if ARCH_ARM
minerd_SOURCES += sha2-arm.S scrypt-arm.S
endif
minerd_LDFLAGS	= $(PTHREAD_FLAGS)
minerd_LDADD	= @LIBCURL@ @JANSSON_LIBS@ @PTHREAD_LIBS@ @WS2_LIBS@ @NCURSES_LIBS@ @PDCURSES_LIBS@ @UDEV_LIBS@ @SETUPAPI_LIBS@
minerd_CPPFLAGS += @LIBCURL_CPPFLAGS@
