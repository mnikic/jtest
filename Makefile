# Host (Linux) test harness for the ext2fs journal.
# Builds journal.c straight from the Hurd tree against the real Hurd lib
# headers, a fake Mach layer (shim/) and a fake ext2fs around it (env/).
#
#   make check            build and run every test
#   make check SAN=address,undefined
#   make check SAN=thread
#   make COV=1 B=build/cov     for scripts/coverage.py
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
# A patched journal.c can come with patched headers of the tree next to
# it (patched/libdiskfs/diskfs.h), found first.  make clean after
# switching.
ifneq ($(abspath $(JOURNAL_C)),$(abspath $(HURD)/ext2fs/journal.c))
CPPFLAGS += -I$(dir $(JOURNAL_C))
endif
CPPFLAGS += -include shim/hurdstat.h
CPPFLAGS += -D_GNU_SOURCE -Ishim -Ienv -I$(HURD) -I$(HURD)/libshouldbeinlibc \
	    -I$(HURD)/libports -I$(HURD)/ext2fs
# Debug messages go to the flight recorder only; it is dumped on aborts.
CPPFLAGS += -DJOURNAL_DEBUG_LIVE=0
LDFLAGS += -pthread

ifdef SAN
CFLAGS += -fsanitize=$(SAN)
LDFLAGS += -fsanitize=$(SAN)
endif

# Coverage (scripts/coverage.py): -Og, so that counts follow the source
# and the extern inlines of the Hurd headers are still there.
ifdef COV
CFLAGS += -Og --coverage
LDFLAGS += --coverage
endif

B ?= build
ENV = $(B)/journal.o $(B)/ihash.o $(B)/fakefs.o $(B)/crash.o
TESTS = $(patsubst tests/%.c,$(B)/%,$(wildcard tests/t_*.c))
BALLOC_TESTS = $(patsubst tests/%.c,$(B)/%,$(wildcard tests/t_balloc*.c))
# Tests on the real getblk.c and truncate.c (and balloc.c) of the tree.
EXT2_TESTS = $(patsubst tests/%.c,$(B)/%,$(wildcard tests/t_trunc*.c))
EXT2_OBJS = $(B)/balloc.o $(B)/getblk.o $(B)/truncate.o $(B)/ext2glue.o \
	    $(B)/nodeglue.o
# Tests on the real inode.c, ialloc.c, orphan.c and hyper.c too, and
# libdiskfs's node cache, references and drop.
INODE_TESTS = $(patsubst tests/%.c,$(B)/%,$(wildcard tests/t_orphan*.c \
					       tests/t_rwcycle*.c \
					       tests/t_abortfiles*.c \
					       tests/t_nodecache*.c))
LIBDISKFS_NODE = node-cache node-drop node-nput node-nputl node-nref \
		 node-nrefl node-nrele node-nrelel node-make node-update \
		 node-times node-lastref node-create validate-owner \
		 validate-group validate-mode
INODE_OBJS = $(B)/balloc.o $(B)/getblk.o $(B)/truncate.o $(B)/ext2glue.o \
	     $(B)/inodeglue.o $(B)/inode.o $(B)/ialloc.o $(B)/orphan.o \
	     $(B)/hyper.o $(patsubst %,$(B)/ld-%.o,$(LIBDISKFS_NODE)) \
	     $(B)/idvec.o

all: $(TESTS) $(B)/base.img $(B)/balloc.img $(B)/big.img $(B)/full.img

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
$(BALLOC_TESTS): $(B)/t_balloc%: tests/t_balloc%.c tests/t_balloc.c \
		 tests/common.h $(ENV) $(B)/balloc.o
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ $< $(ENV) $(B)/balloc.o $(LDFLAGS)

$(B)/getblk.o: $(HURD)/ext2fs/getblk.c | $(B)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c -o $@ $<

# truncate.c's nested functions have trampolines on the stack, which
# ASan's fake stacks (use-after-return) would put on the heap.
TRUNC_CFLAGS = $(if $(findstring address,$(SAN)),\
		 --param asan-use-after-return=0)
