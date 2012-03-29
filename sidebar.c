/*
 * Copyright (C) ????-2004 Justin Hibbits <jrh29@po.cwru.edu>
 * Copyright (C) 2004 Thomer M. Gil <mutt@thomer.com>
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
 *     Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111, USA.
 */


#if HAVE_CONFIG_H
# include "config.h"
#endif

#include "mutt.h"
#include "mutt_menu.h"
#include "mutt_curses.h"
#include "sidebar.h"
#include "buffy.h"
#include "mx.h"
#include <libgen.h>
#include <limits.h>
#include "keymap.h"
#include <stdbool.h>

/*BUFFY *CurBuffy = 0;*/
static BUFFY *TopBuffy = 0;
static BUFFY *BottomBuffy = 0;
static int known_lines = 0;

enum {
	SB_SRC_NONE = 0,
	SB_SRC_VIRT,
	SB_SRC_INCOMING
};
static int sidebar_source = SB_SRC_NONE;

static BUFFY *get_incoming()
{
	switch (sidebar_source) {
	case SB_SRC_NONE:
		sidebar_source = SB_SRC_INCOMING;

		if (option (OPTVIRTSPOOLFILE) && VirtIncoming) {
			sidebar_source = SB_SRC_VIRT;
			return VirtIncoming;
		}
		break;
	case SB_SRC_VIRT:
		if (VirtIncoming) {
			return VirtIncoming;
		}
		break;
	case SB_SRC_INCOMING:
		break;
	}

	return Incoming;	/* default */
}

static int quick_log10(int n)
{
        char string[32];
        sprintf(string, "%d", n);
        return strlen(string);
}

static void calc_boundaries (int menu)
{
	BUFFY *tmp = get_incoming();

	if ( known_lines != LINES ) {
		TopBuffy = BottomBuffy = 0;
		known_lines = LINES;
	}
	for ( ; tmp->next != 0; tmp = tmp->next )
		tmp->next->prev = tmp;

	if ( TopBuffy == 0 && BottomBuffy == 0 )
		TopBuffy = get_incoming();
	if ( BottomBuffy == 0 ) {
		int count = LINES - 2 - (menu != MENU_PAGER || option(OPTSTATUSONTOP));
		BottomBuffy = TopBuffy;
		while ( --count && BottomBuffy->next )
			BottomBuffy = BottomBuffy->next;
	}
	else if ( TopBuffy == CurBuffy->next ) {
		int count = LINES - 2 - (menu != MENU_PAGER);
		BottomBuffy = CurBuffy;
		tmp = BottomBuffy;
		while ( --count && tmp->prev)
			tmp = tmp->prev;
		TopBuffy = tmp;
	}
	else if ( BottomBuffy == CurBuffy->prev ) {
		int count = LINES - 2 - (menu != MENU_PAGER);
		TopBuffy = CurBuffy;
		tmp = TopBuffy;
		while ( --count && tmp->next )
			tmp = tmp->next;
		BottomBuffy = tmp;
	}
}

static char sidebar_buffer[LINE_MAX];
static char *make_sidebar_entry(char *box, int size, int new, int flagged)
{
	char *entry;
	size_t i = 0;
	size_t delim_len = strlen(NONULL(SidebarDelim));
	size_t width = sizeof(sidebar_buffer) - 1;

	entry = &sidebar_buffer[0];
	if (SidebarWidth - delim_len + 1 > width || strlen(box) + 1 > width)
		return NONULL(SidebarDelim);

	entry[SidebarWidth - delim_len + 1] = 0;
	for (; i < SidebarWidth - delim_len + 1; entry[i++] = ' ' );
	i = strlen(box);
	strncpy( entry, box, i < (SidebarWidth - delim_len + 1) ? i : (SidebarWidth - delim_len + 1) );

        if (size == -1)
                snprintf(entry + SidebarWidth - delim_len - 3, width, "?");
        else if ( new ) {
          if (flagged > 0) {
              snprintf(
		        entry + SidebarWidth - delim_len - 5 - quick_log10(size) - quick_log10(new) - quick_log10(flagged),
		        width,
		        "% d(%d)[%d]", size, new, flagged);
          } else {
              snprintf(
                      entry + SidebarWidth - delim_len - 3 - quick_log10(size) - quick_log10(new),
                      width,
                      "% d(%d)", size, new);
          }
        } else if (flagged > 0) {
              snprintf(entry + SidebarWidth - delim_len - 3 - quick_log10(size) - quick_log10(flagged),
		       width,
		       "% d[%d]", size, flagged);
        } else {
              snprintf(entry + SidebarWidth - delim_len - 1 - quick_log10(size),
		       width,
                       "% d", size);
        }
	return entry;
}

