/*
    usbmuxd - iPhone/iPod Touch USB multiplex server daemon

Copyright (C) 2009	Hector Martin "marcan" <hector@marcansoft.com>
Copyright (C) 2009	Nikias Bassen <nikias@gmx.li>
Copyright (C) 2009	Paul Sladen <libiphone@paul.sladen.org>

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 2 or version 3.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA

*/

#define _BSD_SOURCE
#define _GNU_SOURCE

#ifdef HAVE_CONFIG_H
#include <config.h>
#else
#define USBMUXD_VERSION "1.0.8"
#endif

#include <stdio.h>
#include <errno.h>
#include <string.h>
#include <stdlib.h>
#include <signal.h>
#include <unistd.h>
#ifdef WIN32
#include <winsock2.h>
#include <windows.h>
#include <usbmuxd-proto.h>
#include <sock_stuff.h>
#include <libcompat.h>
#include <utils.h>
#else
#include <sys/socket.h>
#include <sys/un.h>
#endif
#include <sys/stat.h>
#include <sys/types.h>
#include <fcntl.h>
#include <getopt.h>
#ifndef WIN32
#include <pwd.h>
#include <grp.h>
#endif

#include "log.h"
#include "usb.h"
#include "device.h"
#include "client.h"

#ifndef WIN32
static const char *socket_path = "/var/run/usbmuxd";
static const char *lockfile = "/var/run/usbmuxd.pid";
#endif

int should_exit;
int should_discover;

static int verbose = 0;
static int foreground = 0;
static int drop_privileges = 0;
static const char *drop_user = NULL;
static int opt_udev = 0;
static int opt_exit = 0;
static int exit_signal = 0;
static int daemon_pipe;

static int report_to_parent = 0;

#ifndef WIN32
int create_socket(void) {
    struct sockaddr_un bind_addr;
    int listenfd;

    if(unlink(socket_path) == -1 && errno != ENOENT) {
        usbmuxd_log(LL_FATAL, "unlink(%s) failed: %s", socket_path, strerror(errno));
        return -1;
    }

    listenfd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (listenfd == -1) {
        usbmuxd_log(LL_FATAL, "socket() failed: %s", strerror(errno));
        return -1;
    }

    bzero(&bind_addr, sizeof(bind_addr));
    bind_addr.sun_family = AF_UNIX;
    strcpy(bind_addr.sun_path, socket_path);
    if (bind(listenfd, (struct sockaddr*)&bind_addr, sizeof(bind_addr)) != 0) {
        usbmuxd_log(LL_FATAL, "bind() failed: %s", strerror(errno));
        return -1;
    }

    // Start listening
    if (listen(listenfd, 5) != 0) {
        usbmuxd_log(LL_FATAL, "listen() failed: %s", strerror(errno));
        return -1;
    }

    chmod(socket_path, 0666);

    return listenfd;
}

void handle_signal(int sig)
{
    if (sig != SIGUSR1 && sig != SIGUSR2) {
        usbmuxd_log(LL_NOTICE,"Caught signal %d, exiting", sig);
        should_exit = 1;
    } else {
        if(opt_udev) {
            if (sig == SIGUSR1) {
                usbmuxd_log(LL_INFO, "Caught SIGUSR1, checking if we can terminate (no more devices attached)...");
                if (device_get_count() > 0) {
                    // we can't quit, there are still devices attached.
                    usbmuxd_log(LL_NOTICE, "Refusing to terminate, there are still devices attached. Kill me with signal 15 (TERM) to force quit.");
                } else {
                    // it's safe to quit
                    should_exit = 1;
                }
            } else if (sig == SIGUSR2) {
                usbmuxd_log(LL_INFO, "Caught SIGUSR2, scheduling device discovery");
                should_discover = 1;
            }
        } else {
            usbmuxd_log(LL_INFO, "Caught SIGUSR1/2 but we weren't started in --udev mode, ignoring");
        }
    }
}

