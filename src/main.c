/* tinyproxy - A fast light-weight HTTP proxy
 *
 * Copyright (C) 1998 Steven Young <sdyoung@miranda.org>
 * Copyright (C) 1998-2002 Robert James Kaes <rjkaes@users.sourceforge.net>
 * Copyright (C) 2000 Chris Lightfoot <chris@ex-parrot.com>
 * Copyright (C) 2009-2010 Mukund Sivaraman <muks@banu.com>
 * Copyright (C) 2009-2010 Michael Adam <obnox@samba.org>
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
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

/* The initialize routine. Basically sets up all the initial stuff (logfile,
 * listening socket, config options, etc.) and then sits there and loops
 * over the new connections until the daemon is closed. Also has additional
 * functions to handle the "user friendly" aspects of a program (usage,
 * stats, etc.) Like any good program, most of the work is actually done
 * elsewhere.
 */

#include "main.h"

#include "anonymous.h"
#include "buffer.h"
#include "conf.h"
#include "daemon.h"
#include "heap.h"
#include "filter.h"
#include "child.h"
#include "log.h"
#include "reqs.h"
#include "sock.h"
#include "stats.h"
#include "utils.h"
#include "sock_proxy.h"
#include "buffer.h"

/*
 * Global Structures
 */
struct config_s *config;
static struct config_s configs[2];
static const char* config_file;
static int port, remote_port;
static const char * hostname;
unsigned int received_sighup = FALSE;   /* boolean */

static struct config_s*
get_next_config(void)
{
        if (config == &configs[0]) return &configs[1];
        return &configs[0];
}

/*
 * Handle a signal
 */
static void
takesig (int sig)
{
        pid_t pid;
        int status;

        switch (sig) {
        case SIGHUP:
                received_sighup = TRUE;
                break;

        case SIGINT:
        case SIGTERM:
                config->quit = TRUE;
#ifdef REMOTE_SOCKET
                // close everything
#endif
                break;

        case SIGCHLD:
                while ((pid = waitpid (-1, &status, WNOHANG)) > 0) ;
                break;
        }

        return;
}

/*
 * Display the version information for the user.
 */
static void
display_version (void)
{
        printf ("%s %s\n", PACKAGE, VERSION);
}

/*
 * Display usage to the user.
 */
static void
display_usage (void)
{
        int features = 0;

        printf ("Usage: %s [options]\n", PACKAGE);
        printf ("\n"
                "Options are:\n"
                "  -d        Do not daemonize (run in foreground).\n"
                "  -c FILE   Use an alternate configuration file.\n"
                "  -h        Display this usage information.\n"
                "  -v        Display version information.\n"
#ifdef REMOTE_SOCKET
                "  -p port  Set proxy port number (default value is 8888)\n"
                "  -r port  Set remote socket port information\n"
#endif
                );

        /* Display the modes compiled into tinyproxy */
        printf ("\nFeatures compiled in:\n");

#ifdef XTINYPROXY_ENABLE
        printf ("    XTinyproxy header\n");
        features++;
#endif /* XTINYPROXY */

#ifdef FILTER_ENABLE
        printf ("    Filtering\n");
        features++;
#endif /* FILTER_ENABLE */

#ifndef NDEBUG
        printf ("    Debugging code\n");
        features++;
#endif /* NDEBUG */

#ifdef TRANSPARENT_PROXY
        printf ("    Transparent proxy support\n");
        features++;
#endif /* TRANSPARENT_PROXY */

#ifdef REVERSE_SUPPORT
        printf ("    Reverse proxy support\n");
        features++;
#endif /* REVERSE_SUPPORT */

#ifdef UPSTREAM_SUPPORT
        printf ("    Upstream proxy support\n");
        features++;
#endif /* UPSTREAM_SUPPORT */

        if (0 == features)
                printf ("    None\n");

        printf ("\n"
                "For support and bug reporting instructions, please visit\n"
                "<https://tinyproxy.github.io/>.\n");
}

static int
get_id (char *str)
{
        char *tstr;

        if (str == NULL)
                return -1;

        tstr = str;
        while (*tstr != 0) {
                if (!isdigit (*tstr))
                        return -1;
                tstr++;
        }

        return atoi (str);
}

/**
 * change_user:
 * @program: The name of the program. Pass argv[0] here.
 *
 * This function tries to change UID and GID to the ones specified in
 * the config file. This function is typically called during
 * initialization when the effective user is root.
 **/
