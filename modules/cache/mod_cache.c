/* Copyright 2000-2005 The Apache Software Foundation or its licensors, as
 * applicable.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#define CORE_PRIVATE

#include "mod_cache.h"

module AP_MODULE_DECLARE_DATA cache_module;
APR_OPTIONAL_FN_TYPE(ap_cache_generate_key) *cache_generate_key;

/* -------------------------------------------------------------- */


/* Handles for cache filters, resolved at startup to eliminate
 * a name-to-function mapping on each request
 */
static ap_filter_rec_t *cache_save_filter_handle;
static ap_filter_rec_t *cache_out_filter_handle;
static ap_filter_rec_t *cache_remove_url_filter_handle;

/*
 * CACHE handler
 * -------------
 *
 * Can we deliver this request from the cache?
 * If yes:
 *   deliver the content by installing the CACHE_OUT filter.
 * If no:
 *   check whether we're allowed to try cache it
 *   If yes:
 *     add CACHE_SAVE filter
 *   If No:
 *     oh well.
 */

static int cache_url_handler(request_rec *r, int lookup)
{
    apr_status_t rv;
    const char *auth;
    apr_uri_t uri;
    char *path;
    cache_provider_list *providers;
    cache_request_rec *cache;
    cache_server_conf *conf;
    apr_bucket_brigade *out;

    /* Delay initialization until we know we are handling a GET */
    if (r->method_number != M_GET) {
        return DECLINED;
    }

    uri = r->parsed_uri;
    path = uri.path;

    conf = (cache_server_conf *) ap_get_module_config(r->server->module_config,
                                                      &cache_module);

    /*
     * Which cache module (if any) should handle this request?
     */
    if (!(providers = ap_cache_get_providers(r, conf, path))) {
        return DECLINED;
    }

    /* make space for the per request config */
    cache = (cache_request_rec *) ap_get_module_config(r->request_config,
                                                       &cache_module);
    if (!cache) {
        cache = apr_pcalloc(r->pool, sizeof(cache_request_rec));
        ap_set_module_config(r->request_config, &cache_module, cache);
    }

    /* save away the possible providers */
    cache->providers = providers;

    /*
     * Are we allowed to serve cached info at all?
     */

    /* find certain cache controlling headers */
    auth = apr_table_get(r->headers_in, "Authorization");

    /* First things first - does the request allow us to return
     * cached information at all? If not, just decline the request.
     */
    if (auth) {
        return DECLINED;
    }

    /*
     * Try to serve this request from the cache.
     *
     * If no existing cache file (DECLINED)
     *   add cache_save filter
     * If cached file (OK)
     *   clear filter stack
     *   add cache_out filter
     *   return OK
     */
    rv = cache_select(r);
    if (rv != OK) {
        if (rv == DECLINED) {
            if (!lookup) {
                ap_log_error(APLOG_MARK, APLOG_DEBUG, APR_SUCCESS, r->server,
                  "Adding CACHE_SAVE filter.");

                /* add cache_save filter to cache this request */
                ap_add_output_filter_handle(cache_save_filter_handle, NULL, r,
                                            r->connection);

                ap_log_error(APLOG_MARK, APLOG_DEBUG, APR_SUCCESS, r->server,
                             "Adding CACHE_REMOVE_URL filter.");

                /* Add cache_remove_url filter to this request to remove a
                 * stale cache entry if needed. Also put the current cache
                 * request rec in the filter context, as the request that
                 * is available later during running the filter maybe
                 * different due to an internal redirect.
                 */
                cache->remove_url_filter =
                    ap_add_output_filter_handle(cache_remove_url_filter_handle, 
                                                cache, r, r->connection);
            }
            else if (cache->stale_headers) {
                ap_log_error(APLOG_MARK, APLOG_DEBUG, APR_SUCCESS, r->server,
                  "Restoring request headers.");

                r->headers_in = cache->stale_headers;
            }
        }
        else {
            /* error */
            ap_log_error(APLOG_MARK, APLOG_ERR, rv, r->server,
                         "cache: error returned while checking for cached "
                         "file by %s cache", cache->provider_name);
        }
        return DECLINED;
    }

    rv = ap_meets_conditions(r);
    if (rv != OK) {
        /* Return cached status. */
        return rv;
    }

    /* If we're a lookup, we can exit now instead of serving the content. */
    if (lookup) {
        return OK;
    }

    /* Serve up the content */

    /* We are in the quick handler hook, which means that no output
     * filters have been set. So lets run the insert_filter hook.
     */
    ap_run_insert_filter(r);
    ap_add_output_filter_handle(cache_out_filter_handle, NULL,
                                r, r->connection);

    /* kick off the filter stack */
    out = apr_brigade_create(r->pool, r->connection->bucket_alloc);
    rv = ap_pass_brigade(r->output_filters, out);
    if (rv != APR_SUCCESS) {
        ap_log_error(APLOG_MARK, APLOG_ERR, rv, r->server,
                     "cache: error returned while trying to return %s "
                     "cached data", 
                     cache->provider_name);
        return rv;
    }

    return OK;
}

