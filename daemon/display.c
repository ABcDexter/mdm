/* -*- Mode: C; tab-width: 8; indent-tabs-mode: t; c-basic-offset: 8 -*-
 *
 * GDM - The GNOME Display Manager
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

#include "config.h"

#include <signal.h>
#include <string.h>
#include <sys/wait.h>
#include <sys/types.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>

#ifdef HAVE_SMF_CONTRACTS
#include <sys/ctfs.h>
#include <sys/contract.h>
#include <sys/contract/process.h>
#include <libcontract.h>
#endif

#include <glib/gi18n.h>

#include "gdm.h"
#include "gdm-net.h"
#include "server.h"
#include "display.h"
#include "slave.h"
#include "misc.h"
#include "choose.h"
#include "auth.h"
#include "gdm-net.h"

#include "gdm-common.h"
#include "gdm-log.h"
#include "gdm-daemon-config.h"

/* External vars */
extern GdmConnection *fifoconn;
extern GdmConnection *pipeconn;
extern GdmConnection *unixconn;
extern int slave_fifo_pipe_fd; /* the slavepipe (like fifo) connection, this is the write end */
extern gint flexi_servers;

/**
 * gdm_display_alloc:
 * @id: Local display number
 * @command: Command line for starting the X server
 *
 * Allocate display structure for a local X server
 */

GdmDisplay *
gdm_display_alloc (gint id, const gchar *command, const gchar *device)
{
    gchar hostname[1024];
    GdmDisplay *d;

    hostname[1023] = '\0';
    if (gethostname (hostname, 1023) == -1)
            strcpy (hostname, "localhost.localdomain");

    d = g_new0 (GdmDisplay, 1);

    d->logout_action = GDM_LOGOUT_ACTION_NONE;
    d->authfile = NULL;
    d->authfile_gdm = NULL;
    d->auths = NULL;
    d->userauth = NULL;
    d->command = g_strdup (command);
    d->cookie = NULL;
    d->dispstat = DISPLAY_UNBORN;
    d->greetpid = 0;
    d->name = g_strdup_printf (":%d", id);
    d->hostname = g_strdup (hostname);
    d->windowpath = NULL;
    /* Not really used for not XDMCP */
    memset (&(d->addr), 0, sizeof (d->addr));
    d->dispnum = id;
    d->servpid = 0;
    d->servstat = SERVER_DEAD;
    d->sesspid = 0;
    d->slavepid = 0;
    d->type = TYPE_STATIC;
    d->attached = TRUE;
    d->sessionid = 0;
    d->acctime = 0;
    d->dsp = NULL;
    d->screenx = 0; /* xinerama offset */
    d->screeny = 0;

    d->handled = TRUE;
    d->tcp_disallowed = FALSE;

    d->priority = 0;
    d->vt = -1;
    d->vtnum = -1;
    if (device != NULL)
	d->device_name = g_strdup (device);
    else
	d->device_name = NULL;

    d->x_servers_order = -1;

    d->last_loop_start_time = 0;
    d->last_start_time = 0;
    d->retry_count = 0;
    d->sleep_before_run = 0;
    d->login = NULL;
    d->preset_user = NULL;

    d->timed_login_ok = FALSE;

    d->slave_notify_fd = -1;
    d->master_notify_fd = -1;

    d->xsession_errors_bytes = 0;
    d->xsession_errors_fd = -1;
    d->session_output_fd = -1;

    d->chooser_output_fd = -1;
    d->chooser_last_line = NULL;

    d->theme_name = NULL;

    return d;
}

