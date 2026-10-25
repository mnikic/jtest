#!/usr/bin/env python3
"""Check that the tests still catch the bugs they were written for.

  scripts/mutants.py [--seeds N] [--parallel P] [--points K]
                     [--check] [--list] [MUTANT...]

Each mutant below puts one bug back into a copy of the Hurd tree (as a
replacement of a snippet of one file), builds the harness against the
copy, and runs the test that must catch it over N seeds (default 3,
deterministic tests once).  It is caught on a seed if the test fails.
A mutant whose snippet is no longer in the tree, exactly once, is STALE:
the code changed, and the entry needs updating.  --check only looks for
stale ones.  The copy is links to the tree but for the mutated file,
under mutants/; a mutant's runs are kept there if one is not caught.

Exits 1 if a mutant is stale, or caught on fewer seeds than its MIN.
"""

import argparse
import concurrent.futures
import os
import shutil
import subprocess
import sys

sys.path.insert (0, os.path.dirname (os.path.abspath (__file__)))
import common

# The tree to mutate: $HURD, as for make, or the one next to jtest.
HURD = os.path.abspath (os.environ.get ("HURD") or
                        os.path.join (common.JTEST, "..", "cross-hurd", "src",
                                      "hurd"))


class Mutant:
    def __init__ (self, name, path, old, new, test, args=(), env=None,
                  min_caught=1, why="", san=None):
        self.name, self.path, self.old, self.new = name, path, old, new
        self.test, self.args, self.env = test, list (args), env or {}
        self.min_caught, self.why, self.san = min_caught, why, san