/*
 * CACHE_OUT filter
 * ----------------
 *
 * Deliver cached content (headers and body) up the stack.
 */
static int cache_out_filter(ap_filter_t *f, apr_bucket_brigade *bb)
{
    request_rec *r = f->r;
    cache_request_rec *cache;

    cache = (cache_request_rec *) ap_get_module_config(r->request_config, 
                                                       &cache_module);

    if (!cache) {
        /* user likely configured CACHE_OUT manually; they should use mod_cache
         * configuration to do that */
        ap_log_error(APLOG_MARK, APLOG_ERR, 0, r->server,
                     "CACHE_OUT enabled unexpectedly");
        ap_remove_output_filter(f);
        return ap_pass_brigade(f->next, bb);
    }

    ap_log_error(APLOG_MARK, APLOG_DEBUG, APR_SUCCESS, r->server,
                 "cache: running CACHE_OUT filter");

    /* restore status of cached response */
    /* XXX: This exposes a bug in mem_cache, since it does not 
     * restore the status into it's handle. */
    r->status = cache->handle->cache_obj->info.status;

    /* recall_headers() was called in cache_select() */
    cache->provider->recall_body(cache->handle, r->pool, bb);

    /* This filter is done once it has served up its content */
    ap_remove_output_filter(f);

    ap_log_error(APLOG_MARK, APLOG_DEBUG, APR_SUCCESS, r->server,
                 "cache: serving %s", r->uri);
    return ap_pass_brigade(f->next, bb);
}


/*
 * CACHE_SAVE filter
 * ---------------
 *
 * Decide whether or not this content should be cached.
 * If we decide no it should not:
 *   remove the filter from the chain
 * If we decide yes it should:
 *   Have we already started saving the response?
 *      If we have started, pass the data to the storage manager via store_body
 *      Otherwise:
 *        Check to see if we *can* save this particular response.
 *        If we can, call cache_create_entity() and save the headers and body
 *   Finally, pass the data to the next filter (the network or whatever)
 */

