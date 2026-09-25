# ##########################################################################
# LZ6 programs - Makefile
# Copyright (C) Yann Collet 2011-2015
#
# GPL v2 License
#
# This program is free software; you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation; either version 2 of the License, or
# (at your option) any later version.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License along
# with this program; if not, write to the Free Software Foundation, Inc.,
# 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
#
# You can contact the author at :
#  - LZ6 source repository : https://github.com/inikep/lz6
#  - LZ6 public forum : https://groups.google.com/forum/#!forum/lz6c
# ##########################################################################
# lz6 : Command Line Utility, supporting gzip-like arguments
# lz6_32: Same as lz6, but forced to compile in 32-bits mode
# frametest  : Test tool, to check lz6frame integrity on target platform
# frametest32: Same as frametest, but forced to compile in 32-bits mode
# fullbench  : Precisely measure speed for each LZ6 function variant
# fullbench32: Same as fullbench, but forced to compile in 32-bits mode
# datagen : generates synthetic data samples for tests & benchmarks
# ##########################################################################

DESTDIR  ?=
PREFIX   ?= /usr/local

BINDIR  := $(PREFIX)/bin
MANDIR  := $(PREFIX)/share/man/man1
INCLUDEDIR := $(PREFIX)/include
LZ6DIR  := ../lib

CFLAGS   ?= -O3 -std=gnu99    # can select customized optimized flags such as for example: make CFLAGS="-g"
FFLAGS   ?= -fomit-frame-pointer -fstrict-aliasing -fforce-addr -ffast-math
WFLAGS   ?= -Wall -Wextra -Wundef -Wshadow -Wcast-align -Wcast-qual -Wstrict-prototypes -pedantic -Wno-variadic-macros
CPPFLAGS := -I$(LZ6DIR) -DXXH_NAMESPACE=LZ6_ 
FLAGS    := $(CFLAGS) $(CPPFLAGS) $(WFLAGS) $(LDFLAGS) $(MOREFLAGS)
# 32-bit seq codec objects: the *_32 targets compile everything else in one
# -O3 command line, and the seq codec miscompiles at -O3
ENTROPY32 := lz6fse_32.o lz6rc_32.o lz6seq_32.o lz6huf_32.o


# Define *.exe as extension for Windows systems
ifneq (,$(filter Windows%,$(OS)))
EXT =.exe
VOID = nul
else
EXT =
VOID = /dev/null
endif


# Default test parameters
TEST_FILES   := COPYING
FUZZER_TIME  := -T9mn


default: lz6

m32: lz6_32 fullbench32 frametest32

bins: lz6 fullbench frametest datagen

all: bins m32

lz6: $(LZ6DIR)/lz6.o $(LZ6DIR)/lz6hc.o $(LZ6DIR)/lz6frame.o $(LZ6DIR)/xxhash.o $(LZ6DIR)/entropy/lz6fse.o $(LZ6DIR)/entropy/lz6rc.o $(LZ6DIR)/entropy/lz6seq.o $(LZ6DIR)/entropy/lz6huf.o bench.o lz6io.o lz6cli.o
	$(CC) $(FLAGS) $^ -o $@$(EXT) -lm

lz6seq_test: $(LZ6DIR)/lz6.o $(LZ6DIR)/lz6hc.o $(LZ6DIR)/entropy/lz6fse.o $(LZ6DIR)/entropy/lz6rc.o $(LZ6DIR)/entropy/lz6seq.o $(LZ6DIR)/entropy/lz6huf.o lz6seq_test.o
	$(CC) $(FLAGS) $^ -o $@$(EXT) -lm

seqframetest: $(LZ6DIR)/lz6.o $(LZ6DIR)/lz6hc.o $(LZ6DIR)/lz6frame.o $(LZ6DIR)/xxhash.o $(LZ6DIR)/entropy/lz6fse.o $(LZ6DIR)/entropy/lz6rc.o $(LZ6DIR)/entropy/lz6seq.o $(LZ6DIR)/entropy/lz6huf.o seqframetest.o
	$(CC) $(FLAGS) $^ -o $@$(EXT) -lm

blockwalk: $(LZ6DIR)/lz6.o $(LZ6DIR)/lz6hc.o $(LZ6DIR)/xxhash.o $(LZ6DIR)/entropy/lz6fse.o $(LZ6DIR)/entropy/lz6rc.o $(LZ6DIR)/entropy/lz6seq.o $(LZ6DIR)/entropy/lz6huf.o blockwalk.o
	$(CC) $(FLAGS) $^ -o $@$(EXT) -lm

