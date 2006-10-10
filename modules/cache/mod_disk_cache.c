/* Licensed to the Apache Software Foundation (ASF) under one or more
 * contributor license agreements.  See the NOTICE file distributed with
 * this work for additional information regarding copyright ownership.
 * The ASF licenses this file to You under the Apache License, Version 2.0
 * (the "License"); you may not use this file except in compliance with
 * the License.  You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "apr_file_io.h"
#include "apr_strings.h"
#include "mod_cache.h"
#include "mod_disk_cache.h"
#include "ap_provider.h"
#include "util_filter.h"
#include "util_script.h"
#include "util_charset.h"

/*
 * mod_disk_cache: Disk Based HTTP 1.1 Cache.
 *
 * Flow to Find the .data file:
 *   Incoming client requests URI /foo/bar/baz
 *   Generate <hash> off of /foo/bar/baz
 *   Open <hash>.header
 *   Read in <hash>.header file (may contain Format #1 or Format #2)
 *   If format #1 (Contains a list of Vary Headers):
 *      Use each header name (from .header) with our request values (headers_in) to
 *      regenerate <hash> using HeaderName+HeaderValue+.../foo/bar/baz
 *      re-read in <hash>.header (must be format #2)
 *   read in <hash>.data
 *
 * Always first in the header file:
 *   disk_cache_format_t format;
 *
 * VARY_FORMAT_VERSION:
 *   apr_time_t expire;
 *   apr_array_t vary_headers (delimited by CRLF)
 *
 * DISK_FORMAT_VERSION:
 *   disk_cache_info_t
 *   entity name (dobj->name) [length is in disk_cache_info_t->name_len]
 *   r->headers_out (delimited by CRLF)
 *   CRLF
 *   r->headers_in (delimited by CRLF)
 *   CRLF
 */

module AP_MODULE_DECLARE_DATA disk_cache_module;

/* Forward declarations */
static int remove_entity(cache_handle_t *h);
static apr_status_t store_headers(cache_handle_t *h, request_rec *r, cache_info *i);
static apr_status_t store_body(cache_handle_t *h, request_rec *r, apr_bucket_brigade *b);
static apr_status_t recall_headers(cache_handle_t *h, request_rec *r);
static apr_status_t recall_body(cache_handle_t *h, apr_pool_t *p, apr_bucket_brigade *bb);
static apr_status_t read_array(request_rec *r, apr_array_header_t* arr,
                               apr_file_t *file);

/*
 * Local static functions
 */

static char *header_file(apr_pool_t *p, disk_cache_conf *conf,
                         disk_cache_object_t *dobj, const char *name)
{
    if (!dobj->hashfile) {
        dobj->hashfile = ap_cache_generate_name(p, conf->dirlevels,
                                                conf->dirlength, name);
    }

    if (dobj->prefix) {
        return apr_pstrcat(p, dobj->prefix, CACHE_VDIR_SUFFIX, "/",
                           dobj->hashfile, CACHE_HEADER_SUFFIX, NULL);
     }
     else {
        return apr_pstrcat(p, conf->cache_root, "/", dobj->hashfile,
                           CACHE_HEADER_SUFFIX, NULL);
     }
}

static char *data_file(apr_pool_t *p, disk_cache_conf *conf,
                       disk_cache_object_t *dobj, const char *name)
{
    if (!dobj->hashfile) {
        dobj->hashfile = ap_cache_generate_name(p, conf->dirlevels,
                                                conf->dirlength, name);
    }

    if (dobj->prefix) {
        return apr_pstrcat(p, dobj->prefix, CACHE_VDIR_SUFFIX, "/",
                           dobj->hashfile, CACHE_DATA_SUFFIX, NULL);
     }
     else {
        return apr_pstrcat(p, conf->cache_root, "/", dobj->hashfile,
                           CACHE_DATA_SUFFIX, NULL);
     }
}

static apr_status_t mkdir_structure(disk_cache_conf *conf, const char *file, apr_pool_t *pool)
{
    apr_status_t rv = APR_SUCCESS;
    char *p;

    for (p = (char*)file + conf->cache_root_len + 1;;) {
        p = strchr(p, '/');
        if (!p)
            break;
        *p = '\0';

        rv = apr_dir_make(file,
                          APR_UREAD|APR_UWRITE|APR_UEXECUTE, pool);
        *p = '/';
        if (rv != APR_SUCCESS && !APR_STATUS_IS_EEXIST(rv)) {
            break;
        }
        ++p;
    }
    if (rv != APR_SUCCESS && !APR_STATUS_IS_EEXIST(rv)) {
        return rv;
    }

    return APR_SUCCESS;
}

/* htcacheclean may remove directories underneath us.
 * So, we'll try renaming three times at a cost of 0.002 seconds.
 */
static apr_status_t safe_file_rename(disk_cache_conf *conf,
                                     const char *src, const char *dest,
                                     apr_pool_t *pool)
{
    apr_status_t rv;

    rv = apr_file_rename(src, dest, pool);

    if (rv != APR_SUCCESS) {
        int i;

        for (i = 0; i < 2 && rv != APR_SUCCESS; i++) {
            /* 1000 micro-seconds aka 0.001 seconds. */
            apr_sleep(1000);

            mkdir_structure(conf, dest, pool);

            rv = apr_file_rename(src, dest, pool);
        }
    }

    return rv;
}

static apr_status_t file_cache_errorcleanup(disk_cache_object_t *dobj, request_rec *r)
{
    /* Remove the header file and the body file. */
    apr_file_remove(dobj->hdrsfile, r->pool);
    apr_file_remove(dobj->datafile, r->pool);

    /* If we opened the temporary data file, close and remove it. */
    if (dobj->tfd) {
        apr_file_close(dobj->tfd);
        apr_file_remove(dobj->tempfile, r->pool);
        dobj->tfd = NULL;
    }

    return APR_SUCCESS;
}


static const char* regen_key(apr_pool_t *p, apr_table_t *headers,
                             apr_array_header_t *varray, const char *oldkey)
{
    struct iovec *iov;
    int i, k;
    int nvec;
    const char *header;
    const char **elts;

    nvec = (varray->nelts * 2) + 1;
    iov = apr_palloc(p, sizeof(struct iovec) * nvec);
    elts = (const char **) varray->elts;

    /* TODO:
     *    - Handle multiple-value headers better. (sort them?)
     *    - Handle Case in-sensitive Values better.
     *        This isn't the end of the world, since it just lowers the cache
     *        hit rate, but it would be nice to fix.
     *
     * The majority are case insenstive if they are values (encoding etc).
     * Most of rfc2616 is case insensitive on header contents.
     *
     * So the better solution may be to identify headers which should be
     * treated case-sensitive?
     *  HTTP URI's (3.2.3) [host and scheme are insensitive]
     *  HTTP method (5.1.1)
     *  HTTP-date values (3.3.1)
     *  3.7 Media Types [exerpt]
     *     The type, subtype, and parameter attribute names are case-
     *     insensitive. Parameter values might or might not be case-sensitive,
     *     depending on the semantics of the parameter name.
     *  4.20 Except [exerpt]
     *     Comparison of expectation values is case-insensitive for unquoted
     *     tokens (including the 100-continue token), and is case-sensitive for
     *     quoted-string expectation-extensions.
     */

    for(i=0, k=0; i < varray->nelts; i++) {
        header = apr_table_get(headers, elts[i]);
        if (!header) {
            header = "";
        }
        iov[k].iov_base = (char*) elts[i];
        iov[k].iov_len = strlen(elts[i]);
        k++;
        iov[k].iov_base = (char*) header;
        iov[k].iov_len = strlen(header);
        k++;
    }
    iov[k].iov_base = (char*) oldkey;
    iov[k].iov_len = strlen(oldkey);
    k++;

    return apr_pstrcatv(p, iov, k, NULL);
}

static int array_alphasort(const void *fn1, const void *fn2)
{
    return strcmp(*(char**)fn1, *(char**)fn2);
}