static void
change_user (const char *program)
{
        if (config->group && strlen (config->group) > 0) {
                int gid = get_id (config->group);

                if (gid < 0) {
                        struct group *thisgroup = getgrnam (config->group);

                        if (!thisgroup) {
                                fprintf (stderr,
                                         "%s: Unable to find group \"%s\".\n",
                                         program, config->group);
                                exit (EX_NOUSER);
                        }

                        gid = thisgroup->gr_gid;
                }

                if (setgid (gid) < 0) {
                        fprintf (stderr,
                                 "%s: Unable to change to group \"%s\".\n",
                                 program, config->group);
                        exit (EX_NOPERM);
                }

#ifdef HAVE_SETGROUPS
                /* Drop all supplementary groups, otherwise these are inherited from the calling process */
                if (setgroups (0, NULL) < 0) {
                        fprintf (stderr,
                                 "%s: Unable to drop supplementary groups.\n",
                                 program);
                        exit (EX_NOPERM);
                }
#endif

                log_message (LOG_INFO, "Now running as group \"%s\".",
                             config->group);
        }

        if (config->user && strlen (config->user) > 0) {
                int uid = get_id (config->user);

                if (uid < 0) {
                        struct passwd *thisuser = getpwnam (config->user);

                        if (!thisuser) {
                                fprintf (stderr,
                                         "%s: Unable to find user \"%s\".\n",
                                         program, config->user);
                                exit (EX_NOUSER);
                        }

                        uid = thisuser->pw_uid;
                }

                if (setuid (uid) < 0) {
                        fprintf (stderr,
                                 "%s: Unable to change to user \"%s\".\n",
                                 program, config->user);
                        exit (EX_NOPERM);
                }

                log_message (LOG_INFO, "Now running as user \"%s\".",
                             config->user);
        }
}

/**
 * convenience wrapper around reload_config_file
 * that also re-initializes logging.
 */
int reload_config (int reload_logging)
{
        int ret;
        struct config_s *c_next = get_next_config();

        if (reload_logging) shutdown_logging ();

        ret = reload_config_file (config_file, c_next);

        if (ret != 0) {
                goto done;
        }

        config = c_next;

        if (reload_logging) ret = setup_logging ();

done:
        return ret;
}

