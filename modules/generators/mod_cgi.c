/* ====================================================================
 * The Apache Software License, Version 1.1
 *
 * Copyright (c) 2000 The Apache Software Foundation.  All rights
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
 * http_script: keeps all script-related ramblings together.
 * 
 * Compliant to CGI/1.1 spec
 * 
 * Adapted by rst from original NCSA code by Rob McCool
 *
 * Apache adds some new env vars; REDIRECT_URL and REDIRECT_QUERY_STRING for
 * custom error responses, and DOCUMENT_ROOT because we found it useful.
 * It also adds SERVER_ADMIN - useful for scripts to know who to mail when 
 * they fail.
 */

#include "apr.h"
#include "apr_strings.h"
#include "apr_thread_proc.h"    /* for RLIMIT stuff */

#define CORE_PRIVATE

#include "ap_buckets.h"
#include "util_filter.h"
#include "ap_config.h"
#include "httpd.h"
#include "http_config.h"
#include "http_request.h"
#include "http_core.h"
#include "http_protocol.h"
#include "http_main.h"
#include "http_log.h"
#include "util_script.h"
#include "ap_mpm.h"
#include "http_conf_globals.h"
#ifdef HAVE_STRING_H
#include <string.h>
#endif
#ifdef HAVE_STRINGS_H
#include <strings.h>
#endif

module AP_MODULE_DECLARE_DATA cgi_module;

/* KLUDGE --- for back-combatibility, we don't have to check ExecCGI
 * in ScriptAliased directories, which means we need to know if this
 * request came through ScriptAlias or not... so the Alias module
 * leaves a note for us.
 */

static int is_scriptaliased(request_rec *r)
{
    const char *t = apr_table_get(r->notes, "alias-forced-type");
    return t && (!strcasecmp(t, "cgi-script"));
}

/* Configuration stuff */

#define DEFAULT_LOGBYTES 10385760
#define DEFAULT_BUFBYTES 1024

typedef struct {
    const char *logname;
    long logbytes;
    int bufbytes;
} cgi_server_conf;

static void *create_cgi_config(apr_pool_t *p, server_rec *s)
{
    cgi_server_conf *c =
    (cgi_server_conf *) apr_pcalloc(p, sizeof(cgi_server_conf));

    c->logname = NULL;
    c->logbytes = DEFAULT_LOGBYTES;
    c->bufbytes = DEFAULT_BUFBYTES;

    return c;
}

static void *merge_cgi_config(apr_pool_t *p, void *basev, void *overridesv)
{
    cgi_server_conf *base = (cgi_server_conf *) basev, *overrides = (cgi_server_conf *) overridesv;

    return overrides->logname ? overrides : base;
}

static const char *set_scriptlog(cmd_parms *cmd, void *dummy, const char *arg)
{
    server_rec *s = cmd->server;
    cgi_server_conf *conf =
    (cgi_server_conf *) ap_get_module_config(s->module_config, &cgi_module);

    conf->logname = arg;
    return NULL;
}

static const char *set_scriptlog_length(cmd_parms *cmd, void *dummy,
					const char *arg)
{
    server_rec *s = cmd->server;
    cgi_server_conf *conf =
    (cgi_server_conf *) ap_get_module_config(s->module_config, &cgi_module);

    conf->logbytes = atol(arg);
    return NULL;
}

static const char *set_scriptlog_buffer(cmd_parms *cmd, void *dummy,
					const char *arg)
{
    server_rec *s = cmd->server;
    cgi_server_conf *conf =
    (cgi_server_conf *) ap_get_module_config(s->module_config, &cgi_module);

    conf->bufbytes = atoi(arg);
    return NULL;
}

static const command_rec cgi_cmds[] =
{
AP_INIT_TAKE1("ScriptLog", set_scriptlog, NULL, RSRC_CONF,
     "the name of a log for script debugging info"),
AP_INIT_TAKE1("ScriptLogLength", set_scriptlog_length, NULL, RSRC_CONF,
     "the maximum length (in bytes) of the script debug log"),
AP_INIT_TAKE1("ScriptLogBuffer", set_scriptlog_buffer, NULL, RSRC_CONF,
     "the maximum size (in bytes) to record of a POST request"),
    {NULL}
};