static int cache_save_filter(ap_filter_t *f, apr_bucket_brigade *in)
{
    int rv = !OK;
    int date_in_errhdr = 0;
    request_rec *r = f->r;
    cache_request_rec *cache;
    cache_server_conf *conf;
    char *url = r->unparsed_uri;
    const char *cc_out, *cl;
    const char *exps, *lastmods, *dates, *etag;
    apr_time_t exp, date, lastmod, now;
    apr_off_t size;
    cache_info *info = NULL;
    char *reason;
    apr_pool_t *p;

    conf = (cache_server_conf *) ap_get_module_config(r->server->module_config,
                                                      &cache_module);

    /* Setup cache_request_rec */
    cache = (cache_request_rec *) ap_get_module_config(r->request_config,
                                                       &cache_module);
    if (!cache) {
        /* user likely configured CACHE_SAVE manually; they should really use
         * mod_cache configuration to do that
         */
        cache = apr_pcalloc(r->pool, sizeof(cache_request_rec));
        ap_set_module_config(r->request_config, &cache_module, cache);
    }

    reason = NULL;
    p = r->pool;
    /*
     * Pass Data to Cache
     * ------------------
     * This section passes the brigades into the cache modules, but only
     * if the setup section (see below) is complete.
     */
    if (cache->block_response) {
        /* We've already sent down the response and EOS.  So, ignore
         * whatever comes now.
         */
        return APR_SUCCESS;
    }

    /* have we already run the cachability check and set up the
     * cached file handle? 
     */
    if (cache->in_checked) {
        /* pass the brigades into the cache, then pass them
         * up the filter stack
         */
        rv = cache->provider->store_body(cache->handle, r, in);
        if (rv != APR_SUCCESS) {
            ap_log_error(APLOG_MARK, APLOG_DEBUG, rv, r->server,
                         "cache: Cache provider's store_body failed!");
            ap_remove_output_filter(f);
        }
        return ap_pass_brigade(f->next, in);
    }

    /*
     * Setup Data in Cache
     * -------------------
     * This section opens the cache entity and sets various caching
     * parameters, and decides whether this URL should be cached at
     * all. This section is* run before the above section.
     */

    /* read expiry date; if a bad date, then leave it so the client can
     * read it 
     */
    exps = apr_table_get(r->err_headers_out, "Expires");
    if (exps == NULL) {
        exps = apr_table_get(r->headers_out, "Expires");
    }
    if (exps != NULL) {
        if (APR_DATE_BAD == (exp = apr_date_parse_http(exps))) {
            exps = NULL;
        }
    }
    else {
        exp = APR_DATE_BAD;
    }

    /* read the last-modified date; if the date is bad, then delete it */
    lastmods = apr_table_get(r->err_headers_out, "Last-Modified");
    if (lastmods == NULL) {
        lastmods = apr_table_get(r->headers_out, "Last-Modified");
    }
    if (lastmods != NULL) {
        lastmod = apr_date_parse_http(lastmods);
        if (lastmod == APR_DATE_BAD) {
            lastmods = NULL;
        }
    }
    else {
        lastmod = APR_DATE_BAD;
    }

    /* read the etag and cache-control from the entity */
    etag = apr_table_get(r->err_headers_out, "Etag");
    if (etag == NULL) {
        etag = apr_table_get(r->headers_out, "Etag");
    }
    cc_out = apr_table_get(r->err_headers_out, "Cache-Control");
    if (cc_out == NULL) {
        cc_out = apr_table_get(r->headers_out, "Cache-Control");
    }

    /*
     * what responses should we not cache?
     *
     * At this point we decide based on the response headers whether it
     * is appropriate _NOT_ to cache the data from the server. There are
     * a whole lot of conditions that prevent us from caching this data.
     * They are tested here one by one to be clear and unambiguous. 
     */
    if (r->status != HTTP_OK && r->status != HTTP_NON_AUTHORITATIVE
        && r->status != HTTP_MULTIPLE_CHOICES
        && r->status != HTTP_MOVED_PERMANENTLY
        && r->status != HTTP_NOT_MODIFIED) {
        /* RFC2616 13.4 we are allowed to cache 200, 203, 206, 300, 301 or 410
         * We don't cache 206, because we don't (yet) cache partial responses.
         * We include 304 Not Modified here too as this is the origin server
         * telling us to serve the cached copy.
         */
        reason = apr_psprintf(p, "Response status %d", r->status);
    } 
    else if (exps != NULL && exp == APR_DATE_BAD) {
        /* if a broken Expires header is present, don't cache it */
        reason = apr_pstrcat(p, "Broken expires header: ", exps, NULL);
    }
    else if (r->args && exps == NULL) {
        /* if query string present but no expiration time, don't cache it
         * (RFC 2616/13.9)
         */
        reason = "Query string present but no expires header";
    }
    else if (r->status == HTTP_NOT_MODIFIED &&
             !cache->handle && !cache->stale_handle) {
        /* if the server said 304 Not Modified but we have no cache
         * file - pass this untouched to the user agent, it's not for us.
         */
        reason = "HTTP Status 304 Not Modified";
    }
    else if (r->status == HTTP_OK && lastmods == NULL && etag == NULL 
             && (exps == NULL) && (conf->no_last_mod_ignore ==0)) {
        /* 200 OK response from HTTP/1.0 and up without Last-Modified,
         * Etag, or Expires headers.
         */
        /* Note: mod-include clears last_modified/expires/etags - this
         * is why we have an optional function for a key-gen ;-) 
         */
        reason = "No Last-Modified, Etag, or Expires headers";
    }
    else if (r->header_only) {
        /* HEAD requests */
        reason = "HTTP HEAD request";
    }
    else if (!conf->store_nostore &&
             ap_cache_liststr(NULL, cc_out, "no-store", NULL)) {
        /* RFC2616 14.9.2 Cache-Control: no-store response
         * indicating do not cache, or stop now if you are
         * trying to cache it.
         */
        /* FIXME: The Cache-Control: no-store could have come in on a 304,
         * FIXME: while the original request wasn't conditional.  IOW, we
         * FIXME:  made the the request conditional earlier to revalidate
         * FIXME: our cached response.
         */
        reason = "Cache-Control: no-store present";
    }
    else if (!conf->store_private &&
             ap_cache_liststr(NULL, cc_out, "private", NULL)) {
        /* RFC2616 14.9.1 Cache-Control: private response
         * this object is marked for this user's eyes only. Behave
         * as a tunnel.
         */
        /* FIXME: See above (no-store) */
        reason = "Cache-Control: private present";
    }
    else if (apr_table_get(r->headers_in, "Authorization") != NULL
             && !(ap_cache_liststr(NULL, cc_out, "s-maxage", NULL)
                  || ap_cache_liststr(NULL, cc_out, "must-revalidate", NULL)
                  || ap_cache_liststr(NULL, cc_out, "public", NULL))) {
        /* RFC2616 14.8 Authorisation:
         * if authorisation is included in the request, we don't cache,
         * but we can cache if the following exceptions are true:
         * 1) If Cache-Control: s-maxage is included
         * 2) If Cache-Control: must-revalidate is included
         * 3) If Cache-Control: public is included
         */
        reason = "Authorization required";
    }
    else if (ap_cache_liststr(NULL, 
                              apr_table_get(r->headers_out, "Vary"),
                              "*", NULL)) {
        reason = "Vary header contains '*'";
    }
    else if (r->no_cache) {
        /* or we've been asked not to cache it above */
        reason = "r->no_cache present";
    }

    if (reason) {
        ap_log_error(APLOG_MARK, APLOG_DEBUG, 0, r->server,
                     "cache: %s not cached. Reason: %s", url, reason);

        /* remove this filter from the chain */
        ap_remove_output_filter(f);

        /* ship the data up the stack */
        return ap_pass_brigade(f->next, in);
    }

    /* Make it so that we don't execute this path again. */
    cache->in_checked = 1;

    /* Set the content length if known. 
     */
    cl = apr_table_get(r->err_headers_out, "Content-Length");
    if (cl == NULL) {
        cl = apr_table_get(r->headers_out, "Content-Length");
    }
    if (cl) {
        char *errp;
        if (apr_strtoff(&size, cl, &errp, 10) || *errp || size < 0) {
            cl = NULL; /* parse error, see next 'if' block */
        }
    }

    if (!cl) {
        /* if we don't get the content-length, see if we have all the 
         * buckets and use their length to calculate the size 
         */
        apr_bucket *e;
        int all_buckets_here=0;
        int unresolved_length = 0;
        size=0;
        for (e = APR_BRIGADE_FIRST(in);
             e != APR_BRIGADE_SENTINEL(in);
             e = APR_BUCKET_NEXT(e))
        {
            if (APR_BUCKET_IS_EOS(e)) {
                all_buckets_here=1;
                break;
            }
            if (APR_BUCKET_IS_FLUSH(e)) {
                unresolved_length = 1;
                continue;
            }
            if (e->length == (apr_size_t)-1) {
                break;
            }
            size += e->length;
        }
        if (!all_buckets_here) {
            size = -1;
        }
    }

    /* It's safe to cache the response.
     *
     * There are two possiblities at this point:
     * - cache->handle == NULL. In this case there is no previously
     * cached entity anywhere on the system. We must create a brand
     * new entity and store the response in it.
     * - cache->stale_handle != NULL. In this case there is a stale
     * entity in the system which needs to be replaced by new
     * content (unless the result was 304 Not Modified, which means
     * the cached entity is actually fresh, and we should update
     * the headers).
     */

    /* Did we have a stale cache entry that really is stale? */
    if (cache->stale_handle) {
        if (r->status == HTTP_NOT_MODIFIED) {
            /* Oh, hey.  It isn't that stale!  Yay! */
            cache->handle = cache->stale_handle;
            info = &cache->handle->cache_obj->info;
            rv = OK;
        }
        else {
            /* Oh, well.  Toss it. */
            cache->provider->remove_entity(cache->stale_handle);
            /* Treat the request as if it wasn't conditional. */
            cache->stale_handle = NULL;
        }
    }

    /* no cache handle, create a new entity */
    if (!cache->handle) {
        rv = cache_create_entity(r, url, size);
        info = apr_pcalloc(r->pool, sizeof(cache_info));
        /* We only set info->status upon the initial creation. */
        info->status = r->status;
    }

    if (rv != OK) {
        /* Caching layer declined the opportunity to cache the response */
        ap_remove_output_filter(f);
        return ap_pass_brigade(f->next, in);
    }

    ap_log_error(APLOG_MARK, APLOG_DEBUG, 0, r->server,
                 "cache: Caching url: %s", url);

    /* We are actually caching this response. So it does not
     * make sense to remove this entity any more.
     */
    ap_log_error(APLOG_MARK, APLOG_DEBUG, 0, r->server,
                 "cache: Removing CACHE_REMOVE_URL filter.");
    ap_remove_output_filter(cache->remove_url_filter);

    /*
     * We now want to update the cache file header information with
     * the new date, last modified, expire and content length and write
     * it away to our cache file. First, we determine these values from
     * the response, using heuristics if appropriate.
     *
     * In addition, we make HTTP/1.1 age calculations and write them away
     * too.
     */

    /* Read the date. Generate one if one is not supplied */
    dates = apr_table_get(r->err_headers_out, "Date");
    if (dates != NULL) {
        date_in_errhdr = 1;
    }
    else {
        dates = apr_table_get(r->headers_out, "Date");
    }
    if (dates != NULL) {
        info->date = apr_date_parse_http(dates);
    }
    else {
        info->date = APR_DATE_BAD;
    }

    now = apr_time_now();
    if (info->date == APR_DATE_BAD) {  /* No, or bad date */
        char *dates;
        /* no date header (or bad header)! */
        /* add one; N.B. use the time _now_ rather than when we were checking
         * the cache 
         */
        if (date_in_errhdr == 1) {
            apr_table_unset(r->err_headers_out, "Date");
        }
        date = now;
        dates = apr_pcalloc(r->pool, MAX_STRING_LEN);
        apr_rfc822_date(dates, now);
        apr_table_set(r->headers_out, "Date", dates);
        ap_log_error(APLOG_MARK, APLOG_DEBUG, 0, r->server,
                     "cache: Added date header");
        info->date = date;
    }
    else {
        date = info->date;
    }

    /* set response_time for HTTP/1.1 age calculations */
    info->response_time = now;

    /* get the request time */
    info->request_time = r->request_time;

    /* check last-modified date */
    if (lastmod != APR_DATE_BAD && lastmod > date) {
        /* if it's in the future, then replace by date */
        lastmod = date;
        lastmods = dates;
        ap_log_error(APLOG_MARK, APLOG_DEBUG, 0, 
                     r->server,
                     "cache: Last modified is in the future, "
                     "replacing with now");
    }

    /* if no expiry date then
     *   if lastmod
     *      expiry date = date + min((date - lastmod) * factor, maxexpire)
     *   else
     *      expire date = date + defaultexpire
     */
    if (exp == APR_DATE_BAD) {
        char expire_hdr[APR_RFC822_DATE_LEN];

        /* if lastmod == date then you get 0*conf->factor which results in
         *   an expiration time of now. This causes some problems with
         *   freshness calculations, so we choose the else path...
         */
        if ((lastmod != APR_DATE_BAD) && (lastmod < date)) {
            apr_time_t x = (apr_time_t) ((date - lastmod) * conf->factor);

            if (x > conf->maxex) {
                x = conf->maxex;
            }
            exp = date + x;
            apr_rfc822_date(expire_hdr, exp);
            apr_table_set(r->headers_out, "Expires", expire_hdr);
        }
        else {
            exp = date + conf->defex;
            apr_rfc822_date(expire_hdr, exp);
            apr_table_set(r->headers_out, "Expires", expire_hdr);
        }
    }
    info->expire = exp;

    /* We found a stale entry which wasn't really stale. */
    if (cache->stale_handle) {
        /* Load in the saved status and clear the status line. */
        r->status = info->status;
        r->status_line = NULL;

        /* RFC 2616 10.3.5 states that entity headers are not supposed
         * to be in the 304 response.  Therefore, we need to combine the
         * response headers with the cached headers *before* we update
         * the cached headers.
         *
         * However, before doing that, we need to first merge in
         * err_headers_out and we also need to strip any hop-by-hop
         * headers that might have snuck in.
         */
        r->headers_out = apr_table_overlay(r->pool, r->headers_out,
                                           r->err_headers_out);
        r->headers_out = ap_cache_cacheable_hdrs_out(r->pool, r->headers_out,
                                                     r->server);
        apr_table_clear(r->err_headers_out);

        /* Merge in our cached headers.  However, keep any updated values. */
        ap_cache_accept_headers(cache->handle, r, 1);
    }

    /* Write away header information to cache. It is possible that we are
     * trying to update headers for an entity which has already been cached.
     *
     * This may fail, due to an unwritable cache area. E.g. filesystem full,
     * permissions problems or a read-only (re)mount. This must be handled 
     * later.
     */
    rv = cache->provider->store_headers(cache->handle, r, info);

    /* Did we just update the cached headers on a revalidated response?
     *
     * If so, we can now decide what to serve to the client.  This is done in
     * the same way as with a regular response, but conditions are now checked
     * against the cached or merged response headers.
     */
    if (cache->stale_handle) {
        apr_bucket_brigade *bb;
        apr_bucket *bkt;
        int status;

        bb = apr_brigade_create(r->pool, r->connection->bucket_alloc);

        /* Restore the original request headers and see if we need to
         * return anything else than the cached response (ie. the original
         * request was conditional).
         */
        r->headers_in = cache->stale_headers;
        status = ap_meets_conditions(r);
        if (status != OK) {
            r->status = status;

            bkt = apr_bucket_flush_create(bb->bucket_alloc);
            APR_BRIGADE_INSERT_TAIL(bb, bkt);
        }
        else {
            cache->provider->recall_body(cache->handle, r->pool, bb);
        }

        cache->block_response = 1;

        /* Before returning we need to handle the possible case of an
         * unwritable cache. Rather than leaving the entity in the cache
         * and having it constantly re-validated, now that we have recalled 
         * the body it is safe to try and remove the url from the cache.
         */
        if (rv != APR_SUCCESS) {
            ap_log_error(APLOG_MARK, APLOG_DEBUG, rv, r->server,
                         "cache: updating headers with store_headers failed. "
                         "Removing cached url.");

            rv = cache->provider->remove_url(cache->stale_handle, r->pool);
            if (rv != OK) {
                /* Probably a mod_disk_cache cache area has been (re)mounted 
                 * read-only, or that there is a permissions problem. 
                 */
                ap_log_error(APLOG_MARK, APLOG_DEBUG, rv, r->server,
                     "cache: attempt to remove url from cache unsuccessful.");
            }
        }

        return ap_pass_brigade(f->next, bb);
    }

    if(rv != APR_SUCCESS) {
        ap_log_error(APLOG_MARK, APLOG_DEBUG, rv, r->server,
                     "cache: store_headers failed");
        ap_remove_output_filter(f);

        return ap_pass_brigade(f->next, in);
    }

    rv = cache->provider->store_body(cache->handle, r, in);
    if (rv != APR_SUCCESS) {
        ap_log_error(APLOG_MARK, APLOG_DEBUG, rv, r->server,
                     "cache: store_body failed");
        ap_remove_output_filter(f);
    }

    return ap_pass_brigade(f->next, in);
}