static void tokens_to_array(apr_pool_t *p, const char *data,
                            apr_array_header_t *arr)
{
    char *token;

    while ((token = ap_get_list_item(p, &data)) != NULL) {
        *((const char **) apr_array_push(arr)) = token;
    }

    /* Sort it so that "Vary: A, B" and "Vary: B, A" are stored the same. */
    qsort((void *) arr->elts, arr->nelts,
         sizeof(char *), array_alphasort);
}

/*
 * Hook and mod_cache callback functions
 */
static int create_entity(cache_handle_t *h, request_rec *r, const char *key, apr_off_t len)
{
    disk_cache_conf *conf = ap_get_module_config(r->server->module_config,
                                                 &disk_cache_module);
    cache_object_t *obj;
    disk_cache_object_t *dobj;

    if (conf->cache_root == NULL) {
        return DECLINED;
    }

    /* Note, len is -1 if unknown so don't trust it too hard */
    if (len > conf->maxfs) {
        ap_log_error(APLOG_MARK, APLOG_DEBUG, 0, r->server,
                     "disk_cache: URL %s failed the size check "
                     "(%" APR_OFF_T_FMT " > %" APR_OFF_T_FMT ")",
                     key, len, conf->maxfs);
        return DECLINED;
    }
    if (len >= 0 && len < conf->minfs) {
        ap_log_error(APLOG_MARK, APLOG_DEBUG, 0, r->server,
                     "disk_cache: URL %s failed the size check "
                     "(%" APR_OFF_T_FMT " < %" APR_OFF_T_FMT ")",
                     key, len, conf->minfs);
        return DECLINED;
    }

    /* Allocate and initialize cache_object_t and disk_cache_object_t */
    h->cache_obj = obj = apr_pcalloc(r->pool, sizeof(*obj));
    obj->vobj = dobj = apr_pcalloc(r->pool, sizeof(*dobj));

    obj->key = apr_pstrdup(r->pool, key);

    dobj->name = obj->key;
    dobj->prefix = NULL;
    /* Save the cache root */
    dobj->root = apr_pstrndup(r->pool, conf->cache_root, conf->cache_root_len);
    dobj->root_len = conf->cache_root_len;
    dobj->datafile = data_file(r->pool, conf, dobj, key);
    dobj->hdrsfile = header_file(r->pool, conf, dobj, key);
    dobj->tempfile = apr_pstrcat(r->pool, conf->cache_root, AP_TEMPFILE, NULL);
    dobj->initial_size = len;
    dobj->file_size = -1;
    dobj->updtimeout = conf->updtimeout;

    return OK;
}


static apr_status_t file_read_timeout(apr_file_t *file, char * buf,
                                      apr_size_t len, apr_time_t timeout)
{
    apr_size_t left, done;
    apr_finfo_t finfo;
    apr_status_t rc;

    done = 0;
    left = len;

    while(1) {
        rc = apr_file_read_full(file, buf+done, left, &len);
        if (rc == APR_SUCCESS) {
           break;
        }
        done += len;
        left -= len;

        if(!APR_STATUS_IS_EOF(rc)) {
            return rc;
        }
        rc = apr_file_info_get(&finfo, APR_FINFO_MTIME, file);
        if(rc != APR_SUCCESS) {
           return rc;
        }
        if(finfo.mtime < (apr_time_now() - timeout) ) {
            return APR_ETIMEDOUT;
        }
        apr_sleep(CACHE_LOOP_SLEEP);
    }

    return APR_SUCCESS;
}


static apr_status_t open_header(cache_handle_t *h, request_rec *r, 
                                const char *key, disk_cache_conf *conf)
{
    int flags;
    disk_cache_format_t format;
    apr_status_t rc;
    const char *nkey = key;
    disk_cache_info_t disk_info;
    cache_object_t *obj = h->cache_obj;
    disk_cache_object_t *dobj = obj->vobj;

    flags = APR_READ|APR_BINARY|APR_BUFFERED;

    rc = apr_file_open(&dobj->hfd, dobj->hdrsfile, flags, 0, r->pool);
    if (rc != APR_SUCCESS) {
        return CACHE_EDECLINED;
    }

    /* read the format from the cache file */
    rc = apr_file_read_full(dobj->hfd, &format, sizeof(format), NULL);
    if(APR_STATUS_IS_EOF(rc)) {
        return CACHE_ENODATA;
    }
    else if(rc != APR_SUCCESS) {
        return rc;
    }

    /* Vary-files are being written to tmpfile and moved in place, so
       the should always be complete */
    if (format == VARY_FORMAT_VERSION) {
        apr_array_header_t* varray;
        apr_time_t expire;

        rc = apr_file_read_full(dobj->hfd, &expire, sizeof(expire), NULL);
        if(rc != APR_SUCCESS) {
            return rc;
        }

        if (expire < r->request_time) {
            return CACHE_EDECLINED;
        }

        varray = apr_array_make(r->pool, 5, sizeof(char*));
        rc = read_array(r, varray, dobj->hfd);
        if (rc != APR_SUCCESS) {
            ap_log_error(APLOG_MARK, APLOG_ERR, rc, r->server,
                         "disk_cache: Cannot parse vary header file: %s",
                         dobj->hdrsfile);
            return CACHE_EDECLINED;
        }
        apr_file_close(dobj->hfd);

        nkey = regen_key(r->pool, r->headers_in, varray, key);

        dobj->prefix = dobj->hdrsfile;
        dobj->hdrsfile = data_file(r->pool, conf, dobj, nkey);

        rc = apr_file_open(&dobj->hfd, dobj->hdrsfile, flags, 0, r->pool);
        if (rc != APR_SUCCESS) {
            dobj->hfd = NULL;
            return CACHE_EDECLINED;
        }
        rc = apr_file_read_full(dobj->hfd, &format, sizeof(format), NULL);
        if(APR_STATUS_IS_EOF(rc)) {
            return CACHE_ENODATA;
        }
        else if(rc != APR_SUCCESS) {
            return rc;
        }
    }

    if(format != DISK_FORMAT_VERSION) {
        ap_log_error(APLOG_MARK, APLOG_INFO, 0, r->server,
                     "disk_cache: File '%s' had a version mismatch. File had "
                     "version: %d (current is %d). Deleted.", dobj->hdrsfile,
                     format, DISK_FORMAT_VERSION);
        file_cache_errorcleanup(dobj, r);
        return CACHE_EDECLINED;
    }

    obj->key = nkey;
    dobj->name = key;

    /* read the data from the header file */
    rc = apr_file_read_full(dobj->hfd, &disk_info, sizeof(disk_info), NULL);
    if(APR_STATUS_IS_EOF(rc)) {
        return CACHE_ENODATA;
    }
    else if(rc != APR_SUCCESS) {
        return rc;
    }

    /* Store it away so we can get it later. */
    dobj->disk_info = disk_info;

    return APR_SUCCESS;
}


static apr_status_t open_header_timeout(cache_handle_t *h, request_rec *r, 
                                const char *key, disk_cache_conf *conf,
                                disk_cache_object_t *dobj)
{
    apr_status_t rc;
    apr_finfo_t finfo;

    while(1) {
        if(dobj->hfd) {
            apr_file_close(dobj->hfd);
            dobj->hfd = NULL;
        }
        rc = open_header(h, r, key, conf);
        if(rc != APR_SUCCESS && rc != CACHE_ENODATA) {
            if(rc != CACHE_EDECLINED) {
                ap_log_error(APLOG_MARK, APLOG_ERR, rc, r->server,
                             "disk_cache: Cannot load header file: %s",
                             dobj->hdrsfile);
            }
            return rc;
        }

        /* Objects with unknown body size will have file_size == -1 until the
           entire body is written and the header updated with the actual size.
           And since we depend on knowing the body size we wait until the size
           is written */
        if(rc == APR_SUCCESS && dobj->disk_info.file_size >= 0) {
            break;
        }
        rc = apr_file_info_get(&finfo, APR_FINFO_MTIME, dobj->hfd);
        if(rc != APR_SUCCESS) {
            return rc;
        }
        if(finfo.mtime < (apr_time_now() - dobj->updtimeout)) {
            ap_log_error(APLOG_MARK, APLOG_WARNING, 0, r->server,
                         "disk_cache: Timed out waiting for header for URL %s"
                         " - caching the body failed?", key);
            return CACHE_EDECLINED;
        }
        apr_sleep(CACHE_LOOP_SLEEP);
    }

    return APR_SUCCESS;
}


