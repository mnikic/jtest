/* What libdiskfs's dir-lookup.c and file-set-trans.c need around them:
   peropens and protids, and no translators.  A protid made for a reply
   gets a port name that jt_protid_of_port turns back into it.  */

#include <stdlib.h>

#include "jtest.h"
#include "../shim/hurd/fsys.h"

/* No translators run, and only the owner calls.  */
int diskfs_shortcut_symlink = 1;
int diskfs_shortcut_chrdev = 1;
int diskfs_shortcut_blkdev = 1;
int diskfs_shortcut_fifo = 1;
int diskfs_shortcut_ifsock = 1;
int diskfs_maxsymlinks = 8;

error_t
fshelp_isowner (io_statbuf_t *st, struct iouser *user)
{
  (void) st; (void) user;
  return 0;
}

error_t
diskfs_validate_rdev_change (struct node *np, dev_t rdev)
{
  (void) np; (void) rdev;
  return 0;
}

int
fshelp_translated (struct transbox *box)
{
  (void) box;
  return 0;
}

error_t
fshelp_fetch_control (struct transbox *box, mach_port_t *control)
{
  (void) box;
  *control = MACH_PORT_NULL;
  return 0;
}

error_t
fshelp_set_active (struct transbox *box, mach_port_t active, int excl)
{
  (void) box; (void) active; (void) excl;
  return 0;
}

error_t
fshelp_set_active_translator (struct port_info *pi, const char *name,
			      const struct transbox *transbox)
{
  (void) pi; (void) name; (void) transbox;
  return 0;
}

kern_return_t
fsys_goaway (mach_port_t fsys, int flags)
{
  (void) fsys; (void) flags;
  return 0;
}

error_t
fshelp_fetch_root (struct transbox *transbox, void *cookie, file_t dotdot,
		   struct iouser *user, int flags,
		   fshelp_fetch_root_callback1_t callback1,
		   fshelp_fetch_root_callback2_t callback2,
		   retry_type *retry, char *retryname, mach_port_t *root)
{
  (void) transbox; (void) cookie; (void) dotdot; (void) user; (void) flags;
  (void) callback1; (void) callback2; (void) retry; (void) retryname;
  (void) root;
  return EOPNOTSUPP;
}

error_t
fshelp_short_circuited_callback1 (void *cookie1, void *cookie2, uid_t *uid,
				  gid_t *gid, char **argz,
				  mach_msg_type_number_t *argz_len)
{
  (void) cookie1; (void) cookie2; (void) uid; (void) gid; (void) argz;
  (void) argz_len;
  return EOPNOTSUPP;
}

static error_t
no_callback1 (void *cookie1, void *cookie2, uid_t *uid, gid_t *gid,
	      char **argz, mach_msg_type_number_t *argz_len)
{
  return fshelp_short_circuited_callback1 (cookie1, cookie2, uid, gid, argz,
					   argz_len);
}

static error_t
no_callback2 (void *cookie1, void *cookie2, int flags,
	      mach_port_t *underlying, mach_msg_type_name_t *underlying_type)
{
  (void) cookie1; (void) cookie2; (void) flags; (void) underlying;
  (void) underlying_type;
  return EOPNOTSUPP;
}

fshelp_fetch_root_callback1_t _diskfs_translator_callback1 = no_callback1;
fshelp_fetch_root_callback2_t _diskfs_translator_callback2 = no_callback2;

error_t
fshelp_rlock_tweak (struct rlock_box *box, pthread_mutex_t *mutex,
		    struct rlock_peropen *po, int open_mode, loff_t file_size,
		    loff_t curpointer, int cmd, struct flock64 *lock,
		    mach_port_t rendezvous)
{
  (void) box; (void) mutex; (void) po; (void) open_mode; (void) file_size;
  (void) curpointer; (void) cmd; (void) lock; (void) rendezvous;
  return 0;
}

error_t
iohelp_create_empty_iouser (struct iouser **user)
{
  *user = calloc (1, sizeof **user);
  return *user ? 0 : ENOMEM;
}

void
iohelp_free_iouser (struct iouser *user)
{
  free (user);
}

/* Peropens and protids.  */

error_t
diskfs_make_peropen (struct node *np, int flags, struct peropen *context,
		     struct peropen **ppo)
{
  (void) context;
  struct peropen *po = calloc (1, sizeof *po);
  if (!po)
    return ENOMEM;
  po->np = np;
  po->openstat = flags;
  po->refcnt = 1;
  diskfs_nref (np);
  *ppo = po;
  return 0;
}

void
diskfs_release_peropen (struct peropen *po)
{
  diskfs_nrele (po->np);
  free (po->path);
  free (po);
}

#define JT_PORTS 256
static struct protid *ports[JT_PORTS];
static pthread_mutex_t ports_lock = PTHREAD_MUTEX_INITIALIZER;

error_t
diskfs_create_protid (struct peropen *po, struct iouser *user,
		      struct protid **cred)
{
  struct protid *pi = calloc (1, sizeof *pi);
  if (!pi)
    return ENOMEM;
  pi->po = po;
  pi->user = user;
  *cred = pi;
  return 0;
}

/* A name for PI's port: its slot in PORTS, plus one.  */
mach_port_t
ports_get_right (void *port)
{
  pthread_mutex_lock (&ports_lock);
  int i;
  for (i = 0; i < JT_PORTS && ports[i]; i++)
    ;
  if (i == JT_PORTS)
    _ext2_panic (__func__, "more than %d protids", JT_PORTS);
  ports[i] = port;
  pthread_mutex_unlock (&ports_lock);
  return i + 1;
}

mach_port_t
ports_get_send_right (void *port)
{
  return ports_get_right (port);
}

struct protid *
jt_protid_of_port (mach_port_t port)
{
  pthread_mutex_lock (&ports_lock);
  struct protid *pi = port > 0 && port <= JT_PORTS ? ports[port - 1] : NULL;
  pthread_mutex_unlock (&ports_lock);
  return pi;
}

void
jt_protid_release (mach_port_t port)
{
  pthread_mutex_lock (&ports_lock);
  struct protid *pi = port > 0 && port <= JT_PORTS ? ports[port - 1] : NULL;
  if (pi)
    ports[port - 1] = NULL;
  pthread_mutex_unlock (&ports_lock);
  if (pi)
    {
      diskfs_release_peropen (pi->po);
      free (pi);
    }
}