/*
 * CACHE_REMOVE_URL filter
 * ---------------
 *
 * This filter gets added in the quick handler every time the CACHE_SAVE filter
 * gets inserted. Its purpose is to remove a confirmed stale cache entry from
 * the cache.
 *
 * CACHE_REMOVE_URL has to be a protocol filter to ensure that is run even if
 * the response is a canned error message, which removes the content filters
 * and thus the CACHE_SAVE filter from the chain.
 *
 * CACHE_REMOVE_URL expects cache request rec within its context because the
 * request this filter runs on can be different from the one whose cache entry
 * should be removed, due to internal redirects.
 *
 * Note that CACHE_SAVE_URL (as a content-set filter, hence run before the
 * protocol filters) will remove this filter if it decides to cache the file.
 * Therefore, if this filter is left in, it must mean we need to toss any
 * existing files.
 */
static int cache_remove_url_filter(ap_filter_t *f, apr_bucket_brigade *in)
{
    request_rec *r = f->r;
    cache_request_rec *cache;

    /* Setup cache_request_rec */
    cache = (cache_request_rec *) f->ctx;

    if (!cache) {
        /* user likely configured CACHE_REMOVE_URL manually; they should really 
         * use mod_cache configuration to do that. So:
         * 1. Remove ourselves 
         * 2. Do nothing and bail out
         */
        ap_log_error(APLOG_MARK, APLOG_DEBUG, 0, r->server,
                     "cache: CACHE_REMOVE_URL enabled unexpectedly");
        ap_remove_output_filter(f);
        return ap_pass_brigade(f->next, in);
    }
    /* Now remove this cache entry from the cache */
    cache_remove_url(cache, r->pool);

    /* remove ourselves */
    ap_remove_output_filter(f);
    return ap_pass_brigade(f->next, in);
}

