# Host (Linux) test harness for the ext2fs journal.
# Builds journal.c straight from the Hurd tree against the real Hurd lib
# headers, a fake Mach layer (shim/) and a fake ext2fs around it (env/).
#
#   make check            build and run every test
#   make check SAN=address,undefined
#   make check SAN=thread
#   make HURD=/path/to/hurd
#   make check JOURNAL_C=patched/journal.c
#   make check BALLOC_C=other/balloc.c

HURD ?= ../cross-hurd/src/hurd
# The journal under test; point it at a patched copy to try a change.
JOURNAL_C ?= $(HURD)/ext2fs/journal.c
# The block allocator linked into the t_balloc* tests.
BALLOC_C ?= $(HURD)/ext2fs/balloc.c

CC ?= gcc
CFLAGS ?= -O1 -g -fno-omit-frame-pointer
CFLAGS += -std=gnu11 -Wall -Wno-unused-function -pthread
CPPFLAGS += -D_GNU_SOURCE -Ishim -Ienv -I$(HURD) -I$(HURD)/libshouldbeinlibc \
	    -I$(HURD)/libports -I$(HURD)/ext2fs
# Debug messages go to the flight recorder only; it is dumped on aborts.
CPPFLAGS += -DJOURNAL_DEBUG_LIVE=0
LDFLAGS += -pthread

ifdef SAN
CFLAGS += -fsanitize=$(SAN)
LDFLAGS += -fsanitize=$(SAN)
endif

B = build
ENV = $(B)/journal.o $(B)/ihash.o $(B)/fakefs.o $(B)/crash.o
TESTS = $(patsubst tests/%.c,$(B)/%,$(wildcard tests/t_*.c))
BALLOC_TESTS = $(patsubst tests/%.c,$(B)/%,$(wildcard tests/t_balloc*.c))

all: $(TESTS) $(B)/base.img $(B)/balloc.img

$(B):
	mkdir -p $@

# journal.c through env/journal_peek.c, which includes it and adds
# accessors for the tests.
$(B)/journal.o: env/journal_peek.c $(JOURNAL_C) $(HURD)/ext2fs/journal.h \
		env/jtest.h | $(B)
	$(CC) $(CPPFLAGS) $(CFLAGS) -DJT_JOURNAL_C='"$(abspath $(JOURNAL_C))"' \
	  -c -o $@ $<

$(B)/ihash.o: $(HURD)/libihash/ihash.c | $(B)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c -o $@ $<

$(B)/fakefs.o: env/fakefs.c env/jtest.h | $(B)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c -o $@ $<

$(B)/crash.o: env/crash.c env/jtest.h | $(B)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c -o $@ $<

$(B)/balloc.o: $(BALLOC_C) | $(B)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c -o $@ $<

# The real allocator, for the t_balloc* tests; the others keep the
# harness's ext2_release_busy_blocks.
$(BALLOC_TESTS): $(B)/t_balloc%: tests/t_balloc%.c tests/common.h $(ENV) \
		 $(B)/balloc.o
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ $< $(ENV) $(B)/balloc.o $(LDFLAGS)

$(B)/t_%: tests/t_%.c tests/common.h $(ENV)
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ $< $(ENV) $(LDFLAGS)

# A fresh ext3 image, as the Hurd makes them.
$(B)/base.img: | $(B)
	rm -f $@
	mke2fs -q -F -t ext3 -o hurd -b 4096 $@ 16M

# For the allocator: four groups, and two empty files to own blocks.
$(B)/balloc.img: | $(B)
	rm -f $@
	mke2fs -q -F -t ext3 -o hurd -b 4096 -g 1024 -N 128 $@ 16M
	debugfs -w -R "write /dev/null pool0" $@ >/dev/null 2>&1
	debugfs -w -R "write /dev/null pool1" $@ >/dev/null 2>&1
	e2fsck -fn $@ >/dev/null

check: all
	@for t in $(TESTS); do \
	  case $$t in *t_balloc*) img=$(B)/balloc.img;; *) img=$(B)/base.img;; esac; \
	  $$t $$img 2>$$t.log || { tail -40 $$t.log; echo "FAIL: $$t (log: $$t.log)"; exit 1; }; \
	done

clean:
	rm -rf $(B)

.PHONY: all check clean