static apr_status_t open_body_timeout(request_rec *r, const char *key, 
                                      disk_cache_object_t *dobj)
{
    apr_off_t off;
    core_dir_config *pdconf = ap_get_module_config(r->per_dir_config,
                                                   &core_module);
    apr_time_t starttime = apr_time_now();
    int flags;
    apr_status_t rc;

    flags = APR_READ|APR_BINARY|APR_BUFFERED;
#if APR_HAS_SENDFILE
    flags |= ((pdconf->enable_sendfile == ENABLE_SENDFILE_OFF)
             ? 0 : APR_SENDFILE_ENABLED);
#endif  

    /* Wait here until we get a body cachefile, data in it, and do quick sanity
     * check */

    while(1) {
        if(dobj->fd == NULL) {
            rc = apr_file_open(&dobj->fd, dobj->datafile, flags, 0, r->pool);
            if(rc != APR_SUCCESS) {
                if(starttime < (apr_time_now() - dobj->updtimeout) ) {
                    ap_log_error(APLOG_MARK, APLOG_WARNING, 0, r->server,
                                 "disk_cache: Timed out waiting for body for "
                                 "URL %s - caching failed?", key);
                    return CACHE_EDECLINED;
                }
                apr_sleep(CACHE_LOOP_SLEEP);
                continue;
            }
        }

        dobj->file_size = 0;
        rc = apr_file_seek(dobj->fd, APR_END, &dobj->file_size);
        if(rc != APR_SUCCESS) {
            return rc;
        }

        if(dobj->initial_size < dobj->file_size) {
            ap_log_error(APLOG_MARK, APLOG_ERR, 0, r->server,
                         "disk_cache: Bad cached body for URL %s, size %"
                         APR_OFF_T_FMT " != %" APR_OFF_T_FMT,  dobj->name,
                         dobj->initial_size, dobj->file_size);
            file_cache_errorcleanup(dobj, r);
            return CACHE_EDECLINED;
        }
        else if(dobj->initial_size > dobj->file_size) {
            /* Still caching or failed? */
            apr_finfo_t finfo;

            rc = apr_file_info_get(&finfo, APR_FINFO_MTIME, dobj->fd);
            if(rc != APR_SUCCESS ||
                    finfo.mtime < (apr_time_now() - dobj->updtimeout) ) 
            {
                ap_log_error(APLOG_MARK, APLOG_WARNING, rc, r->server,
                             "disk_cache: Body for URL %s is too small - "
                             "caching the body failed?", dobj->name);
                return CACHE_EDECLINED;
            }
        }
        if(dobj->file_size == dobj->initial_size) {
            break;
        }
        apr_sleep(CACHE_LOOP_SLEEP);
    }

    /* Go back to the beginning */
    off = 0;
    rc = apr_file_seek(dobj->fd, APR_SET, &off);
    if(rc != APR_SUCCESS) {
        return rc;
    }

    return APR_SUCCESS;
}


static int open_entity(cache_handle_t *h, request_rec *r, const char *key)
{
    apr_status_t rc;
    disk_cache_object_t *dobj;
    cache_info *info;
    apr_size_t len;
    static int error_logged = 0;
    disk_cache_conf *conf = ap_get_module_config(r->server->module_config,
                                                 &disk_cache_module);
    char urlbuff[MAX_STRING_LEN];

    h->cache_obj = NULL;

    /* Look up entity keyed to 'url' */
    if (conf->cache_root == NULL) {
        if (!error_logged) {
            error_logged = 1;
            ap_log_error(APLOG_MARK, APLOG_ERR, 0, r->server,
                         "disk_cache: Cannot cache files to disk without a "
                         "CacheRoot specified.");
        }
        return DECLINED;
    }

    /* Create and init the cache object */
    h->cache_obj = apr_pcalloc(r->pool, sizeof(cache_object_t));
    h->cache_obj->vobj = dobj = apr_pcalloc(r->pool, sizeof(disk_cache_object_t));
    info = &(h->cache_obj->info);

    /* Save the cache root */
    dobj->root = apr_pstrndup(r->pool, conf->cache_root, conf->cache_root_len);
    dobj->root_len = conf->cache_root_len;

    dobj->hdrsfile = header_file(r->pool, conf, dobj, key);

    dobj->updtimeout = conf->updtimeout;

    /* Open header and read basic info, wait until header contains
       valid size information for the body */
    rc = open_header_timeout(h, r, key, conf, dobj);
    if(rc != APR_SUCCESS) {
        return DECLINED;
    }

    info->status = dobj->disk_info.status;
    info->date = dobj->disk_info.date;
    info->expire = dobj->disk_info.expire;
    info->request_time = dobj->disk_info.request_time;
    info->response_time = dobj->disk_info.response_time;

    dobj->initial_size = (apr_off_t) dobj->disk_info.file_size;
    dobj->tempfile = apr_pstrcat(r->pool, conf->cache_root, AP_TEMPFILE, NULL);

    len = dobj->disk_info.name_len;

    if(len > 0) {
        rc = file_read_timeout(dobj->hfd, urlbuff, len, dobj->updtimeout);
        if (rc == APR_ETIMEDOUT) {
            ap_log_error(APLOG_MARK, APLOG_WARNING, rc, r->server,
                         "disk_cache: Timed out waiting for urlbuff for "
                         "URL %s - caching failed?",  key);
            return DECLINED;
        }
        else if(rc != APR_SUCCESS) {
            ap_log_error(APLOG_MARK, APLOG_WARNING, rc, r->server,
                         "disk_cache: Error reading urlbuff for URL %s",
                         key);
            return DECLINED;
        }
    }
    urlbuff[len] = '\0';

    /* check that we have the same URL */
    if (strcmp(urlbuff, dobj->name) != 0) {
        ap_log_error(APLOG_MARK, APLOG_ERR, 0, r->server,
                     "disk_cache: Cached URL %s didn't match requested "
                     "URL %s", urlbuff, dobj->name);
        return DECLINED;
    }

    dobj->datafile = data_file(r->pool, conf, dobj, h->cache_obj->key);
    dobj->tempfile = apr_pstrcat(r->pool, conf->cache_root, AP_TEMPFILE, NULL);

    /* Only need body cachefile if we have a body */
    if(dobj->initial_size > 0) {
        rc = open_body_timeout(r, key, dobj);
        if(rc != APR_SUCCESS) {
            return DECLINED;
        }
    }
    else {
        dobj->file_size = 0;
    }

    ap_log_error(APLOG_MARK, APLOG_DEBUG, 0, r->server,
                 "disk_cache: Recalled status for cached URL %s",  dobj->name);
    return OK;
}


static int remove_entity(cache_handle_t *h)
{
    /* Null out the cache object pointer so next time we start from scratch  */
    h->cache_obj = NULL;
    return OK;
}