static gboolean
gdm_display_check_loop (GdmDisplay *disp)
{
  time_t now;
  time_t since_last;
  time_t since_loop;
  
  now = time (NULL);

  gdm_debug ("loop check: last_start %ld, last_loop %ld, now: %ld, retry_count: %d", (long)disp->last_start_time, (long) disp->last_loop_start_time, (long) now, disp->retry_count);
  
  if (disp->last_loop_start_time > now || disp->last_loop_start_time == 0)
    {
      /* Reset everything if this is the first time in this
       * function, or if the system clock got reset backward.
       */
      disp->last_loop_start_time = now;
      disp->last_start_time = now;
      disp->retry_count = 1;

      gdm_debug ("Resetting counts for loop of death detection");
      
      return TRUE;
    }

  since_loop = now - disp->last_loop_start_time;
  since_last = now - disp->last_start_time;

  /* If it's been at least 1.5 minutes since the last startup loop
   * attempt, then we reset everything.  Or if the last startup was more then
   * 30 seconds ago, then it was likely a successful session.
   */

  if (since_loop >= 90 || since_last >= 30)
    {
      disp->last_loop_start_time = now;
      disp->last_start_time = now;
      disp->retry_count = 1;

      gdm_debug ("Resetting counts for loop of death detection, 90 seconds elapsed since loop started or session lasted more then 30 seconds.");
      
      return TRUE;
    }

  /* If we've tried too many times we bail out. i.e. this means we
   * tried too many times in the 90-second period.
   */
  if (disp->retry_count >= 6) {
	  /* This means we have no clue what's happening,
	   * it's not X server crashing as we would have
	   * cought that elsewhere.  Things are just
	   * not working out, so tell the user.
	   * However this may have been caused by a malicious local user
	   * zapping the display repeatedly, that shouldn't cause gdm
	   * to stop working completely so just wait for 2 minutes,
	   * that should give people ample time to stop gdm if needed,
	   * or just wait for the stupid malicious user to get bored
	   * and go away */
	  char *s = g_strdup_printf (C_(N_("The display server has been shut down "
					   "about 6 times in the last 90 seconds. "
					   "It is likely that something bad is "
					   "going on.  Waiting for 2 minutes "
					   "before trying again on display %s.")),
					disp->name);
	  /* only display a dialog box if this is a local display */
	  if (disp->type == TYPE_STATIC ||
	      disp->type == TYPE_FLEXI) {
		  gdm_text_message_dialog (s);
	  }
	  gdm_error ("%s", s);
	  g_free (s);

	  /* Wait 2 minutes */
	  disp->sleep_before_run = 120;
	  /* well, "last" start time will really be in the future */
	  disp->last_start_time = now + disp->sleep_before_run;

	  disp->retry_count = 1;
	  /* this will reset stuff in the next run (after this
	     "after-two-minutes" server starts) */
	  disp->last_loop_start_time = 0;
	  
	  return TRUE;
  }
  
  /* At least 8 seconds between start attempts, but only after
   * the second start attempt, so you can try to kill gdm from the console
   * in these gaps.
   */
  if (disp->retry_count > 2 && since_last < 8)
    {
      gdm_debug ("Will sleep %ld seconds before next X server restart attempt",
                 (long)(8 - since_last));
      now = time (NULL) + 8 - since_last;
      disp->sleep_before_run = 8 - since_last;
      /* well, "last" start time will really be in the future */
      disp->last_start_time = now + disp->sleep_before_run;
    }
  else
    {
      /* wait one second just for safety (avoids X server races) */
      disp->sleep_before_run = 1;
      disp->last_start_time = now;
    }

  disp->retry_count++;

  return TRUE;
}

