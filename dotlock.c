/*
 * Copyright (C) 1996-8 Michael R. Elkins <me@cs.hmc.edu>
 * Copyright (C) 1998 Thomas Roessler <roessler@guug.de>
 * 
 *     This program is free software; you can redistribute it and/or modify
 *     it under the terms of the GNU General Public License as published by
 *     the Free Software Foundation; either version 2 of the License, or
 *     (at your option) any later version.
 * 
 *     This program is distributed in the hope that it will be useful,
 *     but WITHOUT ANY WARRANTY; without even the implied warranty of
 *     MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *     GNU General Public License for more details.
 * 
 *     You should have received a copy of the GNU General Public License
 *     along with this program; if not, write to the Free Software
 *     Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 */ 


#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <unistd.h>
#include <dirent.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/utsname.h>
#include <errno.h>
#include <time.h>

#include "dotlock.h"
#include "config.h"
#include "reldate.h"

#define MAXLINKS 1024 /* maximum link depth */

#define strfcpy(A,B,C) strncpy(A,B,C), *(A+(C)-1)=0

#ifdef USE_SETGID
#ifdef HAVE_SETEGID
#define SETEGID setegid
#else
#define SETEGID setgid
#endif
#endif

#ifndef S_ISLNK
#define S_ISLNK(x) (((x) & S_IFMT) == S_IFLNK ? 1 : 0)
#endif

static short f_try = 0;
static short f_force = 0;
static short f_unlock = 0;
static short f_priv = 0;
static int   Retry = 5;
static char *Hostname;

#ifdef USE_SETGID
static gid_t UserGid;
static gid_t MailGid;
#endif

#ifndef HAVE_SNPRINTF
extern int snprintf(char *, size_t, const char *, ...);
#endif

static int dotlock_try(const char *);
static int dotlock_unlock(const char *);
static int dotlock_lock(const char *);
static int dotlock_deference_symlink(char *, size_t, const char *);
static int dotlock_allowed(const char *);

static void usage(const char *);
static void dotlock_expand_link(char *, const char *, const char *);
static void BEGIN_PRIVILEGED(void);
static void END_PRIVILEGED(void);

int main(int argc, char **argv)
{
  int i;
  char *p;
  const char *f;
  struct utsname utsname;

#ifdef USE_SETGID
  
  UserGid = getgid();
  MailGid = getegid();

  if(SETEGID(UserGid) != 0)
    return DL_EX_ERROR;

#endif
  
  uname(&utsname);
  if(!(Hostname = strdup(utsname.nodename)))
    return DL_EX_ERROR;
  if((p = strchr(Hostname, '.')))
    *p = '\0';
  
  while((i = getopt(argc, argv, "tfupr:")) != EOF)
  {
    switch(i)
    {
      case 't': f_try = 1; break;
      case 'f': f_force = 1; break;
      case 'u': f_unlock = 1; break;
      case 'p': f_priv = 1; break;
      case 'r': Retry = atoi(optarg); break;
      default: usage(argv[0]);
    }
  }

  if(optind == argc || f_try + f_force + f_unlock > 1 || Retry < 0)
    usage(argv[0]);
  
  f = argv[optind];

  if(f_try)
    return dotlock_try(f);
  else if(f_unlock)
    return dotlock_unlock(f);
  else /* lock */
    return dotlock_lock(f);
}






static void
BEGIN_PRIVILEGED(void)
{
#ifdef USE_SETGID
  if(f_priv)
  {
    if(SETEGID(MailGid) != 0)
    {
      /* perror("setegid"); */
      exit(DL_EX_ERROR);
    }
  }
#endif
}

static void
END_PRIVILEGED(void)
{
#ifdef USE_SETGID
  if(f_priv)
  {
    if(SETEGID(UserGid) != 0)
    {
      /* perror("setegid"); */
      exit(DL_EX_ERROR);
    }
  }
#endif
}


