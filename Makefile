# Host (Linux) test harness for the ext2fs journal.
# Builds journal.c straight from the Hurd tree against the real Hurd lib
# headers, a fake Mach layer (shim/) and a fake ext2fs around it (env/).
#
#   make check            build and run every test
#   make check SAN=address,undefined
#   make check SAN=thread
#   make HURD=/path/to/hurd

HURD ?= ../cross-hurd/src/hurd

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
ENV = $(B)/journal.o $(B)/ihash.o $(B)/fakefs.o
TESTS = $(patsubst tests/%.c,$(B)/%,$(wildcard tests/t_*.c))

all: $(TESTS) $(B)/base.img

$(B):
	mkdir -p $@

$(B)/journal.o: $(HURD)/ext2fs/journal.c $(HURD)/ext2fs/journal.h | $(B)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c -o $@ $<

$(B)/ihash.o: $(HURD)/libihash/ihash.c | $(B)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c -o $@ $<

$(B)/fakefs.o: env/fakefs.c env/jtest.h | $(B)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c -o $@ $<

$(B)/t_%: tests/t_%.c tests/common.h $(ENV)
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ $< $(ENV) $(LDFLAGS)

# A fresh ext3 image, as the Hurd makes them.
$(B)/base.img: | $(B)
	rm -f $@
	mke2fs -q -F -t ext3 -o hurd -b 4096 $@ 16M

check: all
	@for t in $(TESTS); do \
	  $$t $(B)/base.img 2>$$t.log || { tail -40 $$t.log; echo "FAIL: $$t (log: $$t.log)"; exit 1; }; \
	done

clean:
	rm -rf $(B)

.PHONY: all check clean