/* -------------------------------------------------------------- */
/* Setup configurable data */

static void * create_cache_config(apr_pool_t *p, server_rec *s)
{
    cache_server_conf *ps = apr_pcalloc(p, sizeof(cache_server_conf));

    /* array of URL prefixes for which caching is enabled */
    ps->cacheenable = apr_array_make(p, 10, sizeof(struct cache_enable));
    /* array of URL prefixes for which caching is disabled */
    ps->cachedisable = apr_array_make(p, 10, sizeof(struct cache_disable));
    /* maximum time to cache a document */
    ps->maxex = DEFAULT_CACHE_MAXEXPIRE;
    ps->maxex_set = 0;
    /* default time to cache a document */
    ps->defex = DEFAULT_CACHE_EXPIRE;
    ps->defex_set = 0;
    /* factor used to estimate Expires date from LastModified date */
    ps->factor = DEFAULT_CACHE_LMFACTOR;
    ps->factor_set = 0;
    ps->no_last_mod_ignore_set = 0;
    ps->no_last_mod_ignore = 0;
    ps->ignorecachecontrol = 0;
    ps->ignorecachecontrol_set = 0;
    ps->store_private = 0;
    ps->store_private_set = 0;
    ps->store_nostore = 0;
    ps->store_nostore_set = 0;
    /* array of headers that should not be stored in cache */
    ps->ignore_headers = apr_array_make(p, 10, sizeof(char *));
    ps->ignore_headers_set = CACHE_IGNORE_HEADERS_UNSET;
    return ps;
}