/* XXX - This check is not bullet-proof at all.  
 * 
 * The problem is that the user may give us a deeply
 * nested path to a file which has the same name as the
 * file he wants to lock, but different permissions, say, 
 * e.g. /tmp/lots/of/subdirs/var/spool/mail/root.
 * 
 * He may then try to replace /tmp/lots/of/subdirs by a
 * symbolic link to / after we have invoked access() to
 * check the file's permissions.  The lockfile we'd
 * create or remove would then actually be
 * /var/spool/mail/root.
 * 
 * To avoid this attack, we'd have to proceed as follows:
 * 
 * - First, follow symbolic links a la
 *   dotlock_deference_symlink().
 * 
 * - get the result's dirname.
 * 
 * - chdir to this directory.  If you can't, bail out.
 * 
 * - try to open the file in question, only using its
 *   basename.  If you can't, bail out.
 * 
 * - fstat that file and compare the result to a 
 *   subsequent lstat (only using the basename).  If 
 *   the comparison fails, bail out.
 * 
 * - finally generate (or remove) the lock file, only
 *   using the file's basename.
 * 
 * Anyway, I don't see any possibility to abuse the
 * current code for a persisting attack (except maybe
 * when using mode 1777 /var/spool/mail, but in this case
 * there are no privileges to abuse, and all the damage
 * can be done without privileges) - the victim can
 * always use dotlock -p -u to remove the "bad" lock
 * file.
 *
 * Feel free to implement the algorithm given above (or
 * convince me that there is an actual, nasty attack it
 * may avoid).
 * 
 * tlr, Jul 15 1998
 */

static int 
dotlock_allowed(const char *f)
{
  if(access(f, R_OK) || access(f, W_OK))
    return 0;
  else
    return 1;
}
 
static void 
usage(const char *av0)
{
  fprintf(stderr, "dotlock [Mutt %s (%s)]\n", VERSION, ReleaseDate);
  fprintf(stderr, "usage: %s [-t|-f|-u] [-p] [-r <retries>] file\n",
	  av0);

  fputs("\noptions:"
	"\n  -t\t\ttry"
	"\n  -f\t\tforce"
	"\n  -u\t\tunlock"
	"\n  -p\t\tprivileged"
#ifndef USE_SETGID
	" (ignored)"
#endif
	"\n  -r <retries>\tRetry locking"
	"\n", stderr);
  
  exit(DL_EX_ERROR);
}

static void 
dotlock_expand_link (char *newpath, const char *path, const char *link)
{
  const char *lb = NULL;
  size_t len;

  /* link is full path */
  if (*link == '/')
  {
    strfcpy (newpath, link, _POSIX_PATH_MAX);
    return;
  }

  if ((lb = strrchr (path, '/')) == NULL)
  {
    /* no path in link */
    strfcpy (newpath, link, _POSIX_PATH_MAX);
    return;
  }

  len = lb - path + 1;
  memcpy (newpath, path, len);
  strfcpy (newpath + len, link, _POSIX_PATH_MAX - len);
}


/* this is pretty similar to realpath(3) */

static int
dotlock_deference_symlink(char *d, size_t l, const char *path)
{
  struct stat sb;
  char realpath[_POSIX_PATH_MAX];
  const char *pathptr = path;
  int count = 0;
  
  while(count++ < MAXLINKS)
  {
    if(lstat(pathptr, &sb) == -1)
    {
      /* perror(pathptr); */
      return -1;
    }
    
    if(S_ISLNK (sb.st_mode))
    {
      char linkfile[_POSIX_PATH_MAX];
      char linkpath[_POSIX_PATH_MAX];
      int len;

      if((len = readlink(pathptr, linkfile, sizeof(linkfile))) == -1)
      {
	/* perror(pathptr); */
	return -1;
      }
      
      linkfile[len] = '\0';
      dotlock_expand_link(linkpath, pathptr, linkfile);
      strfcpy(realpath, linkpath, sizeof(realpath));
      pathptr = realpath;
    }
    else
      break;
  }

  strfcpy(d, pathptr, l);
  return 0;
}