static int remove_url(cache_handle_t *h, apr_pool_t *p)
{
    apr_status_t rc;
    disk_cache_object_t *dobj;

    /* Get disk cache object from cache handle */
    dobj = (disk_cache_object_t *) h->cache_obj->vobj;
    if (!dobj) {
        return DECLINED;
    }

    /* Delete headers file */
    if (dobj->hdrsfile) {
        ap_log_error(APLOG_MARK, APLOG_DEBUG, 0, NULL,
                     "disk_cache: Deleting %s from cache.", dobj->hdrsfile);

        rc = apr_file_remove(dobj->hdrsfile, p);
        if ((rc != APR_SUCCESS) && !APR_STATUS_IS_ENOENT(rc)) {
            /* Will only result in an output if httpd is started with -e debug.
             * For reason see log_error_core for the case s == NULL.
             */
            ap_log_error(APLOG_MARK, APLOG_DEBUG, rc, NULL,
                   "disk_cache: Failed to delete headers file %s from cache.",
                         dobj->hdrsfile);
            return DECLINED;
        }
    }

     /* Delete data file */
    if (dobj->datafile) {
        ap_log_error(APLOG_MARK, APLOG_DEBUG, 0, NULL,
                     "disk_cache: Deleting %s from cache.", dobj->datafile);

        rc = apr_file_remove(dobj->datafile, p);
        if ((rc != APR_SUCCESS) && !APR_STATUS_IS_ENOENT(rc)) {
            /* Will only result in an output if httpd is started with -e debug.
             * For reason see log_error_core for the case s == NULL.
             */
            ap_log_error(APLOG_MARK, APLOG_DEBUG, rc, NULL,
                      "disk_cache: Failed to delete data file %s from cache.",
                         dobj->datafile);
            return DECLINED;
        }
    }

    /* now delete directories as far as possible up to our cache root */
    if (dobj->root) {
        const char *str_to_copy;

        str_to_copy = dobj->hdrsfile ? dobj->hdrsfile : dobj->datafile;
        if (str_to_copy) {
            char *dir, *slash, *q;

            dir = apr_pstrdup(p, str_to_copy);

            /* remove filename */
            slash = strrchr(dir, '/');
            *slash = '\0';

            /*
             * now walk our way back to the cache root, delete everything
             * in the way as far as possible
             *
             * Note: due to the way we constructed the file names in
             * header_file and data_file, we are guaranteed that the
             * cache_root is suffixed by at least one '/' which will be
             * turned into a terminating null by this loop.  Therefore,
             * we won't either delete or go above our cache root.
             */
            for (q = dir + dobj->root_len; *q ; ) {
                 ap_log_error(APLOG_MARK, APLOG_DEBUG, 0, NULL,
                              "disk_cache: Deleting directory %s from cache",
                              dir);

                 rc = apr_dir_remove(dir, p);
                 if (rc != APR_SUCCESS && !APR_STATUS_IS_ENOENT(rc)) {
                    break;
                 }
                 slash = strrchr(q, '/');
                 *slash = '\0';
            }
        }
    }

    return OK;
}

static apr_status_t read_array(request_rec *r, apr_array_header_t* arr,
                               apr_file_t *file)
{
    char w[MAX_STRING_LEN];
    int p;
    apr_status_t rv;

    while (1) {
        rv = apr_file_gets(w, MAX_STRING_LEN - 1, file);
        if (rv != APR_SUCCESS) {
            ap_log_rerror(APLOG_MARK, APLOG_ERR, 0, r,
                          "Premature end of vary array.");
            return rv;
        }

        p = strlen(w);
        if (p > 0 && w[p - 1] == '\n') {
            if (p > 1 && w[p - 2] == CR) {
                w[p - 2] = '\0';
            }
            else {
                w[p - 1] = '\0';
            }
        }

        /* If we've finished reading the array, break out of the loop. */
        if (w[0] == '\0') {
            break;
        }

       *((const char **) apr_array_push(arr)) = apr_pstrdup(r->pool, w);
    }

    return APR_SUCCESS;
}

static apr_status_t store_array(apr_file_t *fd, apr_array_header_t* arr)
{
    int i;
    apr_status_t rv;
    struct iovec iov[2];
    apr_size_t amt;
    const char **elts;

    elts = (const char **) arr->elts;

    for (i = 0; i < arr->nelts; i++) {
        iov[0].iov_base = (char*) elts[i];
        iov[0].iov_len = strlen(elts[i]);
        iov[1].iov_base = CRLF;
        iov[1].iov_len = sizeof(CRLF) - 1;

        rv = apr_file_writev(fd, (const struct iovec *) &iov, 2,
                             &amt);
        if (rv != APR_SUCCESS) {
            return rv;
        }
    }

    iov[0].iov_base = CRLF;
    iov[0].iov_len = sizeof(CRLF) - 1;

    return apr_file_writev(fd, (const struct iovec *) &iov, 1,
                         &amt);
}

static apr_status_t read_table(request_rec *r,
                               apr_table_t *table, apr_file_t *file)
{
    char w[MAX_STRING_LEN];
    char *l;
    int p;
    apr_status_t rv;

    while (1) {

        /* ### What about APR_EOF? */
        rv = apr_file_gets(w, MAX_STRING_LEN - 1, file);
        if (rv != APR_SUCCESS) {
            return rv;
        }

        /* Delete terminal (CR?)LF */

        p = strlen(w);
        /* Indeed, the host's '\n':
           '\012' for UNIX; '\015' for MacOS; '\025' for OS/390
           -- whatever the script generates.
        */
        if (p > 0 && w[p - 1] == '\n') {
            if (p > 1 && w[p - 2] == CR) {
                w[p - 2] = '\0';
            }
            else {
                w[p - 1] = '\0';
            }
        }

        /* If we've finished reading the headers, break out of the loop. */
        if (w[0] == '\0') {
            break;
        }

#if APR_CHARSET_EBCDIC
        /* Chances are that we received an ASCII header text instead of
         * the expected EBCDIC header lines. Try to auto-detect:
         */
        if (!(l = strchr(w, ':'))) {
            int maybeASCII = 0, maybeEBCDIC = 0;
            unsigned char *cp, native;
            apr_size_t inbytes_left, outbytes_left;

            for (cp = w; *cp != '\0'; ++cp) {
                native = apr_xlate_conv_byte(ap_hdrs_from_ascii, *cp);
                if (apr_isprint(*cp) && !apr_isprint(native))
                    ++maybeEBCDIC;
                if (!apr_isprint(*cp) && apr_isprint(native))
                    ++maybeASCII;
            }
            if (maybeASCII > maybeEBCDIC) {
                ap_log_error(APLOG_MARK, APLOG_ERR, 0, r->server,
                             "disk_cache: CGI Interface Error: Script headers apparently ASCII: (CGI = %s)",
                             r->filename);
                inbytes_left = outbytes_left = cp - w;
                apr_xlate_conv_buffer(ap_hdrs_from_ascii,
                                      w, &inbytes_left, w, &outbytes_left);
            }
        }
#endif /*APR_CHARSET_EBCDIC*/

        /* if we see a bogus header don't ignore it. Shout and scream */
        if (!(l = strchr(w, ':'))) {
            return APR_EGENERAL;
        }

        *l++ = '\0';
        while (*l && apr_isspace(*l)) {
            ++l;
        }

        apr_table_add(table, w, l);
    }

    return APR_SUCCESS;
}


static apr_status_t read_table_timeout(cache_handle_t *handle, request_rec *r,
                               apr_table_t **table, apr_file_t *file,
                               apr_time_t timeout)
{
    apr_off_t off;
    apr_finfo_t finfo;
    apr_status_t rv;

    off = 0;
    rv = apr_file_seek(file, APR_CUR, &off);
    if(rv != APR_SUCCESS) {
        return rv;
    }

    while(1) {
        *table = apr_table_make(r->pool, 20);
        rv = read_table(r, *table, file);
        if(rv == APR_SUCCESS) {
            break;
        }
        apr_table_clear(*table);

        rv = apr_file_seek(file, APR_SET, &off);
        if(rv != APR_SUCCESS) {
            return rv;
        }

        rv = apr_file_info_get(&finfo, APR_FINFO_MTIME, file);
        if(rv != APR_SUCCESS ||
                finfo.mtime < (apr_time_now() - timeout) ) 
        {
            ap_log_rerror(APLOG_MARK, APLOG_ERR, 0, r,
                          "disk_cache: Timed out waiting for cache headers "
                          "URL %s", handle->cache_obj->key);
            return APR_EGENERAL;
        }
        apr_sleep(CACHE_LOOP_SLEEP);
    }

    return APR_SUCCESS;
}


