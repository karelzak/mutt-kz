/*
 * Copyright (C) 1997 Alain Penders <Alain@Finale-Dev.com>
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

#if HAVE_CONFIG_H
# include "config.h"
#endif

#include "mutt.h"

#ifdef USE_COMPRESSED

#include "mx.h"
#include "mailbox.h"
#include "mutt_curses.h"

#include <errno.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

typedef struct
{
  const char *close;	/* close-hook  command */
  const char *open;	/* open-hook   command */
  const char *append;	/* append-hook command */
  off_t size;		/* size of real folder */
} COMPRESS_INFO;


/*
 * ctx - context to lock
 * excl - exclusive lock?
 * retry - should retry if unable to lock?
 */
int mbox_lock_compressed (CONTEXT *ctx, FILE *fp, int excl, int retry)
{
  int r;

  if ((r = mx_lock_file (ctx->realpath, fileno (fp), excl, 1, retry)) == 0)
    ctx->locked = 1;
  else if (retry && !excl)
  {
    ctx->readonly = 1;
    return 0;
  }

  return (r);
}

void mbox_unlock_compressed (CONTEXT *ctx, FILE *fp)
{
  if (ctx->locked)
  {
    fflush (fp);

    mx_unlock_file (ctx->realpath, fileno (fp), 1);
    ctx->locked = 0;
  }
}

static int is_new (const char *path)
{
  return (access (path, W_OK) != 0 && errno == ENOENT) ? 1 : 0;
}

static const char* find_compress_hook (int type, const char *path)
{
  const char* c = mutt_find_hook (type, path);
  return (!c || !*c) ? NULL : c;
}

int mutt_can_read_compressed (const char *path)
{
  return find_compress_hook (M_OPENHOOK, path) ? 1 : 0;
}

/*
 * if the file is new, we really do not append, but create, and so use
 * close-hook, and not append-hook
 */
static const char* get_append_command (const char *path, const CONTEXT* ctx)
{
  COMPRESS_INFO *ci = (COMPRESS_INFO *) ctx->compressinfo;
  return (is_new (path)) ? ci->close : ci->append;
}

int mutt_can_append_compressed (const char *path)
{
  int magic;

  if (is_new (path))
  {
    char *dir_path = safe_strdup(path);
    char *aux = strrchr(dir_path, '/');
    int dir_valid = 1;
    if (aux)
    {
      *aux='\0';
      if (access(dir_path, W_OK|X_OK))
        dir_valid = 0;
    }
    safe_free((void**)&dir_path);
    return dir_valid && (find_compress_hook (M_CLOSEHOOK, path) ? 1 : 0);
  }

  magic = mx_get_magic (path);

  if (magic != 0 && magic != M_COMPRESSED)
    return 0;

  return (find_compress_hook (M_APPENDHOOK, path)
	  || (find_compress_hook (M_OPENHOOK, path)
	      && find_compress_hook (M_CLOSEHOOK, path))) ? 1 : 0;
}

/* open a compressed mailbox */
static COMPRESS_INFO *set_compress_info (CONTEXT *ctx)
{
  COMPRESS_INFO *ci;

  /* Now lets uncompress this thing */
  ci = safe_malloc (sizeof (COMPRESS_INFO));
  ctx->compressinfo = (void*) ci;
  ci->append = find_compress_hook (M_APPENDHOOK, ctx->path);
  ci->open = find_compress_hook (M_OPENHOOK, ctx->path);
  ci->close = find_compress_hook (M_CLOSEHOOK, ctx->path);
  return ci;
}

static void set_path (CONTEXT* ctx)
{
  char tmppath[_POSIX_PATH_MAX];

  /* Setup the right paths */
  ctx->realpath = ctx->path;

  /* Uncompress to /tmp */
  mutt_mktemp (tmppath, sizeof(tmppath));
  ctx->path = safe_malloc (strlen (tmppath) + 1);
  strcpy (ctx->path, tmppath);
}

static int get_size (const char* path)
{
  struct stat sb;
  if (stat (path, &sb) != 0)
    return 0;
  return (sb.st_size);
}

static void store_size (CONTEXT* ctx)
{
  COMPRESS_INFO *ci = (COMPRESS_INFO *) ctx->compressinfo;
  ci->size = get_size (ctx->realpath);
}