static int log_scripterror(request_rec *r, cgi_server_conf * conf, int ret,
			   apr_status_t rv, char *error)
{
    apr_file_t *f = NULL;
    apr_finfo_t finfo;
    char time_str[APR_CTIME_LEN];
    int log_flags = rv ? APLOG_ERR : APLOG_NOERRNO | APLOG_ERR;

    ap_log_rerror(APLOG_MARK, log_flags, rv, r, 
                  "%s: %s", error, r->filename);

    if (!conf->logname ||
        ((apr_stat(&finfo, ap_server_root_relative(r->pool, conf->logname), r->pool) == APR_SUCCESS)
         &&  (finfo.size > conf->logbytes)) ||
          (apr_open(&f, ap_server_root_relative(r->pool, conf->logname),
                   APR_APPEND|APR_WRITE|APR_CREATE, APR_OS_DEFAULT, r->pool) != APR_SUCCESS)) {
	return ret;
    }

    /* "%% [Wed Jun 19 10:53:21 1996] GET /cgi-bin/printenv HTTP/1.0" */
    apr_ctime(time_str, apr_now());
    apr_fprintf(f, "%%%% [%s] %s %s%s%s %s\n", time_str, r->method, r->uri,
	    r->args ? "?" : "", r->args ? r->args : "", r->protocol);
    /* "%% 500 /usr/local/apache/cgi-bin */
    apr_fprintf(f, "%%%% %d %s\n", ret, r->filename);

    apr_fprintf(f, "%%error\n%s\n", error);

    apr_close(f);
    return ret;
}

/* Soak up stderr from a script and redirect it to the error log. 
 */
static void log_script_err(request_rec *r, apr_file_t *script_err)
{
    char argsbuffer[HUGE_STRING_LEN];
    char *newline;

    while (apr_fgets(argsbuffer, HUGE_STRING_LEN, script_err) == 0) {
        newline = strchr(argsbuffer, '\n');
        if (newline) {
            *newline = '\0';
        }
        ap_log_rerror(APLOG_MARK, APLOG_ERR | APLOG_NOERRNO, 0, r, 
                      "%s", argsbuffer);            
    }
}

static int log_script(request_rec *r, cgi_server_conf * conf, int ret,
		  char *dbuf, const char *sbuf, apr_file_t *script_in, 
                  apr_file_t *script_err)
{
    apr_array_header_t *hdrs_arr = apr_table_elts(r->headers_in);
    apr_table_entry_t *hdrs = (apr_table_entry_t *) hdrs_arr->elts;
    char argsbuffer[HUGE_STRING_LEN];
    apr_file_t *f = NULL;
    int i;
    apr_finfo_t finfo;
    char time_str[APR_CTIME_LEN];

    if (!conf->logname ||
        ((apr_stat(&finfo, ap_server_root_relative(r->pool, conf->logname), r->pool) == APR_SUCCESS)
         &&  (finfo.size > conf->logbytes)) ||
         (apr_open(&f, ap_server_root_relative(r->pool, conf->logname),
                  APR_APPEND|APR_WRITE|APR_CREATE, APR_OS_DEFAULT, r->pool) != APR_SUCCESS)) {
	/* Soak up script output */
	while (apr_fgets(argsbuffer, HUGE_STRING_LEN, script_in) == 0)
	    continue;

        log_script_err(r, script_err);
	return ret;
    }

    /* "%% [Wed Jun 19 10:53:21 1996] GET /cgi-bin/printenv HTTP/1.0" */
    apr_ctime(time_str, apr_now());
    apr_fprintf(f, "%%%% [%s] %s %s%s%s %s\n", time_str, r->method, r->uri,
	    r->args ? "?" : "", r->args ? r->args : "", r->protocol);
    /* "%% 500 /usr/local/apache/cgi-bin" */
    apr_fprintf(f, "%%%% %d %s\n", ret, r->filename);

    apr_puts("%request\n", f);
    for (i = 0; i < hdrs_arr->nelts; ++i) {
	if (!hdrs[i].key)
	    continue;
	apr_fprintf(f, "%s: %s\n", hdrs[i].key, hdrs[i].val);
    }
    if ((r->method_number == M_POST || r->method_number == M_PUT)
	&& *dbuf) {
	apr_fprintf(f, "\n%s\n", dbuf);
    }

    apr_puts("%response\n", f);
    hdrs_arr = apr_table_elts(r->err_headers_out);
    hdrs = (apr_table_entry_t *) hdrs_arr->elts;

    for (i = 0; i < hdrs_arr->nelts; ++i) {
	if (!hdrs[i].key)
	    continue;
	apr_fprintf(f, "%s: %s\n", hdrs[i].key, hdrs[i].val);
    }

    if (sbuf && *sbuf)
	apr_fprintf(f, "%s\n", sbuf);

    if (apr_fgets(argsbuffer, HUGE_STRING_LEN, script_in) == 0) {
	apr_puts("%stdout\n", f);
	apr_puts(argsbuffer, f);
	while (apr_fgets(argsbuffer, HUGE_STRING_LEN, script_in) == 0)
	    apr_puts(argsbuffer, f);
	apr_puts("\n", f);
    }

    if (apr_fgets(argsbuffer, HUGE_STRING_LEN, script_err) == 0) {
	apr_puts("%stderr\n", f);
	apr_puts(argsbuffer, f);
	while (apr_fgets(argsbuffer, HUGE_STRING_LEN, script_err) == 0)
	    apr_puts(argsbuffer, f);
	apr_puts("\n", f);
    }

    apr_close(script_in);
    apr_close(script_err);

    apr_close(f);
    return ret;
}