void set_signal_handlers(void)
{
    struct sigaction sa;
    sigset_t set;

    // Mask all signals we handle. They will be unmasked by ppoll().
    sigemptyset(&set);
    sigaddset(&set, SIGINT);
    sigaddset(&set, SIGQUIT);
    sigaddset(&set, SIGTERM);
    sigaddset(&set, SIGUSR1);
    sigaddset(&set, SIGUSR2);
    sigprocmask(SIG_SETMASK, &set, NULL);
    
    memset(&sa, 0, sizeof(struct sigaction));
    sa.sa_handler = handle_signal;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGQUIT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGUSR1, &sa, NULL);
    sigaction(SIGUSR2, &sa, NULL);
}
#endif

#if defined(__FreeBSD__) || defined(__APPLE__)
static int ppoll(struct pollfd *fds, nfds_t nfds, const struct timespec *timeout, const sigset_t *sigmask)
{
    int ready;
    sigset_t origmask;
    int to = timeout->tv_sec*1000 + timeout->tv_nsec/1000000;

    sigprocmask(SIG_SETMASK, sigmask, &origmask);
    ready = poll(fds, nfds, to);
    sigprocmask(SIG_SETMASK, &origmask, NULL);

    return ready;
}
#else if defined(WIN32)
static int ppoll(struct pollfd *fds, int nfds, const struct timespec *timeout)
{
    int to = timeout->tv_sec*1000 + timeout->tv_nsec/1000000;

    return WSAPoll(fds, nfds, to);
}
#endif

int main_loop(int listenfd)
{
    int to, cnt, i, dto;
    struct fdlist pollfds;
    struct timespec tspec;

#ifndef WIN32
    sigset_t empty_sigset;
    sigemptyset(&empty_sigset); // unmask all signals
#endif

    fdlist_create(&pollfds);
    while(!should_exit) {
        usbmuxd_log(LL_FLOOD, "main_loop iteration");
        to = usb_get_timeout();
        usbmuxd_log(LL_FLOOD, "USB timeout is %d ms", to);
        dto = device_get_timeout();
        usbmuxd_log(LL_FLOOD, "Device timeout is %d ms", to);
        if(dto < to)
            to = dto;

        fdlist_reset(&pollfds);
        fdlist_add(&pollfds, FD_LISTEN, listenfd, POLLIN);
        usb_get_fds(&pollfds);
        client_get_fds(&pollfds);
        usbmuxd_log(LL_FLOOD, "fd count is %d", pollfds.count);

        tspec.tv_sec = to / 1000;
        tspec.tv_nsec = (to % 1000) * 1000000;
#ifndef WIN32
        cnt = ppoll(pollfds.fds, pollfds.count, &tspec, &empty_sigset);
#else
        cnt = ppoll(pollfds.fds, pollfds.count, &tspec);
#endif
        usbmuxd_log(LL_FLOOD, "poll() returned %d", cnt);
        if(cnt == -1) {
            if(errno == EINTR) {
                if(should_exit) {
                    usbmuxd_log(LL_INFO, "Event processing interrupted");
                    break;
                }
                if(should_discover) {
                    should_discover = 0;
                    usbmuxd_log(LL_INFO, "Device discovery triggered by udev");
                    usb_discover();
                }
            }
        } else if(cnt == 0) {
            if(usb_process() < 0) {
                usbmuxd_log(LL_FATAL, "usb_process() failed");
                fdlist_free(&pollfds);
                return -1;
            }
            device_check_timeouts();
        } else {
            int done_usb = 0;
            for(i=0; i<pollfds.count; i++) {
                if(pollfds.fds[i].revents) {
                    if(!done_usb && pollfds.owners[i] == FD_USB) {
                        if(usb_process() < 0) {
                            usbmuxd_log(LL_FATAL, "usb_process() failed");
                            fdlist_free(&pollfds);
                            return -1;
                        }
                        done_usb = 1;
                    }
                    if(pollfds.owners[i] == FD_LISTEN) {
                        if(client_accept(listenfd) < 0) {
                            usbmuxd_log(LL_FATAL, "client_accept() failed");
                            fdlist_free(&pollfds);
                            return -1;
                        }
                    }
                    if(pollfds.owners[i] == FD_CLIENT) {
                        client_process(pollfds.fds[i].fd, pollfds.fds[i].revents);
                    }
                }
            }
        }
    }
    fdlist_free(&pollfds);
    return 0;
}