/*
 * Reads headers from a buffer and returns an array of headers.
 * Returns NULL on file error
 * This routine tries to deal with too long lines and continuation lines.
 * @@@: XXX: FIXME: currently the headers are passed thru un-merged.
 * Is that okay, or should they be collapsed where possible?
 */
static apr_status_t recall_headers(cache_handle_t *h, request_rec *r)
{
    disk_cache_object_t *dobj = (disk_cache_object_t *) h->cache_obj->vobj;
    apr_status_t rv;

    /* This case should not happen... */
    if (!dobj->hfd) {
        /* XXX log message */
        return APR_NOTFOUND;
    }

    rv = read_table_timeout(h, r, &(h->resp_hdrs), dobj->hfd, dobj->updtimeout);
    if(rv != APR_SUCCESS) {
        ap_log_error(APLOG_MARK, APLOG_ERR, rv, r->server,
                     "disk_cache: Timed out waiting for response headers "
                     "for URL %s - caching failed?",  dobj->name);
        return rv;
    }

    rv = read_table_timeout(h, r, &(h->req_hdrs), dobj->hfd, dobj->updtimeout);
    if(rv != APR_SUCCESS) {
        ap_log_error(APLOG_MARK, APLOG_ERR, rv, r->server,
                     "disk_cache: Timed out waiting for request headers "
                     "for URL %s - caching failed?",  dobj->name);
        return rv;
    }

    apr_file_close(dobj->hfd);
    dobj->hfd = NULL;

    ap_log_error(APLOG_MARK, APLOG_DEBUG, 0, r->server,
                 "disk_cache: Recalled headers for URL %s",  dobj->name);
    return APR_SUCCESS;
}

static apr_status_t recall_body(cache_handle_t *h, apr_pool_t *p, apr_bucket_brigade *bb)
{
    apr_bucket *e;
    disk_cache_object_t *dobj = (disk_cache_object_t*) h->cache_obj->vobj;

    apr_brigade_insert_file(bb, dobj->fd, 0, dobj->file_size, p);

    e = apr_bucket_eos_create(bb->bucket_alloc);
    APR_BRIGADE_INSERT_TAIL(bb, e);

    return APR_SUCCESS;
}

static apr_status_t store_table(apr_file_t *fd, apr_table_t *table)
{
    int i;
    apr_status_t rv;
    struct iovec iov[4];
    apr_size_t amt;
    apr_table_entry_t *elts;

    elts = (apr_table_entry_t *) apr_table_elts(table)->elts;
    for (i = 0; i < apr_table_elts(table)->nelts; ++i) {
        if (elts[i].key != NULL) {
            iov[0].iov_base = elts[i].key;
            iov[0].iov_len = strlen(elts[i].key);
            iov[1].iov_base = ": ";
            iov[1].iov_len = sizeof(": ") - 1;
            iov[2].iov_base = elts[i].val;
            iov[2].iov_len = strlen(elts[i].val);
            iov[3].iov_base = CRLF;
            iov[3].iov_len = sizeof(CRLF) - 1;

            rv = apr_file_writev(fd, (const struct iovec *) &iov, 4,
                                 &amt);
            if (rv != APR_SUCCESS) {
                return rv;
            }
        }
    }
    iov[0].iov_base = CRLF;
    iov[0].iov_len = sizeof(CRLF) - 1;
    rv = apr_file_writev(fd, (const struct iovec *) &iov, 1,
                         &amt);
    return rv;
}


static apr_status_t open_new_file(request_rec *r, const char *filename,
                                  apr_file_t **fd, disk_cache_conf *conf)
{
    int flags = APR_CREATE | APR_WRITE | APR_BINARY | APR_BUFFERED | APR_EXCL;
    apr_status_t rv;

    while(1) {
        rv = apr_file_open(fd, filename, flags, 
                APR_FPROT_UREAD | APR_FPROT_UWRITE, r->pool);

        /* FIXME: Debug */
        ap_log_error(APLOG_MARK, APLOG_DEBUG, rv, r->server,
                     "disk_cache: open_new_file: Opening %s", filename);

        if(APR_STATUS_IS_EEXIST(rv)) {
            apr_finfo_t finfo;

            rv = apr_stat(&finfo, filename, APR_FINFO_MTIME, r->pool);
            if(APR_STATUS_IS_ENOENT(rv)) {
                /* Someone else has already removed it, try again */
                continue;
            }
            else if(rv != APR_SUCCESS) {
                return rv;
            }

            if(finfo.mtime < (apr_time_now() - conf->updtimeout) ) {
                /* Something stale that's left around */

                rv = apr_file_remove(filename, r->pool);
                if(rv != APR_SUCCESS && !APR_STATUS_IS_ENOENT(rv)) {
                    ap_log_error(APLOG_MARK, APLOG_ERR, rv, r->server,
                                 "disk_cache: open_new_file: Failed to "
                                 "remove old %s", filename);
                    return rv;
                }
                continue;
            }
            else {
                /* Someone else has just created the file, return identifiable
                   status so calling function can do the right thing */

                return CACHE_EEXIST;
            }
        }
        else if(APR_STATUS_IS_ENOENT(rv)) {
            /* The directory for the file didn't exist */

            rv = mkdir_structure(conf, filename, r->pool);
            if(rv != APR_SUCCESS) {
                ap_log_error(APLOG_MARK, APLOG_ERR, rv, r->server,
                             "disk_cache: open_new_file: Failed to make "
                             "directory for %s", filename);
                return rv;
            }
            continue;
        }
        else if(rv == APR_SUCCESS) {
            return APR_SUCCESS;
        }
        else {
            ap_log_error(APLOG_MARK, APLOG_ERR, rv, r->server,
                         "disk_cache: open_new_file: Failed to open %s",
                         filename);
            return rv;
        }
    }

    /* We should never get here, so */
    return APR_EGENERAL;
}


static apr_status_t store_vary_header(cache_handle_t *h, disk_cache_conf *conf,
                                       request_rec *r, cache_info *info,
                                       const char *varyhdr)
{
    disk_cache_object_t *dobj = (disk_cache_object_t*) h->cache_obj->vobj;
    apr_array_header_t* varray;
    const char *vfile;
    apr_status_t rv;
    int flags;
    disk_cache_format_t format = VARY_FORMAT_VERSION;
    struct iovec iov[2];
    apr_size_t amt;

    if(dobj->prefix != NULL) {
        vfile = dobj->prefix;
    }
    else {
        vfile = dobj->hdrsfile;
    }

    flags = APR_CREATE | APR_WRITE | APR_BINARY | APR_EXCL | APR_BUFFERED;
    rv = apr_file_mktemp(&dobj->tfd, dobj->tempfile, flags, r->pool);
    if (rv != APR_SUCCESS) {
        return rv;
    }

    iov[0].iov_base = (void*)&format;
    iov[0].iov_len = sizeof(format);

    iov[1].iov_base = (void*)&info->expire;
    iov[1].iov_len = sizeof(info->expire);

    rv = apr_file_writev(dobj->tfd, (const struct iovec *) &iov, 2, &amt);
    if (rv != APR_SUCCESS) {
        file_cache_errorcleanup(dobj, r);
        return rv;
    }

    varray = apr_array_make(r->pool, 6, sizeof(char*));
    tokens_to_array(r->pool, varyhdr, varray);

    rv = store_array(dobj->tfd, varray);
    if (rv != APR_SUCCESS) {
        file_cache_errorcleanup(dobj, r);
        return rv;
    }

    rv = apr_file_close(dobj->tfd);
    dobj->tfd = NULL;
    if (rv != APR_SUCCESS) {
        file_cache_errorcleanup(dobj, r);
        return rv;
    }

    rv = safe_file_rename(conf, dobj->tempfile, vfile, r->pool);
    if (rv != APR_SUCCESS) {
        ap_log_error(APLOG_MARK, APLOG_ERR, rv, r->server,
                     "disk_cache: rename tempfile to varyfile failed: "
                     "%s -> %s", dobj->tempfile, vfile);
        file_cache_errorcleanup(dobj, r);
        return rv;
    }

    dobj->tempfile = apr_pstrcat(r->pool, conf->cache_root, AP_TEMPFILE, NULL);

    if(dobj->prefix == NULL) {
        const char *tmp = regen_key(r->pool, r->headers_in, varray, dobj->name);

        dobj->prefix = dobj->hdrsfile;
        dobj->hdrsfile = header_file(r->pool, conf, dobj, tmp);
    }

    ap_log_error(APLOG_MARK, APLOG_DEBUG, 0, r->server,
                 "disk_cache: Stored vary header for URL %s", dobj->name);

    return APR_SUCCESS;
}


