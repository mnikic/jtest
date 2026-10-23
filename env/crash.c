/* Checking the images a crash at each point of the write log leaves.  */

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include "jtest.h"

static const char *
tmp_dir (void)
{
  const char *dir = getenv ("JT_TMP");
  struct stat st;

  if (dir)
    return dir;
  if (!stat ("/dev/shm", &st) && S_ISDIR (st.st_mode))
    return "/dev/shm";
  return "build";
}

/* Check the crash after the first N log entries, the last one torn with
   TORN, in IMG.  Returns NULL if it is right, or what is wrong.  */
static const char *
check_one (size_t n, int torn, const char *img, const char *fsck_out,
	   jt_crash_check_fn check, void *arg)
{
  static char msg[512];
  int status;

  jt_build_image (n, torn, img);
  /* Only replay the journal: e2fsck -fy would also repair what it
     finds, and the check after it would not see it.  */
  status = jt_run ("e2fsck -E journal_only -y %s >%s 2>&1", img, fsck_out);
  if (status & ~1)
    {
      snprintf (msg, sizeof msg, "e2fsck -E journal_only exit %d", status);
      return msg;
    }
  status = jt_run ("e2fsck -fn %s >%s 2>&1", img, fsck_out);
  if (status)
    {
      snprintf (msg, sizeof msg, "e2fsck -fn exit %d after the replay",
		status);
      return msg;
    }
  return check (n, torn, img, arg);
}

/* Worker W of JOBS: check every JOBS-th point.  Exits 0 if all are right,
   1 after reporting the first that is not.  */
static void
worker (const char *name, int w, int jobs, const size_t *points,
	size_t npoints, jt_crash_check_fn check, void *arg)
{
  char img[256], fsck_out[sizeof img + 8];

  snprintf (img, sizeof img, "%s/jtest-%s-%d-%d.img", tmp_dir (), name,
	    (int) getppid (), w);
  snprintf (fsck_out, sizeof fsck_out, "%s.fsck", img);

  for (size_t i = w; i < npoints; i += jobs)
    {
      size_t n = points[i] >> 1;
      int torn = points[i] & 1;
      const char *err = check_one (n, torn, img, fsck_out, check, arg);

      if (err)
	{
	  char keep[256];

	  /* Keep the image as the crash left it, before e2fsck.  */
	  snprintf (keep, sizeof keep, "build/%s-crash-%zu%s.img", name, n,
		    torn ? "-torn" : "");
	  jt_build_image (n, torn, keep);
	  jt_run ("cp %s %s.replayed && e2fsck -E journal_only -y %s.replayed "
		  ">%s.fsck 2>&1 && e2fsck -fn %s.replayed >>%s.fsck 2>&1",
		  keep, keep, keep, keep, keep, keep);
	  strcpy (keep + strlen (keep) - 4, ".log");
	  FILE *f = fopen (keep, "w");
	  if (f)
	    {
	      jt_dump_log (f, 0, jt_log_count ());
	      fclose (f);
	    }
	  keep[strlen (keep) - 4] = 0;
	  /* One write, so that reports of several workers do not mix.  */
	  char report[2048];
	  int len = snprintf (report, sizeof report,
			      "%s: crash after write %zu%s: %s\n"
			      "  crashed image %s.img, replayed by e2fsck in "
			      "%s.img.replayed\n  (its output in %s.img.fsck), "
			      "the write log in %s.log\n",
			      name, n, torn ? " (torn)" : "", err, keep, keep,
			      keep, keep);
	  if (write (2, report, len) < 0)
	    _exit (1);
	  unlink (img);
	  _exit (1);
	}
    }
  unlink (img);
  unlink (fsck_out);
  _exit (0);
}

size_t
jt_crash_check (const char *name, jt_crash_check_fn check, void *arg)
{
  size_t n_log = jt_log_count ();

  if (getenv ("JT_NO_CRASH"))
    return 0;
  const char *env = getenv ("JT_CRASH_POINTS");
  size_t want = env ? strtoul (env, NULL, 0) : 300;
  size_t step = want && n_log > want ? n_log / want : 1;
  int jobs = getenv ("JT_JOBS") ? atoi (getenv ("JT_JOBS"))
    : (int) sysconf (_SC_NPROCESSORS_ONLN);
  size_t *points = malloc ((2 * (n_log / step) + 4) * sizeof *points);
  size_t npoints = 0;
  pid_t *pids;
  int failed = 0;

  /* Every STEP-th prefix, and the whole log.  A point is N << 1 | TORN.  */
  for (size_t n = 0;; n += step)
    {
      if (n > n_log)
	n = n_log;
      points[npoints++] = n << 1;
      if (n > 0 && jt_log_entry (n - 1)->data)
	points[npoints++] = n << 1 | 1;
      if (n == n_log)
	break;
    }

  if (jobs < 1)
    jobs = 1;
  pids = calloc (jobs, sizeof *pids);
  fflush (NULL);
  for (int w = 0; w < jobs; w++)
    {
      pids[w] = fork ();
      if (pids[w] < 0)
	{
	  perror ("fork");
	  exit (2);
	}
      if (pids[w] == 0)
	worker (name, w, jobs, points, npoints, check, arg);
    }

  for (int w = 0; w < jobs; w++)
    {
      int status;
      while (waitpid (pids[w], &status, 0) < 0 && errno == EINTR)
	;
      if (!WIFEXITED (status) || WEXITSTATUS (status))
	{
	  /* One report is enough.  */
	  if (!failed)
	    for (int o = w + 1; o < jobs; o++)
	      kill (pids[o], SIGTERM);
	  failed = 1;
	}
    }
  free (pids);
  free (points);
  if (failed)
    exit (1);
  return npoints;
}