void set_curbuffy(char buf[LONG_STRING])
{
  BUFFY* tmp = CurBuffy = get_incoming();

  if (!get_incoming())
    return;

  while(1) {
    if(!strcmp(tmp->path, buf)) {
      CurBuffy = tmp;
      break;
    }

    if(tmp->next)
      tmp = tmp->next;
    else
      break;
  }
}

int draw_sidebar(int menu) {

	char folder_buffer[LINE_MAX];
	int lines = (option(OPTHELP) ? 1 : 0) + (option(OPTSTATUSONTOP) ? 1 : 0);
	BUFFY *tmp;
#ifndef USE_SLANG_CURSES
        attr_t attrs;
#endif
        short delim_len = strlen(NONULL(SidebarDelim));
        short color_pair;
	char *maildir = NONULL(Maildir);

        static bool initialized /* = false*/;
        static int prev_show_value;
        static short saveSidebarWidth;

        /* initialize first time */
        if(!initialized) {
                prev_show_value = option(OPTSIDEBAR);
                saveSidebarWidth = SidebarWidth;
                if(!option(OPTSIDEBAR)) SidebarWidth = 0;
                initialized = true;
        }

        /* save or restore the value SidebarWidth */
        if(prev_show_value != option(OPTSIDEBAR)) {
                if(prev_show_value && !option(OPTSIDEBAR)) {
                        saveSidebarWidth = SidebarWidth;
                        SidebarWidth = 0;
                } else if(!prev_show_value && option(OPTSIDEBAR)) {
                        SidebarWidth = saveSidebarWidth;
                }
                prev_show_value = option(OPTSIDEBAR);
        }


/*	if ( SidebarWidth == 0 ) return 0; */
       if (SidebarWidth > 0 && option (OPTSIDEBAR)
           && delim_len >= SidebarWidth) {
         unset_option (OPTSIDEBAR);
         /* saveSidebarWidth = SidebarWidth; */
         if (saveSidebarWidth > delim_len) {
           SidebarWidth = saveSidebarWidth;
           mutt_error (_("Value for sidebar_delim is too long. Disabling sidebar."));
           sleep (2);
         } else {
           SidebarWidth = 0;
           mutt_error (_("Value for sidebar_delim is too long. Disabling sidebar. Please set your sidebar_width to a sane value."));
           sleep (4); /* the advise to set a sane value should be seen long enough */
         }
         saveSidebarWidth = 0;
         return (0);
       }

    if ( SidebarWidth == 0 || !option(OPTSIDEBAR)) {
      if (SidebarWidth > 0) {
        saveSidebarWidth = SidebarWidth;
        SidebarWidth = 0;
      }
      unset_option(OPTSIDEBAR);
      return 0;
    }

        /* get attributes for divider */
	SETCOLOR(MT_COLOR_SIDEBAR);
#ifndef USE_SLANG_CURSES
        attr_get(&attrs, &color_pair, 0);
#else
        color_pair = attr_get();
#endif
	/* SETCOLOR(MT_COLOR_SIDEBAR); */

	/* draw the divider */

	for ( ; lines < LINES-1-(menu != MENU_PAGER || option(OPTSTATUSONTOP)); lines++ ) {
		move(lines, SidebarWidth - delim_len);
		if (option (OPTASCIICHARS))
			addstr (NONULL (SidebarDelim));
		else if (!option (OPTASCIICHARS) && !strcmp (NONULL(SidebarDelim), "|"))
			addch (ACS_VLINE);
		else if ((Charset_is_utf8) && !strcmp (NONULL(SidebarDelim), "|"))
			addstr ("\342\224\202");
		else
			addstr (NONULL (SidebarDelim));
	}

	if ( get_incoming() == 0 )
		return 0;
	lines = option(OPTHELP) ? 1 : 0; /* go back to the top */
	lines += option(OPTSTATUSONTOP) ? 1 : 0;

	if ( known_lines != LINES || TopBuffy == 0 || BottomBuffy == 0 )
		calc_boundaries(menu);
	if ( CurBuffy == 0 )
		CurBuffy = get_incoming();

	tmp = TopBuffy;

	SETCOLOR(MT_COLOR_NORMAL);

	for ( ; tmp && lines < LINES-1 - (menu != MENU_PAGER || option(OPTSTATUSONTOP)); tmp = tmp->next ) {
		short maildir_is_prefix = 0;
		int sidebar_folder_depth;
		char *sidebar_folder_name;
		char *safe_path;
		char *base_name;

		if ( !tmp->path || *tmp->path == '\0' ) {
			move( lines, 0 );
			lines++;
			continue;
		}

		safe_path = strdup(tmp->path);
		if ( !safe_path ) {
			move( lines, 0 );
			lines++;
			continue;
		}

		base_name = basename(safe_path);
		free(safe_path);
		if ( !base_name ) {
			move( lines, 0 );
			lines++;
			continue;
		}

		if ( tmp == CurBuffy )
			SETCOLOR(MT_COLOR_INDICATOR);
		else if ( tmp->msg_unread > 0 )
			SETCOLOR(MT_COLOR_NEW);
		else if ( tmp->msg_flagged > 0 )
		        SETCOLOR(MT_COLOR_FLAGGED);
		else
			SETCOLOR(MT_COLOR_NORMAL);

		move( lines, 0 );
		if ( Context && !strcmp( tmp->path, Context->path ) ) {
			tmp->msg_unread = Context->unread;
			tmp->msgcount = Context->msgcount;
			tmp->msg_flagged = Context->flagged;
		}
		/* check whether Maildir is a prefix of the current folder's path */
		if ( (strlen(tmp->path) > strlen(maildir)) &&
			(strncmp(maildir, tmp->path, strlen(maildir)) == 0) )
			maildir_is_prefix = 1;
		/* calculate depth of current folder and generate its display name with indented spaces */
		sidebar_folder_name = base_name;
		sidebar_folder_depth = 0;
		if ( maildir_is_prefix ) {
			char *tmp_folder_name;
			int i;
			tmp_folder_name = tmp->path + strlen(maildir);
			for (i = 0; i < strlen(tmp->path) - strlen(maildir); i++) {
				if (tmp_folder_name[i] == '/') sidebar_folder_depth++;
			}
			if (sidebar_folder_depth > 0) {
				if (sidebar_folder_depth + strlen(base_name) + 1 > sizeof(folder_buffer)) {
					move( lines, 0 );
					lines++;
					continue;
				}
				sidebar_folder_name = &folder_buffer[0];
				for (i=0; i < sidebar_folder_depth; i++)
					sidebar_folder_name[i]=' ';
				sidebar_folder_name[i]=0;
				strncat(sidebar_folder_name, base_name, strlen(base_name) + sidebar_folder_depth);
			}
		}
#ifdef USE_NOTMUCH
		else if (tmp->magic == M_NOTMUCH)
			sidebar_folder_name = tmp->desc;
#endif
		printw( "%.*s", SidebarWidth - delim_len + 1,
			make_sidebar_entry(sidebar_folder_name, tmp->msgcount,
			tmp->msg_unread, tmp->msg_flagged));
		lines++;
	}
	SETCOLOR(MT_COLOR_NORMAL);
	for ( ; lines < LINES-1 - (menu != MENU_PAGER || option(OPTSTATUSONTOP)); lines++ ) {
		int i = 0;
		move( lines, 0 );
		for ( ; i < SidebarWidth - delim_len; i++ )
			addch(' ');
	}
	return 0;
}