static apr_status_t store_disk_header(disk_cache_object_t *dobj,
                                       request_rec *r, cache_info *info)
{
    disk_cache_format_t format = DISK_FORMAT_VERSION;
    struct iovec iov[3];
    int niov;
    disk_cache_info_t disk_info;
    apr_size_t amt;
    apr_status_t rv;

    disk_info.date = info->date;
    disk_info.expire = info->expire;
    disk_info.entity_version = dobj->disk_info.entity_version++;
    disk_info.request_time = info->request_time;
    disk_info.response_time = info->response_time;
    disk_info.status = info->status;
    disk_info.file_size = dobj->initial_size;

    niov = 0;
    iov[niov].iov_base = (void*)&format;
    iov[niov++].iov_len = sizeof(format);
    iov[niov].iov_base = (void*)&disk_info;
    iov[niov++].iov_len = sizeof(disk_cache_info_t);

    disk_info.name_len = strlen(dobj->name);
    iov[niov].iov_base = (void*)dobj->name;
    iov[niov++].iov_len = disk_info.name_len;

    rv = apr_file_writev(dobj->hfd, (const struct iovec *) &iov, niov, &amt);
    if (rv != APR_SUCCESS) {
        file_cache_errorcleanup(dobj, r);
        return rv;
    }

    if (r->headers_out) {
        apr_table_t *headers_out;

        headers_out = ap_cache_cacheable_hdrs_out(r->pool, r->headers_out,
                                                  r->server);

        if (!apr_table_get(headers_out, "Content-Type")
            && r->content_type) {
            apr_table_setn(headers_out, "Content-Type",
                           ap_make_content_type(r, r->content_type));
        }

        headers_out = apr_table_overlay(r->pool, headers_out,
                                        r->err_headers_out);
        rv = store_table(dobj->hfd, headers_out);
        if (rv != APR_SUCCESS) {
            file_cache_errorcleanup(dobj, r);
            return rv;
        }
    }

    /* Parse the vary header and dump those fields from the headers_in. */
    /* FIXME: Make call to the same thing cache_select calls to crack Vary. */
    if (r->headers_in) {
        apr_table_t *headers_in;

        headers_in = ap_cache_cacheable_hdrs_out(r->pool, r->headers_in,
                                                 r->server);
        rv = store_table(dobj->hfd, headers_in);
        if (rv != APR_SUCCESS) {
            file_cache_errorcleanup(dobj, r);
            return rv;
        }
    }

    return APR_SUCCESS;
}


static apr_status_t store_headers(cache_handle_t *h, request_rec *r, 
                                  cache_info *info)
{
    disk_cache_conf *conf = ap_get_module_config(r->server->module_config,
                                                 &disk_cache_module);
    apr_status_t rv;
    int flags=0, rewriting;
    disk_cache_object_t *dobj = (disk_cache_object_t*) h->cache_obj->vobj;


    /* This is flaky... we need to manage the cache_info differently */
    h->cache_obj->info = *info;

    if(dobj->hfd) {
        ap_log_error(APLOG_MARK, APLOG_INFO, 0, r->server,
                     "disk_cache: Rewriting headers for URL %s", dobj->name);

        rewriting = TRUE;
    }
    else {
        ap_log_error(APLOG_MARK, APLOG_INFO, 0, r->server,
                     "disk_cache: Storing new headers for URL %s", dobj->name);

        rewriting = FALSE;
    }

    if (r->headers_out) {
        const char *tmp;

        tmp = apr_table_get(r->headers_out, "Vary");

        if (tmp) {
            rv = store_vary_header(h, conf, r, info, tmp);
            if(rv != APR_SUCCESS) {
                return rv;
            }
        }
    } 

    if(rewriting) {
        /* Assume we are just rewriting the header if we have an fd. The
           fd might be readonly though, in that case reopen it for writes.
           Something equivalent to fdopen would have been handy. */

        flags = apr_file_flags_get(dobj->hfd);

        if(!(flags & APR_WRITE)) {
            apr_file_close(dobj->hfd);
            rv = apr_file_open(&dobj->hfd, dobj->hdrsfile, 
                    APR_WRITE | APR_BINARY | APR_BUFFERED, 0, r->pool);
            if (rv != APR_SUCCESS) {
                dobj->hfd = NULL;
                return rv;
            }
        }
        else {
            /* We can write here, so let's just move to the right place */
            apr_off_t off=0;
            rv = apr_file_seek(dobj->hfd, APR_SET, &off);
            if (rv != APR_SUCCESS) {
                return rv;
            }
        }
    }
    else {
        rv = open_new_file(r, dobj->hdrsfile, &(dobj->hfd), conf);
        if(rv == CACHE_EEXIST) {
            dobj->skipstore = TRUE;
        }
        else if(rv != APR_SUCCESS) {
            return rv;
        }
    }

    if(dobj->skipstore) {
        ap_log_error(APLOG_MARK, APLOG_DEBUG, 0, r->server,
                     "disk_cache: Skipping store for URL %s: Someone else "
                     "beat us to it",  dobj->name);
        return APR_SUCCESS;
    }

    rv = store_disk_header(dobj, r, info);
    if(rv != APR_SUCCESS) {
        return rv;
    }

    /* If the body size is unknown, the header file will be rewritten later
       so we can't close it */
    if(dobj->initial_size < 0) {
        rv = apr_file_flush(dobj->hfd);
    }
    else {
        rv = apr_file_close(dobj->hfd);
        dobj->hfd = NULL;
    }
    if(rv != APR_SUCCESS) {
        return rv;
    }

    ap_log_error(APLOG_MARK, APLOG_DEBUG, 0, r->server,
                 "disk_cache: Stored headers for URL %s",  dobj->name);
    return APR_SUCCESS;
}


static apr_status_t copy_body(apr_pool_t *p,
                              apr_file_t *srcfd, apr_off_t srcoff, 
                              apr_file_t *destfd, apr_off_t destoff, 
                              apr_off_t len)
{
    apr_status_t rc;
    apr_size_t size;
    apr_finfo_t finfo;
    apr_time_t starttime = apr_time_now();

    char *buf = apr_palloc(p, CACHE_BUF_SIZE);
    if (!buf) {
        return APR_ENOMEM;
    }

    if(srcoff != 0) {
        rc = apr_file_seek(srcfd, APR_SET, &srcoff);
        if(rc != APR_SUCCESS) {
            return rc;
        }
    }

    if(destoff != 0) {
        rc = apr_file_seek(destfd, APR_SET, &destoff);
        if(rc != APR_SUCCESS) {
            return rc;
        }
    }

    /* Tried doing this with mmap, but sendfile on Linux got confused when
       sending a file while it was being written to from an mmapped area.
       The traditional way seems to be good enough, and less complex.
     */
    while(len > 0) {
        size=MIN(len, CACHE_BUF_SIZE);

        rc = apr_file_read_full (srcfd, buf, size, NULL);
        if(rc != APR_SUCCESS) {
            return rc;
        }

        rc = apr_file_write_full(destfd, buf, size, NULL);
        if(rc != APR_SUCCESS) {
            return rc;
        }
        len -= size;
    }

    /* Check if file has changed during copying. This is not 100% foolproof
       due to NFS attribute caching when on NFS etc. */
    /* FIXME: Can we assume that we're always copying an entire file? In that
              case we can check if the current filesize matches the length
              we think it is */
    rc = apr_file_info_get(&finfo, APR_FINFO_MTIME, srcfd);
    if(rc != APR_SUCCESS) {
        return rc;
    }
    if(starttime < finfo.mtime) {
        return APR_EGENERAL;
    }

    return APR_SUCCESS;
}