int
main (int argc, char **argv)
{
        int opt, daemonized = TRUE;
        char * token = NULL;
        char * tmp = NULL;

        srand(time(NULL)); /* for hashmap seeds */

        /* Only allow u+rw bits. This may be required for some versions
         * of glibc so that mkstemp() doesn't make us vulnerable.
         */
        umask (0177);

        log_message (LOG_INFO, "Initializing " PACKAGE " ...");

        if (config_compile_regex()) {
                exit (EX_SOFTWARE);
        }

        config_file = SYSCONFDIR "/tinyproxy.conf";

        while ((opt = getopt (argc, argv, "c:vdhp:r:")) != EOF) {
                tmp = optarg;
                switch (opt) {
                case 'v':
                        display_version ();
                        exit (EX_OK);

                case 'd':
                        daemonized = FALSE;
                        break;

                case 'c':
                        config_file = optarg;
                        break;

#ifdef REMOTE_SOCKET
                case 'p':
                        port = atoi(tmp);
                        break;
                case 'r':
                        /* Format must be host:port */

                        /* 1. Extract the first part : host */
                        hostname = strtok(tmp, ":");
                        //strncpy(config->remote_socket_host, token, MAX_HOST_LEN);

                        /* 2. Extract the port and convert it to int */
                        remote_port = atoi( strtok(NULL, ":") );

                        //config->remote_socket_port = atoi(token);
                        break;
#endif
                case 'h':
                        display_usage ();
                        exit (EX_OK);

                default:
                        display_usage ();
                        exit (EX_USAGE);
                }
        }

        printf("port %d, hostname %s:%d\n", port, hostname, remote_port);

        if (reload_config(0)) {
                exit (EX_SOFTWARE);
        }

        init_stats ();

        /* If ANONYMOUS is turned on, make sure that Content-Length is
         * in the list of allowed headers, since it is required in a
         * HTTP/1.0 request. Also add the Content-Type header since it
         * goes hand in hand with Content-Length. */
        if (is_anonymous_enabled (config)) {
                anonymous_insert (config, "Content-Length");
                anonymous_insert (config, "Content-Type");
        }

        if (daemonized == TRUE) {
                if (!config->syslog && config->logf_name == NULL)
                        fprintf(stderr, "WARNING: logging deactivated "
                                "(can't log to stdout when daemonized)\n");

                makedaemon ();
        }

        if (set_signal_handler (SIGPIPE, SIG_IGN) == SIG_ERR) {
                fprintf (stderr, "%s: Could not set the \"SIGPIPE\" signal.\n",
                         argv[0]);
                exit (EX_OSERR);
        }

#ifdef FILTER_ENABLE
        if (config->filter)
                filter_init ();
#endif /* FILTER_ENABLE */


#ifdef REMOTE_SOCKET

        config->port = port;
        strncpy(config->remote_socket_host, hostname, MAX_HOST_LEN);
        config->remote_socket_port = remote_port;

        if( init_remote_socket() < 0 )
        {
                fprintf (stderr, "%s: Could not create listening sockets 1.\n",
                                argv[0]);
                exit (EX_OSERR);
        }

        fprintf (stdout, "%s: Before remote_start_server.\n",
                         argv[0]);

        if( start_remote_socket_server(config->remote_socket_port) == -1){

                fprintf (stdout, "%s: Merde.\n",
                         argv[0]);
                return -1;
        }
        fprintf (stdout, "%s: After remote_start_server.\n",
                         argv[0]);



#endif /* #ifdef REMOTE_SOCKET */

        /* Start listening on the selected port. */
        if (child_listening_sockets(config->listen_addrs, config->port) < 0) {
                fprintf (stderr, "%s: Could not create listening sockets 2 %d.\n",
                         argv[0], errno);
                exit (EX_OSERR);
        }

        /* Create pid file before we drop privileges */
        if (config->pidpath) {
                if (pidfile_create (config->pidpath) < 0) {
                        fprintf (stderr, "%s: Could not create PID file.\n",
                                 argv[0]);
                        exit (EX_OSERR);
                }
        }

        /* Switch to a different user if we're running as root */
        if (geteuid () == 0)
                change_user (argv[0]);
        else
                log_message (LOG_WARNING,
                             "Not running as root, so not changing UID/GID.");

        /* Create log file after we drop privileges */
        if (setup_logging ()) {
                exit (EX_SOFTWARE);
        }

        /* These signals are only for the parent process. */
        log_message (LOG_INFO, "Setting the various signals.");

        if (set_signal_handler (SIGCHLD, takesig) == SIG_ERR) {
                fprintf (stderr, "%s: Could not set the \"SIGCHLD\" signal.\n",
                         argv[0]);
                exit (EX_OSERR);
        }

        if (set_signal_handler (SIGTERM, takesig) == SIG_ERR) {
                fprintf (stderr, "%s: Could not set the \"SIGTERM\" signal.\n",
                         argv[0]);
                exit (EX_OSERR);
        }

        if (set_signal_handler (SIGHUP, takesig) == SIG_ERR) {
                fprintf (stderr, "%s: Could not set the \"SIGHUP\" signal.\n",
                         argv[0]);
                exit (EX_OSERR);
        }

        /* Start the main loop */
        log_message (LOG_INFO, "Starting main loop. Accepting connections.");

        if (0){

                unsigned char buff[1000];
                unsigned char receivedBuff[1000];
                int idx = 0;
                int fds[2][2];

                int bytes_received = 0;

                struct buffer_s * pBuff = new_buffer();
                struct buffer_s * rBuff = new_buffer();

                add_to_buffer( pBuff, buff, sizeof(buff));

                for(idx = 0; idx < sizeof(buff); ++idx){
                        buff[idx] = (char)idx;
                }

                for(idx = 0; idx < sizeof(receivedBuff); ++idx){
                        if( (idx % 8) == 0) {
                                printf("\n");
                        }

                        printf("0x%02x ", buff[idx]);
                }

                printf("\n");
                printf("\n");
                printf("\n");

                remote_open("www.google.fr", 80, fds);

                write(fds[CLIENT_OFFSET], buff, sizeof(buff));

                while(bytes_received < sizeof(buff) ){
                        log_message (LOG_INFO, "received %d bytes", bytes_received);
                        bytes_received +=
                                read (fds[SERVER_OFFSET], receivedBuff, sizeof(buff) - bytes_received);
                }

                log_message (LOG_INFO, "received        %d bytes", bytes_received);

                printf("idx = %d", idx);

                for(idx = 0; idx < bytes_received; ++idx){
                        printf("0x%02x\n", receivedBuff[idx]);
                }

                printf("idx = %d\n", idx);
        }


        child_main_loop ();

        log_message (LOG_INFO, "Shutting down.");

        child_kill_children (SIGTERM);
        child_close_sock ();

        /* Remove the PID file */
        if (config->pidpath != NULL && unlink (config->pidpath) < 0) {
                log_message (LOG_WARNING,
                             "Could not remove PID file \"%s\": %s.",
                             config->pidpath, strerror (errno));
        }

#ifdef FILTER_ENABLE
        if (config->filter)
                filter_destroy ();
#endif /* FILTER_ENABLE */

        shutdown_logging ();

        return EXIT_SUCCESS;
}
