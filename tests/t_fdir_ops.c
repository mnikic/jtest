/* The real ext2fs/dir.c on the harness's file pager (env/fpager.c): the
   root directory's entries made, removed and rewritten, as libdiskfs's
   dir-link.c, dir-unlink.c and dir-rename.c drive diskfs_lookup_hard and
   diskfs_direnter_hard, diskfs_dirremove_hard and
   diskfs_dirrewrite_hard.

   Each operation is a handle, with the root locked:
     enter    a new file (diskfs_create_node, no name) gets a name: a
	      lookup with CREATE finds the slot, the file's link count goes
	      up (off the orphan list) and diskfs_direnter_hard writes the
	      entry, growing the directory when no block has room (EXTEND);
     remove   a lookup with REMOVE finds a name, diskfs_dirremove_hard
	      takes it out; a file whose last name goes is freed;
     rewrite  a lookup with RENAME finds name B, diskfs_dirrewrite_hard
	      makes it name A's file, and B's file loses that name;
     check    every name looked up (LOOKUP) and the entries read
	      (diskfs_get_directs), against the model.
   Names are long, so that few fit in a block.  Pages of the root are
   evicted at random, and some operations commit.

   After the unmount and in every crash image: e2fsck -fn is clean (the
   entries, the link counts and the orphans agree), and the root's
   entries (debugfs ls -p) are those after one operation, no older than
   one whose commit returned.  */

#include "files.h"

/* t_fdir_mt.c builds this test with threads, under its own name.  */
#ifndef NT
#define NT 1
#define OPS 300
#define T_NAME "t_fdir_ops"
#endif
#define MAXE 60
#define NAMELEN 200

struct entry
{
  char name[NAMELEN + 1];
  ino_t ino;
};

/* The model: the entries now, and a sorted text of them after each
   operation.  */
static struct entry ents[MAXE];
static int nents;
static char **snaps;
static size_t *synced;
static int nsnaps;
static unsigned long n_enter, n_remove, n_rewrite, n_check, n_extend;
static unsigned next_id;

static void
make_name (char *buf, unsigned id)
{
  int n = snprintf (buf, NAMELEN + 1, "e%05u-", id);
  memset (buf + n, 'a' + id % 26, NAMELEN - n);
  buf[NAMELEN] = 0;
}

static int
cmp_lines (const void *a, const void *b)
{
  return strcmp (*(char *const *) a, *(char *const *) b);
}

/* The entries as sorted "INO NAME" lines, which is how the check reads
   them back.  */
static char *
model_text (void)
{
  char *lines[MAXE];
  size_t len = 1;
  for (int i = 0; i < nents; i++)
    {
      CHECK (asprintf (&lines[i], "%lu %s\n", (unsigned long) ents[i].ino,
		       ents[i].name) > 0, "asprintf");
      len += strlen (lines[i]);
    }
  qsort (lines, nents, sizeof *lines, cmp_lines);
  char *t = malloc (len);
  t[0] = 0;
  for (int i = 0; i < nents; i++)
    {
      strcat (t, lines[i]);
      free (lines[i]);
    }
  return t;
}

static void
op_done (int sync_now)
{
  snaps[nsnaps] = model_text ();
  synced[nsnaps] = sync_now ? 0 : SIZE_MAX;
  nsnaps++;
}

/* A lookup of NAME in the root with FLAGS, as diskfs_lookup does it.
   Returns the error; *NPP is the node found, locked, or NULL.  */
static error_t
lookup (const char *name, int flags, struct node **npp, struct dirstat *ds)
{
  *npp = NULL;
  return diskfs_lookup_hard (root, name, flags, npp, ds, &cred);
}

static void
op_enter (unsigned id)
{
  struct node *np;
  struct dirstat *ds = alloca (diskfs_dirstat_size);
  char name[NAMELEN + 1];
  make_name (name, id);

  CHECK (!diskfs_create_node (root, NULL, S_IFREG | 0644, &np, &cred, NULL),
	 "create");
  struct node *found;
  error_t err = lookup (name, CREATE, &found, ds);
  CHECK (err == ENOENT && !found, "lookup CREATE of a new name: %s",
	 strerror (err));
  /* dir_link: the link count first, then the entry.  */
  np->dn_stat.st_nlink++;
  np->dn_set_ctime = 1;
  diskfs_orphan_del (np);
  diskfs_node_update (np, 0);
  off_t before = root->dn_stat.st_size;
  CHECK (!diskfs_direnter_hard (root, name, np, ds, &cred), "direnter");
  if (root->dn_stat.st_size > before)
    n_extend++;
  strcpy (ents[nents].name, name);
  ents[nents].ino = np->cache_id;
  nents++;
  diskfs_nput (np);
  n_enter++;
}

/* NP lost a name: dir_unlink's steps after diskfs_dirremove.  NP is
   locked; it is released.  */
static void
lost_name (struct node *np)
{
  np->dn_stat.st_nlink--;
  np->dn_set_ctime = 1;
  if (np->dn_stat.st_nlink == 0)
    diskfs_orphan_add (np);
  diskfs_node_update (np, 0);
  diskfs_nput (np);
}

