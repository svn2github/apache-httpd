/* ====================================================================
 * The Apache Software License, Version 1.1
 *
 * Copyright (c) 2000-2001 The Apache Software Foundation.  All rights
 * reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in
 *    the documentation and/or other materials provided with the
 *    distribution.
 *
 * 3. The end-user documentation included with the redistribution,
 *    if any, must include the following acknowledgment:
 *       "This product includes software developed by the
 *        Apache Software Foundation (http://www.apache.org/)."
 *    Alternately, this acknowledgment may appear in the software itself,
 *    if and wherever such third-party acknowledgments normally appear.
 *
 * 4. The names "Apache" and "Apache Software Foundation" must
 *    not be used to endorse or promote products derived from this
 *    software without prior written permission. For written
 *    permission, please contact apache@apache.org.
 *
 * 5. Products derived from this software may not be called "Apache",
 *    nor may "Apache" appear in their name, without prior written
 *    permission of the Apache Software Foundation.
 *
 * THIS SOFTWARE IS PROVIDED ``AS IS'' AND ANY EXPRESSED OR IMPLIED
 * WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED.  IN NO EVENT SHALL THE APACHE SOFTWARE FOUNDATION OR
 * ITS CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF
 * USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
 * ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
 * OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT
 * OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 * ====================================================================
 *
 * This software consists of voluntary contributions made by many
 * individuals on behalf of the Apache Software Foundation.  For more
 * information on the Apache Software Foundation, please see
 * <http://www.apache.org/>.
 *
 * Portions of this software are based upon public domain software
 * originally written at the National Center for Supercomputing Applications,
 * University of Illinois, Urbana-Champaign.
 */

/*
 * This file will include OS specific functions which are not inlineable.
 * Any inlineable functions should be defined in os-inline.c instead.
 */

#include "httpd.h"
#include "http_core.h"
#include "os.h"
#include "scoreboard.h"
#include "http_log.h"

static FILE *sock_fp;

int tpf_select(int maxfds, fd_set *reads, fd_set *writes, fd_set *excepts, struct timeval *tv)
{
/* We're going to force our way through select.  We're only interested reads and TPF allows
   2billion+ socket descriptors for we don't want an fd_set that big.  Just assume that maxfds-1
   contains the socket descriptor we're interested in.  If it's 0, leave it alone. */

    int sockets[1];
    int no_reads = 0;
    int no_writes = 0;
    int no_excepts = 0;
    int timeout = 0;
    int rv;
    
    if(maxfds) {
        if(tv)
            timeout = tv->tv_sec * 1000 + tv->tv_usec;
        sockets[0] = maxfds-1;
        no_reads++;
    }
        else
        sockets[0] = 0;
        
    ap_check_signals();
    rv = select(sockets, no_reads, no_writes, no_excepts, timeout);
    ap_check_signals();
    
    return rv;

}

int tpf_accept(int sockfd, struct sockaddr *peer, int *paddrlen)
{
    int socks[1];
    int rv;

    ap_check_signals();
    socks[0] = sockfd;
    rv = select(socks, 1, 0, 0, 1000);
    errno = sock_errno();
    if(rv>0) {
        ap_check_signals();
        rv = accept(sockfd, peer, paddrlen);
        errno = sock_errno();
    }    
    return rv;
}
   
/* the getpass function is not usable on TPF */
char *getpass(const char* prompt)
{
    errno = EIO;
    return((char *)NULL);
}

#ifndef __PIPE_
int pipe(int fildes[2])
{
    errno = ENOSYS;
    return(-1);
}
#endif
  
/* fork and exec functions are not defined on
   TPF due to the implementation of tpf_fork() */
 
pid_t fork(void)
{
    errno = ENOSYS;
    return(-1);
}

int execl(const char *path, const char *arg0, ...)
{
    errno = ENOSYS;
    return(-1);
}

int execle(const char *path, const char *arg0, ...)
{
    errno = ENOSYS;
    return(-1);
}

int execve(const char *path, char *const argv[], char *const envp[])
{
    errno = ENOSYS;
    return(-1);
}

int execvp(const char *file, char *const argv[])
{
    errno = ENOSYS;
    return(-1);
}



int ap_tpf_spawn_child(apr_pool_t *p, int (*func) (void *, child_info *),
                       void *data, enum kill_conditions kill_how,
                       int *pipe_in, int *pipe_out, int *pipe_err,
                       int out_fds[], int in_fds[], int err_fds[])