J = "ext2fs/journal.c"
MUTANTS = [
    # Reads after a failed flush (0ef4a0b25).
    Mutant ("failed-flush-reads", J,
            """  journal_buffer_t *found = NULL;
  for (diskfs_transaction_t *t = journal->j_checkpoint_list; t;""",
            """  journal_buffer_t *found = NULL;
  for (diskfs_transaction_t *t = NULL; t;""",
            "t_ioerr", ["checkpoint"],
            why="a page-in after a failed flush misses the checkpointed shadow"),
    # Aborted commits (062ebfc64, bd73496d2).
    Mutant ("abort-reads", J,
            """  jb = aborted ? journal_map_lookup (&aborted->t_buffer_map, b) : NULL;
  if (jb && jb->needs_flush)
    return jb;""",
            """  jb = NULL;""",
            "t_ioerr", ["log-data"],
            why="reads miss the aborted transaction's shadows"),
    Mutant ("abort-holds-writes", J,
            """  jb_aborted = aborted ? journal_map_lookup (&aborted->t_buffer_map, b)
    : NULL;""",
            """  jb_aborted = NULL;""",
            "t_ioerr", ["commit"],
            why="pager writes of aborted blocks go home"),
    Mutant ("abort-refuses-commits", J,
            """  /* After an abort nothing commits again.  */
  if (journal->j_errno)""",
            """  if (0)""",
            "t_ioerr", ["log-data"],
            why="commits go on after an abort"),
    Mutant ("abort-commit-error", J,
            """  if (journal->j_errno && !tid_gt (journal->j_aborted_tid, tid))""",
            """  if (0)""",
            "t_ioerr", ["log-data"],
            why="a commit after the abort returns 0"),
    Mutant ("abort-mount-check", J,
            """  else if (jsb->s_errno != 0)""",
            """  else if (0)""",
            "t_ioerr", ["commit"],
            why="an aborted journal can be mounted before e2fsck"),
    Mutant ("abort-readonly", J,
            """      if (journal->j_errno && !diskfs_readonly)""",
            """      if (0)""",
            "t_ioerr", ["log-data"],
            why="kjournald never makes the filesystem read-only"),
    Mutant ("abort-no-inhibit", J,
            """  journal->j_errno = err;
  pthread_cond_signal (&journal->j_flusher_wakeup);""",
            """  journal->j_errno = err;
  diskfs_hard_readonly = diskfs_readonly = 1;
  pthread_cond_signal (&journal->j_flusher_wakeup);""",
            "t_abortfiles",
            why="read-only set with RPCs in flight"),
    # Escaped blocks (987473d24) and the log format.
    Mutant ("escape-flag", J,
            """      if (jb->jb_escaped)
	flags |= JBD2_FLAG_ESCAPE;""",
            """      (void) 0;""",
            "t_logfmt", ["escape"],
            why="an escaped block is not tagged so"),
    Mutant ("revoke-records", J,
            """  err = journal_write_revoke_records (journal, txn);""",
            """  err = 0;""",
            "t_free_only",
            why="no revoke records: a freed block's old copy is replayed"),
    # Commits (edc81c9aa, a136e413c, f993252f5).
    Mutant ("empty-log-start", J,
            """  if (journal->j_disk_tail == 0)
    {""",
            """  if (0)
    {""",
            "t_crash",
            why="a commit into an empty log is durable before s_start is"),
    Mutant ("free-only-commit", J,
            """  if (txn->t_buffer_map.size == 0 && !txn->t_freed_blocks
      && txn->t_revoke_map.size == 0)""",
            """  if (txn->t_buffer_map.size == 0)""",
            "t_free_only",
            why="a transaction that only frees is never committed"),
    Mutant ("notify-committed", J,
            """  if (commit && commit->t_state == T_COMMITTED)
    journal_notify_txn_locked (commit, start_block, n_blocks);""",
            """  if (commit)
    journal_notify_txn_locked (commit, start_block, n_blocks);""",
            "t_notify_race",
            why="a home write notifies a transaction still taking edits"),
    # The read-only switch (1d791e793 aside, which only ASan sees).
    Mutant ("restart-sequence", J,
            """  journal->j_transaction_sequence = be32toh (jsb->s_sequence);""",
            """  journal->j_transaction_sequence = 1;""",
            "t_rwcycle",
            why="a restarted journal reuses transaction numbers"),
    Mutant ("quiesce-checkpoints", J,
            """    journal_flush_checkpoints_locked (ext2_journal, UINT32_MAX, 0);""",
            """    0;""",
            "t_rwcycle",
            why="stopping the journal leaves blocks only in the log"),
    # A stopped journal's journal_t outlives it (cbb7243b3; 1d791e793,
    # reverted, freed it): only ASan sees the use after the free.
    Mutant ("journal-t-kept", J,
            """  /* The reference journal_create took.  */
  if (jnode)
    diskfs_nrele (jnode);
}""",
            """  /* The reference journal_create took.  */
  if (jnode)
    diskfs_nrele (jnode);
  free (journal);
}""",
            "t_rwcycle", san="address,undefined",
            why="a page-in in flight at the stop locks a freed journal"),
    # The orphan list (05c25a017, bb62f8b7f, b3335f38e and orphan.c).
    Mutant ("orphan-add-superblock", "ext2fs/orphan.c",
            """  pthread_mutex_unlock (&orphan_lock);

  /* hyper.c handles the superblock's get_write_access -> memcpy -> mark_dirty! */
  diskfs_set_hypermetadata (0, 0);""",
            """  pthread_mutex_unlock (&orphan_lock);""",
            "t_orphan",
            why="the list head is not journaled with the inode"),
    Mutant ("orphan-del-relink", "ext2fs/orphan.c",
            """      prev_di->i_dtime = htole32 (my_next);""",
            """      (void) my_next;""",
            "t_orphan",
            why="removing an orphan does not relink its predecessor"),
    Mutant ("orphan-recover-walk", "ext2fs/orphan.c",
            """      di = dino_ref (inum);
      next_orphan = le32toh (di->i_dtime);
      dino_deref (di);""",
            """      di = dino_ref (inum);
      next_orphan = 0;
      dino_deref (di);""",
            "t_orphan",
            why="recovery stops after the first orphan"),
    Mutant ("orphan-recover-guard", "ext2fs/orphan.c",
            """  if (live)
    return;
""",
            "",
            "t_rwcycle",
            why="recovery on a switch to writable breaks the live list"),
    Mutant ("write-node-dtime", "ext2fs/inode.c",
            """      if (!diskfs_node_disknode (np)->on_orphan_list)
	{""",
            """      if (1)
	{""",
            "t_orphan",
            why="write_node overwrites the next-orphan pointer"),
    Mutant ("write-node-aborted", "ext2fs/inode.c",
            """      assert_backtrace (!diskfs_readonly || journal_aborted ());""",
            """      assert_backtrace (!diskfs_readonly);""",
            "t_abortfiles",
            why="writing a node after an abort asserts"),
    Mutant ("readonly-recover", "ext2fs/hyper.c",
            """  if (!readonly)
    ext2_recover_orphan_list ();""",
            """  (void) 0;""",
            "t_orphan",
            why="orphans are not recovered when the filesystem becomes writable"),
    Mutant ("mkfile-orphan", "libdiskfs/node-create.c",
            """  if (!err && !name)
    diskfs_orphan_add (np);""",
            """  (void) 0;""",
            "t_orphan",
            why="dir_mkfile's file is not on the orphan list"),
    # truncate.c (de05e77).
    Mutant ("trunc-first", "ext2fs/truncate.c",
            """      for (index = first; index < addr_per_block; index++)""",
            """      for (index = 0; index < addr_per_block; index++)""",
            "t_trunc",
            why="a truncation frees blocks below the new end"),
    Mutant ("trunc-reserve", "ext2fs/truncate.c",
            """	  journal_get_write_access (txn, *p);
	  record_indir_poke (node, ind_bh);""",
            """	  record_indir_poke (node, ind_bh);""",
            "t_trunc",
            why="a surviving indirect block is edited unreserved"),
    # The barriers, seen only by crashes that lose writes since the last
    # flush (JT_REORDER).
    Mutant ("flush-before-commit", J,
            """  /* Ensure Data is on disk */
  flush_to_disk ();
""",
            "",
            "t_crash", env={"JT_REORDER": "4"},
            why="the commit block can reach the disk before its log data"),
    Mutant ("flush-after-commit", J,
            """  /* Ensure Commit is persistent */
  flush_to_disk ();
""",
            "",
            "t_sync", env={"JT_REORDER": "4"},
            why="a commit returns before its commit block is on disk"),
    # Failed home writes (jtest coverage gaps).
    Mutant ("run-claims-kept", J,
            """	  if (actual_blocks < flush_count && ext2_journal == journal)
	    journal_clear_flushing_locked (b + actual_blocks,
					   flush_count - actual_blocks);""",
            """	  if (0)
	    journal_clear_flushing_locked (b + actual_blocks,
					   flush_count - actual_blocks);""",
            "t_ioerr", ["pager-run"],
            why="a failed write of several blocks keeps its claims"),
    Mutant ("quiesce-ignores-error", J,
            """  if (io_error)
    {
      JRNL_LOG_WARN ("[QUIESCE] I/O error""",
            """  if (0)
    {
      JRNL_LOG_WARN ("[QUIESCE] I/O error""",
            "t_wrap", ["ckpt-umount"],
            why="a failed checkpoint at the unmount leaves the journal clean"),
    # An edit without a reservation while the commit writes the log.
    Mutant ("late-edit-into-commit", J,
            """      else if (!jb_run && commit->t_state == T_FLUSHING)""",
            """      else if (0)""",
            "t_late_edit",
            why="the page goes into the shadow the commit is writing"),
    Mutant ("late-edit-dropped", J,
            """	      if (run && !journal_get_write_access_locked (run, b, !external))""",
            """	      if (0)""",
            "t_late_edit",
            why="the edit is not carried into the next transaction"),
    # Freed blocks given back in transaction order (e57b53ac9).
    Mutant ("release-unordered", J,
            """  while (tid_gt (freed_tid, journal->j_released_tid + 1))
    JOURNAL_WAIT (&journal->j_commit_done, journal);""",
            "",
            "t_release_order",
            why="a newer commit's release goes before an older one's"),
    # Log space (t_logpress).
    Mutant ("commit-space-no-empty", J,
            """  if (journal->j_free < needed)
    journal_flush_checkpoints_locked (journal, UINT32_MAX, 0);""",
            """  if (0)
    journal_flush_checkpoints_locked (journal, UINT32_MAX, 0);""",
            "t_logpress",
            why="a transaction bigger than a forced checkpoint frees has no room"),
    Mutant ("wait-committed-frees", J,
            """  else if (tid_gt (ext2_journal->j_last_committed_tid,
		   ext2_journal->j_released_tid))""",
            """  else if (0)""",
            "t_release_order",
            why="a wait for freed blocks gives up while a commit's are on the way"),
    # libdiskfs's node cache (4f48c9dde).
    Mutant ("nodecache-nref-locked", "libdiskfs/node-cache.c",
            """  refcounts_ref (&np->refcounts, &result);
  pthread_rwlock_unlock (&nodecache_lock);
 referenced:""",
            """  diskfs_nref (np);
  result.hard = 2;
  pthread_rwlock_unlock (&nodecache_lock);
 referenced:""",
            "t_nodecache", ["lastref"],
            why="a lookup locks the node with nodecache_lock held"),
    Mutant ("nodecache-loser-unmarked", "libdiskfs/node-cache.c",
            """      forget_unread_node (np);
      np = tmp;""",
            """      diskfs_nput (np);
      np = tmp;""",
            "t_nodecache", ["lostrace"],
            why="the losing lookup drops its unread node as one without links"),
    Mutant ("nodecache-readerr-leak", "libdiskfs/node-cache.c",
            """    diskfs_nrele_light (np);

    forget_unread_node (np);""",
            """
    forget_unread_node (np);""",
            "t_nodecache", ["readerr"],
            why="a node whose read failed keeps the cache's reference"),
    Mutant ("nodecache-readwait", "libdiskfs/node-cache.c",
            """  if (gone)
    {""",
            """  if (0)
    {""",
            "t_nodecache", ["readwait"],
            why="a lookup hands out a node whose read failed while it waited"),
    Mutant ("nodecache-slot-unlocked", "libdiskfs/node-cache.c",
            """  pthread_rwlock_rdlock (&nodecache_lock);
  int gone = np->slot == NULL;
  pthread_rwlock_unlock (&nodecache_lock);""",
            """  int gone = np->slot == NULL;""",
            "t_fdir_ren", san="thread",
            why="the slot is read without nodecache_lock while the table "
            "moves its entries (TSan)"),
    # Directory blocks through a file pager (t_fdir, env/fpager.c).
    Mutant ("dir-overlay-off", J,
            """  if (!err && journal && *read_amount > 0)
    /* Pass the actual amount read, just in case it was a short read */""",
            """  if (0)
    /* Pass the actual amount read, just in case it was a short read */""",
            "t_fdir",
            why="a page-in of an intercepted directory block reads home"),
    Mutant ("dir-image-lost", J,
            """      memcpy (jb->jb_shadow_data, journal_external_scratch, block_size);
      jb->needs_copy = 0;""",
            """      jb->needs_copy = 0;""",
            "t_fdir",
            why="journal_mark_dirty_external drops the directory block's image"),
    Mutant ("dir-hazard-off", J,
            """  if (jb_run || jb_commit || jb_aborted)
    {""",
            """  if (0)
    {""",
            "t_fdir",
            why="a page-out of an uncommitted directory block goes home"),
    # io_write on a full disk (t_fwrite_enospc).
    Mutant ("enospc-no-retry", J,
            """  if (*retries >= 3 || ext2_busy_blocks () == 0)
    return 0;""",
            """  if (1)
    return 0;""",
            "t_fwrite_enospc",
            why="a write whose only free blocks are its transaction's frees "
            "fails with ENOSPC"),
    Mutant ("io-write-retry-reoffset", "libdiskfs/io-write.c",
            """  if (off == -1)
    {
      if (cred->po->openstat & O_APPEND)""",
            """  if (offset == -1)
    {
      if (cred->po->openstat & O_APPEND)""",
            "t_fwrite_enospc",
            why="a retried append goes past the end its first try made"),
    # libdiskfs's name cache against the directories (t_fdir_ren).
    Mutant ("nc-dirremove-no-purge", "libdiskfs/dirremove.c",
            """  diskfs_purge_lookup_cache (dp, np);
""", "",
            "t_fdir_ren", why="a removed name stays in the name cache"),
    Mutant ("nc-direnter-no-enter", "libdiskfs/direnter.c",
            """  diskfs_enter_lookup_cache (dp, np, name);
""", "",
            "t_fdir_ren", why="a new name keeps its cached absence"),
    # Directory operations on a full disk (t_fdir_enospc), and a rename
    # of a directory that fails after it changed ".." (t_fdir_renfail).
    Mutant ("dir-retry-mkdir", "libdiskfs/dir-mkdir.c",
            """diskfs_retry_enospc (try_mkdir""", """(try_mkdir""",
            "t_fdir_enospc", why="a mkdir does not try again"),
    Mutant ("dir-retry-link", "libdiskfs/dir-link.c",
            """diskfs_retry_enospc (try_link""", """(try_link""",
            "t_fdir_enospc", why="a link does not try again"),
    Mutant ("dir-retry-rename", "libdiskfs/dir-rename.c",
            """diskfs_retry_enospc (try_rename""", """(try_rename""",
            "t_fdir_enospc", why="a rename does not try again"),
    Mutant ("dir-retry-lookup", "libdiskfs/dir-lookup.c",
            """  if (!(flags & O_CREAT))""", """  if (1)""",
            "t_fdir_enospc", why="a create does not try again"),
    Mutant ("dir-lookup-no-copy", "libdiskfs/dir-lookup.c",
            """(memcpy (name, filename, len),""",
            """(name = (char *) filename,""",
            "t_fdir_enospc",
            why="a retried create gets the path the first try cut"),
    Mutant ("dir-retry-symlink", "libdiskfs/file-set-trans.c",
            """diskfs_retry_enospc (try_set_translator""",
            """(try_set_translator""",
            "t_fdir_enospc", why="a symlink does not try again"),
    Mutant ("rename-dotdot-not-undone", "libdiskfs/dir-renamed.c",
            """      if (fdp != tdp)
	{
	  error_t undo = undo_dotdot (fnp, fdp, tdp, fromcred);""",
            """      if (0)
	{
	  error_t undo = undo_dotdot (fnp, fdp, tdp, fromcred);""",
            "t_fdir_renfail",
            why="a directory left where it was keeps the new parent as \"..\""),
    Mutant ("rename-emlink-late", "libdiskfs/dir-renamed.c",
            """  if ((fdp != tdp && tdp->dn_stat.st_nlink == diskfs_link_max - 1)
      || fnp->dn_stat.st_nlink == diskfs_link_max - 1)""",
            """  if (fdp != tdp && tdp->dn_stat.st_nlink == diskfs_link_max - 1)""",
            "t_fdir_renfail",
            why="the moved directory's link count is not checked first"),
    Mutant ("rename-direnter-ds-kept", "libdiskfs/dir-renamed.c",
            """      err = diskfs_direnter (tdp, toname, fnp, ds, tocred);
      ds = 0;""",
            """      err = diskfs_direnter (tdp, toname, fnp, ds, tocred);""",
            "t_fdir_renfail",
            why="a failed direnter's mapping is unmapped twice"),
    # libdiskfs's renames and entries (t_fdir_renlock, t_fdir_ren,
    # t_fdir_dead).
    Mutant ("rename-bottom-up", "libdiskfs/dir-renamed.c",
            """  int tdp_above = fdp != tdp && in_chain (chain, nchain, tdp);""",
            """  int tdp_above = 0;""",
            "t_fdir_renlock",
            why="a rename onto an ancestor of the source's parent locks it "
            "after its descendant"),
    Mutant ("rename-bottom-up-move", "libdiskfs/dir-renamed.c",
            """  int tdp_above = fdp != tdp && in_chain (chain, nchain, tdp);""",
            """  int tdp_above = 0;""",
            "t_fdir_renlock", ["up"],
            why="a move up locks the source's parent before its parent"),
    Mutant ("rename-file-target-late", "libdiskfs/dir-renamed.c",
            """      if (!err && tnp && (in_chain (chain, nchain, tnp)
			  || !S_ISDIR (tnp->dn_stat.st_mode)))""",
            """      if (!err && tnp && in_chain (chain, nchain, tnp))""",
            "t_fdir_renlock", ["file"],
            why="the source's parent is locked after a file target that can "
            "be linked in it"),
    Mutant ("direnter-dead-dir", "libdiskfs/direnter.c",
            """  if (dp->dn_stat.st_nlink == 0)
    {
      diskfs_drop_dirstat (dp, ds);""",
            """  if (0)
    {
      diskfs_drop_dirstat (dp, ds);""",
            "t_fdir_dead",
            why="a removed directory takes a new entry"),
    # The real dir.c (t_fdir_ops).
    Mutant ("dirblock-unjournaled", "ext2fs/dir.c",
            """  txn = journal_thread_transaction ();
  err = journal_mark_dirty_external (txn, blk, (const void *) data);""",
            """  txn = journal_thread_transaction ();
  err = 0;
  journal_release_access (txn, blk);""",
            "t_fdir_ops",
            why="a directory block's edit is not handed to the journal"),
    Mutant ("dirremove-mark-early", "ext2fs/dir.c",
            """  if (ds->preventry == 0)
    ds->entry->inode = htole32 (0);
  else
    {
      assert_backtrace ((vm_address_t) ds->entry - (vm_address_t) ds->preventry
			== le16toh (ds->preventry->rec_len));
      ds->preventry->rec_len = htole16( le16toh (ds->preventry->rec_len) + ds->entry->rec_len);
    }

  dp->dn_set_mtime = 1;
  diskfs_node_disknode (dp)->info.i_flags &= ~EXT2_BTREE_FL;

  err = dirblock_mark_dirty (blk, ds->mapbuf + ds->idx * DIRBLKSIZ);
  munmap ((caddr_t) ds->mapbuf, ds->mapextent);
  if (err)
    return err;

  /* If we are keeping count of this block, then keep the count up
     to date. */
  if (diskfs_node_disknode (dp)->dirents
      && diskfs_node_disknode (dp)->dirents[ds->idx] != -1)
    diskfs_node_disknode (dp)->dirents[ds->idx]--;""",
            """  err = dirblock_mark_dirty (blk, ds->mapbuf + ds->idx * DIRBLKSIZ);
  if (ds->preventry == 0)
    ds->entry->inode = htole32 (0);
  else
    {
      assert_backtrace ((vm_address_t) ds->entry - (vm_address_t) ds->preventry
			== le16toh (ds->preventry->rec_len));
      ds->preventry->rec_len = htole16( le16toh (ds->preventry->rec_len) + ds->entry->rec_len);
    }

  dp->dn_set_mtime = 1;
  diskfs_node_disknode (dp)->info.i_flags &= ~EXT2_BTREE_FL;

  munmap ((caddr_t) ds->mapbuf, ds->mapextent);
  if (err)
    return err;

  /* If we are keeping count of this block, then keep the count up
     to date. */
  if (diskfs_node_disknode (dp)->dirents
      && diskfs_node_disknode (dp)->dirents[ds->idx] != -1)
    diskfs_node_disknode (dp)->dirents[ds->idx]--;""",
            "t_fdir_ops",
            why="a removal's block goes to the journal before the entry is gone"),
    Mutant ("extend-unreserved", "ext2fs/dir.c",
            """      err = dirblock_get_write_access (dp, ds->idx, 1, &blk);
      if (err)
	{
	  munmap ((caddr_t) ds->mapbuf, ds->mapextent);
	  return err;
	}""",
            """      pthread_rwlock_wrlock (&diskfs_node_disknode (dp)->alloc_lock);
      err = ext2_getblk (dp, ds->idx, 1, &blk);
      pthread_rwlock_unlock (&diskfs_node_disknode (dp)->alloc_lock);
      if (err)
	{
	  munmap ((caddr_t) ds->mapbuf, ds->mapextent);
	  return err;
	}""",
            "t_fdir_ops",
            why="EXTEND writes its new block without reserving it"),
]