static const char *
compresshook_format_str (char *dest, size_t destlen, size_t col, char op,
			 const char *src, const char *fmt,
			 const char *ifstring, const char *elsestring,
			 unsigned long data, format_flag flags)
{
  char tmp[SHORT_STRING];

  CONTEXT *ctx = (CONTEXT *) data;
  switch (op)
  {
  case 'f':
    snprintf (tmp, sizeof (tmp), "%%%ss", fmt);
    snprintf (dest, destlen, tmp, ctx->realpath);
    break;
  case 't':
    snprintf (tmp, sizeof (tmp), "%%%ss", fmt);
    snprintf (dest, destlen, tmp, ctx->path);
    break;
  }
  return (src);
}

/*
 * check that the command has both %f and %t
 * 0 means OK, -1 means error
 */
int mutt_test_compress_command (const char* cmd)
{
  return (strstr (cmd, "%f") && strstr (cmd, "%t")) ? 0 : -1;
}

static char *get_compression_cmd (const char* cmd, const CONTEXT* ctx)
{
  char expanded[_POSIX_PATH_MAX];
  mutt_FormatString (expanded, sizeof (expanded), 0, cmd,
		     compresshook_format_str, (unsigned long) ctx, 0);
  return safe_strdup (expanded);
}

int mutt_check_mailbox_compressed (CONTEXT* ctx)
{
  COMPRESS_INFO *ci = (COMPRESS_INFO *) ctx->compressinfo;
  if (ci->size != get_size (ctx->realpath))
  {
    FREE (&ctx->compressinfo);
    FREE (&ctx->realpath);
    mutt_error _("Mailbox was corrupted!");
    return (-1);
  }
  return (0);
}

int mutt_open_read_compressed (CONTEXT *ctx)
{
  char *cmd;
  FILE *fp;
  int rc;

  COMPRESS_INFO *ci = set_compress_info (ctx);
  if (!ci->open) {
    ctx->magic = 0;
    FREE (&ctx->compressinfo);
    return (-1);
  }
  if (!ci->close || access (ctx->path, W_OK) != 0)
    ctx->readonly = 1;

  set_path (ctx);
  store_size (ctx);

  if (!ctx->quiet)
    mutt_message (_("Decompressing %s..."), ctx->realpath);

  cmd = get_compression_cmd (ci->open, ctx);
  if (cmd == NULL)
    return (-1);
  dprint (2, (debugfile, "DecompressCmd: '%s'\n", cmd));

  if ((fp = fopen (ctx->realpath, "r")) == NULL)
  {
    mutt_perror (ctx->realpath);
    FREE (&cmd);
    return (-1);
  }
  mutt_block_signals ();
  if (mbox_lock_compressed (ctx, fp, 0, 1) == -1)
  {
    fclose (fp);
    mutt_unblock_signals ();
    mutt_error _("Unable to lock mailbox!");
    FREE (&cmd);
    return (-1);
  }

  endwin ();
  fflush (stdout);
  fprintf (stderr, _("Decompressing %s...\n"),ctx->realpath);
  rc = mutt_system (cmd);
  mbox_unlock_compressed (ctx, fp);
  mutt_unblock_signals ();
  fclose (fp);

  if (rc)
  {
    mutt_any_key_to_continue (NULL);
    ctx->magic = 0;
    FREE (&ctx->compressinfo);
    mutt_error (_("Error executing: %s : unable to open the mailbox!\n"), cmd);
  }
  FREE (&cmd);
  if (rc)
    return (-1);

  if (mutt_check_mailbox_compressed (ctx))
    return (-1);

  ctx->magic = mx_get_magic (ctx->path);

  return (0);
}

void restore_path (CONTEXT* ctx)
{
  FREE (&ctx->path);
  ctx->path = ctx->realpath;
}

/* remove the temporary mailbox */
void remove_file (CONTEXT* ctx)
{
  if (ctx->magic == M_MBOX || ctx->magic == M_MMDF)
    remove (ctx->path);
}

int mutt_open_append_compressed (CONTEXT *ctx)
{
  FILE *fh;
  COMPRESS_INFO *ci = set_compress_info (ctx);

  if (!get_append_command (ctx->path, ctx))
  {
    if (ci->open && ci->close)
      return (mutt_open_read_compressed (ctx));

    ctx->magic = 0;
    FREE (&ctx->compressinfo);
    return (-1);
  }

  set_path (ctx);

  if (!is_new (ctx->realpath))
      if ((fh = fopen (ctx->path, "w")))
	fclose (fh);
  /* No error checking - the parent function will catch it */

  return (0);
}