static void
op_remove (int i)
{
  struct node *np;
  struct dirstat *ds = alloca (diskfs_dirstat_size);
  error_t err = lookup (ents[i].name, REMOVE, &np, ds);
  CHECK (!err && np && np->cache_id == ents[i].ino, "lookup REMOVE: %s",
	 strerror (err));
  CHECK (!diskfs_dirremove_hard (root, ds), "dirremove");
  lost_name (np);
  ents[i] = ents[--nents];
  n_remove++;
}

static void
op_rewrite (int a, int b)
{
  struct node *npa, *npb;
  struct dirstat *ds = alloca (diskfs_dirstat_size);
  error_t err = lookup (ents[a].name, LOOKUP, &npa, NULL);
  CHECK (!err && npa, "lookup of A: %s", strerror (err));
  /* B is to name A's file: A's gains a link.  As dir-rename.c does, A
     is unlocked before B is looked up, so that two files are never
     locked at once.  */
  npa->dn_stat.st_nlink++;
  npa->dn_set_ctime = 1;
  diskfs_node_update (npa, 0);
  pthread_mutex_unlock (&npa->lock);
  err = lookup (ents[b].name, RENAME, &npb, ds);
  CHECK (!err && npb && npb->cache_id == ents[b].ino, "lookup RENAME of B: "
	 "%s", strerror (err));
  CHECK (!diskfs_dirrewrite_hard (root, npa, ds), "dirrewrite");
  /* B's first: its lock is held, and letting A go can lock A.  */
  lost_name (npb);
  diskfs_nrele (npa);
  ents[b].ino = ents[a].ino;
  n_rewrite++;
}

static void
op_check (void)
{
  for (int i = 0; i < nents; i++)
    {
      struct node *np;
      error_t err = lookup (ents[i].name, LOOKUP, &np, NULL);
      CHECK (!err && np, "lookup of %.20s...: %s", ents[i].name,
	     strerror (err));
      CHECK (np->cache_id == ents[i].ino, "%.20s... names inode %lu, not "
	     "%lu", ents[i].name, (unsigned long) np->cache_id,
	     (unsigned long) ents[i].ino);
      diskfs_nput (np);
    }
  /* The entries read as readdir does, past ".", ".." and lost+found.  */
  char *data = NULL;
  mach_msg_type_number_t datacnt = 0;
  int amt = 0;
  CHECK (!diskfs_get_directs (root, 0, -1, &data, &datacnt, 0, &amt),
	 "get_directs");
  CHECK (amt == nents + 3, "readdir: %d entries, %d expected", amt,
	 nents + 3);
  if (data)
    munmap (data, datacnt);
  n_check++;
}

/* The root's entries in IMG as sorted "INO NAME" lines, past ".", ".."
   and lost+found.  */
static char *
image_text (const char *img)
{
  char cmd[512];
  snprintf (cmd, sizeof cmd, "debugfs -R 'ls -p /' %s 2>/dev/null", img);
  FILE *p = popen (cmd, "r");
  CHECK (p, "debugfs");
  char *lines[MAXE + 8];
  int n = 0;
  size_t len = 1;
  char buf[NAMELEN + 64];
  while (fgets (buf, sizeof buf, p))
    {
      /* /INODE/MODE/UID/GID/NAME/SIZE/ */
      unsigned long ino;
      char *s = buf, *name;
      if (*s++ != '/')
	continue;
      ino = strtoul (s, &s, 10);
      for (int f = 0; f < 3 && s; f++)
	s = strchr (s + 1, '/');
      if (!s)
	continue;
      name = s + 1;
      char *end = strchr (name, '/');
      if (!end)
	continue;
      *end = 0;
      if (!strcmp (name, ".") || !strcmp (name, "..")
	  || !strcmp (name, "lost+found") || ino == 0)
	continue;
      CHECK (n < MAXE + 8, "too many entries");
      CHECK (asprintf (&lines[n], "%lu %s\n", ino, name) > 0, "asprintf");
      len += strlen (lines[n++]);
    }
  pclose (p);
  qsort (lines, n, sizeof *lines, cmp_lines);
  char *t = malloc (len);
  t[0] = 0;
  for (int i = 0; i < n; i++)
    {
      strcat (t, lines[i]);
      free (lines[i]);
    }
  return t;
}

static const char *
check (size_t n, int torn, const char *img, void *arg)
{
  static char msg[256];
  (void) torn;
  (void) arg;
  char *got = image_text (img);
  int durable = 0, j;
  for (j = 0; j < nsnaps; j++)
    if (__atomic_load_n (&synced[j], __ATOMIC_ACQUIRE) <= n)
      durable = j;
  for (j = nsnaps - 1; j >= 0; j--)
    if (!strcmp (got, snaps[j]))
      break;
  if (j < 0)
    snprintf (msg, sizeof msg, "the root's entries are those after no "
	      "operation (%zu bytes of them)", strlen (got));
  else if (j < durable)
    snprintf (msg, sizeof msg, "the root's entries are those after "
	      "operation %d, but %d had committed", j, durable);
  free (got);
  return j < 0 || j < durable ? msg : NULL;
}