lz6_32: $(LZ6DIR)/lz6.c $(LZ6DIR)/lz6hc.c $(LZ6DIR)/lz6frame.c $(LZ6DIR)/xxhash.c bench.c lz6io.c lz6cli.c $(ENTROPY32)
	$(CC) -m32 $(FLAGS) $^ -o $@$(EXT) -lm

fullbench: $(LZ6DIR)/lz6.o $(LZ6DIR)/lz6hc.o $(LZ6DIR)/lz6frame.o $(LZ6DIR)/xxhash.o fullbench.o
	$(CC) $(FLAGS) $^ -o $@$(EXT)

fullbench32: $(LZ6DIR)/lz6.c $(LZ6DIR)/lz6hc.c $(LZ6DIR)/lz6frame.c $(LZ6DIR)/xxhash.c fullbench.c
	$(CC) -m32 $(FLAGS) $^ -o $@$(EXT)

frametest: $(LZ6DIR)/lz6frame.o $(LZ6DIR)/lz6.o $(LZ6DIR)/lz6hc.o $(LZ6DIR)/xxhash.o $(LZ6DIR)/entropy/lz6fse.o $(LZ6DIR)/entropy/lz6rc.o $(LZ6DIR)/entropy/lz6seq.o $(LZ6DIR)/entropy/lz6huf.o frametest.c
	$(CC) $(FLAGS) $^ -o $@$(EXT) -lm

frametest32: $(LZ6DIR)/lz6frame.c $(LZ6DIR)/lz6.c $(LZ6DIR)/lz6hc.c $(LZ6DIR)/xxhash.c $(ENTROPY32) frametest.c
	$(CC) -m32 $(FLAGS) $^ -o $@$(EXT) -lm

datagen : datagen.o datagencli.c
	$(CC) $(FLAGS) $^ -o $@$(EXT)

.c.o:
	$(CC) $(FLAGS) $< -c -o $@

# The seq codec (lz6fse/lz6seq) miscompiles under -O3 — build at -O2.
# 32-bit builds get their own -m32 -O2 objects (see ENTROPY32).
%_32.o: $(LZ6DIR)/entropy/%.c
	$(CC) -m32 -O2 $(CPPFLAGS) -std=c99 -Wall $< -c -o $@
$(LZ6DIR)/entropy/lz6fse.o: $(LZ6DIR)/entropy/lz6fse.c
	$(CC) -O2 $(CPPFLAGS) -std=c99 -Wall $< -c -o $@
$(LZ6DIR)/entropy/lz6seq.o: $(LZ6DIR)/entropy/lz6seq.c
	$(CC) -O2 $(CPPFLAGS) -std=c99 -Wall $< -c -o $@
$(LZ6DIR)/entropy/lz6huf.o: $(LZ6DIR)/entropy/lz6huf.c
	$(CC) -O2 $(CPPFLAGS) -std=c99 -Wall $< -c -o $@

clean:
	@$(MAKE) -C $(LZ6DIR) $@
	@rm -f core *.o *.test tmp* \
        lz6$(EXT) lz6_32$(EXT) \
        fullbench$(EXT) fullbench32$(EXT) \
        frametest$(EXT) frametest32$(EXT) \
        datagen$(EXT)
	@echo Cleaning completed


#------------------------------------------------------------------------
#make install is validated only for Linux, OSX, kFreeBSD and Hurd targets
ifneq (,$(filter $(shell uname),Linux Darwin GNU/kFreeBSD GNU MSYS_NT-10.0))

install: lz6
	@echo Installing binaries
	@install -d -m 755 $(DESTDIR)$(BINDIR)/ $(DESTDIR)$(MANDIR)/ $(DESTDIR)$(INCLUDEDIR)/
	@install -m 755 lz6 $(DESTDIR)$(BINDIR)/lz6
	@ln -sf lz6 $(DESTDIR)$(BINDIR)/lz6cat
	@ln -sf lz6 $(DESTDIR)$(BINDIR)/unlz6
	@install -m 644 $(LZ6DIR)/entropy/lz6seq.h $(DESTDIR)$(INCLUDEDIR)/lz6seq.h
	@echo Installing man pages
	@install -m 644 lz6.1 $(DESTDIR)$(MANDIR)/lz6.1
	@ln -sf lz6.1 $(DESTDIR)$(MANDIR)/lz6cat.1
	@ln -sf lz6.1 $(DESTDIR)$(MANDIR)/unlz6.1
	@echo lz6 installation completed

