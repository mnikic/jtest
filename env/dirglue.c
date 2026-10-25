/* What libdiskfs's directory operations (dir-rename.c, lookup.c and the
   rest the t_fdir* tests link) need beyond the harness's other glue.  */

#include "jtest.h"

/* ext2fs.c's.  */
int diskfs_link_max = EXT2_LINK_MAX;
int diskfs_name_max = EXT2_NAME_LEN;

/* libdiskfs's; the tests set it to the root.  */
struct node *diskfs_root_node;

/* No one watches directories in the harness.  */
void
diskfs_notice_dirchange (struct node *dp, enum dir_changed_type type,
			 const char *name)
{
  (void) dp; (void) type; (void) name;
}

/* One user, allowed everything.  */
error_t
fshelp_access (io_statbuf_t *st, int op, struct iouser *user)
{
  (void) st; (void) op; (void) user;
  return 0;
}

error_t
fshelp_checkdirmod (io_statbuf_t *dir, io_statbuf_t *st,
		    struct iouser *user)
{
  (void) dir; (void) st; (void) user;
  return 0;
}