/* Operations; with NT > 1 threads make them at once, the root's lock
   putting them in the order the model has.  */
static void *
worker (void *arg)
{
  unsigned seed = (unsigned) (uintptr_t) arg;

  for (int op = 0; op < OPS; op++)
    {
      int r = rand_r (&seed) % 10;
      diskfs_transaction_t *txn = diskfs_journal_start_transaction ();
      pthread_mutex_lock (&root->lock);
      if (nents == 0 || (r < 5 && nents < MAXE))
	op_enter (next_id++);
      else if (r < 8)
	op_remove (rand_r (&seed) % nents);
      else if (nents >= 2)
	{
	  int a = rand_r (&seed) % nents, b = rand_r (&seed) % nents;
	  if (a != b && ents[a].ino != ents[b].ino)
	    op_rewrite (a, b);
	}
      if (rand_r (&seed) % 8 == 0)
	op_check ();
      op_done (0);
      int j = nsnaps - 1;
      size_t pages = (root->dn_stat.st_size >> log2_block_size) + 1;
      pthread_mutex_unlock (&root->lock);
      if (rand_r (&seed) % 5 == 0)
	{
	  CHECK (!jt_commit (txn), "commit");
	  /* Durable from here on.  */
	  __atomic_store_n (&synced[j], jt_log_count (), __ATOMIC_RELEASE);
	}
      else
	diskfs_journal_stop_transaction (txn);
      for (int e = rand_r (&seed) % 3; e > 0; e--)
	jt_fp_pageout (root, rand_r (&seed) % pages, 1, 0);
    }
  return NULL;
}

int
main (int argc, char **argv)
{
  const char *image = argc > 1 ? argv[1] : "build/balloc.img";
  unsigned seed = argc > 2 ? atoi (argv[2]) : 1;

  jt_mount (image, 3, 0);
  jt_inode_globals ();
  files_init ();
  /* The image's root has two files already.  */
  pthread_mutex_lock (&root->lock);
  for (int i = 0; i < 2; i++)
    {
      struct node *np;
      snprintf (ents[nents].name, sizeof ents[nents].name, "pool%d", i);
      CHECK (!lookup (ents[nents].name, LOOKUP, &np, NULL) && np,
	     "lookup %s", ents[nents].name);
      ents[nents++].ino = np->cache_id;
      diskfs_nput (np);
    }
  pthread_mutex_unlock (&root->lock);
  snaps = calloc (NT * OPS + 1, sizeof *snaps);
  synced = calloc (NT * OPS + 1, sizeof *synced);
  op_done (1);
  synced[0] = 0;

  /* The periodic sync, every few milliseconds, unless
     JT_NO_PERIODIC_SYNC (JT_PERIODIC_SYNC_US: at most that long apart,
     default 2000): it writes every cached node, each under its
     lock, in a handle of its own, and commits.  */
  int psync = !getenv ("JT_NO_PERIODIC_SYNC");
  if (psync)
    jt_periodic_sync_start (getenv ("JT_PERIODIC_SYNC_US")
			    ? atoi (getenv ("JT_PERIODIC_SYNC_US")) : 2000, seed);
  pthread_t t[NT];
  for (int i = 0; i < NT; i++)
    pthread_create (&t[i], NULL, worker, (void *) (uintptr_t) (seed * 31 + i));
  for (int i = 0; i < NT; i++)
    pthread_join (t[i], NULL);
  unsigned long syncs = psync ? jt_periodic_sync_stop () : 0;
  pthread_mutex_lock (&root->lock);
  op_check ();
  pthread_mutex_unlock (&root->lock);

  unsigned long warnings = jt_umount ();
  CHECK (warnings == 0, "%lu journal warnings", warnings);
  CHECK (jt_fp_stale_reads () == 0, "%lu stale file page-ins",
	 jt_fp_stale_reads ());
  const char *out = "build/" T_NAME ".img";
  jt_save_image (out);
  CHECK (jt_run ("e2fsck -fn %s >%s.fsck 2>&1", out, out) == 0,
	 "e2fsck -fn %s is not clean after the unmount (see %s.fsck)", out,
	 out);
  char *got = image_text (out);
  CHECK (!strcmp (got, snaps[nsnaps - 1]), "the root's entries after the "
	 "unmount are not the last operation's");
  free (got);

  size_t checked = jt_crash_check (T_NAME, check, NULL);
  printf (T_NAME ": OK (%d threads; %lu enters, %d of them growing the "
	  "directory to %ld bytes, %lu removes, %lu rewrites, %lu checks; "
	  "%lu syncs; %zu writes, %zu crash images)\n", NT, n_enter,
	  (int) n_extend, (long) root->dn_stat.st_size, n_remove, n_rewrite,
	  n_check, syncs, jt_log_count (), checked);
  return 0;
}
