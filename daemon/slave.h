/* GDM - The GNOME Display Manager
 * Copyright (C) 1998, 1999, 2000 Martin K. Petersen <mkp@mkp.net>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#ifndef GDM_SLAVE_H
#define GDM_SLAVE_H

#include <glib.h>

#include "gdm.h"
#include "display.h"

typedef enum {
        GDM_SESSION_RECORD_TYPE_LOGIN,
        GDM_SESSION_RECORD_TYPE_LOGOUT,
        GDM_SESSION_RECORD_TYPE_FAILED_ATTEMPT
} GdmSessionRecordType;

void     gdm_slave_start       (GdmDisplay *d);
void     gdm_slave_greeter_ctl_no_ret (char cmd, const char *str);
char    *gdm_slave_greeter_ctl (char cmd, const char *str);
gboolean gdm_slave_greeter_check_interruption (void);
gboolean gdm_slave_action_pending (void);

void	 gdm_slave_send		(const char *str, gboolean wait_for_ack);
void	 gdm_slave_send_num	(const char *opcode, long num);
void     gdm_slave_send_string	(const char *opcode, const char *str);
gboolean gdm_slave_final_cleanup (void);

void     gdm_slave_whack_temp_auth_file (void);

gboolean gdm_slave_check_user_wants_to_log_in (const char *user);
gboolean gdm_is_session_magic (const char *session_name);

/* This is the slave child handler so that we can chain to it from elsewhere */
void	 gdm_slave_child_handler (int sig);
void     gdm_slave_write_utmp_wtmp_record (GdmDisplay *d,
                                 GdmSessionRecordType record_type,
                                 const gchar *username,
                                 GPid  pid);
gchar *  gdm_slave_get_display_device (GdmDisplay *d);


#endif /* GDM_SLAVE_H */

/* EOF */