static void * merge_cache_config(apr_pool_t *p, void *basev, void *overridesv)
{
    cache_server_conf *ps = apr_pcalloc(p, sizeof(cache_server_conf));
    cache_server_conf *base = (cache_server_conf *) basev;
    cache_server_conf *overrides = (cache_server_conf *) overridesv;

    /* array of URL prefixes for which caching is disabled */
    ps->cachedisable = apr_array_append(p, 
                                        base->cachedisable, 
                                        overrides->cachedisable);
    /* array of URL prefixes for which caching is enabled */
    ps->cacheenable = apr_array_append(p, 
                                       base->cacheenable, 
                                       overrides->cacheenable);
    /* maximum time to cache a document */
    ps->maxex = (overrides->maxex_set == 0) ? base->maxex : overrides->maxex;
    /* default time to cache a document */
    ps->defex = (overrides->defex_set == 0) ? base->defex : overrides->defex;
    /* factor used to estimate Expires date from LastModified date */
    ps->factor =
        (overrides->factor_set == 0) ? base->factor : overrides->factor;

    ps->no_last_mod_ignore =
        (overrides->no_last_mod_ignore_set == 0)
        ? base->no_last_mod_ignore
        : overrides->no_last_mod_ignore;
    ps->ignorecachecontrol  =
        (overrides->ignorecachecontrol_set == 0)
        ? base->ignorecachecontrol
        : overrides->ignorecachecontrol;
    ps->store_private  =
        (overrides->store_private_set == 0)
        ? base->store_private
        : overrides->store_private;
    ps->store_nostore  =
        (overrides->store_nostore_set == 0)
        ? base->store_nostore
        : overrides->store_nostore;
    ps->ignore_headers =
        (overrides->ignore_headers_set == CACHE_IGNORE_HEADERS_UNSET)
        ? base->ignore_headers
        : overrides->ignore_headers;
    return ps;
}
static const char *set_cache_ignore_no_last_mod(cmd_parms *parms, void *dummy,
                                                int flag)
{
    cache_server_conf *conf;

    conf =
        (cache_server_conf *)ap_get_module_config(parms->server->module_config,
                                                  &cache_module);
    conf->no_last_mod_ignore = flag;
    conf->no_last_mod_ignore_set = 1;
    return NULL;

}