def overlay (dest, path):
    """A copy of the Hurd tree in DEST, links but for the directory of
    PATH, which has links but for PATH: returned, to be written."""
    shutil.rmtree (dest, ignore_errors=True)
    os.makedirs (dest)
    top = path.split ("/")[0]
    for entry in os.listdir (HURD):
        if entry != top:
            os.symlink (os.path.join (HURD, entry), os.path.join (dest, entry))
    os.makedirs (os.path.join (dest, top))
    for entry in os.listdir (os.path.join (HURD, top)):
        if entry != os.path.basename (path):
            os.symlink (os.path.join (HURD, top, entry),
                        os.path.join (dest, top, entry))
    return os.path.join (dest, path)


def snippet_count (m):
    with open (os.path.join (HURD, m.path)) as f:
        return f.read ().count (m.old)


def run_mutant (m, a, jobs):
    base = os.path.join (common.JTEST, a.out, m.name)
    target = overlay (os.path.join (base, "hurd"), m.path)
    with open (os.path.join (HURD, m.path)) as f:
        text = f.read ()
    with open (target, "w") as f:
        f.write (text.replace (m.old, m.new))
    build = os.path.join (base, "build")
    make = ["make", "-s", "-C", common.JTEST, "B=" + build,
            "HURD=" + os.path.join (base, "hurd"), os.path.join (build, m.test),
            os.path.join (build, "base.img"), os.path.join (build, "balloc.img"),
            os.path.join (build, "big.img"), os.path.join (build, "full.img")]
    if m.san:
        make.append ("SAN=" + m.san)
    if subprocess.run (make, stdout=subprocess.DEVNULL,
                       stderr=subprocess.DEVNULL).returncode:
        return m, None, "the build failed"
    seeds = [1] if m.test in common.DETERMINISTIC else range (1, a.seeds + 1)
    env = dict (m.env, JT_CRASH_POINTS=str (a.points))
    caught, line = 0, ""
    for seed in seeds:
        cwd = os.path.join (base, "seed-%d" % seed)
        common.workdir (cwd, build)
        status, _ = common.run (os.path.join (build, m.test), m.test, seed,
                                cwd, m.args, env, a.timeout, jobs)
        if status != 0:
            caught += 1
            line = line or common.failure_line (cwd) or (
                "timeout" if status is None else "exit %d" % status)
    if caught >= min (m.min_caught, len (seeds)):
        shutil.rmtree (base, ignore_errors=True)
    return m, (caught, len (seeds)), line