static void
whack_old_slave (GdmDisplay *d, gboolean kill_connection)
{
    time_t t = time (NULL);
    gboolean waitsleep = TRUE;

    if (kill_connection) {
	    /* This should never happen, but just in case */
	    if (d->socket_conn != NULL) {
		    GdmConnection *conn = d->socket_conn;
		    d->socket_conn = NULL;
		    gdm_connection_set_close_notify (conn, NULL, NULL);
	    }
    }

    if (d->master_notify_fd >= 0) {
	    VE_IGNORE_EINTR (close (d->master_notify_fd));
	    d->master_notify_fd = -1;
    }

    /* if we have DISPLAY_DEAD set, then this has already been killed */
    if (d->dispstat == DISPLAY_DEAD)
	    waitsleep = FALSE;

    /* Kill slave */
    if (d->slavepid > 1 &&
	(d->dispstat == DISPLAY_DEAD || kill (d->slavepid, SIGTERM) == 0)) {
	    int exitstatus;
	    int ret;
wait_again:
		
	    if (waitsleep)
		    /* wait for some signal, yes this is a race */
		    sleep (10);
	    waitsleep = TRUE;
	    errno = 0;
	    ret = waitpid (d->slavepid, &exitstatus, WNOHANG);
	    if (ret <= 0) {
		    /* rekill the slave to tell it to
		       hurry up and die if we're getting
		       killed ourselves */
		    if ((gdm_daemon_config_signal_terminthup_was_notified ()) ||
			(t + 10 <= time (NULL))) {
			    gdm_debug ("whack_old_slave: GOT ANOTHER SIGTERM (or it was 10 secs already), killing slave again with SIGKILL");
			    t = time (NULL);
			    kill (d->slavepid, SIGKILL);
			    goto wait_again;
		    } else if (ret < 0 && errno == EINTR) {
			    goto wait_again;
		    }
	    }

	    if (WIFSIGNALED (exitstatus)) {
		    gdm_debug ("whack_old_slave: Slave crashed (signal %d), killing its children",
			       (int)WTERMSIG (exitstatus));

		    if (d->sesspid > 1)
			    kill (-(d->sesspid), SIGTERM);
		    d->sesspid = 0;
		    if (d->greetpid > 1)
			    kill (-(d->greetpid), SIGTERM);
		    d->greetpid = 0;
		    if (d->chooserpid > 1)
			    kill (-(d->chooserpid), SIGTERM);
		    d->chooserpid = 0;
		    if (d->servpid > 1)
			    kill (d->servpid, SIGTERM);
		    d->servpid = 0;
	    }
    }
    d->slavepid = 0;
}

#ifdef HAVE_SMF_CONTRACTS
static int contracts_fd = -1;

void
contracts_pre_fork ()
{
   const char *errmsg = "opening process contract template";

	/*
	 * On failure, just continue since it is better to start with
	 * children in the same contract than to not start them at all.
	 */
	if (contracts_fd == -1) {
		if ((contracts_fd = open64 (CTFS_ROOT "/process/template",
					    O_RDWR)) == -1)
			goto exit;

		errmsg = "setting contract terms";
		if ((errno = ct_pr_tmpl_set_param (contracts_fd, CT_PR_PGRPONLY)))
			goto exit;

		if ((errno = ct_tmpl_set_informative (contracts_fd, CT_PR_EV_HWERR)))
			goto exit;

		if ((errno = ct_pr_tmpl_set_fatal (contracts_fd, CT_PR_EV_HWERR)))
			goto exit;

		if ((errno = ct_tmpl_set_critical (contracts_fd, 0)))
			goto exit;
	}

	errmsg = "setting active template";
	if ((errno = ct_tmpl_activate (contracts_fd)))
		goto exit;

	gdm_debug ("Set active contract");
	return;

exit:
	if (contracts_fd != -1)
		(void) close (contracts_fd);

	contracts_fd = -1;

	if (errno) {
		gdm_debug (
			"Error setting up active contract template: %s while %s",
			strerror (errno), errmsg);
	}
}

void
contracts_post_fork_child ()
{
	/* Clear active template so no new contracts are created on fork */
	if (contracts_fd == -1)
		return;

	if ((errno = (ct_tmpl_clear (contracts_fd)))) {
		gdm_debug (
			"Error clearing active contract template (child): %s",
			strerror (errno));
	} else {
		gdm_debug ("Cleared active contract template (child)");
	}

	(void) close (contracts_fd);

	contracts_fd = -1;
}