// TODO: revise code for WIN32 service 
/**
 * make this program run detached from the current console
 */
//static int daemonize(void)
//{
//    pid_t pid;
//    pid_t sid;
//    int pfd[2];
//    int res;
//
//    // already a daemon
//    if (getppid() == 1)
//        return 0;
//
//    if((res = pipe(pfd)) < 0) {
//        usbmuxd_log(LL_FATAL, "pipe() failed.");
//        return res;
//    }
//
//    pid = fork();
//    if (pid < 0) {
//        usbmuxd_log(LL_FATAL, "fork() failed.");
//        return pid;
//    }
//
//    if (pid > 0) {
//        // exit parent process
//        int status;
//        close(pfd[1]);
//
//        if((res = read(pfd[0],&status,sizeof(int))) != sizeof(int)) {
//            fprintf(stderr, "usbmuxd: ERROR: Failed to get init status from child, check syslog for messages.\n");
//            exit(1);
//        }
//        if(status != 0)
//            fprintf(stderr, "usbmuxd: ERROR: Child process exited with error %d, check syslog for messages.\n", status);
//        exit(status);
//    }
//    // At this point we are executing as the child process
//    // but we need to do one more fork
//
//    daemon_pipe = pfd[1];
//    close(pfd[0]);
//    report_to_parent = 1;
//
//    // Change the file mode mask
//    umask(0);
//
//    // Create a new SID for the child process
//    sid = setsid();
//    if (sid < 0) {
//        usbmuxd_log(LL_FATAL, "setsid() failed.");
//        return -1;
//    }
//
//    pid = fork();
//    if (pid < 0) {
//        usbmuxd_log(LL_FATAL, "fork() failed (second).");
//        return pid;
//    }
//
//    if (pid > 0) {
//        // exit parent process
//        close(daemon_pipe);
//        exit(0);
//    }
//
//    // Change the current working directory.
//    if ((chdir("/")) < 0) {
//        usbmuxd_log(LL_FATAL, "chdir() failed");
//        return -2;
//    }
//    // Redirect standard files to /dev/null
//    if (!freopen("/dev/null", "r", stdin)) {
//        usbmuxd_log(LL_FATAL, "Redirection of stdin failed.");
//        return -3;
//    }
//    if (!freopen("/dev/null", "w", stdout)) {
//        usbmuxd_log(LL_FATAL, "Redirection of stdout failed.");
//        return -3;
//    }
//
//    return 0;
//}

static int notify_parent(int status)
{
    int res;

    report_to_parent = 0;
    if ((res = write(daemon_pipe, &status, sizeof(int))) != sizeof(int)) {
        usbmuxd_log(LL_FATAL, "Could not notify parent!");
        if(res >= 0)
            return -2;
        else
            return res;
    }
    close(daemon_pipe);
    if (!freopen("/dev/null", "w", stderr)) {
        usbmuxd_log(LL_FATAL, "Redirection of stderr failed.");
        return -1;
    }
    return 0;
}

static void usage()
{
    printf("usage: usbmuxd [options]\n");
    printf("\t-h|--help                 Print this message.\n");
    printf("\t-v|--verbose              Be verbose (use twice or more to increase).\n");
    printf("\t-f|--foreground           Do not daemonize (implies one -v).\n");
    printf("\t-U|--user USER            Change to this user after startup (needs usb privileges).\n");
    printf("\t-u|--udev                 Run in udev operation mode.\n");
    printf("\t-x|--exit                 Tell a running instance to exit if there are no devices\n");
    printf("\t                          connected (must be in udev mode).\n");
    printf("\t-X|--force-exit           Tell a running instance to exit, even if there are still\n");
    printf("\t                          devices connected (always works).\n");
    printf("\n");
}