static apr_status_t replace_brigade_with_cache(cache_handle_t *h,
                                               request_rec *r,
                                               apr_bucket_brigade *bb)
{
    apr_status_t rv;
    apr_bucket *e;
    disk_cache_object_t *dobj = (disk_cache_object_t *) h->cache_obj->vobj;

    if(dobj->fd) {
        apr_file_close(dobj->fd);
        dobj->fd = NULL;
    }
    rv = open_body_timeout(r, dobj->name, dobj);
    if (rv != APR_SUCCESS) {
        if(rv != CACHE_EDECLINED) {
            ap_log_error(APLOG_MARK, APLOG_ERR, rv, r->server,
                         "disk_cache: Error opening datafile %s for URL %s",
                         dobj->datafile, dobj->name);
        }
        return rv;
    }

    /* First, empty the brigade */
    e  = APR_BRIGADE_FIRST(bb);
    while (e != APR_BRIGADE_SENTINEL(bb)) {
        apr_bucket *d;
        d = e;
        e = APR_BUCKET_NEXT(e);
        apr_bucket_delete(d);
    }

    /* Then, populate it with our cached instance */
    rv = recall_body(h, r->pool, bb);
    if (rv != APR_SUCCESS) {
        ap_log_error(APLOG_MARK, APLOG_ERR, 0, r->server,
                     "disk_cache: Error serving URL %s from cache", dobj->name);
        return rv;
    }
    ap_log_error(APLOG_MARK, APLOG_DEBUG, 0, r->server,
                 "disk_cache: Serving cached body for URL %s", dobj->name);

    return APR_SUCCESS;
}


static apr_status_t store_body(cache_handle_t *h, request_rec *r,
                               apr_bucket_brigade *bb)
{
    apr_bucket *e;
    apr_status_t rv;
    int copy_file = FALSE;
    disk_cache_object_t *dobj = (disk_cache_object_t *) h->cache_obj->vobj;
    disk_cache_conf *conf = ap_get_module_config(r->server->module_config,
                                                 &disk_cache_module);

    if(r->no_cache) {
        ap_log_error(APLOG_MARK, APLOG_DEBUG, 0, r->server,
                     "disk_cache: store_body called for URL %s even though"
                     "no_cache is set", dobj->name);
        file_cache_errorcleanup(dobj, r);
        return APR_EGENERAL;
    }

    if(dobj->initial_size == 0) {
        /* Don't waste a body cachefile on a 0 length body */
        return APR_SUCCESS;
    }

    if(dobj->skipstore) {
        /* Someone else beat us to storing this object */
        /* FIXME: Read-while-caching here */
        return APR_SUCCESS;
    }

    if(!dobj->fd) {
        rv = open_new_file(r, dobj->datafile, &(dobj->fd), conf);
        if(rv == CACHE_EEXIST) {
            /* Someone else beat us to storing this */
            /* FIXME: Read-while-caching here later on */
            return APR_SUCCESS;
        }
        else if(rv != APR_SUCCESS) {
            return rv;
        }
        dobj->file_size = 0;
    }

    /* Check if this is a complete single sequential file, eligable for
     * file copy.
     */
    if(dobj->file_size == 0 && APR_BUCKET_IS_EOS(APR_BRIGADE_LAST(bb)))
    {
        apr_off_t begin = -1;
        apr_off_t pos = -1;
        apr_file_t *fd = NULL;
        apr_bucket_file *a;

        copy_file = TRUE;

        for (e = APR_BRIGADE_FIRST(bb);
                e != APR_BRIGADE_SENTINEL(bb);
                e = APR_BUCKET_NEXT(e))
        {
            if(APR_BUCKET_IS_EOS(e)) {
                break;
            }
            if(!APR_BUCKET_IS_FILE(e)) {
                copy_file = FALSE;
                break;
            }

            a = e->data;

            if(begin < 0) {
                begin = pos = e->start;
                fd = a->fd;
            }

            if(fd != a->fd || pos != e->start) {
                copy_file = FALSE;
                break;
            }

            pos += e->length;
        }

        if(copy_file) {
            dobj->file_size = pos;
        }
    }

    if(copy_file) {
        apr_bucket_file *a;

        ap_log_error(APLOG_MARK, APLOG_INFO, 0, r->server,
                     "disk_cache: Copying body for URL %s, len %"
                     APR_OFF_T_FMT, dobj->name, dobj->file_size);

        e = APR_BRIGADE_FIRST(bb);
        a = e->data;

        rv = copy_body(r->pool, a->fd, e->start, dobj->fd, 0, 
                       dobj->file_size);
        if(rv != APR_SUCCESS) {
            ap_log_error(APLOG_MARK, APLOG_ERR, rv, r->server,
                         "disk_cache: Copying body failed, "
                         "URL %s", dobj->name);
            file_cache_errorcleanup(dobj, r);
            return rv;
        }

    }
    else {
        ap_log_error(APLOG_MARK, APLOG_INFO, 0, r->server,
                     "disk_cache: Caching body for URL %s", dobj->name);

        for (e = APR_BRIGADE_FIRST(bb);
                e != APR_BRIGADE_SENTINEL(bb);
                e = APR_BUCKET_NEXT(e))
        {   
            const char *str;
            apr_size_t length, written;

            /* Ignore the non-data-buckets */
            if(APR_BUCKET_IS_METADATA(e)) {
                continue;
            }

            rv = apr_bucket_read(e, &str, &length, APR_BLOCK_READ);
            if (rv != APR_SUCCESS) {
                ap_log_error(APLOG_MARK, APLOG_ERR, rv, r->server,
                             "disk_cache: Error when reading bucket for URL %s",
                             dobj->name);
                file_cache_errorcleanup(dobj, r);
                return rv;
            }
            rv = apr_file_write_full(dobj->fd, str, length, &written);
            if (rv != APR_SUCCESS) {
                ap_log_error(APLOG_MARK, APLOG_ERR, rv, r->server,
                             "disk_cache: Error when writing cache file for "
                             "URL %s", dobj->name);
                file_cache_errorcleanup(dobj, r);
                return rv;
            }
            dobj->file_size += written;
            if (dobj->file_size > conf->maxfs) {
                ap_log_error(APLOG_MARK, APLOG_DEBUG, 0, r->server,
                             "disk_cache: URL %s failed the size check "
                             "(%" APR_OFF_T_FMT " > %" APR_OFF_T_FMT ")",
                             dobj->name, dobj->file_size, conf->maxfs);
                file_cache_errorcleanup(dobj, r);
                return APR_EGENERAL;
            }
        }
    }


    /* Drop out here if this wasn't the end */
    if (!APR_BUCKET_IS_EOS(APR_BRIGADE_LAST(bb))) {
        return APR_SUCCESS;
    }

    if(!copy_file) {
        ap_log_error(APLOG_MARK, APLOG_DEBUG, 0, r->server,
                     "disk_cache: Done caching URL %s, len %" APR_OFF_T_FMT,
                     dobj->name, dobj->file_size);

        /* FIXME: Do we really need to check r->no_cache here since we checked
           it in the beginning? */
        if (r->no_cache || r->connection->aborted) {
            ap_log_error(APLOG_MARK, APLOG_INFO, 0, r->server,
                         "disk_cache: Discarding body for URL %s "
                         "because connection has been aborted.",
                         h->cache_obj->key);
            /* Remove the intermediate cache file and return non-APR_SUCCESS */
            file_cache_errorcleanup(dobj, r);
            return APR_EGENERAL;
        }
        if (dobj->file_size < conf->minfs) {
            ap_log_error(APLOG_MARK, APLOG_DEBUG, 0, r->server,
                         "disk_cache: URL %s failed the size check "
                         "(%" APR_OFF_T_FMT "<%" APR_OFF_T_FMT ")",
                         h->cache_obj->key, dobj->file_size, conf->minfs);
            /* Remove the intermediate cache file and return non-APR_SUCCESS */
            file_cache_errorcleanup(dobj, r);
            return APR_EGENERAL;
        }
        if(dobj->initial_size < 0) {
            /* Update header information now that we know the size */
            dobj->initial_size = dobj->file_size;
            rv = store_headers(h, r, &(h->cache_obj->info));
            if(rv != APR_SUCCESS) {
                file_cache_errorcleanup(dobj, r);
                return rv;
            }
        }
        else if(dobj->initial_size != dobj->file_size) {
            ap_log_error(APLOG_MARK, APLOG_DEBUG, 0, r->server,
                         "disk_cache: URL %s - body size mismatch: suggested %"
                         APR_OFF_T_FMT "  bodysize %" APR_OFF_T_FMT ")",
                         dobj->name, dobj->initial_size, dobj->file_size);
            file_cache_errorcleanup(dobj, r);
            return APR_EGENERAL;
        }
    }

    /* All checks were fine, close output file */
    rv = apr_file_close(dobj->fd);
    dobj->fd = NULL;
    if(rv != APR_SUCCESS) {
        file_cache_errorcleanup(dobj, r);
        return rv;
    }

    /* Redirect to cachefile if we copied a plain file */
    if(copy_file) {
        rv = replace_brigade_with_cache(h, r, bb);
        if(rv != APR_SUCCESS) {
            return rv;
        }
    }

    return APR_SUCCESS;
}