void
contracts_post_fork_parent (int fork_succeeded)
{
	char path[PATH_MAX];
	int cfd;
	ct_stathdl_t status;
	ctid_t latest;

	/* Clear active template, abandon latest contract. */
	if (contracts_fd == -1)
		return;

	if ((errno = ct_tmpl_clear (contracts_fd)))
		gdm_debug ("Error while clearing active contract template: %s",
			   strerror (errno));
	else
		gdm_debug ("Cleared active contract template (parent)");

	if (!fork_succeeded)
		return;

	if ((cfd = open64 (CTFS_ROOT "/process/latest", O_RDONLY)) == -1) {
		gdm_debug ("Error getting latest contract: %s",
			   strerror(errno));
		return;
	}

	if ((errno = ct_status_read (cfd, CTD_COMMON, &status)) != 0) {
		gdm_debug ("Error getting latest contract ID: %s",
			   strerror(errno));
		(void) close (cfd);
		return;
	}

	latest = ct_status_get_id (status);
	ct_status_free (status);
	(void) close (cfd);


	if ((snprintf (path, PATH_MAX, CTFS_ROOT "/all/%ld/ctl", latest)) >=
	     PATH_MAX) {
		gdm_debug ("Error opening the latest contract ctl file: %s",
			   strerror (ENAMETOOLONG));
		return;
	}

	cfd = open64 (path, O_WRONLY);
	if (cfd == -1) {
		gdm_debug ("Error opening the latest contract ctl file: %s",
			   strerror (errno));
		return;
	}
 
	if ((errno = ct_ctl_abandon (cfd)))
		gdm_debug ("Error abandoning latest contract: %s",
			   strerror (errno));
	else
		gdm_debug ("Abandoned latest contract");

	(void) close (cfd);
}
#endif HAVE_SMF_CONTRACTS

/**
 * gdm_display_manage:
 * @d: Pointer to a GdmDisplay struct
 *
 * Manage (Initialize and start login session) display
 */

gboolean 
gdm_display_manage (GdmDisplay *d)
{
    pid_t pid;
    int fds[2];

    if (!d) 
	return FALSE;

    gdm_debug ("gdm_display_manage: Managing %s", d->name);

    if (pipe (fds) < 0) {
	    gdm_error (_("%s: Cannot create pipe"), "gdm_display_manage");
    }

    if ( ! gdm_display_check_loop (d))
	    return FALSE;

    if (d->slavepid != 0)
	    gdm_debug ("gdm_display_manage: Old slave pid is %d", (int)d->slavepid);

    /* If we have an old slave process hanging around, kill it */
    /* This shouldn't be a normal code path however, so it doesn't matter
     * that we are hanging */
    whack_old_slave (d, FALSE /* kill_connection */);

    /* Ensure that /tmp/.ICE-unix and /tmp/.X11-unix exist and have the
     * correct permissions */
    gdm_ensure_sanity ();

    d->managetime = time (NULL);

    gdm_debug ("Forking slave process");

#ifdef HAVE_SMF_CONTRACTS
    contracts_pre_fork ();
#endif

    /* Fork slave process */
    pid = d->slavepid = fork ();

    switch (pid) {

    case 0:
#ifdef HAVE_SMF_CONTRACTS
        contracts_post_fork_child ();
#endif

	setpgid (0, 0);

	/* Make the slave it's own leader.  This 1) makes killing -pid of
	 * the daemon work more sanely because the daemon can whack the
	 * slave much better itself */
	setsid ();

	/* In the child setup empty mask and set all signals to
	 * default values, we'll make them more fun later */
	gdm_unset_signals ();

	d->slavepid = getpid ();

	gdm_connection_close (fifoconn);
	fifoconn = NULL;
	gdm_connection_close (pipeconn);
	pipeconn = NULL;
	gdm_connection_close (unixconn);
	unixconn = NULL;

	gdm_log_shutdown ();

	/* Close everything */
	gdm_close_all_descriptors (0 /* from */, fds[0] /* except */, slave_fifo_pipe_fd /* except2 */);

	/* No error checking here - if it's messed the best response
         * is to ignore & try to continue */
	gdm_open_dev_null (O_RDONLY); /* open stdin - fd 0 */
	gdm_open_dev_null (O_RDWR); /* open stdout - fd 1 */
	gdm_open_dev_null (O_RDWR); /* open stderr - fd 2 */

	gdm_log_init ();

	d->slave_notify_fd = fds[0];

	fcntl (d->slave_notify_fd, F_SETFL, fcntl (d->slave_notify_fd, F_GETFL) | O_NONBLOCK);

	gdm_slave_start (d);
	/* should never retern */

	/* yaikes, how did we ever get here? */
	gdm_server_stop (d);
	_exit (DISPLAY_REMANAGE);

	break;

    case -1:
	d->slavepid = 0;
	gdm_error (_("%s: Failed forking GDM slave process for %s"),
		   "gdm_display_manage",
		   d->name);

	return FALSE;

    default:
	gdm_debug ("gdm_display_manage: Forked slave: %d",
		   (int)pid);
	d->master_notify_fd = fds[1];
	VE_IGNORE_EINTR (close (fds[0]));
	break;
    }

#ifdef HAVE_SMF_CONTRACTS
    contracts_post_fork_parent ((pid > 0));
#endif

    /* invalidate chosen hostname */
    g_free (d->chosen_hostname);
    d->chosen_hostname = NULL;

    /* use_chooser can only be temporary, if you want it permanent you set it
       up in the server definition with "chooser=true" and it will get set up
       during server command line resolution */
    d->use_chooser = FALSE;

    if (SERVER_IS_LOCAL (d)) {
	    d->dispstat = DISPLAY_ALIVE;
    }

    /* reset sleep to 1, to sleep just in case (avoids X server races) */
    d->sleep_before_run = 1;

    return TRUE;
}


