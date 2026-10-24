# ################################################################
# LZ6 library - Makefile
# Copyright (C) Yann Collet 2011-2015
# All rights reserved.
# 
# BSD license
# Redistribution and use in source and binary forms, with or without modification,
# are permitted provided that the following conditions are met:
# 
# * Redistributions of source code must retain the above copyright notice, this
#   list of conditions and the following disclaimer.
# 
# * Redistributions in binary form must reproduce the above copyright notice, this
#   list of conditions and the following disclaimer in the documentation and/or
#   other materials provided with the distribution.
# 
# THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
# ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
# WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
# DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR
# ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
# (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
# LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON
# ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
# (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
# SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
# 
# You can contact the author at :
#  - LZ6 source repository : https://github.com/inikep/lz6
#  - LZ6 forum froup : https://groups.google.com/forum/#!forum/lz6c
# ################################################################

# Version numbers
LIBVER_MAJOR:=$(shell sed -n '/define LZ6_VERSION_MAJOR/s/.*[[:blank:]]\([0-9][0-9]*\).*/\1/p' < lz6.h)
LIBVER_MINOR:=$(shell sed -n '/define LZ6_VERSION_MINOR/s/.*[[:blank:]]\([0-9][0-9]*\).*/\1/p' < lz6.h)
LIBVER_PATCH:=$(shell sed -n '/define LZ6_VERSION_RELEASE/s/.*[[:blank:]]\([0-9][0-9]*\).*/\1/p' < lz6.h)
LIBVER := $(LIBVER_MAJOR).$(LIBVER_MINOR).$(LIBVER_PATCH)
VERSION?= $(LIBVER)

DESTDIR?=
PREFIX ?= /usr/local
CPPFLAGS= -DXXH_NAMESPACE=LZ6_
CFLAGS ?= -O3
CFLAGS += -std=c99 -Wall -Wextra -Wundef -Wshadow -Wcast-align -Wcast-qual -Wstrict-prototypes -pedantic
# The seq codec (lz6seq/lz6fse) miscompiles under -O3 (rANS buffer
# writes) — keep it at -O2 where it round-trips correctly.
SEQ_CFLAGS = -O2
FLAGS   = $(CPPFLAGS) $(CFLAGS) $(LDFLAGS)

LIBDIR?= $(PREFIX)/lib
INCLUDEDIR=$(PREFIX)/include


# OS X linker doesn't support -soname, and use different extension
# see : https://developer.apple.com/library/mac/documentation/DeveloperTools/Conceptual/DynamicLibraries/100-Articles/DynamicLibraryDesignGuidelines.html
ifeq ($(shell uname), Darwin)
	SHARED_EXT = dylib
	SHARED_EXT_MAJOR = $(LIBVER_MAJOR).$(SHARED_EXT)
	SHARED_EXT_VER = $(LIBVER).$(SHARED_EXT)
	SONAME_FLAGS = -install_name $(PREFIX)/lib/liblz6.$(SHARED_EXT_MAJOR) -compatibility_version $(LIBVER_MAJOR) -current_version $(LIBVER)
else
	SONAME_FLAGS = -Wl,-soname=liblz6.$(SHARED_EXT).$(LIBVER_MAJOR)
	SHARED_EXT = so
	SHARED_EXT_MAJOR = $(SHARED_EXT).$(LIBVER_MAJOR)
	SHARED_EXT_VER = $(SHARED_EXT).$(LIBVER)
endif

default: liblz6

all: liblz6

LIBSRC_CORE    = lz6.c lz6hc.c lz6frame.c xxhash.c
LIBSRC_ENTROPY = entropy/lz6fse.c entropy/lz6rc.c entropy/lz6seq.c entropy/lz6huf.c
LIBBUILD       = .lz6build