/* close a compressed mailbox */
void mutt_fast_close_compressed (CONTEXT *ctx)
{
  dprint (2, (debugfile, "mutt_fast_close_compressed called on '%s'\n",
	      ctx->path));

  if (ctx->compressinfo)
  {
    if (ctx->fp)
      fclose (ctx->fp);
    ctx->fp = NULL;
    /* if the folder was removed, remove the gzipped folder too */
    if ((ctx->magic > 0)
	&& (access (ctx->path, F_OK) != 0)
	&& ! option (OPTSAVEEMPTY))
      remove (ctx->realpath);
    else
      remove_file (ctx);

    restore_path (ctx);
    FREE (&ctx->compressinfo);
  }
}

/* return 0 on success, -1 on failure */
int mutt_sync_compressed (CONTEXT* ctx)
{
  char *cmd;
  int rc = 0;
  FILE *fp;
  COMPRESS_INFO *ci = (COMPRESS_INFO *) ctx->compressinfo;

  if (!ctx->quiet)
    mutt_message (_("Compressing %s..."), ctx->realpath);

  cmd = get_compression_cmd (ci->close, ctx);
  if (cmd == NULL)
    return (-1);

  if ((fp = fopen (ctx->realpath, "a")) == NULL)
  {
    mutt_perror (ctx->realpath);
    FREE (&cmd);
    return (-1);
  }
  mutt_block_signals ();
  if (mbox_lock_compressed (ctx, fp, 1, 1) == -1)
  {
    fclose (fp);
    mutt_unblock_signals ();
    mutt_error _("Unable to lock mailbox!");
    store_size (ctx);
    FREE (&cmd);
    return (-1);
  }

  dprint (2, (debugfile, "CompressCommand: '%s'\n", cmd));

  endwin ();
  fflush (stdout);
  fprintf (stderr, _("Compressing %s...\n"), ctx->realpath);
  if (mutt_system (cmd))
  {
    mutt_any_key_to_continue (NULL);
    mutt_error (_("%s: Error compressing mailbox! Original mailbox deleted, uncompressed one kept!\n"), ctx->path);
    rc = -1;
  }

  mbox_unlock_compressed (ctx, fp);
  mutt_unblock_signals ();
  fclose (fp);

  FREE (&cmd);

  store_size (ctx);

  return (rc);
}

int mutt_slow_close_compressed (CONTEXT *ctx)
{
  FILE *fp;
  const char *append;
  char *cmd;
  COMPRESS_INFO *ci = (COMPRESS_INFO *) ctx->compressinfo;

  dprint (2, (debugfile, "mutt_slow_close_compressed called on '%s'\n",
	      ctx->path));

  if (! (ctx->append
	 && ((append = get_append_command (ctx->realpath, ctx))
	     || (append = ci->close))))
  {
    /* if we can not or should not append, we only have to remove the */
    /* compressed info, because sync was already called               */
    mutt_fast_close_compressed (ctx);
    return (0);
  }

  if (ctx->fp)
    fclose (ctx->fp);
  ctx->fp = NULL;

  if (!ctx->quiet)
  {
    if (append == ci->close)
      mutt_message (_("Compressing %s..."), ctx->realpath);
    else
      mutt_message (_("Compressed-appending to %s..."), ctx->realpath);
  }

  cmd = get_compression_cmd (append, ctx);
  if (cmd == NULL)
    return (-1);

  if ((fp = fopen (ctx->realpath, "a")) == NULL)
  {
    mutt_perror (ctx->realpath);
    FREE (&cmd);
    return (-1);
  }
  mutt_block_signals ();
  if (mbox_lock_compressed (ctx, fp, 1, 1) == -1)
  {
    fclose (fp);
    mutt_unblock_signals ();
    mutt_error _("Unable to lock mailbox!");
    FREE (&cmd);
    return (-1);
  }

  dprint (2, (debugfile, "CompressCmd: '%s'\n", cmd));

  endwin ();
  fflush (stdout);

  if (append == ci->close)
    fprintf (stderr, _("Compressing %s...\n"), ctx->realpath);
  else
    fprintf (stderr, _("Compressed-appending to %s...\n"), ctx->realpath);

  if (mutt_system (cmd))
  {
    mutt_any_key_to_continue (NULL);
    mutt_error (_(" %s: Error compressing mailbox!  Uncompressed one kept!\n"),
		ctx->path);
    FREE (&cmd);
    mbox_unlock_compressed (ctx, fp);
    mutt_unblock_signals ();
    fclose (fp);
    return (-1);
  }

  mbox_unlock_compressed (ctx, fp);
  mutt_unblock_signals ();
  fclose (fp);
  remove_file (ctx);
  restore_path (ctx);
  FREE (&cmd);
  FREE (&ctx->compressinfo);

  return (0);
}

#endif /* USE_COMPRESSED */