static apr_status_t run_cgi_child(apr_file_t **script_out,
                                  apr_file_t **script_in,
                                  apr_file_t **script_err, 
                                  const char *command,
                                  const char * const argv[],
                                  request_rec *r, apr_pool_t *p)
{
    const char * const *env;
    apr_procattr_t *procattr;
    apr_proc_t *procnew = apr_pcalloc(p, sizeof(*procnew));
    apr_status_t rc = APR_SUCCESS;
#if defined(RLIMIT_CPU)  || defined(RLIMIT_NPROC) || \
    defined(RLIMIT_DATA) || defined(RLIMIT_VMEM) || defined (RLIMIT_AS)
    core_dir_config *conf;
    conf = (core_dir_config *) ap_get_module_config(r->per_dir_config,                                                              &core_module);
#endif


#ifdef DEBUG_CGI
#ifdef OS2
    /* Under OS/2 need to use device con. */
    FILE *dbg = fopen("con", "w");
#else
    FILE *dbg = fopen("/dev/tty", "w");
#endif
    int i;
#endif

    RAISE_SIGSTOP(CGI_CHILD);
#ifdef DEBUG_CGI
    fprintf(dbg, "Attempting to exec %s as %sCGI child (argv0 = %s)\n",
	    r->filename, cld->nph ? "NPH " : "", argv0);
#endif

    ap_add_cgi_vars(r);
    env = (const char * const *)ap_create_environment(p, r->subprocess_env);

#ifdef DEBUG_CGI
    fprintf(dbg, "Environment: \n");
    for (i = 0; env[i]; ++i)
	fprintf(dbg, "'%s'\n", env[i]);
#endif

    /* Transmute ourselves into the script.
     * NB only ISINDEX scripts get decoded arguments.
     */
    if (((rc = apr_createprocattr_init(&procattr, p)) != APR_SUCCESS) ||
        ((rc = apr_setprocattr_io(procattr, 
                                 APR_CHILD_BLOCK, 
                                 APR_CHILD_BLOCK,
                                 APR_CHILD_BLOCK)) != APR_SUCCESS) ||
        ((rc = apr_setprocattr_dir(procattr, 
                                  ap_make_dirstr_parent(r->pool, r->filename))) != APR_SUCCESS) ||
#ifdef RLIMIT_CPU
        ((rc = apr_setprocattr_limit(procattr, APR_LIMIT_CPU, conf->limit_cpu)) != APR_SUCCESS) ||
#endif
#if defined(RLIMIT_DATA) || defined(RLIMIT_VMEM) || defined(RLIMIT_AS)
        ((rc = apr_setprocattr_limit(procattr, APR_LIMIT_MEM, conf->limit_mem)) != APR_SUCCESS) ||
#endif
#ifdef RLIMIT_NPROC
        ((rc = apr_setprocattr_limit(procattr, APR_LIMIT_NPROC, conf->limit_nproc)) != APR_SUCCESS) ||
#endif
        ((rc = apr_setprocattr_cmdtype(procattr, APR_PROGRAM)) != APR_SUCCESS)) {
        /* Something bad happened, tell the world. */
	ap_log_rerror(APLOG_MARK, APLOG_ERR, rc, r,
		      "couldn't set child process attributes: %s", r->filename);
    }
    else {
        rc = ap_os_create_privileged_process(r, procnew, command, argv, env, procattr, p);
    
        if (rc != APR_SUCCESS) {
            /* Bad things happened. Everyone should have cleaned up. */
            ap_log_rerror(APLOG_MARK, APLOG_ERR, rc, r,
                        "couldn't create child process: %d: %s", rc, r->filename);
        }
        else {
            apr_note_subprocess(p, procnew, kill_after_timeout);

            *script_in = procnew->out;
            if (!script_in)
                return APR_EBADF;
            apr_set_pipe_timeout(*script_in, 
                                 (int)(r->server->timeout * APR_USEC_PER_SEC));

            *script_out = procnew->in;
            if (!*script_out)
                return APR_EBADF;
            apr_set_pipe_timeout(*script_out, 
                                 (int)(r->server->timeout * APR_USEC_PER_SEC));

            *script_err = procnew->err;
            if (!*script_err)
                return APR_EBADF;
            apr_set_pipe_timeout(*script_err, 
                                 (int)(r->server->timeout * APR_USEC_PER_SEC));
        }
    }
    return (rc);
}

