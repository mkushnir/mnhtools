# consider headers:
#   - include_HEADERS
#   - nobase_include_HEADERS
#   - pkginclude_HEADERS
#   - dist_HEADERS
#   - nodist_HEADERS
#   - noinst_HEADERS

BUILT_SOURCES = diag.c diag.h
EXTRA_DIST = $(diags)
CLEANFILES = $(BUILT_SOURCES) *.core core
#CLEANFILES += *.in
AM_MAKEFLAGS = -s

noinst_HEADERS = mnhtesto.h units.h

bin_PROGRAMS = mnhtesto mnhtestc

nobase_include_HEADERS =

mnhtesto_SOURCES = mnhtesto.c units.c mnhtesto-main.c
nodist_mnhtesto_SOURCES = diag.c

mnhtestc_SOURCES = mnhtestc.c mnhtestc-main.c
nodist_mnhtestc_SOURCES = diag.c

diags = diag.txt

if DEBUG
DEBUG_FLAGS = -g -O0 @CC_DEBUG@
else
DEBUG_FLAGS = -DNDEBUG -O3 -fomit-frame-pointer
endif

mnhtesto_CFLAGS = $(DEBUG_FLAGS) -Wall -Wextra -Werror -std=c99 @_GNU_SOURCE_MACRO@ @_XOPEN_SOURCE_MACRO@ -I$(top_srcdir)/src -I$(top_srcdir) -I$(includedir)
mnhtesto_LDFLAGS = -all-static -L$(libdir) -lmnfcgi -lmrkapp -lmrkthr -lmrkcommon -lmndiag -lm

mnhtestc_CFLAGS = $(DEBUG_FLAGS) -Wall -Wextra -Werror -std=c99 @_GNU_SOURCE_MACRO@ @_XOPEN_SOURCE_MACRO@ -I$(top_srcdir)/src -I$(top_srcdir) -I$(includedir)
#mnhtestc_LDFLAGS = -all-static -L$(libdir) -lmrkapp -lmrkthr -lmrkcommon -lz -lssl -lcrypto
mnhtestc_LDFLAGS = -L$(libdir) -lmrkapp -lmrkthr -lmrkcommon -lmndiag -lz -lssl -lcrypto

diag.c diag.h: $(diags)
	$(AM_V_GEN) cat $(diags) | sort -u >diag.txt.tmp && mndiagen -S diag.txt.tmp -L mnhtools -H diag.h -C diag.c *.[ch]

run: all
	for i in $(bin_PROGRAMS); do if test -x ./$$i; then LD_LIBRARY_PATH=$(libdir) ./$$i; fi; done;

testrun:
	for i in $(SUBDIRS); do if test "$$i" != "."; then cd $$i && $(MAKE) testrun && cd ..; fi; done;