void set_buffystats(CONTEXT* Context)
{
        BUFFY *tmp = get_incoming();
        while(tmp) {
                if(Context && !strcmp(tmp->path, Context->path)) {
			tmp->msg_unread = Context->unread;
			tmp->msgcount = Context->msgcount;
                        break;
                }
                tmp = tmp->next;
        }
}

void scroll_sidebar(int op, int menu)
{
        if(!SidebarWidth) return;
        if(!CurBuffy) return;

	switch (op) {
		case OP_SIDEBAR_NEXT:
			if ( CurBuffy->next == NULL ) return;
			CurBuffy = CurBuffy->next;
			break;
		case OP_SIDEBAR_PREV:
			if ( CurBuffy->prev == NULL ) return;
			CurBuffy = CurBuffy->prev;
			break;
		case OP_SIDEBAR_SCROLL_UP:
			CurBuffy = TopBuffy;
			if ( CurBuffy != get_incoming() ) {
				calc_boundaries(menu);
				CurBuffy = CurBuffy->prev;
			}
			break;
		case OP_SIDEBAR_SCROLL_DOWN:
			CurBuffy = BottomBuffy;
			if ( CurBuffy->next ) {
				calc_boundaries(menu);
				CurBuffy = CurBuffy->next;
			}
			break;
		default:
			return;
	}
	calc_boundaries(menu);
	draw_sidebar(menu);
}

/* switch between regualar and virtual folders */
void toggle_sidebar(int menu)
{
	if (sidebar_source == -1)
		get_incoming();

	if (sidebar_source == SB_SRC_INCOMING && VirtIncoming)
		sidebar_source = SB_SRC_VIRT;
	else
		sidebar_source = SB_SRC_INCOMING;

	TopBuffy = NULL;
	BottomBuffy = NULL;

	set_curbuffy("");	/* default is the first mailbox */
	draw_sidebar(menu);
}