uninstall:
	rm -f $(DESTDIR)$(BINDIR)/lz6cat
	rm -f $(DESTDIR)$(BINDIR)/unlz6
	rm -f $(DESTDIR)$(BINDIR)/lz6
	rm -f $(DESTDIR)$(INCLUDEDIR)/lz6seq.h
	rm -f $(DESTDIR)$(MANDIR)/lz6.1
	rm -f $(DESTDIR)$(MANDIR)/lz6cat.1
	rm -f $(DESTDIR)$(MANDIR)/unlz6.1
	@echo lz6 programs successfully uninstalled

test: test-lz6 test-frametest test-fullbench test-mem

test32: test-lz6_32 test-frametest32 test-fullbench32 test-mem32

test-all: test test32

test-lz6-sparse: lz6 datagen
	@echo "\n ---- test sparse file support ----"
	./datagen -g5M  -P100 > tmpSrc
	./lz6 -B4 tmpSrc | ./lz6 -dv --sparse > tmpB4
	diff -s tmpSrc tmpB4
	./lz6 -B5 tmpSrc | ./lz6 -dv --sparse > tmpB5
	diff -s tmpSrc tmpB5
	./lz6 -B6 tmpSrc | ./lz6 -dv --sparse > tmpB6
	diff -s tmpSrc tmpB6
	./lz6 -B7 tmpSrc | ./lz6 -dv --sparse > tmpB7
	diff -s tmpSrc tmpB7
	./lz6 tmpSrc | ./lz6 -dv --no-sparse > tmpNoSparse
	diff -s tmpSrc tmpNoSparse
	ls -ls tmp*
	./datagen -s1 -g1200007 -P100 | ./lz6 | ./lz6 -dv --sparse > tmpOdd   # Odd size file (to generate non-full last block)
	./datagen -s1 -g1200007 -P100 | diff -s - tmpOdd
	ls -ls tmpOdd
	@rm tmp*
	@echo "\n Compatibility with Console :"
	echo "Hello World 1 !" | ./lz6 | ./lz6 -d -c
	echo "Hello World 2 !" | ./lz6 | ./lz6 -d | cat
	echo "Hello World 3 !" | ./lz6 --no-frame-crc | ./lz6 -d -c
	@echo "\n Compatibility with Append :"
	./datagen -P100 -g1M > tmp1M
	cat tmp1M tmp1M > tmp2M
	./lz6 -B5 -v tmp1M tmpC
	./lz6 -d -v tmpC tmpR
	./lz6 -d -v tmpC >> tmpR
	ls -ls tmp*
	diff tmp2M tmpR
	@rm tmp*

test-lz6-contentSize: lz6 datagen
	@echo "\n ---- test original size support ----"
	./datagen -g15M > tmp
	./lz6 -v tmp | ./lz6 -t
	./lz6 -v --content-size tmp | ./lz6 -d > tmp2
	diff -s tmp tmp2
	# test large size [2-4] GB
	@./datagen -g3G -P100 | ./lz6 | ./lz6 --decompress --force --sparse - tmp
	@ls -ls tmp
	./lz6 --quiet --content-size tmp | ./lz6 --verbose --decompress --force --sparse - tmp2
	@ls -ls tmp2
	@rm tmp*

test-lz6-frame-concatenation: lz6 datagen
	@echo "\n ---- test frame concatenation ----"
	@echo -n > empty.test
	@echo hi > nonempty.test
	cat nonempty.test empty.test nonempty.test > orig.test
	@./lz6 -zq empty.test > empty.lz6.test
	@./lz6 -zq nonempty.test > nonempty.lz6.test
	cat nonempty.lz6.test empty.lz6.test nonempty.lz6.test > concat.lz6.test
	./lz6 -d concat.lz6.test > result.test
	sdiff orig.test result.test
	@rm *.test
	@echo frame concatenation test completed

test-lz6-multiple: lz6 datagen
	@echo "\n ---- test multiple files ----"
	@./datagen -s1        > tmp1 2> $(VOID)
	@./datagen -s2 -g100K > tmp2 2> $(VOID)
	@./datagen -s3 -g1M   > tmp3 2> $(VOID)
	./lz6 -f -m tmp*
	ls -ls tmp*
	rm tmp1 tmp2 tmp3
	./lz6 -df -m *.lz6
	ls -ls tmp*
	./lz6 -f -m tmp1 notHere tmp2; echo $$?
	@rm tmp*