static int
dotlock_lock(const char *f)
{
  char realpath[_POSIX_PATH_MAX];
  char lockfile[_POSIX_PATH_MAX];
  char nfslockfile[_POSIX_PATH_MAX];
  size_t prev_size = 0;
  int fd;
  int count = 0;
  struct stat sb;
  time_t t;
  
  if(dotlock_deference_symlink(realpath, sizeof(realpath), f) == -1)
    return DL_EX_ERROR;

  if(dotlock_allowed(realpath) == -1)
    return DL_EX_ERROR;
  
  snprintf(nfslockfile, sizeof(nfslockfile), "%s.%s.%d",
	   realpath, Hostname, (int) getpid());
  snprintf(lockfile, sizeof(lockfile), "%s.lock", realpath);

  
  BEGIN_PRIVILEGED();

  unlink(nfslockfile);

  while ((fd = open (nfslockfile, O_WRONLY | O_EXCL | O_CREAT, 0)) < 0)
  {
    END_PRIVILEGED();

  
    if (errno != EAGAIN)
    {
      /* perror ("cannot open NFS lock file"); */
      return DL_EX_ERROR;
    }

    
    BEGIN_PRIVILEGED();
  }

  END_PRIVILEGED();

  
  close(fd);
  
  while(1)
  {

    BEGIN_PRIVILEGED();
    link(nfslockfile, lockfile);
    END_PRIVILEGED();

    if(stat(nfslockfile, &sb) != 0)
    {
      /* perror("stat"); */
      return DL_EX_ERROR;
    }
    
    if(sb.st_nlink == 2)
      break;
    
    if(count == 0)
      prev_size = sb.st_size;
    
    if(prev_size == sb.st_size && ++count > Retry)
    {
      if(f_force)
      {
	BEGIN_PRIVILEGED();
	unlink(lockfile);
	END_PRIVILEGED();

	count = 0;
	continue;
      }
      else
      {
	BEGIN_PRIVILEGED();
	unlink(nfslockfile);
	END_PRIVILEGED();
	return DL_EX_EXIST;
      }
    }
    
    prev_size = sb.st_size;
    
    /* don't trust sleep(3) as it may be interrupted
     * by users sending signals. 
     */
    
    t = time(NULL);
    do {
      sleep(1);
    } while (time(NULL) == t);
  }
  
  BEGIN_PRIVILEGED();
  unlink(nfslockfile);
  END_PRIVILEGED();

  return DL_EX_OK;
}


static int
dotlock_unlock(const char *f)
{
  char lockfile[_POSIX_PATH_MAX];
  char realpath[_POSIX_PATH_MAX];
  int i;

  if(dotlock_deference_symlink(realpath, sizeof(realpath), f) == -1)
    return DL_EX_ERROR;

  if(dotlock_allowed(realpath) == -1)
    return DL_EX_ERROR;
  
  snprintf(lockfile, sizeof(lockfile), "%s.lock",
	   realpath);
  
  BEGIN_PRIVILEGED();
  i = unlink(lockfile);
  END_PRIVILEGED();
  
  if(i == -1)
    return DL_EX_ERROR;
  
  return DL_EX_OK;
}


static int
dotlock_try(const char *f)
{
  char realpath[_POSIX_PATH_MAX];
  char *p;
  struct stat sb;

  if(dotlock_deference_symlink(realpath, sizeof(realpath), f) == -1)
    return DL_EX_ERROR;

  if(dotlock_allowed(realpath) == -1)
    return DL_EX_IMPOSSIBLE;
  
  if((p = strrchr(realpath, '/')))
    *p = '\0';
  else
    strfcpy(realpath, ".", sizeof(realpath));
  
  if(access(realpath, W_OK) == 0)
    return DL_EX_OK;

  if(stat(realpath, &sb) == 0)
  {
    if((sb.st_mode & S_IWGRP) == S_IWGRP && sb.st_gid == MailGid)
      return DL_EX_NEED_PRIVS;
  }

  return DL_EX_IMPOSSIBLE;
}
