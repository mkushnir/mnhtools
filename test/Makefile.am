CLEANFILES = *.core core
#CLEANFILES += *.in
AM_MAKEFLAGS = -s

# consider headers:
#   - include_HEADERS
#   - nobase_include_HEADERS
#   - pkginclude_HEADERS
#   - dist_HEADERS
#   - nodist_HEADERS
#   - noinst_HEADERS
noinst_HEADERS = unittest.h

noinst_PROGRAMS=testfoo gendata

BUILT_SOURCES = diag.c diag.h
EXTRA_DIST = $(diags) runscripts

#noinst_HEADERS =

diags = diag.txt

if DEBUG
DEBUG_FLAGS = -g -O0 @CC_DEBUG@
else
DEBUG_FLAGS = -DNDEBUG -O3 -fomit-frame-pointer
endif

nodist_testfoo_SOURCES = diag.c
testfoo_SOURCES = testfoo.c
testfoo_CFLAGS = $(DEBUG_FLAGS) -Wall -Wextra -Werror -std=c99 @_GNU_SOURCE_MACRO@ @_XOPEN_SOURCE_MACRO@ -I$(top_srcdir)/test -I$(top_srcdir)/src -I$(top_srcdir) -I$(includedir)
#testfoo_LDFLAGS =  -L$(libdir) -lmrkcommon -lmndiag
testfoo_LDFLAGS = -L$(libdir) -lmndiag

nodist_gendata_SOURCES = diag.c
gendata_SOURCES = gendata.c
gendata_CFLAGS = $(DEBUG_FLAGS) -Wall -Wextra -Werror -std=c99 @_GNU_SOURCE_MACRO@ @_XOPEN_SOURCE_MACRO@ -I$(top_srcdir)/test -I$(top_srcdir)/src -I$(top_srcdir) -I$(includedir)
gendata_LDFLAGS =  -L$(libdir) -lmrkcommon -lmndiag
#gendata_LDFLAGS = 

diag.c diag.h: $(diags)
	$(AM_V_GEN) cat $(diags) | sort -u >diag.txt.tmp && mndiagen -S diag.txt.tmp -L mnhtools -H diag.h -C diag.c ../*.[ch] ./*.[ch]

testfoo-run: testfoo
	@if test -x $>; \
	    then LD_LIBRARY_PATH=$(libdir) ./$> $${$(>)_ARGS}; \
	    else true; \
	fi

testrun: all
	for i in $(noinst_PROGRAMS); do if test -x ./$$i; then LD_LIBRARY_PATH=$(libdir) ./$$i; fi; done;