test-lz6-basic: lz6 datagen
	@echo "\n ---- test lz6 basic compression/decompression ----"
	./datagen -g0     | ./lz6 -v     | ./lz6 -t
	./datagen -g16KB  | ./lz6 -9     | ./lz6 -t
	./datagen -g20KB > tmpSrc
	./lz6 < tmpSrc | ./lz6 -d > tmpRes
	diff -q tmpSrc tmpRes
	./lz6 --no-frame-crc < tmpSrc | ./lz6 -d > tmpRes
	diff -q tmpSrc tmpRes
	./datagen         | ./lz6        | ./lz6 -t
	./datagen -g6M -P99 | ./lz6 -9B | ./lz6 -t
	./datagen -g17M   | ./lz6 -9v    | ./lz6 -qt
	./datagen -g33M   | ./lz6 --no-frame-crc | ./lz6 -t
	./datagen -g256MB | ./lz6 -vqB4 | ./lz6 -t
	./datagen -g6GB   | ./lz6 -vqB5 | ./lz6 -qt
	./datagen -g6GB   | ./lz6 -vq9B | ./lz6 -qt
	@rm tmp*

test-lz6: lz6 datagen test-lz6-basic test-lz6-multiple test-lz6-sparse test-lz6-contentSize test-lz6-frame-concatenation
	@echo "\n ---- test pass-through ----"
	./datagen | ./lz6 -tf

test-interop-32-64: lz6 lz6_32 datagen
	@echo "\n ---- test interoperability 32-bits -vs- 64 bits ----"
	./datagen -g16KB  | ./lz6_32 -9     | ./lz6    -t
	./datagen -P10    | ./lz6    -9B4   | ./lz6_32 -t
	./datagen         | ./lz6_32        | ./lz6    -t
	./datagen -g1M    | ./lz6    -3B5   | ./lz6_32 -t
	./datagen -g256MB | ./lz6_32 -vqB4  | ./lz6    -qt
	./datagen -g1G -P90 | ./lz6         | ./lz6_32 -t
	./datagen -g6GB   | ./lz6_32 -vq9B  | ./lz6    -qt

test-lz6_32-basic: lz6_32 datagen
	@echo "\n ---- test lz6_32 32-bits version ----"
	./datagen -g16KB  | ./lz6_32 -9     | ./lz6_32 -t
	./datagen         | ./lz6_32        | ./lz6_32 -t
	./datagen -g256MB | ./lz6_32 -vqB4  | ./lz6_32 -qt
	./datagen -g6GB   | ./lz6_32 -vqB5  | ./lz6_32 -qt

test-lz6_32: test-lz6_32-basic test-interop-32-64

test-fullbench: fullbench
	./fullbench --no-prompt $(NB_LOOPS) $(TEST_FILES)

test-fullbench32: fullbench32
	./fullbench32 --no-prompt $(NB_LOOPS) $(TEST_FILES)

test-fuzzer: fuzzer
	./fuzzer $(FUZZER_TIME)

test-fuzzer32: fuzzer32
	./fuzzer32 $(FUZZER_TIME)

test-frametest-gdb: frametest
	echo >gdb_cmd run $(FUZZER_TIME) && echo >>gdb_cmd bt
	gdb --batch --command ./gdb_cmd ./frametest

test-frametest: frametest
	./frametest $(FUZZER_TIME)

test-frametest32: frametest32
	./frametest32 $(FUZZER_TIME)

test-mem: lz6 datagen frametest fullbench
	@echo "\n ---- valgrind tests : memory analyzer ----"
	valgrind --leak-check=yes --error-exitcode=1 ./datagen -g50M > $(VOID)
	./datagen -g16KB > tmp
	valgrind --leak-check=yes --error-exitcode=1 ./lz6 -9 -B -f tmp $(VOID)
	./datagen -g16KB -s2 > tmp2
	./datagen -g16KB -s3 > tmp3
	valgrind --leak-check=yes --error-exitcode=1 ./lz6 --force --multiple tmp tmp2 tmp3
	./datagen -g16MB > tmp
	valgrind --leak-check=yes --error-exitcode=1 ./lz6 -9 -B5 -f tmp tmp2
	valgrind --leak-check=yes --error-exitcode=1 ./lz6 -t tmp2
	valgrind --leak-check=yes --error-exitcode=1 ./lz6 -bi1 tmp
	valgrind --leak-check=yes --error-exitcode=1 ./fullbench -i1 tmp tmp2
	./datagen -g256MB > tmp
	valgrind --leak-check=yes --error-exitcode=1 ./lz6 -B4 -f -vq tmp $(VOID)
	rm tmp*
	valgrind --leak-check=yes --error-exitcode=1 ./frametest -i256

test-mem32: lz6_32 datagen
# unfortunately, valgrind doesn't seem to work with non-native binary...

endif