{

   int                      i, temp_out, temp_in, temp_err, save_errno, pid, result=0;
   int                      fd_flags_out, fd_flags_in, fd_flags_err;
   struct tpf_fork_input    fork_input;
   TPF_FORK_CHILD           *cld = (TPF_FORK_CHILD *) data;
   apr_array_header_t       *env_arr = apr_table_elts ((array_header *) cld->subprocess_env);
   table_entry              *elts = (table_entry *) env_arr->elts;



   if (func) {
      if (result=func(data, NULL)) {
          return 0;                    /* error from child function */
      }
   }

   if (pipe_out) {
      fd_flags_out = fcntl(out_fds[0], F_GETFD);
      fcntl(out_fds[0], F_SETFD, FD_CLOEXEC);
      temp_out = dup(STDOUT_FILENO);
      fcntl(temp_out, F_SETFD, FD_CLOEXEC);
      dup2(out_fds[1], STDOUT_FILENO);
   }


   if (pipe_in) {
      fd_flags_in = fcntl(in_fds[1], F_GETFD);
      fcntl(in_fds[1], F_SETFD, FD_CLOEXEC);
      temp_in = dup(STDIN_FILENO);
      fcntl(temp_in, F_SETFD, FD_CLOEXEC);
      dup2(in_fds[0], STDIN_FILENO);
   }

   if (pipe_err) {
      fd_flags_err = fcntl(err_fds[0], F_GETFD);
      fcntl(err_fds[0], F_SETFD, FD_CLOEXEC);
      temp_err = dup(STDERR_FILENO);
      fcntl(temp_err, F_SETFD, FD_CLOEXEC);
      dup2(err_fds[1], STDERR_FILENO);
   }

   if (cld->subprocess_env) {
      for (i = 0; i < env_arr->nelts; ++i) {
           if (!elts[i].key)
               continue;
           setenv (elts[i].key, elts[i].val, 1);
       }
   }

   fork_input.program = (const char*) cld->filename;
   fork_input.prog_type = cld->prog_type;
   fork_input.istream = TPF_FORK_IS_BALANCE;
   fork_input.ebw_data_length = 0;
   fork_input.ebw_data = NULL;
   fork_input.parm_data = NULL;


   if ((pid = tpf_fork(&fork_input)) < 0) {
       save_errno = errno;
       if (pipe_out) {
           close(out_fds[0]);
       }
       if (pipe_in) {
           close(in_fds[1]);
       }
       if (pipe_err) {
           close(err_fds[0]);
       }
       errno = save_errno;
       pid = 0;
   }

   if (cld->subprocess_env) {
       for (i = 0; i < env_arr->nelts; ++i) {
            if (!elts[i].key)
                continue;
            unsetenv (elts[i].key);
       }
   }

   if (pipe_out) {
       close(out_fds[1]);
       dup2(temp_out, STDOUT_FILENO);
       close(temp_out);
       fcntl(out_fds[0], F_SETFD, fd_flags_out);
   }

   if (pipe_in) {
       close(in_fds[0]);
       dup2(temp_in, STDIN_FILENO);
       close(temp_in);
       fcntl(in_fds[1], F_SETFD, fd_flags_in);
   }


   if (pipe_err) {
       close(err_fds[1]);
       dup2(temp_err, STDERR_FILENO);
       close(temp_err);
       fcntl(err_fds[0], F_SETFD, fd_flags_err);
   }


   if (pid) {

       apr_pool_note_subprocess(p, pid, kill_how);

       if (pipe_out) {
          *pipe_out = out_fds[0];
       }
       if (pipe_in) {
          *pipe_in = in_fds[1];
       }
       if (pipe_err) {
          *pipe_err = err_fds[0];
       }
   }

   return pid;

}

pid_t os_fork(server_rec *s, int slot)
{
    struct tpf_fork_input fork_input;
    APACHE_TPF_INPUT input_parms;
    int count;
    listen_rec *lr;

    fflush(stdin);
    if (dup2(fileno(sock_fp), STDIN_FILENO) == -1)
        ap_log_error(APLOG_MARK, APLOG_CRIT, errno, s,
        "unable to replace stdin with sock device driver");
    fflush(stdout);
    if (dup2(fileno(sock_fp), STDOUT_FILENO) == -1)
        ap_log_error(APLOG_MARK, APLOG_CRIT, errno, s,
        "unable to replace stdout with sock device driver");
    input_parms.generation = ap_my_generation;
#ifdef SCOREBOARD_FILE
    input_parms.scoreboard_fd = scoreboard_fd;
#else /* must be USE_TPF_SCOREBOARD or USE_SHMGET_SCOREBOARD */
    input_parms.scoreboard_heap = ap_scoreboard_image;
#endif

    lr = ap_listeners;
    count = 0;
    do {
        input_parms.listeners[count] = lr->fd;
        lr = lr->next;
        count++;
    } while(lr != ap_listeners);

    input_parms.slot = slot;
    input_parms.restart_time = ap_restart_time;
    fork_input.ebw_data = &input_parms;
    fork_input.program = ap_server_argv0;
    fork_input.prog_type = TPF_FORK_NAME;
    fork_input.istream = TPF_FORK_IS_BALANCE;
    fork_input.ebw_data_length = sizeof(input_parms);
    fork_input.parm_data = "-x";
    return tpf_fork(&fork_input);
}

int os_check_server(char *server) {
#ifndef USE_TPF_DAEMON
    int rv;
    int *current_acn;
    if((rv = inetd_getServerStatus(server)) == INETD_SERVER_STATUS_INACTIVE)
        return 1;
    else {
        current_acn = (int *)cinfc_fast(CINFC_CMMACNUM);
        if(ecbp2()->ce2acn != *current_acn)
            return 1;
    }
#endif
    return 0;
}

void os_note_additional_cleanups(apr_pool_t *p, int sd) {
    char sockfilename[50];
    /* write the socket to file so that TPF socket device driver will close socket in case
       we happen to abend. */
    sprintf(sockfilename, "/dev/tpf.socket.file/%.8X", sd);
    sock_fp = fopen(sockfilename, "r+");
    ap_note_cleanups_for_file(p, sock_fp);  /* arrange to close on exec or restart */
    fcntl(sd,F_SETFD,FD_CLOEXEC);
}

void os_tpf_child(APACHE_TPF_INPUT *input_parms) {
    tpf_child = 1;
    ap_my_generation = input_parms->generation;
    ap_restart_time = input_parms->restart_time;
}

AP_DECLARE(apr_status_t) ap_os_create_privileged_process(
    const request_rec *r,
    apr_proc_t *newproc, const char *progname,
    const char * const *args,
    const char * const *env,
    apr_procattr_t *attr, apr_pool_t *p)
{
    return apr_proc_create(newproc, progname, args, env, attr, p);
}