/**
 * gdm_display_unmanage:
 * @d: Pointer to a GdmDisplay struct
 *
 * Stop services for a display
 */
void
gdm_display_unmanage (GdmDisplay *d)
{
    if (!d)
	return;

    gdm_debug ("gdm_display_unmanage: Stopping %s (slave pid: %d)",
	       d->name, (int)d->slavepid);

    /* whack connections about this display */
    if (unixconn != NULL)
      gdm_kill_subconnections_with_display (unixconn, d);

    /* Kill slave, this may in fact hang for a bit at least until the
     * slave dies, which should be ASAP though */
    whack_old_slave (d, TRUE /* kill_connection */);
    
    d->dispstat = DISPLAY_DEAD;
    if (d->type != TYPE_STATIC || d->removeconf)
	gdm_display_dispose (d);

    gdm_debug ("gdm_display_unmanage: Display stopped");
}


/* Why recount?  It's just a lot more robust this way and
   gets around those nasty one off errors and races.  And we never
   have so many displays that this would get too slow. */
static void
count_session_limits (void)
{
	GSList *li;
	GSList *displays;

	displays = gdm_daemon_config_get_display_list ();

	flexi_servers = 0;

	for (li = displays; li != NULL; li = li->next) {
		GdmDisplay *d = li->data;

		if (SERVER_IS_FLEXI (d)) {
			flexi_servers++;
		}
	}
}

/**
 * gdm_display_dispose:
 * @d: Pointer to a GdmDisplay struct
 *
 * Deallocate display and all its resources
 */