static apr_status_t build_argv_list(const char ***argv, request_rec *r,
                                    apr_pool_t *p)
{
    int numwords, x, idx;
    char *w;
    const char *args = r->args;

    if (!args || !args[0] || ap_strchr_c(args, '=')) {
        numwords = 1;
    }
    else {
        /* count the number of keywords */
        for (x = 0, numwords = 2; args[x]; x++) {
            if (args[x] == '+') {
                ++numwords;
            }
        }
    }
    /* Everything is - 1 to account for the first parameter which is the
     * program name.  We didn't used to have to do this, but APR wants it.
     */ 
    if (numwords > APACHE_ARG_MAX - 1) {
        numwords = APACHE_ARG_MAX - 1;	/* Truncate args to prevent overrun */
    }
    *argv = apr_palloc(p, (numwords + 2) * sizeof(char *));
 
    for (x = 1, idx = 1; x < numwords; x++) {
        w = ap_getword_nulls(p, &args, '+');
        ap_unescape_url(w);
        (*argv)[idx++] = ap_escape_shell_cmd(p, w);
    }
    (*argv)[idx] = NULL;

    return APR_SUCCESS;
}

static apr_status_t build_command_line(const char **cmd, request_rec *r,
                                       apr_pool_t *p)
{
#ifdef WIN32
    char *quoted_filename = NULL;
    char *interpreter = NULL;
    char *arguments = NULL;
    file_type_e fileType;
#endif
    const char *argv0;

    /* Allow suexec's "/" check to succeed */
    if ((argv0 = strrchr(r->filename, '/')) != NULL)
        argv0++;
    else
        argv0 = r->filename;

#ifdef WIN32 
    *cmd = NULL;
    fileType = ap_get_win32_interpreter(r, &interpreter, &arguments);

    if (fileType == eFileTypeUNKNOWN) {
        ap_log_rerror(APLOG_MARK, APLOG_ERR|APLOG_NOERRNO, 0, r,
                      "%s is not executable; ensure interpreted scripts have "
                      "\"#!\" first line", 
                      r->filename);
        return APR_EBADF;
    }

    /*
     * Build the command string to pass to ap_os_create_privileged_process()
     */
    quoted_filename = apr_pstrcat(p, "\"", r->filename, "\"", NULL);
    if (interpreter && *interpreter) {
        if (arguments && *arguments)
            *cmd = apr_pstrcat(p, interpreter, " ", quoted_filename, " ", 
                              arguments, NULL);
        else
            *cmd = apr_pstrcat(p, interpreter, " ", quoted_filename, " ", NULL);
    }
    else if (arguments && *arguments) {
        *cmd = apr_pstrcat(p, quoted_filename, " ", arguments, NULL);
    }
    else {
        *cmd = apr_pstrcat(p, quoted_filename, NULL);
    }
#else
    *cmd = argv0;
#endif
    return APR_SUCCESS;
}