static const char *set_cache_ignore_cachecontrol(cmd_parms *parms,
                                                 void *dummy, int flag)
{
    cache_server_conf *conf;

    conf =
        (cache_server_conf *)ap_get_module_config(parms->server->module_config,
                                                  &cache_module);
    conf->ignorecachecontrol = flag;
    conf->ignorecachecontrol_set = 1;
    return NULL;
}

static const char *set_cache_store_private(cmd_parms *parms, void *dummy,
                                           int flag)
{
    cache_server_conf *conf;

    conf =
        (cache_server_conf *)ap_get_module_config(parms->server->module_config,
                                                  &cache_module);
    conf->store_private = flag;
    conf->store_private_set = 1;
    return NULL;
}

static const char *set_cache_store_nostore(cmd_parms *parms, void *dummy,
                                           int flag)
{
    cache_server_conf *conf;

    conf =
        (cache_server_conf *)ap_get_module_config(parms->server->module_config,
                                                  &cache_module);
    conf->store_nostore = flag;
    conf->store_nostore_set = 1;
    return NULL;
}

static const char *add_ignore_header(cmd_parms *parms, void *dummy,
                                     const char *header)
{
    cache_server_conf *conf;
    char **new;

    conf =
        (cache_server_conf *)ap_get_module_config(parms->server->module_config,
                                                  &cache_module);
    if (!strncasecmp(header, "None", 4)) {
        /* if header None is listed clear array */
        conf->ignore_headers->nelts = 0;
    }
    else {
        if ((conf->ignore_headers_set == CACHE_IGNORE_HEADERS_UNSET) ||
            (conf->ignore_headers->nelts)) {
            /* Only add header if no "None" has been found in header list
             * so far.
             * (When 'None' is passed, IGNORE_HEADERS_SET && nelts == 0.)
             */
            new = (char **)apr_array_push(conf->ignore_headers);
            (*new) = (char *)header;
        }
    }
    conf->ignore_headers_set = CACHE_IGNORE_HEADERS_SET;
    return NULL;
}

static const char *add_cache_enable(cmd_parms *parms, void *dummy, 
                                    const char *type, 
                                    const char *url)
{
    cache_server_conf *conf;
    struct cache_enable *new;

    if (*type == '/') {
        return apr_psprintf(parms->pool,
          "provider (%s) starts with a '/'.  Are url and provider switched?",
          type);
    }

    conf =
        (cache_server_conf *)ap_get_module_config(parms->server->module_config,
                                                  &cache_module);
    new = apr_array_push(conf->cacheenable);
    new->type = type;
    new->url = url;
    new->urllen = strlen(url);
    return NULL;
}

static const char *add_cache_disable(cmd_parms *parms, void *dummy,
                                     const char *url)
{
    cache_server_conf *conf;
    struct cache_disable *new;

    conf =
        (cache_server_conf *)ap_get_module_config(parms->server->module_config,
                                                  &cache_module);
    new = apr_array_push(conf->cachedisable);
    new->url = url;
    new->urllen = strlen(url);
    return NULL;
}

static const char *set_cache_maxex(cmd_parms *parms, void *dummy,
                                   const char *arg)
{
    cache_server_conf *conf;

    conf =
        (cache_server_conf *)ap_get_module_config(parms->server->module_config,
                                                  &cache_module);
    conf->maxex = (apr_time_t) (atol(arg) * MSEC_ONE_SEC);
    conf->maxex_set = 1;
    return NULL;
}

static const char *set_cache_defex(cmd_parms *parms, void *dummy,
                                   const char *arg)
{
    cache_server_conf *conf;

    conf =
        (cache_server_conf *)ap_get_module_config(parms->server->module_config,
                                                  &cache_module);
    conf->defex = (apr_time_t) (atol(arg) * MSEC_ONE_SEC);
    conf->defex_set = 1;
    return NULL;
}

static const char *set_cache_factor(cmd_parms *parms, void *dummy,
                                    const char *arg)
{
    cache_server_conf *conf;
    double val;

    conf =
        (cache_server_conf *)ap_get_module_config(parms->server->module_config,
                                                  &cache_module);
    if (sscanf(arg, "%lg", &val) != 1) {
        return "CacheLastModifiedFactor value must be a float";
    }
    conf->factor = val;
    conf->factor_set = 1;
    return NULL;
}