static void *create_config(apr_pool_t *p, server_rec *s)
{
    disk_cache_conf *conf = apr_pcalloc(p, sizeof(disk_cache_conf));

    /* XXX: Set default values */
    conf->dirlevels = DEFAULT_DIRLEVELS;
    conf->dirlength = DEFAULT_DIRLENGTH;
    conf->maxfs = DEFAULT_MAX_FILE_SIZE;
    conf->minfs = DEFAULT_MIN_FILE_SIZE;
    conf->updtimeout = DEFAULT_UPDATE_TIMEOUT;

    conf->cache_root = NULL;
    conf->cache_root_len = 0;

    return conf;
}

/*
 * mod_disk_cache configuration directives handlers.
 */
static const char
*set_cache_root(cmd_parms *parms, void *in_struct_ptr, const char *arg)
{
    disk_cache_conf *conf = ap_get_module_config(parms->server->module_config,
                                                 &disk_cache_module);
    conf->cache_root = arg;
    conf->cache_root_len = strlen(arg);
    /* TODO: canonicalize cache_root and strip off any trailing slashes */

    return NULL;
}

/*
 * Consider eliminating the next two directives in favor of
 * Ian's prime number hash...
 * key = hash_fn( r->uri)
 * filename = "/key % prime1 /key %prime2/key %prime3"
 */
static const char
*set_cache_dirlevels(cmd_parms *parms, void *in_struct_ptr, const char *arg)
{
    disk_cache_conf *conf = ap_get_module_config(parms->server->module_config,
                                                 &disk_cache_module);
    int val = atoi(arg);
    if (val < 1)
        return "CacheDirLevels value must be an integer greater than 0";
    if (val * conf->dirlength > CACHEFILE_LEN)
        return "CacheDirLevels*CacheDirLength value must not be higher than 20";
    conf->dirlevels = val;
    return NULL;
}
static const char
*set_cache_dirlength(cmd_parms *parms, void *in_struct_ptr, const char *arg)
{
    disk_cache_conf *conf = ap_get_module_config(parms->server->module_config,
                                                 &disk_cache_module);
    int val = atoi(arg);
    if (val < 1)
        return "CacheDirLength value must be an integer greater than 0";
    if (val * conf->dirlevels > CACHEFILE_LEN)
        return "CacheDirLevels*CacheDirLength value must not be higher than 20";

    conf->dirlength = val;
    return NULL;
}

static const char
*set_cache_minfs(cmd_parms *parms, void *in_struct_ptr, const char *arg)
{
    disk_cache_conf *conf = ap_get_module_config(parms->server->module_config,
                                                 &disk_cache_module);

    if (apr_strtoff(&conf->minfs, arg, NULL, 0) != APR_SUCCESS ||
            conf->minfs < 0) 
    {
        return "CacheMinFileSize argument must be a non-negative integer representing the min size of a file to cache in bytes.";
    }
    return NULL;
}

static const char
*set_cache_maxfs(cmd_parms *parms, void *in_struct_ptr, const char *arg)
{
    disk_cache_conf *conf = ap_get_module_config(parms->server->module_config,
                                                 &disk_cache_module);
    if (apr_strtoff(&conf->maxfs, arg, NULL, 0) != APR_SUCCESS ||
            conf->maxfs < 0) 
    {
        return "CacheMaxFileSize argument must be a non-negative integer representing the max size of a file to cache in bytes.";
    }
    return NULL;
}


static const char
*set_cache_updtimeout(cmd_parms *parms, void *in_struct_ptr, const char *arg)
{
    apr_int64_t val;
    disk_cache_conf *conf = ap_get_module_config(parms->server->module_config,
                                                 &disk_cache_module);

    if (apr_strtoff(&val, arg, NULL, 0) != APR_SUCCESS || val < 0) 
    {
        return "CacheUpdateTimeout argument must be a non-negative integer representing the timeout in milliseconds for cache update operations";
    }

    conf->updtimeout = val * 1000;

    return NULL;
}


static const command_rec disk_cache_cmds[] =
{
    AP_INIT_TAKE1("CacheRoot", set_cache_root, NULL, RSRC_CONF,
                 "The directory to store cache files"),
    AP_INIT_TAKE1("CacheDirLevels", set_cache_dirlevels, NULL, RSRC_CONF,
                  "The number of levels of subdirectories in the cache"),
    AP_INIT_TAKE1("CacheDirLength", set_cache_dirlength, NULL, RSRC_CONF,
                  "The number of characters in subdirectory names"),
    AP_INIT_TAKE1("CacheMinFileSize", set_cache_minfs, NULL, RSRC_CONF,
                  "The minimum file size to cache a document"),
    AP_INIT_TAKE1("CacheMaxFileSize", set_cache_maxfs, NULL, RSRC_CONF,
                  "The maximum file size to cache a document"),
    AP_INIT_TAKE1("CacheUpdateTimeout", set_cache_updtimeout, NULL, RSRC_CONF,
                  "Timeout in ms for cache updates"),
    {NULL}
};

static const cache_provider cache_disk_provider =
{
    &remove_entity,
    &store_headers,
    &store_body,
    &recall_headers,
    &recall_body,
    &create_entity,
    &open_entity,
    &remove_url,
};

static void disk_cache_register_hook(apr_pool_t *p)
{
    /* cache initializer */
    ap_register_provider(p, CACHE_PROVIDER_GROUP, "disk", "0",
                         &cache_disk_provider);
}

module AP_MODULE_DECLARE_DATA disk_cache_module = {
    STANDARD20_MODULE_STUFF,
    NULL,                       /* create per-directory config structure */
    NULL,                       /* merge per-directory config structures */
    create_config,              /* create per-server config structure */
    NULL,                       /* merge per-server config structures */
    disk_cache_cmds,            /* command apr_table_t */
    disk_cache_register_hook    /* register hooks */
};