static int cgi_handler(request_rec *r)
{
    int retval, nph, dbpos = 0;
    const char *argv0;
    const char *command;
    const char **argv;
    char *dbuf = NULL;
    apr_file_t *script_out = NULL, *script_in = NULL, *script_err = NULL;
    ap_bucket_brigade *bb;
    ap_bucket *b;
    char argsbuffer[HUGE_STRING_LEN];
    int is_included = !strcmp(r->protocol, "INCLUDED");
    apr_pool_t *p;
    cgi_server_conf *conf;
    apr_status_t rv;

    if(strcmp(r->handler,CGI_MAGIC_TYPE) && strcmp(r->handler,"cgi-script"))
	return DECLINED;

    p = r->main ? r->main->pool : r->pool;

    if (r->method_number == M_OPTIONS) {
	/* 99 out of 100 CGI scripts, this is all they support */
	r->allowed |= (1 << M_GET);
	r->allowed |= (1 << M_POST);
	return DECLINED;
    }

    if ((argv0 = strrchr(r->filename, '/')) != NULL)
	argv0++;
    else
	argv0 = r->filename;

    nph = !(strncmp(argv0, "nph-", 4));
    conf = ap_get_module_config(r->server->module_config, &cgi_module);

    if (!(ap_allow_options(r) & OPT_EXECCGI) && !is_scriptaliased(r))
        return log_scripterror(r, conf, HTTP_FORBIDDEN, 0,
                               "Options ExecCGI is off in this directory");
    if (nph && is_included)
        return log_scripterror(r, conf, HTTP_FORBIDDEN, 0,
                               "attempt to include NPH CGI script");

#if defined(OS2) || defined(WIN32)
    /* XXX: This is wrong.  As an option, perhaps, but not by default...
     *      we are back to the same argument that a url should be a unique
     *      entity until the sysadmin overrides that behavior.
     * Allow for cgi files without the .EXE extension on them under OS/2 
     */
    if (r->finfo.protection == 0) {
        apr_finfo_t finfo;
        char *newfile;
        apr_status_t rv;

        newfile = apr_pstrcat(r->pool, r->filename, ".EXE", NULL);
        if (((rv = apr_stat(&finfo, newfile, r->pool)) != APR_SUCCESS) || 
            (finfo.filetype != APR_REG)) {
            return log_scripterror(r, conf, HTTP_NOT_FOUND, rv,
                                   "script not found or unable to stat");
        } else {
            r->filename = newfile;
        }
    }
#else
    if (r->finfo.protection == 0)
	return log_scripterror(r, conf, HTTP_NOT_FOUND, 0,
			       "script not found or unable to stat");
#endif
    if (r->finfo.filetype == APR_DIR)
	return log_scripterror(r, conf, HTTP_FORBIDDEN, 0,
			       "attempt to invoke directory as script");

/*
    if (!ap_suexec_enabled) {
	if (!ap_can_exec(&r->finfo))
	    return log_scripterror(r, conf, HTTP_FORBIDDEN, 0,
				   "file permissions deny server execution");
    }

*/
    if ((retval = ap_setup_client_block(r, REQUEST_CHUNKED_ERROR)))
	return retval;

    ap_add_common_vars(r);

    /* build the command line */
    if ((rv = build_command_line(&command, r, p)) != APR_SUCCESS) {
	ap_log_rerror(APLOG_MARK, APLOG_ERR, rv, r,
	      "couldn't spawn child process: %s", r->filename);
	return HTTP_INTERNAL_SERVER_ERROR;
    }
    /* build the argument list */
    else if ((rv = build_argv_list(&argv, r, p)) != APR_SUCCESS) {
	ap_log_rerror(APLOG_MARK, APLOG_ERR, rv, r,
		      "couldn't spawn child process: %s", r->filename);
	return HTTP_INTERNAL_SERVER_ERROR;
    }
    argv[0] = apr_pstrdup(p, command);

    /* run the script in its own process */
    if ((rv = run_cgi_child(&script_out, &script_in, &script_err,
                            command, argv, r, p)) != APR_SUCCESS) {
        ap_log_rerror(APLOG_MARK, APLOG_ERR, rv, r,
                      "couldn't spawn child process: %s", r->filename);
        return HTTP_INTERNAL_SERVER_ERROR;
    }

    /* Transfer any put/post args, CERN style...
     * Note that we already ignore SIGPIPE in the core server.
     */
    if (ap_should_client_block(r)) {
	int dbsize, len_read;
        apr_size_t bytes_written, bytes_to_write;
        apr_status_t rv;

	if (conf->logname) {
	    dbuf = apr_pcalloc(r->pool, conf->bufbytes + 1);
	    dbpos = 0;
	}

	while ((len_read =
		ap_get_client_block(r, argsbuffer, HUGE_STRING_LEN)) > 0) {
	    if (conf->logname) {
		if ((dbpos + len_read) > conf->bufbytes) {
		    dbsize = conf->bufbytes - dbpos;
		}
		else {
		    dbsize = len_read;
		}
		memcpy(dbuf + dbpos, argsbuffer, dbsize);
		dbpos += dbsize;
	    }
            /* Keep writing data to the child until done or too much time
             * elapses with no progress or an error occurs.
             */
            bytes_written = 0;
            do {
                bytes_to_write = len_read - bytes_written;
                rv = apr_write(script_out, argsbuffer + bytes_written, 
                               &bytes_to_write);
                bytes_written += bytes_to_write;
            } while (rv == APR_SUCCESS && bytes_written < len_read);
	    if (rv != APR_SUCCESS || bytes_written < len_read) {
		/* silly script stopped reading, soak up remaining message */
		while (ap_get_client_block(r, argsbuffer, HUGE_STRING_LEN) > 0) {
		    /* dump it */
		}
		break;
	    }
	}
	apr_flush(script_out);
    }

    apr_close(script_out);

    /* Handle script return... */
    if (script_in && !nph) {
	const char *location;
	char sbuf[MAX_STRING_LEN];
	int ret;

	if ((ret = ap_scan_script_header_err(r, script_in, sbuf))) {
	    return log_script(r, conf, ret, dbuf, sbuf, script_in, script_err);
	}

	location = apr_table_get(r->headers_out, "Location");

	if (location && location[0] == '/' && r->status == 200) {

	    /* Soak up all the script output */
	    while (apr_fgets(argsbuffer, HUGE_STRING_LEN, script_in) == 0) {
		continue;
	    }
            log_script_err(r, script_err);
	    /* This redirect needs to be a GET no matter what the original
	     * method was.
	     */
	    r->method = apr_pstrdup(r->pool, "GET");
	    r->method_number = M_GET;

	    /* We already read the message body (if any), so don't allow
	     * the redirected request to think it has one.  We can ignore 
	     * Transfer-Encoding, since we used REQUEST_CHUNKED_ERROR.
	     */
	    apr_table_unset(r->headers_in, "Content-Length");

	    ap_internal_redirect_handler(location, r);
	    return OK;
	}
	else if (location && r->status == 200) {
	    /* XX Note that if a script wants to produce its own Redirect
	     * body, it now has to explicitly *say* "Status: 302"
	     */
	    return HTTP_MOVED_TEMPORARILY;
	}

	ap_send_http_header(r);
	if (!r->header_only) {
            bb = ap_brigade_create(r->pool);
	    b = ap_bucket_create_pipe(script_in);
	    AP_BRIGADE_INSERT_TAIL(bb, b);
            b = ap_bucket_create_eos();
	    AP_BRIGADE_INSERT_TAIL(bb, b);
	    ap_pass_brigade(r->output_filters, bb);
	}

        log_script_err(r, script_err);
	apr_close(script_err);
    }

    if (script_in && nph) {
        bb = ap_brigade_create(r->pool);
	b = ap_bucket_create_pipe(script_in);
	AP_BRIGADE_INSERT_TAIL(bb, b);
	b = ap_bucket_create_eos();
	AP_BRIGADE_INSERT_TAIL(bb, b);
        ap_pass_brigade(r->output_filters, bb);
    }

    return OK;			/* NOT r->status, even if it has changed. */
}

static void register_hooks(void)
{
    ap_hook_handler(cgi_handler, NULL, NULL, AP_HOOK_MIDDLE);
}

module AP_MODULE_DECLARE_DATA cgi_module =
{
    STANDARD20_MODULE_STUFF,
    NULL,			/* dir config creater */
    NULL,			/* dir merger --- default is to override */
    create_cgi_config,		/* server config */
    merge_cgi_config,		/* merge server config */
    cgi_cmds,			/* command apr_table_t */
    register_hooks		/* register hooks */
};