static int cache_post_config(apr_pool_t *p, apr_pool_t *plog,
                             apr_pool_t *ptemp, server_rec *s)
{
    /* This is the means by which unusual (non-unix) os's may find alternate
     * means to run a given command (e.g. shebang/registry parsing on Win32)
     */
    cache_generate_key = APR_RETRIEVE_OPTIONAL_FN(ap_cache_generate_key);
    if (!cache_generate_key) {
        cache_generate_key = cache_generate_key_default;
    }
    return OK;
}


static const command_rec cache_cmds[] =
{
    /* XXX
     * Consider a new config directive that enables loading specific cache
     * implememtations (like mod_cache_mem, mod_cache_file, etc.).
     * Rather than using a LoadModule directive, admin would use something
     * like CacheModule  mem_cache_module | file_cache_module, etc,
     * which would cause the approprpriate cache module to be loaded.
     * This is more intuitive that requiring a LoadModule directive.
     */

    AP_INIT_TAKE2("CacheEnable", add_cache_enable, NULL, RSRC_CONF,
                  "A cache type and partial URL prefix below which "
                  "caching is enabled"),
    AP_INIT_TAKE1("CacheDisable", add_cache_disable, NULL, RSRC_CONF,
                  "A partial URL prefix below which caching is disabled"),
    AP_INIT_TAKE1("CacheMaxExpire", set_cache_maxex, NULL, RSRC_CONF,
                  "The maximum time in seconds to cache a document"),
    AP_INIT_TAKE1("CacheDefaultExpire", set_cache_defex, NULL, RSRC_CONF,
                  "The default time in seconds to cache a document"),
    AP_INIT_FLAG("CacheIgnoreNoLastMod", set_cache_ignore_no_last_mod, NULL,
                 RSRC_CONF,
                 "Ignore Responses where there is no Last Modified Header"),
    AP_INIT_FLAG("CacheIgnoreCacheControl", set_cache_ignore_cachecontrol,
                 NULL, RSRC_CONF,
                 "Ignore requests from the client for uncached content"),
    AP_INIT_FLAG("CacheStorePrivate", set_cache_store_private,
                 NULL, RSRC_CONF,
                 "Ignore 'Cache-Control: private' and store private content"),
    AP_INIT_FLAG("CacheStoreNoStore", set_cache_store_nostore,
                 NULL, RSRC_CONF,
                 "Ignore 'Cache-Control: no-store' and store sensitive content"),
    AP_INIT_ITERATE("CacheIgnoreHeaders", add_ignore_header, NULL, RSRC_CONF,
                    "A space separated list of headers that should not be "
                    "stored by the cache"),
    AP_INIT_TAKE1("CacheLastModifiedFactor", set_cache_factor, NULL, RSRC_CONF,
                  "The factor used to estimate Expires date from "
                  "LastModified date"),
    {NULL}
};

static void register_hooks(apr_pool_t *p)
{
    /* cache initializer */
    /* cache handler */
    ap_hook_quick_handler(cache_url_handler, NULL, NULL, APR_HOOK_FIRST);
    /* cache filters 
     * XXX The cache filters need to run right after the handlers and before
     * any other filters. Consider creating AP_FTYPE_CACHE for this purpose.
     * Make them AP_FTYPE_CONTENT for now.
     * XXX ianhH:they should run AFTER all the other content filters.
     */
    cache_save_filter_handle = 
        ap_register_output_filter("CACHE_SAVE", 
                                  cache_save_filter, 
                                  NULL,
                                  AP_FTYPE_CONTENT_SET+1);
    /* CACHE_OUT must go into the filter chain before SUBREQ_CORE to
     * handle subrequsts. Decrementing filter type by 1 ensures this 
     * happens.
     */
    cache_out_filter_handle = 
        ap_register_output_filter("CACHE_OUT", 
                                  cache_out_filter, 
                                  NULL,
                                  AP_FTYPE_CONTENT_SET+1);
    /* CACHE_REMOVE_URL has to be a protocol filter to ensure that is
     * run even if the response is a canned error message, which
     * removes the content filters.
     */
    cache_remove_url_filter_handle =
        ap_register_output_filter("CACHE_REMOVE_URL",
                                  cache_remove_url_filter,
                                  NULL,
                                  AP_FTYPE_PROTOCOL);
    ap_hook_post_config(cache_post_config, NULL, NULL, APR_HOOK_REALLY_FIRST);
}

module AP_MODULE_DECLARE_DATA cache_module =
{
    STANDARD20_MODULE_STUFF,
    NULL,                   /* create per-directory config structure */
    NULL,                   /* merge per-directory config structures */
    create_cache_config,    /* create per-server config structure */
    merge_cache_config,     /* merge per-server config structures */
    cache_cmds,             /* command apr_table_t */
    register_hooks
};