# Objects, not sources: the static library holds object files (it held the .c files), the shared one is linked from position-independent objects and
# against libm (the seq codec calls log2), and the entropy sources are built at -O2, the level the codec is tested and released at (see SEQ_CFLAGS above).
liblz6: $(LIBSRC_CORE) $(LIBSRC_ENTROPY)
	@echo compiling static library
	@rm -rf $(LIBBUILD) && mkdir -p $(LIBBUILD)/static $(LIBBUILD)/pic
	@for f in $(LIBSRC_CORE); do $(CC) $(CPPFLAGS) $(CFLAGS) -c $$f -o $(LIBBUILD)/static/`basename $$f .c`.o || exit 1; done
	@for f in $(LIBSRC_ENTROPY); do $(CC) $(CPPFLAGS) $(CFLAGS) $(SEQ_CFLAGS) -c $$f -o $(LIBBUILD)/static/`basename $$f .c`.o || exit 1; done
	@rm -f $@.a && $(AR) rcs $@.a $(LIBBUILD)/static/*.o
	@echo compiling dynamic library $(LIBVER)
	@for f in $(LIBSRC_CORE); do $(CC) $(CPPFLAGS) $(CFLAGS) -fPIC -c $$f -o $(LIBBUILD)/pic/`basename $$f .c`.o || exit 1; done
	@for f in $(LIBSRC_ENTROPY); do $(CC) $(CPPFLAGS) $(CFLAGS) $(SEQ_CFLAGS) -fPIC -c $$f -o $(LIBBUILD)/pic/`basename $$f .c`.o || exit 1; done
	@$(CC) $(CFLAGS) $(LDFLAGS) -shared $(LIBBUILD)/pic/*.o $(SONAME_FLAGS) -o $@.$(SHARED_EXT_VER) -lm
	@rm -rf $(LIBBUILD)
	@echo creating versioned links
	@ln -sf $@.$(SHARED_EXT_VER) $@.$(SHARED_EXT_MAJOR)
	@ln -sf $@.$(SHARED_EXT_VER) $@.$(SHARED_EXT)

clean:
	@rm -f core *.o *.a *.$(SHARED_EXT) *.$(SHARED_EXT).* liblz6.pc
	@echo Cleaning library completed


#------------------------------------------------------------------------
#make install is validated only for Linux, OSX, kFreeBSD and Hurd targets
ifneq (,$(filter $(shell uname),Linux Darwin GNU/kFreeBSD GNU))

liblz6.pc: liblz6.pc.in Makefile
	@echo creating pkgconfig
	@sed -e 's|@PREFIX@|$(PREFIX)|' \
            -e 's|@LIBDIR@|$(LIBDIR)|' \
            -e 's|@INCLUDEDIR@|$(INCLUDEDIR)|' \
            -e 's|@VERSION@|$(VERSION)|' \
             $< >$@

install: liblz6 liblz6.pc
	@install -d -m 755 $(DESTDIR)$(LIBDIR)/pkgconfig/ $(DESTDIR)$(INCLUDEDIR)/
	@install -m 755 liblz6.$(SHARED_EXT_VER) $(DESTDIR)$(LIBDIR)
	@ln -sf liblz6.$(SHARED_EXT_VER) $(DESTDIR)$(LIBDIR)/liblz6.$(SHARED_EXT_MAJOR)
	@ln -sf liblz6.$(SHARED_EXT_VER) $(DESTDIR)$(LIBDIR)/liblz6.$(SHARED_EXT)
	@install -m 644 liblz6.pc $(DESTDIR)$(LIBDIR)/pkgconfig/
	@install -m 644 liblz6.a $(DESTDIR)$(LIBDIR)/liblz6.a
	@install -m 644 lz6.h $(DESTDIR)$(INCLUDEDIR)/lz6.h
	@install -m 644 lz6hc.h $(DESTDIR)$(INCLUDEDIR)/lz6hc.h
	@install -m 644 lz6frame.h $(DESTDIR)$(INCLUDEDIR)/lz6frame.h
	@install -m 644 entropy/lz6fse.h entropy/lz6rc.h entropy/lz6seq.h entropy/lz6huf.h $(DESTDIR)$(INCLUDEDIR)/
	@echo lz6 static and shared libraries installed

uninstall:
	@rm -f $(DESTDIR)$(LIBDIR)/liblz6.$(SHARED_EXT)
	@rm -f $(DESTDIR)$(LIBDIR)/liblz6.$(SHARED_EXT_MAJOR)
	@rm -f $(DESTDIR)$(LIBDIR)/pkgconfig/liblz6.pc
	@rm -f $(DESTDIR)$(LIBDIR)/liblz6.$(SHARED_EXT_VER)
	@rm -f $(DESTDIR)$(LIBDIR)/liblz6.a
	@rm -f $(DESTDIR)$(INCLUDEDIR)/lz6.h
	@rm -f $(DESTDIR)$(INCLUDEDIR)/lz6hc.h
	@rm -f $(DESTDIR)$(INCLUDEDIR)/lz6frame.h
	@rm -f $(DESTDIR)$(INCLUDEDIR)/lz6fse.h $(DESTDIR)$(INCLUDEDIR)/lz6rc.h $(DESTDIR)$(INCLUDEDIR)/lz6seq.h $(DESTDIR)$(INCLUDEDIR)/lz6huf.h
	@echo lz6 libraries successfully uninstalled

endif