def main ():
    ap = argparse.ArgumentParser (description="Run the known mutants.")
    ap.add_argument ("--seeds", type=int, default=3)
    ap.add_argument ("--parallel", type=int, default=4)
    ap.add_argument ("--points", type=int, default=100)
    ap.add_argument ("--timeout", type=int, default=1800)
    ap.add_argument ("--out", default="mutants")
    ap.add_argument ("--check", action="store_true")
    ap.add_argument ("--list", action="store_true")
    ap.add_argument ("--test", help="run every mutant chosen against this "
                     "test instead of its own, with no arguments; a miss "
                     "is then only information")
    ap.add_argument ("names", nargs="*")
    a = ap.parse_args ()

    chosen = [m for m in MUTANTS if not a.names or m.name in a.names]
    if a.test:
        for m in chosen:
            m.test, m.args, m.env, m.min_caught = a.test, [], dict (m.env), 1
    unknown = set (a.names) - {m.name for m in MUTANTS}
    if unknown:
        sys.exit ("no such mutant: " + ", ".join (sorted (unknown)))
    if a.list:
        for m in chosen:
            print ("%-24s %-14s %s" % (m.name, m.test, m.why))
        return

    stale = [m for m in chosen if snippet_count (m) != 1]
    for m in stale:
        print ("%-24s STALE: its snippet is in %s %d times, not once"
               % (m.name, m.path, snippet_count (m)))
    if a.check:
        if not stale:
            print ("%d mutants, none stale" % len (chosen))
        sys.exit (1 if stale else 0)

    jobs = max (1, (os.cpu_count () or 1) // a.parallel)
    bad = len (stale)
    with concurrent.futures.ThreadPoolExecutor (a.parallel) as pool:
        futures = [pool.submit (run_mutant, m, a, jobs)
                   for m in chosen if m not in stale]
        for f in concurrent.futures.as_completed (futures):
            m, result, line = f.result ()
            if result is None:
                print ("%-24s BUILD FAILED" % m.name, flush=True)
                bad += 1
                continue
            caught, runs = result
            ok = caught >= min (m.min_caught, runs)
            bad += not ok
            print ("%-24s %-14s caught %d/%d%s  %s"
                   % (m.name, m.test, caught, runs, "" if ok else "  MISSED",
                      line if caught else m.why), flush=True)
    print ("\n%d mutants: %d stale, missed or not built" % (len (chosen), bad))
    sys.exit (1 if bad else 0)


if __name__ == "__main__":
    main ()