static void parse_opts(int argc, char **argv)
{
    static struct option longopts[] = {
        {"help", 0, NULL, 'h'},
        {"foreground", 0, NULL, 'f'},
        {"verbose", 0, NULL, 'v'},
        {"user", 2, NULL, 'U'},
        {"udev", 0, NULL, 'u'},
        {"exit", 0, NULL, 'x'},
        {"force-exit", 0, NULL, 'X'},
        {NULL, 0, NULL, 0}
    };
    int c;

    while (1) {
        c = getopt_long(argc, argv, "hfvuU:xX", longopts, (int *) 0);
        if (c == -1) {
            break;
        }

        switch (c) {
        case 'h':
            usage();
            exit(0);
        case 'f':
            foreground = 1;
            break;
        case 'v':
            ++verbose;
            break;
        case 'U':
            drop_privileges = 1;
            drop_user = optarg;
            break;
        case 'u':
            opt_udev = 1;
            break;
        case 'x':
            opt_exit = 1;
            /* me exit_signal = SIGUSR1; */
            break;
        case 'X':
            opt_exit = 1;
            exit_signal = SIGTERM;
            break;
        default:
            usage();
            exit(2);
        }
    }
}

int main(int argc, char *argv[])
{
    int listenfd;
    int res = 0;
    int lfd;
#ifndef WIN32
    struct flock lock;
    char pids[10];
#endif

    parse_opts(argc, argv);

    argc -= optind;
    argv += optind;

    if (!foreground) {
        verbose += LL_WARNING;
        log_enable_syslog();
    } else {
        verbose += LL_NOTICE;
    }

    /* set log level to specified verbosity */
    log_level = verbose;

    usbmuxd_log(LL_NOTICE, "usbmuxd v%s starting up", USBMUXD_VERSION);
    should_exit = 0;
    should_discover = 0;

// TODO: revise code
#ifndef WIN32
    set_signal_handlers();
    signal(SIGPIPE, SIG_IGN);

    res = lfd = open(lockfile, O_WRONLY|O_CREAT, 0644);
    if(res == -1) {
        usbmuxd_log(LL_FATAL, "Could not open lockfile");
        goto terminate;
    }
    lock.l_type = F_WRLCK;
    lock.l_whence = SEEK_SET;
    lock.l_start = 0;
    lock.l_len = 0;
    fcntl(lfd, F_GETLK, &lock);
    close(lfd);
    if (lock.l_type != F_UNLCK) { // if there's a lock (i.e. instance of usbmuxd is already running)
        if (opt_exit) {
            if (lock.l_pid && !kill(lock.l_pid, 0)) {
                usbmuxd_log(LL_NOTICE, "Sending signal %d to instance with pid %d", exit_signal, lock.l_pid);
                res = 0;
                if (kill(lock.l_pid, exit_signal) < 0) {
                    usbmuxd_log(LL_FATAL, "Could not deliver signal %d to pid %d", exit_signal, lock.l_pid);
                    res = -1;
                }
                goto terminate;
            } else {
                usbmuxd_log(LL_ERROR, "Could not determine pid of the other running instance!");
                res = -1;
                goto terminate;
            }
        } else {
            if (!opt_udev) {
                usbmuxd_log(LL_ERROR, "Another instance is already running (pid %d). exiting.", lock.l_pid);
                res = -1;
            } else {
                usbmuxd_log(LL_NOTICE, "Another instance is already running (pid %d). Telling it to check for devices.", lock.l_pid);
                if (lock.l_pid && !kill(lock.l_pid, 0)) {
                    usbmuxd_log(LL_NOTICE, "Sending signal SIGUSR2 to instance with pid %d", lock.l_pid);
                    res = 0;
                    if (kill(lock.l_pid, SIGUSR2) < 0) {
                        usbmuxd_log(LL_FATAL, "Could not deliver SIGUSR2 to pid %d", lock.l_pid);
                        res = -1;
                    }
                } else {
                    usbmuxd_log(LL_ERROR, "Could not determine pid of the other running instance!");
                    res = -1;
                }
            }
            goto terminate;
        }
    }
    unlink(lockfile);

    if (opt_exit) {
        usbmuxd_log(LL_NOTICE, "No running instance found, none killed. exiting.");
        goto terminate;
    }

    // TODO: revise code
    //if (!foreground) {
    //    if ((res = daemonize()) < 0) {
    //        fprintf(stderr, "usbmuxd: FATAL: Could not daemonize!\n");
    //        usbmuxd_log(LL_FATAL, "Could not daemonize!");
    //        goto terminate;
    //    }
    //}

    // now open the lockfile and place the lock
    res = lfd = open(lockfile, O_WRONLY|O_CREAT|O_TRUNC|O_EXCL, 0644);
    if(res < 0) {
        usbmuxd_log(LL_FATAL, "Could not open lockfile");
        goto terminate;
    }
    lock.l_type = F_WRLCK;
    lock.l_whence = SEEK_SET;
    lock.l_start = 0;
    lock.l_len = 0;
    if ((res = fcntl(lfd, F_SETLK, &lock)) < 0) {
        usbmuxd_log(LL_FATAL, "Lockfile locking failed!");
        goto terminate;
    }
    sprintf(pids, "%d", getpid());
    if ((res = write(lfd, pids, strlen(pids))) != strlen(pids)) {
        usbmuxd_log(LL_FATAL, "Could not write pidfile!");
        if(res >= 0)
            res = -2;
        goto terminate;
    }
#endif

    usbmuxd_log(LL_INFO, "Creating socket");

#ifdef WIN32
    res = listenfd = create_socket(USBMUXD_SOCKET_PORT);
#else	
    res = listenfd = create_socket();
#endif

    if(listenfd < 0)
        goto terminate;

#ifndef WIN32
    // drop elevated privileges
    if (drop_privileges && (getuid() == 0 || geteuid() == 0)) {
        struct passwd *pw;
        if (!drop_user) {
            usbmuxd_log(LL_FATAL, "No user to drop privileges to?");
            res = -1;
            goto terminate;
        }
        pw = getpwnam(drop_user);
        if (!pw) {
            usbmuxd_log(LL_FATAL, "Dropping privileges failed, check if user '%s' exists!", drop_user);
            res = -1;
            goto terminate;
        }
        if (pw->pw_uid == 0) {
            usbmuxd_log(LL_INFO, "Not dropping privileges to root");
        } else {
            if ((res = initgroups(drop_user, pw->pw_gid)) < 0) {
                usbmuxd_log(LL_FATAL, "Failed to drop privileges (cannot set supplementary groups)");
                goto terminate;
            }
            if ((res = setgid(pw->pw_gid)) < 0) {
                usbmuxd_log(LL_FATAL, "Failed to drop privileges (cannot set group ID to %d)", pw->pw_gid);
                goto terminate;
            }
            if ((res = setuid(pw->pw_uid)) < 0) {
                usbmuxd_log(LL_FATAL, "Failed to drop privileges (cannot set user ID to %d)", pw->pw_uid);
                goto terminate;
            }

            // security check
            if (setuid(0) != -1) {
                usbmuxd_log(LL_FATAL, "Failed to drop privileges properly!");
                res = -1;
                goto terminate;
            }
            if (getuid() != pw->pw_uid || getgid() != pw->pw_gid) {
                usbmuxd_log(LL_FATAL, "Failed to drop privileges properly!");
                res = -1;
                goto terminate;
            }
            usbmuxd_log(LL_NOTICE, "Successfully dropped privileges to '%s'", drop_user);
        }
    }
#endif

    client_init();
    device_init();
    usbmuxd_log(LL_INFO, "Initializing USB");
    if((res = usb_init()) < 0)
        goto terminate;

    usbmuxd_log(LL_INFO, "%d device%s detected", res, (res==1)?"":"s");

    usbmuxd_log(LL_NOTICE, "Initialization complete");

    if (report_to_parent)
        if((res = notify_parent(0)) < 0)
            goto terminate;

    if(opt_udev)
        usb_autodiscover(0); // discovery triggered by udev

    res = main_loop(listenfd);
    if(res < 0)
        usbmuxd_log(LL_FATAL, "main_loop failed");

    usbmuxd_log(LL_NOTICE, "usbmuxd shutting down");
    device_kill_connections();
    usb_shutdown();
    device_shutdown();
    client_shutdown();
    usbmuxd_log(LL_NOTICE, "Shutdown complete");

terminate:
    log_disable_syslog();

    if (res < 0)
        res = -res;
    else
        res = 0;
    if (report_to_parent)
        notify_parent(res);

    return res;
}
