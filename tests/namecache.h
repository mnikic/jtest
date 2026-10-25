/* libdiskfs's name cache against the directory itself.

   With DP locked, nothing changes its entries, so what the cache says of
   NAME (diskfs_check_lookup_cache) must agree with a scan of DP
   (diskfs_lookup_hard): a cached node must be the node the name is, and
   a cached absence must be a name that is not there.  Knowing nothing is
   always right.

   A lookup of ".." unlocks DP for a moment, in both: only check it when
   no rename runs.  */

#ifndef JT_NAMECACHE_H
#define JT_NAMECACHE_H

static unsigned long nc_checks, nc_hits, nc_negative;

/* Check NAME in DP, locked, as CR.  Returns NULL if the cache agrees,
   or what is wrong.  */
static const char *
nc_check (struct node *dp, const char *name, struct protid *cr)
{
  static __thread char msg[160];
  ino_t cached = 0;
  int negative = 0;

  struct node *np = diskfs_check_lookup_cache (dp, name);
  if (np == (struct node *) -1)
    negative = 1;
  else if (np)
    {
      cached = np->cache_id;
      if (np == dp)
	diskfs_nrele (np);
      else
	diskfs_nput (np);
    }
  __atomic_add_fetch (&nc_checks, 1, __ATOMIC_RELAXED);
  if (!negative && !cached)
    return NULL;
  if (negative)
    __atomic_add_fetch (&nc_negative, 1, __ATOMIC_RELAXED);
  else
    __atomic_add_fetch (&nc_hits, 1, __ATOMIC_RELAXED);

  np = NULL;
  error_t err = diskfs_lookup_hard (dp, name, LOOKUP, &np, NULL, cr);
  ino_t real = 0;
  if (!err && np)
    {
      real = np->cache_id;
      if (np == dp)
	diskfs_nrele (np);
      else
	diskfs_nput (np);
    }

  if (negative && real)
    snprintf (msg, sizeof msg, "the name cache says %s is not in directory "
	      "%llu, but it is inode %llu", name,
	      (unsigned long long) dp->cache_id, (unsigned long long) real);
  else if (!negative && real != cached)
    snprintf (msg, sizeof msg, "the name cache says %s in directory %llu "
	      "is inode %llu, but it is %s %llu", name,
	      (unsigned long long) dp->cache_id,
	      (unsigned long long) cached, real ? "inode" : "not there, error",
	      (unsigned long long) (real ? real : (ino_t) err));
  else
    return NULL;
  return msg;
}

#endif