$(B)/truncate.o: $(HURD)/ext2fs/truncate.c | $(B)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(TRUNC_CFLAGS) -c -o $@ $<

$(B)/ext2glue.o: env/ext2glue.c env/jtest.h | $(B)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c -o $@ $<

$(B)/nodeglue.o: env/nodeglue.c env/jtest.h | $(B)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c -o $@ $<

$(B)/inodeglue.o: env/inodeglue.c env/jtest.h | $(B)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c -o $@ $<

# Where inode.c, ialloc.c, orphan.c and hyper.c come from: a directory
# of patched copies can stand in for the tree's.
INODE_SRC ?= $(HURD)/ext2fs
$(B)/inode.o $(B)/ialloc.o $(B)/orphan.o $(B)/hyper.o: $(B)/%.o: \
		$(INODE_SRC)/%.c | $(B)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c -o $@ $<

$(B)/idvec.o: $(HURD)/libshouldbeinlibc/idvec.c | $(B)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c -o $@ $<

# A patched copy can stand in for the tree's.
NODE_CREATE_C ?= $(HURD)/libdiskfs/node-create.c
$(B)/ld-node-create.o: $(NODE_CREATE_C) | $(B)
	$(CC) $(CPPFLAGS) -I$(HURD)/libdiskfs $(CFLAGS) -c -o $@ $<

NODE_CACHE_C ?= $(HURD)/libdiskfs/node-cache.c
$(B)/ld-node-cache.o: $(NODE_CACHE_C) | $(B)
	$(CC) $(CPPFLAGS) -I$(HURD)/libdiskfs $(CFLAGS) -c -o $@ $<

$(B)/ld-%.o: $(HURD)/libdiskfs/%.c | $(B)
	$(CC) $(CPPFLAGS) -I$(HURD)/libdiskfs $(CFLAGS) -c -o $@ $<

$(INODE_TESTS): $(B)/%: tests/%.c tests/common.h tests/files.h $(ENV) \
		$(INODE_OBJS)
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ $< $(ENV) $(INODE_OBJS) $(LDFLAGS) \
	  -Wl,--no-warn-execstack -Wl,--wrap=diskfs_set_node_times \
	  -Wl,--wrap=diskfs_make_node_alloc -Wl,--wrap=diskfs_node_norefs

# Tests on the real directory code too, with a file pager for nodes
# (env/fpager.c), and libdiskfs's directory operations around dir.c.
DIR_LIBDISKFS = dir-rename dir-renamed lookup direnter dirremove dirrewrite \
		name-cache dir-init dir-clear
# A patched copy can stand in for the tree's.
DIR_RENAMED_C ?= $(HURD)/libdiskfs/dir-renamed.c
$(B)/ld-dir-renamed.o: $(DIR_RENAMED_C) | $(B)
	$(CC) $(CPPFLAGS) -I$(HURD)/libdiskfs $(CFLAGS) -c -o $@ $<
DIRENTER_C ?= $(HURD)/libdiskfs/direnter.c
$(B)/ld-direnter.o: $(DIRENTER_C) | $(B)
	$(CC) $(CPPFLAGS) -I$(HURD)/libdiskfs $(CFLAGS) -c -o $@ $<
DIR_LIBDISKFS_OBJS = $(patsubst %,$(B)/ld-%.o,$(DIR_LIBDISKFS)) \
		     $(B)/dirglue.o $(B)/murmur3.o
$(B)/dirglue.o: env/dirglue.c env/jtest.h | $(B)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c -o $@ $<
$(B)/murmur3.o: $(HURD)/libihash/murmur3.c | $(B)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c -o $@ $<
DIR_TESTS = $(patsubst tests/%.c,$(B)/%,$(wildcard tests/t_fdir*.c))
$(B)/fpager.o: env/fpager.c env/jtest.h | $(B)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c -o $@ $<
$(B)/btrace.o: env/btrace.c env/jtest.h | $(B)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c -o $@ $<
BTRACE_WRAPS = -Wl,--wrap=ext2_new_block -Wl,--wrap=ext2_free_blocks \
	       -Wl,--wrap=journal_get_write_access_external \
	       -Wl,--wrap=journal_mark_dirty_external \
	       -Wl,--wrap=journal_release_access -Wl,--wrap=diskfs_truncate