void
gdm_display_dispose (GdmDisplay *d)
{

    if (d == NULL)
	return;

    /* paranoia */
    if (unixconn != NULL)
            gdm_kill_subconnections_with_display (unixconn, d);

    if (d->socket_conn != NULL) {
	    GdmConnection *conn = d->socket_conn;
	    d->socket_conn = NULL;
	    gdm_connection_set_close_notify (conn, NULL, NULL);
    }

    if (d->slave_notify_fd >= 0) {
	    VE_IGNORE_EINTR (close (d->slave_notify_fd));
	    d->slave_notify_fd = -1;
    }

    if (d->master_notify_fd >= 0) {
	    VE_IGNORE_EINTR (close (d->master_notify_fd));
	    d->master_notify_fd = -1;
    }

    gdm_daemon_config_display_list_remove (d);

    d->dispstat = DISPLAY_DEAD;
    d->type = -1;

    count_session_limits ();

    if (d->name) {
	gdm_debug ("gdm_display_dispose: Disposing %s", d->name);
	g_free (d->name);
	d->name = NULL;
    }
    
    g_free (d->chosen_hostname);
    d->chosen_hostname = NULL;

    g_free (d->hostname);
    d->hostname = NULL;

    g_free (d->windowpath);
    d->windowpath = NULL;

    g_free (d->addrs);
    d->addrs = NULL;
    d->addr_count = 0;

    g_free (d->authfile);
    d->authfile = NULL;

    g_free (d->authfile_gdm);
    d->authfile_gdm = NULL;

    if (d->type == TYPE_XDMCP_PROXY) {
	    if (d->parent_auth_file != NULL) {
		    VE_IGNORE_EINTR (g_unlink (d->parent_auth_file));
	    }
	    g_free (d->parent_auth_file);
	    d->parent_auth_file = NULL;
    }

    if (d->parent_temp_auth_file != NULL) {
	    VE_IGNORE_EINTR (g_unlink (d->parent_temp_auth_file));
    }
    g_free (d->parent_temp_auth_file);
    d->parent_temp_auth_file = NULL;

    if (d->auths) {
	    gdm_auth_free_auth_list (d->auths);
	    d->auths = NULL;
    }

    if (d->local_auths) {
	    gdm_auth_free_auth_list (d->local_auths);
	    d->local_auths = NULL;
    }

    g_free (d->userauth);
    d->userauth = NULL;

    g_free (d->windowpath);
    d->windowpath = NULL;

    g_free (d->command);
    d->command = NULL;

    g_free (d->device_name);
    d->device_name = NULL;

    g_free (d->cookie);
    d->cookie = NULL;

    g_free (d->bcookie);
    d->bcookie = NULL;

    if (d->indirect_id > 0)
	    gdm_choose_indirect_dispose_empty_id (d->indirect_id);
    d->indirect_id = 0;

    g_free (d->parent_disp);
    d->parent_disp = NULL;

    g_free (d->parent_auth_file);
    d->parent_auth_file = NULL;

    g_free (d->login);
    d->login = NULL;

    g_free (d->preset_user);
    d->preset_user = NULL;

    g_free (d->xsession_errors_filename);
    d->xsession_errors_filename = NULL;

    if (d->session_output_fd >= 0) {
	    VE_IGNORE_EINTR (close (d->session_output_fd));
	    d->session_output_fd = -1;
    }

    if (d->xsession_errors_fd >= 0) {
	    VE_IGNORE_EINTR (close (d->xsession_errors_fd));
	    d->xsession_errors_fd = -1;
    }

    g_free (d->chooser_last_line);
    d->chooser_last_line = NULL;

    if (d->chooser_output_fd >= 0) {
	    VE_IGNORE_EINTR (close (d->chooser_output_fd));
	    d->chooser_output_fd = -1;
    }

    g_free (d->theme_name);
    d->theme_name = NULL;

    g_free (d->xserver_session_args);
    d->xserver_session_args = NULL;

    g_free (d);
}


/**
 * gdm_display_lookup:
 * @pid: pid of slave process to look up
 *
 * Return the display managed by pid
 */

GdmDisplay *
gdm_display_lookup (pid_t pid)
{
    GSList *li;
    GSList *displays;

    displays = gdm_daemon_config_get_display_list ();

    /* Find slave in display list */
    for (li = displays; li != NULL; li = li->next) {
	    GdmDisplay *d = li->data;

	    if (d != NULL &&
		pid == d->slavepid)
		    return d;
    }
     
    /* Slave not found */
    return NULL;
}


/* EOF */