DIR_C ?= $(HURD)/ext2fs/dir.c
# shim/dir: the Hurd's struct dirent, for dir.c only.
$(B)/dir.o: $(DIR_C) shim/dir/dirent.h | $(B)
	$(CC) -Ishim/dir $(CPPFLAGS) $(CFLAGS) -c -o $@ $<

$(B)/t_fdir_mt: tests/t_fdir_ops.c
$(DIR_TESTS): $(B)/%: tests/%.c tests/common.h tests/files.h $(ENV) \
	      $(INODE_OBJS) $(B)/fpager.o $(B)/dir.o $(DIR_LIBDISKFS_OBJS) \
	      $(B)/btrace.o
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ $< $(ENV) $(INODE_OBJS) \
	  $(B)/fpager.o $(B)/dir.o $(DIR_LIBDISKFS_OBJS) $(B)/btrace.o \
	  $(LDFLAGS) -Wl,--no-warn-execstack $(BTRACE_WRAPS) \
	  -Wl,--wrap=diskfs_set_node_times -Wl,--wrap=diskfs_make_node_alloc \
	  -Wl,--wrap=diskfs_node_norefs -Wl,--wrap=munmap -Wl,--wrap=mmap

$(EXT2_TESTS): $(B)/t_trunc%: tests/t_trunc%.c tests/common.h $(ENV) \
	       $(EXT2_OBJS)
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ $< $(ENV) $(EXT2_OBJS) $(LDFLAGS) \
	  -Wl,--no-warn-execstack

$(B)/t_logpress: tests/t_threads.c
$(B)/t_%: tests/t_%.c tests/common.h $(ENV)
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ $< $(ENV) $(LDFLAGS)

# A fresh ext3 image, as the Hurd makes them.
$(B)/base.img: | $(B)
	rm -f $@
	mke2fs -q -F -t ext3 -o hurd -b 4096 $@ 16M

# For t_logfmt's large transactions: a journal of 4096 blocks.
$(B)/big.img: | $(B)
	rm -f $@
	mke2fs -q -F -t ext3 -o hurd -b 4096 -J size=16 $@ 64M

# For the allocator: four groups, and two empty files to own blocks.
$(B)/balloc.img: | $(B)
	rm -f $@
	mke2fs -q -F -t ext3 -o hurd -b 4096 -g 1024 -N 128 $@ 16M
	debugfs -w -R "write /dev/null pool0" $@ >/dev/null 2>&1
	debugfs -w -R "write /dev/null pool1" $@ >/dev/null 2>&1

# balloc.img with a file that takes all but a few dozen of its free
# blocks (some go to the file's indirect blocks), for t_balloc_full.
$(B)/full.img: $(B)/balloc.img
	cp $< $@
	n=$$(dumpe2fs -h $@ 2>/dev/null | awk '/^Free blocks:/ { print $$3 }'); \
	  yes | head -c $$(( (n - 28) * 4096 )) >$@.data
	debugfs -w -R "write $@.data filler" $@ >/dev/null 2>&1
	rm -f $@.data
	e2fsck -fn $@ >/dev/null

check: all
	@for t in $(TESTS); do \
	  case $$t in *t_balloc_full*|*t_balloc_lock*) img=$(B)/full.img;; *t_balloc*|*t_trunc*|*t_orphan*|*t_rwcycle*|*t_abortfiles*|*t_nodecache*|*t_fdir*) img=$(B)/balloc.img;; *) img=$(B)/base.img;; esac; \
	  $$t $$img 2>$$t.log || { tail -40 $$t.log; echo "FAIL: $$t (log: $$t.log)"; exit 1; }; \
	done

clean:
	rm -rf $(B)

.PHONY: all check clean
