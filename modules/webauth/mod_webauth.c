/*
 * Core WebAuth Apache module code.
 *
 * Written by Roland Schemers
 * Copyright 2002, 2003, 2004, 2006, 2008, 2009, 2010
 *     Board of Trustees, Leland Stanford Jr. University
 *
 * See LICENSE for licensing terms.
 */

#include <modules/webauth/mod_webauth.h>
#include <util/macros.h>

module AP_MODULE_DECLARE_DATA webauth_module;


static void
dont_cache(MWA_REQ_CTXT *rc)
{
    rc->r->no_cache = 1;
    rc->r->mtime = apr_time_now();
    apr_table_addn(rc->r->err_headers_out, "Pragma", "no-cache");
    apr_table_setn(rc->r->err_headers_out, "Cache-Control",
                   "no-cache, no-store");
}


static int
do_redirect(MWA_REQ_CTXT *rc)
{
    dont_cache(rc);
    return HTTP_MOVED_TEMPORARILY;
}


/*
 * remove a string from the end of another string
 */
static void
strip_end(char *c, const char *t)
{
    char *p;
    if (c != NULL) {
        p = ap_strstr(c, t);
        if (p != NULL)
            *p = '\0';
    }
}


/*
 * return 1 if current request is "https"
 */
static int
is_https(request_rec *r)
{
    const char *scheme;

    /* Apache 2.2 renamed this function but there doesn't appear to be a good
       way of detecting Apache 2.2.  It did, however, also rename a macro, so
       use that as a cheat. */
#ifdef ap_http_method
    scheme = ap_run_http_method(r);
#else
    scheme = ap_run_http_scheme(r);
#endif
    return (scheme != NULL) && strcmp(scheme, "https") == 0;
}


/* remove any webauth_* cookies and tokens from Referer
   before proxying the request */
static void
strip_webauth_info(MWA_REQ_CTXT *rc)
{
    char *c;
    size_t cookie_start, copy;
    char *d, *s;
    const char *mwa_func = "strip_webauth_cookies";

    c = (char*) apr_table_get(rc->r->headers_in, "Referer");
    if (c != NULL)
        strip_end(c, WEBAUTHR_MAGIC);

    c = (char*) apr_table_get(rc->r->headers_in, "Cookie");

    if (c == NULL || (ap_strstr(c, "webauth_") == NULL))
        return;

    if (rc->sconf->debug)
        ap_log_error(APLOG_MARK, APLOG_DEBUG, 0, rc->r->server,
                     "mod_webauth: %s: need to strip: %s", mwa_func, c);
    s = d = c;
    cookie_start = copy = 1;
    while (*s) {
        if (cookie_start && *s != ' ') {
            copy = strncmp(s, "webauth_", 8) != 0;
            cookie_start = 0;
        } else if (*s == ';') {
            cookie_start = 1;
        }
        if (copy) {
            if (d != s)
                *d = *s;
            d++;
        }
        s++;
    }

    /* strip of trailing space */
    while (d > c && *(d-1) == ' ')
        d--;

    /* null-terminate */
    *d = '\0';

    if (*c == '\0') {
        apr_table_unset(rc->r->headers_in, "Cookie");
        if (rc->sconf->debug)
            ap_log_error(APLOG_MARK, APLOG_DEBUG, 0, rc->r->server,
                         "mod_webauth: %s: no cookies after strip", mwa_func);
    } else {
        /* we modified the Cookie header in place */
        if (rc->sconf->debug)
            ap_log_error(APLOG_MARK, APLOG_DEBUG, 0, rc->r->server,
                         "mod_webauth: %s: after strip: %s", mwa_func, c);
    }
}


/*
 * find a cookie in the Cookie header and return its value, otherwise
 * return NULL.
 */
static char *
find_cookie(MWA_REQ_CTXT *rc, const char *name)
{
    const char *c;
    char *cs, *ce, *cval;
    int len;

    c = apr_table_get(rc->r->headers_in, "Cookie");
    if (c == NULL) 
        return NULL;

    len = strlen(name);

    while ((cs = ap_strstr(c, name))) {
        if (cs[len] == '=') {
            cs += len+1;
            break;
        }
        c += len;
    }

    if (cs == NULL)
        return NULL;

    ce = ap_strchr(cs, ';');

    if (ce == NULL) {
        cval = apr_pstrdup(rc->r->pool, cs);
    } else {
        cval = apr_pstrmemdup(rc->r->pool, cs, ce-cs);
    }

    return cval;
}


/*
 * nuke a cooke by directly updating r->err_headers_out. If
 * if_set is true, then only nuke the cookie if its set.
 */
static void
nuke_cookie(MWA_REQ_CTXT *rc, const char *name, int if_set)
{
    char *cookie;

    if (if_set && find_cookie(rc, name) == NULL)
        return;

    cookie = apr_psprintf(rc->r->pool,
                          "%s=; path=/; expires=%s;%s",
                          name,
                          "Thu, 26-Mar-1998 00:00:01 GMT",
                          is_https(rc->r) ? "secure" : "");
    if (rc->sconf->debug)
        ap_log_error(APLOG_MARK, APLOG_DEBUG, 0, rc->r->server,
                     "mod_webauth: nuking cookie(%s): (%s)", 
                     name, cookie);
    apr_table_addn(rc->r->err_headers_out, "Set-Cookie", cookie);
}


/*
 * add set cookie header
 */
static int
set_pending_cookie_cb(void *rec, const char *key, const char *value)
{
    MWA_REQ_CTXT *rc = (MWA_REQ_CTXT*) rec;
    if (strncmp(key, "mod_webauth_COOKIE_", 19) == 0) {
        apr_table_addn(rc->r->err_headers_out, "Set-Cookie", value);
        if (rc->sconf->debug)
            ap_log_error(APLOG_MARK, APLOG_DEBUG, 0, rc->r->server,
                         "mod_webauth: set_pending_cookie_cb: %s", value);
    }

    return 1;
}


/* see if we have to update our cookies and save them in err_headers_out
 * so the always gets sent.
 */
static void
set_pending_cookies(MWA_REQ_CTXT *rc) 
{
    apr_table_t *t;
    if (rc->r->main != NULL)
        t = rc->r->main->notes;
    else 
        t = rc->r->notes;
    apr_table_do(set_pending_cookie_cb, rc, t, NULL);
}


/*
 * stores a cookie that will get set in fixups
 */
static void
fixup_setcookie(MWA_REQ_CTXT *rc, const char *name, const char *value)
{
    mwa_setn_note(rc->r, 
                  "mod_webauth_COOKIE_",
                  name,
                  "%s=%s; path=/;%s",
                  name,
                  value,
                  is_https(rc->r) ? "secure" : "");
}


/*
 * set environment variables in the subprocess_env table.
 * also handles WebAuthVarPrefix
 */
static void
mwa_setenv(MWA_REQ_CTXT *rc, const char *name, const char *value)
{
    if (rc->sconf->debug)
        ap_log_error(APLOG_MARK, APLOG_INFO, 0, rc->r->server,
                     "mod_webauth: mwa_setenv: (%s) (%s)",
                     name, value);
    apr_table_setn(rc->r->subprocess_env, name, value);
    if (rc->dconf->var_prefix != NULL) {
        name = apr_pstrcat(rc->r->pool, 
                          rc->dconf->var_prefix, name, NULL);
        apr_table_setn(rc->r->subprocess_env, name, value);
    }
}


/*
 * enumerate through all webauth_ cookies and nuke them.
 */
static void
nuke_all_webauth_cookies(MWA_REQ_CTXT *rc)
{
    int i;
    char **p;
    apr_array_header_t *cookies;

    cookies = mwa_get_webauth_cookies(rc->r);

    if (cookies == NULL)
        return;

    p = (char**)cookies->elts;

    for (i = 0; i < cookies->nelts; i++) {
        char *val;

        val = ap_strchr(p[i], '=');
        if (val) {
            *val++ = '\0';
            /* don't nuke any webkdc cookies, which noramlly wouldn't
               show up, but due during development */
            if (strncmp(p[i], "webauth_wpt", 11) != 0) {
                nuke_cookie(rc, p[i], 1);
            }
        }
    }
}


/* FIXME: should we pass some query paramters along with
 *        failure_redirect to indicate what failure occured? 
 *        
 */
static int
failure_redirect(MWA_REQ_CTXT *rc)
{
    char *redirect_url, *uri;
    const char *mwa_func="failure_redirect";

    ap_discard_request_body(rc->r);

    uri = rc->dconf->failure_url;

    if (uri == NULL) {
        ap_log_error(APLOG_MARK, APLOG_WARNING, 0, rc->r->server,
                     "mod_webauth: %s: no URL configured", mwa_func);
        set_pending_cookies(rc);
        return HTTP_INTERNAL_SERVER_ERROR;
    }

    if (uri[0] != '/') {
        redirect_url = uri;
    } else {
        redirect_url = ap_construct_url(rc->r->pool, uri, rc->r);
    }

    apr_table_setn(rc->r->err_headers_out, "Location", redirect_url);
                
    if (rc->sconf->debug)
        ap_log_error(APLOG_MARK, APLOG_DEBUG, 0, rc->r->server,
                     "mod_webauth: %s: redirect(%s)", mwa_func, redirect_url);

    set_pending_cookies(rc);
    return do_redirect(rc);
}


static int
login_canceled_redirect(MWA_REQ_CTXT *rc)
{
    char *redirect_url, *uri;
    const char *mwa_func = "login_canceled_redirect";
    ap_discard_request_body(rc->r);

    uri = rc->dconf->login_canceled_url;

    if (uri == NULL) {
        ap_log_error(APLOG_MARK, APLOG_WARNING, 0, rc->r->server,
                   "mod_webauth: %s: no URL configured!", mwa_func);
        return failure_redirect(rc);
    }

    if (uri[0] != '/') {
        redirect_url = uri;
    } else {
        redirect_url = ap_construct_url(rc->r->pool, uri, rc->r);
    }

    apr_table_setn(rc->r->err_headers_out, "Location", redirect_url);
                
    if (rc->sconf->debug)
        ap_log_error(APLOG_MARK, APLOG_DEBUG, 0, rc->r->server,
                     "mod_webauth: %s: redirect(%s)", mwa_func, redirect_url);

    set_pending_cookies(rc);
    return do_redirect(rc);
}


static int 
die(const char *message, server_rec *s)
{
    if (s) {
        ap_log_error(APLOG_MARK, APLOG_ERR, 0, s,
                     "mod_webauth: fatal error: %s", message);
    }
    printf("mod_webauth: fatal error: %s", message);
    exit(1);
}


static void 
die_directive(server_rec *s, const char *dir, apr_pool_t *ptemp)
{
    char *msg;

    if (s->is_virtual) {
        msg = apr_psprintf(ptemp, 
                          "directive %s must be set for virtual host %s:%d", 
                          dir, s->defn_name, s->defn_line_number);
    } else {
        msg = apr_psprintf(ptemp, 
                          "directive %s must be set in main config", 
                          dir);
    }
    die(msg, s);
}


/*
 * called on restarts
 */
static apr_status_t
mod_webauth_cleanup(void *data)
{
    server_rec *s = (server_rec*) data;
    server_rec *t;
    MWA_SCONF *sconf = (MWA_SCONF*)ap_get_module_config(s->module_config,
                                                        &webauth_module);

    if (sconf->debug) {
        ap_log_error(APLOG_MARK, APLOG_DEBUG, 0, s, "mod_webauth: cleanup");
    }

    /* walk through list of servers and clean up */
    for (t=s; t; t=t->next) {
        MWA_SCONF *tconf = (MWA_SCONF*)ap_get_module_config(t->module_config,
                                                            &webauth_module);

        if (tconf->ring && tconf->free_ring) {
            if (sconf->debug) {
                ap_log_error(APLOG_MARK, APLOG_DEBUG, 0, s, 
                             "mod_webauth: cleanup ring: %s",
                             tconf->keyring_path);
            }
            webauth_keyring_free(tconf->ring);
            tconf->ring = NULL;
            tconf->free_ring = 0;
        }

        /* service_token is currently never set in the parent,
         * add it here in case we change caching strategy.
         */
        if (tconf->service_token) {
            apr_pool_destroy(tconf->service_token->pool);
            tconf->service_token = NULL;
            if (sconf->debug) {
                ap_log_error(APLOG_MARK, APLOG_DEBUG, 0, s, 
                             "mod_webauth: cleanup service_token: %s",
                             tconf->st_cache_path);
            }
        }
    }
    return APR_SUCCESS;
}


/*
 * check server conf directives for server,
 * also cache keyring
 */
static void
init_sconf(server_rec *s, MWA_SCONF *bconf, 
           apr_pool_t *pconf, apr_pool_t *ptemp)
{
    MWA_SCONF *sconf;

    sconf = (MWA_SCONF*)ap_get_module_config(s->module_config,
                                                 &webauth_module);

#define CHECK_DIR(field,dir) \
            if (sconf->field == NULL) die_directive(s, dir, ptemp);

    CHECK_DIR(login_url, CD_LoginURL);
    CHECK_DIR(keyring_path, CD_Keyring);
    CHECK_DIR(webkdc_url, CD_WebKdcURL);
    CHECK_DIR(keytab_path, CD_Keytab);
    /*CHECK_DIR(cred_cache_dir, CD_CredCacheDir);*/
    CHECK_DIR(webkdc_principal, CD_WebKdcPrincipal);
    CHECK_DIR(st_cache_path, CD_ServiceTokenCache);

#undef CHECK_DIR

    /* init mutex first */
    if (sconf->mutex == NULL) {
        apr_thread_mutex_create(&sconf->mutex,
                                APR_THREAD_MUTEX_DEFAULT,
                                pconf);
    }

    /* load up the keyring */
    if (sconf->ring == NULL) {
        if ((bconf->ring != NULL) &&
            (strcmp(sconf->keyring_path, bconf->keyring_path) == 0)) {
            sconf->ring = bconf->ring;
            sconf->free_ring = 0;
        } else {
            mwa_cache_keyring(s, sconf);
            if (sconf->ring)
                sconf->free_ring = 1;
        }
    }

    /* unlink any existing service-token cache */
    /* FIXME: should this be a directive? */
    if (unlink(sconf->st_cache_path) == -1) {
        if (errno != ENOENT) {
            ap_log_error(APLOG_MARK, APLOG_ERR, 0, NULL,
                         "mod_webauth: init_sconf: unlink(%s) errno(%d)", 
                         sconf->st_cache_path, errno);
        }
    }

#if 0
    /* if'd out, since we are now unlinking the service token cache */
    /* load service token cache. must be done after we init keyring  */
    (void)mwa_get_service_token(s, sconf, ptemp, 1);
#endif 

}


/*
 * called after config has been loaded in parent process
 */
static int
mod_webauth_init(apr_pool_t *pconf, apr_pool_t *plog UNUSED,
                 apr_pool_t *ptemp, server_rec *s)
{
    MWA_SCONF *sconf;
    server_rec *scheck;
    char *version;

    sconf = (MWA_SCONF*)ap_get_module_config(s->module_config,
                                                 &webauth_module);

    /* FIXME: this needs to be configurable at some point */
    mwa_register_cred_interface(s, sconf, pconf, mwa_krb5_cred_interface);

    if (sconf->debug)
        ap_log_error(APLOG_MARK, APLOG_DEBUG, 0, s, "mod_webauth: initializing");

    apr_pool_cleanup_register(pconf, s, 
                              mod_webauth_cleanup,
                              apr_pool_cleanup_null);

    for (scheck=s; scheck; scheck=scheck->next) {
        init_sconf(scheck, sconf, pconf, ptemp);
    }

    version = apr_pstrcat(ptemp, "WebAuth/", webauth_info_version(), NULL);
    ap_add_version_component(pconf, version);

    ap_log_error(APLOG_MARK, APLOG_NOTICE, 0, s, 
                 "mod_webauth: initialized (%s)%s",
                 webauth_info_version(),
                 sconf->debug ?
                 apr_pstrcat(ptemp," (", webauth_info_build(), ")", NULL) : 
                 "");
 
    return OK;
}


/*
 * called once per-child
 */
static void
mod_webauth_child_init(apr_pool_t *p UNUSED, server_rec *s UNUSED)
{
    /* nothing for now */
}


/*
**
**  per-server configuration structure handling
**
*/

static void *
config_server_create(apr_pool_t *p, server_rec *s UNUSED)
{
    MWA_SCONF *sconf;

    sconf = apr_pcalloc(p, sizeof(MWA_SCONF));

    /* init defaults */
    sconf->token_max_ttl = DF_TokenMaxTTL;
    sconf->subject_auth_type = DF_SubjectAuthType;
    sconf->strip_url = DF_StripURL;
    sconf->require_ssl = DF_RequireSSL;
    sconf->keyring_auto_update = DF_KeyringAutoUpdate;
    sconf->keyring_key_lifetime = DF_KeyringKeyLifetime;
    sconf->webkdc_cert_check = DF_WebKdcSSLCertCheck;
    sconf->extra_redirect = DF_ExtraRedirect;
    return (void *)sconf;
}


static void *
config_dir_create(apr_pool_t *p, char *path UNUSED)
{
    MWA_DCONF *dconf;

    dconf = apr_pcalloc(p, sizeof(MWA_DCONF));

    /* init defaults */
    dconf->extra_redirect = DF_ExtraRedirect;

    return (void *)dconf;
}


#define MERGE_PTR(field) \
    conf->field = (oconf->field != NULL) ? oconf->field : bconf->field

#define MERGE_INT(field) \
    conf->field = oconf->field ? oconf->field : bconf->field


static void *
config_server_merge(apr_pool_t *p, void *basev, void *overv)
{
    MWA_SCONF *conf, *bconf, *oconf;

    conf = (MWA_SCONF*) apr_pcalloc(p, sizeof(MWA_SCONF));
    bconf = (MWA_SCONF*) basev;
    oconf = (MWA_SCONF*) overv;

    conf->token_max_ttl = oconf->token_max_ttl_ex ?
        oconf->token_max_ttl : bconf->token_max_ttl;

    conf->subject_auth_type = oconf->subject_auth_type_ex ?
        oconf->subject_auth_type : bconf->subject_auth_type;

    conf->strip_url = oconf->strip_url_ex ?
        oconf->strip_url : bconf->strip_url;

    conf->debug = oconf->debug_ex ? oconf->debug : bconf->debug;

    conf->require_ssl = oconf->require_ssl_ex ? 
        oconf->require_ssl : bconf->require_ssl;

    conf->ssl_redirect = oconf->ssl_redirect_ex ? 
        oconf->ssl_redirect : bconf->ssl_redirect;

    conf->extra_redirect = oconf->extra_redirect_ex ?
        oconf->extra_redirect : bconf->extra_redirect;
    conf->extra_redirect_ex = oconf->extra_redirect_ex || 
        bconf->extra_redirect_ex;

    conf->webkdc_cert_check = oconf->webkdc_cert_check_ex ? 
        oconf->webkdc_cert_check : bconf->webkdc_cert_check;

    conf->keyring_auto_update = oconf->keyring_auto_update_ex ? 
        oconf->keyring_auto_update : bconf->keyring_auto_update;

    conf->keyring_key_lifetime = oconf->keyring_key_lifetime_ex ? 
        oconf->keyring_key_lifetime : bconf->keyring_key_lifetime;

    conf->ssl_redirect_port = oconf->ssl_redirect_port_ex ? 
        oconf->ssl_redirect_port : bconf->ssl_redirect_port;

    MERGE_PTR(webkdc_url);
    MERGE_PTR(webkdc_principal);
    MERGE_PTR(webkdc_cert_file);
    MERGE_PTR(login_url);
    MERGE_PTR(auth_type);
    MERGE_PTR(keyring_path);
    MERGE_PTR(keytab_path);
    /* always use oconf's keytab_principal if 
       oconf's keytab_path is specified */
    if (oconf->keytab_path)
        conf->keytab_principal = oconf->keytab_principal;
    else
        conf->keytab_principal = bconf->keytab_principal;
    MERGE_PTR(cred_cache_dir);
    MERGE_PTR(st_cache_path);
    return (void *)conf;
}


static void *
config_dir_merge(apr_pool_t *p, void *basev, void *overv)
{
    MWA_DCONF *conf, *bconf, *oconf;

    conf = (MWA_DCONF*) apr_pcalloc(p, sizeof(MWA_DCONF));
    bconf = (MWA_DCONF*) basev;
    oconf = (MWA_DCONF*) overv;

    conf->force_login = oconf->force_login_ex ? 
        oconf->force_login : bconf->force_login;
    conf->force_login_ex = oconf->force_login_ex || bconf->force_login_ex;

    conf->use_creds = oconf->use_creds_ex ? 
        oconf->use_creds : bconf->use_creds;
    conf->use_creds_ex = oconf->use_creds_ex || bconf->use_creds_ex;

    conf->do_logout = oconf->do_logout_ex ? 
        oconf->do_logout : bconf->do_logout;
    conf->do_logout_ex = oconf->do_logout_ex || bconf->do_logout_ex;

    conf->extra_redirect = oconf->extra_redirect_ex ?
        oconf->extra_redirect : bconf->extra_redirect;
    conf->extra_redirect_ex = oconf->extra_redirect_ex || 
        bconf->extra_redirect_ex;

    conf->ssl_return = oconf->ssl_return_ex ?
        oconf->ssl_return : bconf->ssl_return;
    conf->ssl_return_ex = oconf->ssl_return_ex || 
        bconf->ssl_return_ex;

    conf->dont_cache = oconf->dont_cache_ex ? 
        oconf->dont_cache : bconf->dont_cache;
    conf->dont_cache_ex = oconf->dont_cache_ex || bconf->dont_cache_ex;

    MERGE_INT(app_token_lifetime);
    MERGE_INT(inactive_expire);
    MERGE_INT(last_use_update_interval);
    MERGE_PTR(return_url);
    MERGE_PTR(post_return_url);
    MERGE_PTR(login_canceled_url);
    MERGE_PTR(failure_url);
    MERGE_PTR(var_prefix);
#ifndef NO_STANFORD_SUPPORT
    MERGE_PTR(su_authgroups);
#endif
    if (bconf->creds == NULL) {
        conf->creds = oconf->creds;
    } else if (oconf->creds == NULL) {
        conf->creds = bconf->creds;
    } else {
        /* FIXME: should probably remove dups */
        conf->creds = apr_array_append(p, bconf->creds, oconf->creds);
    }

    return (void *)conf;
}

#undef MERGE_PTR
#undef MERGE_INT


static const char *
status_check_access(const char *path, apr_int32_t flag, request_rec *r)
{
    apr_status_t st;
    apr_file_t *f;
    char errbuff[512];

    st = apr_file_open(&f, path, flag, APR_UREAD|APR_UWRITE, r->pool);
    if (st != APR_SUCCESS) {
        errbuff[0] = 0;
        apr_strerror(st, errbuff, sizeof(errbuff)-1);
        return apr_pstrdup(r->pool, errbuff);
    } else {
        apr_file_close(f);
        return "ok";
    }
}


static void
dt_str(const char *n, const char *v, request_rec *r) 
{
    ap_rprintf(r, "<dt><strong>%s:</strong> "
               /*"<tt>%s</tt></dt>\n", n, v);*/
               "<font size=\"+1\"><tt>%s</tt></font></dt>\n", n, v);
}


static void
dd_dir_str(const char *n, const char *v, request_rec *r) 
{
    if (v == NULL) {
        return;
        /*v = "(value not set)";*/
    }

    ap_rprintf(r, "<dd><tt>%s %s</tt></dd>",
               ap_escape_html(r->pool, n),
               ap_escape_html(r->pool, v));
}


static void
dd_dir_int(const char *n, int v, request_rec *r) 
{
    ap_rprintf(r, "<dd><tt>%s %d</tt></dd>",
               ap_escape_html(r->pool, n),
               v);
}


static void
dd_dir_time(const char *n, time_t t, request_rec *r) 
{
    char buffer[APR_CTIME_LEN+1];

    apr_ctime(buffer, (apr_time_from_sec(t)));
    dd_dir_str(n, buffer, r);
}


/* The content handler */
static int 
handler_hook(request_rec *r)
{
    MWA_DCONF *dconf;
    MWA_SCONF *sconf;
    MWA_SERVICE_TOKEN *st;

    if (strcmp(r->handler, "webauth")) {
        return DECLINED;
    }

    r->allowed |= (AP_METHOD_BIT << M_GET);
    if (r->method_number != M_GET)
	return DECLINED;

    dconf = (MWA_DCONF*)ap_get_module_config(r->per_dir_config,
                                                &webauth_module);

    sconf = (MWA_SCONF*)ap_get_module_config(r->server->module_config,
                                                &webauth_module);

    r->content_type = "text/html";      

    if (!sconf->debug) {
        ap_rputs(DOCTYPE_HTML_3_2
                 "<html><head><title>mod_webauth status</title></head>\n", r);
        ap_rputs("<body><h1 align=\"center\">mod_webauth status</h1>\n", r);
        ap_rputs("<b>You must have \"WebAuthDebug on\" in your config file "
                 "to enable this information.</b>", r);
        ap_rputs("</body></html>\n", r);
        return OK;
    }

    ap_rputs(DOCTYPE_HTML_3_2
	     "<html><head><title>mod_webauth status</title></head>\n", r);
    ap_rputs("<body><h1 align=\"center\">mod_webauth status</h1>\n", r);
    ap_rputs("<hr/>", r);

    ap_rputs("<dl>", r);
    dt_str("Server Version", ap_get_server_version(), r);
    dt_str("Server Built",   ap_get_server_built(), r);
    dt_str("Hostname/port", 
           apr_psprintf(r->pool, "%s:%u",
                        ap_get_server_name(r), ap_get_server_port(r)), r);
    ap_rputs("</dl>", r);
    ap_rputs("<hr/>", r);

    ap_rputs("<dl>", r);
    
    dt_str("WebAuth Info Version", webauth_info_version(), r);
    dt_str("WebAuth Info Build", webauth_info_build(), r);

    ap_rputs("<dt><strong>Current Configuration (server directives only):</strong></dt>\n", r);


    dd_dir_str("WebAuthAuthType", sconf->auth_type, r);
    dd_dir_str("WebAuthCredCacheDir", sconf->cred_cache_dir, r);
    dd_dir_str("WebAuthDebug", sconf->debug ? "on" : "off", r);
    dd_dir_str("WebAuthKeyRing", sconf->keyring_path, r);
    dd_dir_str("WebAuthKeyRingAutoUpdate", sconf->keyring_auto_update ? "on" : "off", r);
    dd_dir_str("WebAuthKeyRingKeyLifetime", 
               apr_psprintf(r->pool, "%ds", sconf->keyring_key_lifetime), r);
    if (sconf->keytab_principal == NULL) {
        dd_dir_str("WebAuthKeytab", sconf->keytab_path, r);
    } else {
        dd_dir_str("WebAuthKeytab",
                   apr_psprintf(r->pool, "%s %s", 
                                sconf->keytab_path,
                                sconf->keytab_principal), r);
    }
    dd_dir_str("WebAuthLoginUrl", sconf->login_url, r);
    dd_dir_str("WebAuthServiceTokenCache", sconf->st_cache_path, r);
    dd_dir_str("WebAuthSubjectAuthType", sconf->subject_auth_type, r);
    dd_dir_str("WebAuthSSLRedirect", sconf->ssl_redirect ? "on" : "off", r);
    if (sconf->ssl_redirect_port != 0) {
        dd_dir_str("WebAuthSSLRedirectPort", 
                   apr_psprintf(r->pool, "%d", sconf->ssl_redirect_port), r);
    }
    dd_dir_str("WebAuthTokenMaxTTL", 
               apr_psprintf(r->pool, "%ds", sconf->token_max_ttl), r);
    dd_dir_str("WebAuthWebKdcPrincipal", sconf->webkdc_principal, r);
    dd_dir_str("WebAuthWebKdcSSLCertFile", sconf->webkdc_cert_file, r);
    dd_dir_str("WebAuthWebKdcSSLCertCheck", 
               sconf->webkdc_cert_check ? "on" : "off", r);
    dd_dir_str("WebAuthWebKdcURL", sconf->webkdc_url, r);

    ap_rputs("</dl>", r);
    ap_rputs("<hr/>", r);

    ap_rputs("<dl>", r);
    dt_str("Keyring read check", 
           status_check_access(sconf->keyring_path, APR_READ, r), r);
    ap_rputs("<dt><strong>Keyring info:</strong></dt>\n", r);

    if (sconf->ring == NULL) {
        ap_rputs("<dd>"
                 "keyring is NULL. This usually indicates a permissions "
                 "problem with the keyring file."
                 "</dd>", r);
    } else {
        unsigned long i;

        dd_dir_int("num_entries", sconf->ring->num_entries, r);
        for (i = 0; i < sconf->ring->num_entries; i++) {
            dd_dir_time(apr_psprintf(r->pool, "entry %lu creation time", i),
                        sconf->ring->entries[i].creation_time, r);
            dd_dir_time(apr_psprintf(r->pool, "entry %lu valid after", i),
                        sconf->ring->entries[i].valid_after, r);
        }
    }

    ap_rputs("</dl>", r);
    ap_rputs("<hr/>", r);

    ap_rputs("<dl>", r);

    dt_str("Keytab read check", 
           status_check_access(sconf->keytab_path, APR_READ, r), r);
    ap_rputs("</dl>", r);
    ap_rputs("<hr/>", r);

    ap_rputs("<dl>", r);

    st = mwa_get_service_token(r->server, sconf, r->pool, 0);

    dt_str("Service Token Cache read/write check", 
           status_check_access(sconf->st_cache_path, 
                               APR_READ|APR_WRITE|APR_CREATE, r), r);
    ap_rputs("<dt><strong>Service Token info:</strong></dt>\n", r);
    
    if (st == NULL) {
        ap_rputs("<dd>"
                 "service_token is NULL. This usually indicates a permissions "
                 "problem with the service token cache and/or keytab file ."
                 "</dd>", r);
    } else {
        dd_dir_time("created", st->created, r);
        dd_dir_time("expires", st->expires, r);
        dd_dir_time("next_renewal_attempt", st->next_renewal_attempt, r);
        if (st->last_renewal_attempt != 0)
            dd_dir_time("last_renewal_attempt", st->last_renewal_attempt, r);
    }

    ap_rputs("</dl>", r);
    ap_rputs("<hr/>", r);
    ap_rputs(ap_psignature("",r), r);
    ap_rputs("</body></html>\n", r);
    return OK;
}


/*
 * return the name of the app cookie
 */
static const char *
app_cookie_name(void)
{
    return "webauth_at";
}


/*
 * given the proxy_type return the cookie name to use 
 */
static char *
proxy_cookie_name(const char *proxy_type, MWA_REQ_CTXT *rc) 
{
    return apr_pstrcat(rc->r->pool, "webauth_pt_", proxy_type, NULL);
}


/*
 * given the proxy_type return the cookie name to use 
 */
static char *
cred_cookie_name(const char *cred_type, 
                 const char *cred_server,
                 MWA_REQ_CTXT *rc) 
{
    char *p;
    /* if cred_server has an '=' in it we need to change it to '-'
       instead, since cookie names can't have an '=' in it. The
       risk of a potential collision with another valid cred name
       that has a '-' in it already is small enough not to worry.
       It might turn out we need to do more extensive encoding/decoding
       later anyways... */
    if ((p=ap_strchr(cred_server, '='))) {
        cred_server = apr_pstrdup(rc->r->pool, cred_server);
        while(p) {
            *p = '-';
            p=ap_strchr(cred_server, '=');
        }
    }
    return apr_pstrcat(rc->r->pool, "webauth_ct_", cred_type, "_",
                       cred_server, NULL);
}


/*
 * create a proxy-token cookie.
 */
static int
make_proxy_cookie(const char *proxy_type,
                  const char *subject, 
                  void *wpt,
                  size_t wpt_len,
                  time_t expiration_time, 
                  MWA_REQ_CTXT *rc)
{
    WEBAUTH_ATTR_LIST *alist;
    char *token, *btoken;
    size_t tlen, olen;
    int status;
    const char *mwa_func="make_proxy_cookie";
    time_t creation_time;

    status = WA_ERR_NONE;

    alist = webauth_attr_list_new(10);
    if (alist == NULL) {
        ap_log_error(APLOG_MARK, APLOG_EMERG, 0, rc->r->server,
                     "mod_webauth: %s: webauth_attr_list_new failed", 
                     mwa_func);
        return 0;
    }

    creation_time = time(NULL);

    SET_TOKEN_TYPE(WA_TT_PROXY);
    SET_PROXY_TYPE(proxy_type);
    SET_SUBJECT(subject);
    SET_WEBKDC_TOKEN(wpt, wpt_len);
    SET_EXPIRATION_TIME(expiration_time);
    SET_CREATION_TIME(creation_time);
    
    tlen = webauth_token_encoded_length(alist);
    token = (char*)apr_palloc(rc->r->pool, tlen);

    if (rc->sconf->ring == NULL)
        return 0;

    status = webauth_token_create(alist, 0, token, &olen, tlen,
                                  rc->sconf->ring);
    webauth_attr_list_free(alist);

    if (status != WA_ERR_NONE) {
        mwa_log_webauth_error(rc->r->server, status, mwa_func,
                              "webauth_token_create", subject);
        return 0;
    }

    btoken = (char*) apr_palloc(rc->r->pool, apr_base64_encode_len(olen));
    apr_base64_encode(btoken, token, olen);

    fixup_setcookie(rc, proxy_cookie_name(proxy_type, rc), btoken);
    
    rc->pt = 
        (MWA_PROXY_TOKEN*)apr_pcalloc(rc->r->pool, sizeof(MWA_PROXY_TOKEN));
    rc->pt->proxy_type = apr_pstrdup(rc->r->pool, proxy_type);
    rc->pt->subject = apr_pstrdup(rc->r->pool, subject);
    rc->pt->creation_time = creation_time;
    rc->pt->expiration_time = expiration_time;
    rc->pt->wpt = apr_pstrmemdup(rc->r->pool, wpt, wpt_len);
    rc->pt->wpt_len = wpt_len;
    return 1;
}


/*
 * create a cred-token cookie.
 */
static int
make_cred_cookie(MWA_CRED_TOKEN *ct,
                  MWA_REQ_CTXT *rc)
{
    WEBAUTH_ATTR_LIST *alist;
    char *token, *btoken;
    size_t tlen, olen;
    int status;
    const char *mwa_func="make_cred_cookie";
    time_t creation_time;

    status = WA_ERR_NONE;

    alist = webauth_attr_list_new(10);
    if (alist == NULL) {
        ap_log_error(APLOG_MARK, APLOG_EMERG, 0, rc->r->server,
                     "mod_webauth: %s: webauth_attr_list_new failed", 
                     mwa_func);
        return 0;
    }

    creation_time = time(NULL);

    SET_TOKEN_TYPE(WA_TT_CRED);
    SET_CRED_TYPE(ct->cred_type);
    SET_CRED_SERVER(ct->cred_server);
    SET_SUBJECT(ct->subject);
    SET_CRED_DATA(ct->cred_data, ct->cred_data_len);
    SET_EXPIRATION_TIME(ct->expiration_time);
    SET_CREATION_TIME(ct->creation_time);
    
    tlen = webauth_token_encoded_length(alist);
    token = (char*)apr_palloc(rc->r->pool, tlen);

    if (rc->sconf->ring == NULL)
        return 0;

    status = webauth_token_create(alist, 0, token, &olen, tlen,
                                  rc->sconf->ring);
    webauth_attr_list_free(alist);

    if (status != WA_ERR_NONE) {
        mwa_log_webauth_error(rc->r->server, status, mwa_func,
                              "webauth_token_create", ct->subject);
        return 0;
    }

    btoken = (char*) apr_palloc(rc->r->pool, apr_base64_encode_len(olen));
    apr_base64_encode(btoken, token, olen);

    fixup_setcookie(rc,
                    cred_cookie_name(ct->cred_type, ct->cred_server, rc),
                    btoken);
    return 1;
}


/*
 * create/update an app-token cookie. If creation_time is 0 it means
 * we are creating an app-token, otherwise we are updating an
 * existing one.
 */
static int
make_app_cookie(const char *subject, 
                time_t creation_time,
                time_t expiration_time, 
                time_t last_used_time,
                MWA_REQ_CTXT *rc)
{
    WEBAUTH_ATTR_LIST *alist;
    char *token, *btoken;
    size_t tlen, olen;
    int status;
    const char *mwa_func = "make_app_cookie";

    status = WA_ERR_NONE;

    alist = webauth_attr_list_new(10);
    if (alist == NULL) {
        ap_log_error(APLOG_MARK, APLOG_EMERG, 0, rc->r->server,
                     "mod_webauth: %s: webauth_attr_list_new failed",
                     mwa_func);
        return 0;
    }

    if (creation_time == 0) {
        creation_time = time(NULL);
        if (rc->dconf->app_token_lifetime) {
            expiration_time = creation_time + rc->dconf->app_token_lifetime;
        }
        last_used_time = 
            rc->dconf->last_use_update_interval ? creation_time : 0;
    }

    SET_TOKEN_TYPE(WA_TT_APP);
    SET_SUBJECT(subject);
    SET_EXPIRATION_TIME(expiration_time);
    SET_CREATION_TIME(creation_time);
    
    if (last_used_time) {
        SET_LASTUSED_TIME(last_used_time);
    }

    tlen = webauth_token_encoded_length(alist);
    token = apr_palloc(rc->r->pool, tlen);

    if (rc->sconf->ring == NULL)
        return 0;

    status = webauth_token_create(alist, 0, token, 
                                  &olen, tlen, rc->sconf->ring);
    webauth_attr_list_free(alist);

    if (status != WA_ERR_NONE) {
        mwa_log_webauth_error(rc->r->server, status, mwa_func,
                              "webauth_token_create", subject);
        return 0;
    }

    btoken = apr_palloc(rc->r->pool, apr_base64_encode_len(olen));
    apr_base64_encode(btoken, token, olen);

    fixup_setcookie(rc, app_cookie_name(), btoken);

    rc->at.subject = apr_pstrdup(rc->r->pool, subject);
    rc->at.last_used_time = last_used_time;
    rc->at.creation_time = creation_time;
    rc->at.expiration_time = expiration_time;
    return 1;
}


/*
 * checks last-use-time in token, returns 0 if expired, 1 if ok.
 * potentially updates app-token and cookie
 */
static int
app_token_maint(MWA_REQ_CTXT *rc)
{
    time_t curr;

    if (!rc->dconf->inactive_expire && 
        !rc->dconf->last_use_update_interval) {
        return 1;
    }

    if (rc->at.last_used_time == 0) {
        /* doesn't have last-used-time, bail */
        return 0;
    }

    curr = time(NULL);

    /* see if its inactive */
    if (rc->dconf->inactive_expire && 
        (rc->at.last_used_time + rc->dconf->inactive_expire < curr)) {
        if (rc->sconf->debug)
            ap_log_error(APLOG_MARK, APLOG_DEBUG, 0, rc->r->server,
                         "mod_webauth: manage_inactivity: inactive(%s)",
                         rc->at.subject);
        return 0;
    }

    /* see if last_used_time is older then the interval we update
       tokens */
    if ((rc->dconf->last_use_update_interval == 0) ||
        (rc->at.last_used_time + rc->dconf->last_use_update_interval > curr)) {
        /*
         *ap_log_error(APLOG_MARK, APLOG_ERR, 0, rc->r->server,
         * "mod_webauth: manage_inactivity: not updating yet(%s)",
         * rc->at.subject);
        */
        return 1;
    }

    /* update last-used-time, write new token */
    rc->at.last_used_time = curr;

    /* FIXME: hum, if this fails, should we expire cookie? */
    make_app_cookie(rc->at.subject,
                   rc->at.creation_time, 
                   rc->at.expiration_time, 
                   rc->at.last_used_time, rc);
    return 1;
}


/*
 * parse an app-token, store in rc->at. 
 * return 0 on failure, 1 on success
 */
static int
parse_app_token(char *token, MWA_REQ_CTXT *rc)
{
    WEBAUTH_ATTR_LIST *alist;
    size_t blen;
    int status;
    const char *tt;
    char *sub;
    int result = 0;
    const char *mwa_func = "parse_app_token";

    ap_unescape_url(token);
    blen = apr_base64_decode(token, token);
    status = WA_ERR_NONE;

    /* parse the token, TTL is zero because app-tokens don't have ttl,
     * just expiration
     */

    if (rc->sconf->ring == NULL)
        return 0;

    status = webauth_token_parse(token, blen, 0, rc->sconf->ring, &alist);

    if (status != WA_ERR_NONE) {
        mwa_log_webauth_error(rc->r->server, status, mwa_func,
                              "webauth_token_parse", token);
        return 0;
    }

    /* make sure its an app-token */
    tt = mwa_get_str_attr(alist, WA_TK_TOKEN_TYPE, rc->r,
                          "parse_app_token", NULL);
    if (tt == NULL || strcmp(tt, WA_TT_APP) != 0) {
        ap_log_error(APLOG_MARK, APLOG_ERR, 0, rc->r->server,
                     "mod_webauth: %s: token type(%s) not (%s)",
                     mwa_func, tt ? tt : "(null)", WA_TT_APP);
        goto cleanup;
    }

    /* pull out subject */
    sub = mwa_get_str_attr(alist, WA_TK_SUBJECT, rc->r, mwa_func, NULL);
    
    if (sub == NULL) {
        goto cleanup;
    }

    rc->at.subject = apr_pstrdup(rc->r->pool, sub);

    /* get the times here, as token_maint might update last_used_time */
    webauth_attr_list_get_time(alist, WA_TK_LASTUSED_TIME,
                               &rc->at.last_used_time, WA_F_NONE);

    webauth_attr_list_get_time(alist, WA_TK_CREATION_TIME,
                               &rc->at.creation_time, WA_F_NONE);

    webauth_attr_list_get_time(alist, WA_TK_EXPIRATION_TIME,
                               &rc->at.expiration_time, WA_F_NONE);

    /* update last-use-time, check inactivity */
    result = app_token_maint(rc);
    if (result == 0) {
        /* clear out rc->at, since we couldn't use this app-token */
        memset(&rc->at, 0, sizeof(rc->at));        
    }

 cleanup:
    webauth_attr_list_free(alist);
    return result;
}


/*
 * check cookie for valid app-token. If an epxired one is found,
 * do a Set-Cookie to blank it out.
 */
static int
parse_app_token_cookie(MWA_REQ_CTXT *rc)
{
    char *cval;
    const char *mwa_func = "parse_app_token_cookie";
    const char *cname = app_cookie_name();

    cval = find_cookie(rc, cname);
    if (cval == NULL)
        return 0;

    if (!parse_app_token(cval, rc)) {
        /* we coudn't use the cookie, lets set it up to be nuked */
        fixup_setcookie(rc, cname, "");
        return 0;
    }  else {
        if (rc->sconf->debug)
            ap_log_error(APLOG_MARK, APLOG_DEBUG, 0, rc->r->server,
                         "mod_webauth: %s: found valid %s cookie for (%s)", 
                         mwa_func, cname, rc->at.subject);
        return 1;
    }
}


/*
 * parse a proxy-token from a cookie. 
 * return pointer to it on success, NULL on failure.
 */
static MWA_PROXY_TOKEN *
parse_proxy_token(char *token, MWA_REQ_CTXT *rc)
{
    WEBAUTH_ATTR_LIST *alist;
    size_t blen;
    int status;
    const char *tt;
    MWA_PROXY_TOKEN pt, *npt;
    const char *mwa_func="parse_proxy_token";

    ap_unescape_url(token);
    blen = apr_base64_decode(token, token);
    status = WA_ERR_NONE;
    npt = NULL;

    /* parse the token, TTL is zero because proxy-tokens don't have ttl,
     * just expiration
     */

    if (rc->sconf->ring == NULL)
        return NULL;

    status = webauth_token_parse(token, blen, 0, rc->sconf->ring, &alist);

    if (status != WA_ERR_NONE) {
        mwa_log_webauth_error(rc->r->server, status,
                              mwa_func, "webauth_token_parse", NULL);
        return NULL;
    }

    /* make sure its an proxy-token */
    tt = mwa_get_str_attr(alist, WA_TK_TOKEN_TYPE, rc->r, mwa_func, NULL);
    if (tt == NULL || strcmp(tt, WA_TT_PROXY) != 0) {
        ap_log_error(APLOG_MARK, APLOG_ERR, 0, rc->r->server,
                     "mod_webauth: %s: token type(%s) not (%s)",
                     mwa_func, tt ? tt : "(null)", WA_TT_PROXY);
        goto cleanup;
    }

    /* pull out subject */
    pt.subject = mwa_get_str_attr(alist, WA_TK_SUBJECT, rc->r, mwa_func, NULL);
    
    if (pt.subject == NULL) {
        goto cleanup;
    }

    /* pull out type */
    pt.proxy_type = mwa_get_str_attr(alist, WA_TK_PROXY_TYPE,
                                     rc->r, mwa_func, NULL);
    if (pt.proxy_type == NULL) {
        goto cleanup;
    }

    webauth_attr_list_get_time(alist, WA_TK_CREATION_TIME,
                               &pt.creation_time, WA_F_NONE);

    webauth_attr_list_get_time(alist, WA_TK_EXPIRATION_TIME,
                               &pt.expiration_time, WA_F_NONE);

    status = webauth_attr_list_get(alist, WA_TK_WEBKDC_TOKEN,
                                   &pt.wpt, &pt.wpt_len, WA_F_NONE);

    if (status != WA_ERR_NONE) {
        ap_log_error(APLOG_MARK, APLOG_ERR, 0, rc->r->server,
                     "mod_webauth: %s: "
                     "can't get proxy data from proxy-token",
                     mwa_func);
        goto cleanup;
    }

    npt = (MWA_PROXY_TOKEN*)apr_pcalloc(rc->r->pool, sizeof(MWA_PROXY_TOKEN));

    /* need to strdup/copy stuff into the request pool */
    npt->proxy_type = apr_pstrdup(rc->r->pool, pt.proxy_type);
    npt->subject = apr_pstrdup(rc->r->pool, pt.subject);
    npt->creation_time = pt.creation_time;
    npt->expiration_time = pt.expiration_time;
    npt->wpt = apr_pstrmemdup(rc->r->pool, pt.wpt, pt.wpt_len);
    npt->wpt_len = pt.wpt_len;

 cleanup:
    webauth_attr_list_free(alist);
    return npt;
}


/*
 * check cookie for valid proxy-token. If an epxired one is found,
 * do a Set-Cookie to blank it out.
 */
static MWA_PROXY_TOKEN *
parse_proxy_token_cookie(MWA_REQ_CTXT *rc, char *proxy_type)
{
    char *cval;
    char *cname = proxy_cookie_name(proxy_type, rc);
    MWA_PROXY_TOKEN *pt;
    const char *mwa_func = "parse_proxy_token_cookie";

    cval = find_cookie(rc, cname);
    if (cval == NULL)
        return 0;

    pt =  parse_proxy_token(cval, rc);

    if (pt == NULL) {
        /* we coudn't use the cookie, lets set it up to be nuked */
        fixup_setcookie(rc, cname, "");
    }  else {
        if (rc->sconf->debug)
            ap_log_error(APLOG_MARK, APLOG_DEBUG, 0, rc->r->server,
                         "mod_webauth: %s: found valid %s cookie for (%s)", 
                         mwa_func, cname, rc->at.subject);
    }
    return pt;
}


static WEBAUTH_KEY *
get_session_key(char *token, MWA_REQ_CTXT *rc)
{
    WEBAUTH_ATTR_LIST *alist;
    WEBAUTH_KEY *key;
    const char *tt;
    size_t klen, blen;
    int status;
    ssize_t i;
    const char *mwa_func = "get_session_key";

    ap_unescape_url(token);
    blen = apr_base64_decode(token, token);
    key = NULL;
    status = WA_ERR_NONE;

    /* parse the token, TTL is zero because app-tokens don't have ttl,
     * just expiration
     */

    if (rc->sconf->ring == NULL)
        return NULL;

    status = webauth_token_parse(token, blen, 0, rc->sconf->ring, &alist);

    if (status != WA_ERR_NONE) {
        mwa_log_webauth_error(rc->r->server, status,
                              mwa_func, "webauth_token_parse", NULL);
        return NULL;
    }

    /* make sure its an app-token */
    tt = mwa_get_str_attr(alist, WA_TK_TOKEN_TYPE, rc->r,
                          "check_cookie", NULL);
    if (tt == NULL || strcmp(tt, WA_TT_APP) != 0) {
        ap_log_error(APLOG_MARK, APLOG_ERR, 0, rc->r->server,
                     "mod_webauth: %s: token type(%s) not (%s)",
                     mwa_func, tt ? tt : "(null)", WA_TT_APP);
        goto cleanup;
    }

    /* pull out session key */
    status = webauth_attr_list_find(alist, WA_TK_SESSION_KEY, &i);
    if (i == -1) {
        ap_log_error(APLOG_MARK, APLOG_ERR, 0, rc->r->server, 
                    "mod_webauth: %s: can't find session key in token",
                     mwa_func);
        goto cleanup;
    }

    klen = alist->attrs[i].length;

    if (klen != WA_AES_128 && 
        klen != WA_AES_192 &&
        klen != WA_AES_256) {
        ap_log_error(APLOG_MARK, APLOG_ERR, 0, rc->r->server, 
                     "mod_webauth: get_session_key: invalid key length: %lu",
                     (unsigned long) klen);
        goto cleanup;
    }

    key = apr_palloc(rc->r->pool, sizeof(WEBAUTH_KEY));
    key->type = WA_AES_KEY;
    key->data = apr_palloc(rc->r->pool, klen);
    memcpy(key->data, alist->attrs[i].value, klen);
    key->length = klen;

 cleanup:
    webauth_attr_list_free(alist);
    return key;
}


static int
handle_id_token(WEBAUTH_ATTR_LIST *alist, MWA_REQ_CTXT *rc)
{
    const char *subject;
    int status;
    ssize_t i;
    const char *sa;
    const char *mwa_func="handle_id_token";

    sa = mwa_get_str_attr(alist, WA_TK_SUBJECT_AUTH, rc->r, mwa_func, NULL);
    if (sa == NULL ) {
        /* nothing, we already logged an error */
        return 0;
    }

    status = webauth_attr_list_find(alist, WA_TK_SUBJECT_AUTH_DATA, &i);
    if (i != -1) {
        MWA_CRED_INTERFACE *mci = mwa_find_cred_interface(rc->r->server, sa);

        if (mci == NULL)
            return 0;

        subject = mci->validate_sad(rc,
                                    alist->attrs[i].value,
                                    alist->attrs[i].length);
    } else if (strcmp(sa, WA_SA_WEBKDC) == 0) {
        /* subject is all set */
        const char *tsub = mwa_get_str_attr(alist, WA_TK_SUBJECT,
                                            rc->r, mwa_func, NULL);
        subject = apr_pstrdup(rc->r->pool, tsub);
    } else {
        ap_log_error(APLOG_MARK, APLOG_ERR, 0, rc->r->server,
                     "mod_webauth: %s: unknown subject auth type: %s",
                     mwa_func, sa);
        subject = NULL;
    }
        
    if (subject != NULL) {
        time_t expiration_time;

        /* wheeee! create an app-token! */

        status = webauth_attr_list_get_time(alist, WA_TK_EXPIRATION_TIME,
                                            &expiration_time,
                                            WA_F_NONE);
        if (status != WA_ERR_NONE) {
                ap_log_error(APLOG_MARK, APLOG_ERR, 0, rc->r->server,
                             "mod_webauth: %s: "
                             "can't get expiration time from id token",
                             mwa_func);
                return 0;
        }

        if (rc->sconf->debug)
            ap_log_error(APLOG_MARK, APLOG_DEBUG, 0, rc->r->server,
                         "mod_webauth: %s: got subject(%s) from id token", 
                         mwa_func,subject);

        make_app_cookie(subject, 0, expiration_time, 0, rc);
    } else {
        /* everyone else should have logged something, right? */
    }
    return subject != NULL;
}


static int
handle_proxy_token(WEBAUTH_ATTR_LIST *alist, MWA_REQ_CTXT *rc) 
{
    char *sub, *pt;
    void *wpt;
    int status;
    size_t wpt_len;
    time_t expiration_time;
    const char *mwa_func="handle_proxy_token";

    sub = mwa_get_str_attr(alist, WA_TK_SUBJECT, rc->r, mwa_func, NULL);
    if (sub == NULL ) {
        /* nothing, we already logged an error */
        return 0;
    }

    pt = mwa_get_str_attr(alist, WA_TK_PROXY_TYPE, rc->r, mwa_func, NULL);
    if (pt == NULL ) {
        /* nothing, we already logged an error */
        return 0;
    }

    status = webauth_attr_list_get_time(alist, WA_TK_EXPIRATION_TIME,
                                        &expiration_time,
                                        WA_F_NONE);
    if (status != WA_ERR_NONE) {
        ap_log_error(APLOG_MARK, APLOG_ERR, 0, rc->r->server,
                     "mod_webauth: %s: "
                     "can't get expiration time from proxy token",
                     mwa_func);
        return 0;
    }

    status = webauth_attr_list_get(alist, WA_TK_WEBKDC_TOKEN,
                                   &wpt, &wpt_len, WA_F_NONE);
    if (status != WA_ERR_NONE) {
        ap_log_error(APLOG_MARK, APLOG_ERR, 0, rc->r->server,
                     "mod_webauth: %s: "
                     "can't get proxy data from proxy-token",
                     mwa_func);
        return 0;
    }

    if (rc->sconf->debug)
        ap_log_error(APLOG_MARK, APLOG_DEBUG, 0, rc->r->server,
                     "mod_webauth: %s: got subject(%s) from proxy token", 
                     mwa_func, sub);

    /* FIXME: app-tokens where subject-auth-type is krb5 to need to
       request an id-token from the WebKDC, assuming the proxy-type
       is also krb5 */
    return make_proxy_cookie(pt, sub, wpt, wpt_len, expiration_time,rc) &&
        make_app_cookie(sub, 0, expiration_time, 0, rc);
}


static int
handle_error_token(WEBAUTH_ATTR_LIST *alist, MWA_REQ_CTXT *rc)
{
    int error_code;
    static const char *mwa_func = "handle_error_token";
    const char *log_message;
    const char *ec = mwa_get_str_attr(alist, WA_TK_ERROR_CODE,
                                      rc->r, mwa_func, NULL);
    const char *em = mwa_get_str_attr(alist, WA_TK_ERROR_MESSAGE,
                                      rc->r, mwa_func, NULL);
    if (ec == NULL || em == NULL) {
        /* nothing, we already logged an error */
        return OK;
    }

    error_code = atoi(ec);

    if (rc->sconf->debug) 
        ap_log_error(APLOG_MARK, APLOG_DEBUG, 0, rc->r->server,
                     "mod_webauth: %s: parsed an error token(%d, %s)", 
                     mwa_func, error_code, em);

    switch (error_code) {
        case WA_PEC_LOGIN_CANCELED:
            /* user canceled out of login page */
            return login_canceled_redirect(rc);
            break;
        default:
            /* catch other all other errors that we aren't expecting, which
               is pretty much all of them, since if the server can't
               even parse our request-token, it can't figure out the return
               url.
             */
            log_message = "unhandled error";
            break;
    }

    ap_log_error(APLOG_MARK, APLOG_ALERT, 0, rc->r->server,
                 "mod_webauth: %s: %s: %s (%d)",
                         mwa_func, log_message, em, error_code);
        
    return failure_redirect(rc);

}


/*
 * return OK or an HTTP_* code.
 */
static int 
parse_returned_token(char *token, WEBAUTH_KEY *key, MWA_REQ_CTXT *rc)
{
    WEBAUTH_ATTR_LIST *alist;
    size_t blen;
    int status, code;
    const char *token_type;
    static const char *mwa_func = "parse_returned_token";

    code = OK;

    /* if we successfully parse an id-token, write out new webauth_at cookie */
    ap_unescape_url(token);
    blen = apr_base64_decode(token, token);

    status = webauth_token_parse_with_key(token, blen,
                                          rc->sconf->token_max_ttl, 
                                          key, &alist);

    if (status != WA_ERR_NONE) {
        mwa_log_webauth_error(rc->r->server, status, mwa_func,
                              "webauth_token_parse_with_key", NULL);
        return code;
    }

    /* get the token-type to see what we should do with it */
    token_type = mwa_get_str_attr(alist, WA_TK_TOKEN_TYPE, rc->r, 
                                  mwa_func, NULL);
    if (token_type == NULL) {
        webauth_attr_list_free(alist);
        return code;
    }

    if (strcmp(token_type, WA_TT_ID) == 0) {
        if (!handle_id_token(alist, rc)) {
            /* FIXME: WHAT DO WE DO? failure redirect or ...? 
               doing nothing will cause another redirect for auth... 
             */
        }
    } else if (strcmp(token_type, WA_TT_PROXY) == 0) {
        if (!handle_proxy_token(alist, rc)) {
            /* FIXME: WHAT DO WE DO? failure redirect or ...? 
               doing nothing will cause another redirect for auth... 
             */
        }
    } else if (strcmp(token_type, WA_TT_ERROR) == 0) {

        code = handle_error_token(alist, rc);

    } else {
        ap_log_error(APLOG_MARK, APLOG_ERR, 0, rc->r->server,
                     "mod_webauth: %s: unhandled token type(%s)",
                     mwa_func,
                     token_type);
    }
    webauth_attr_list_free(alist);

    return code;
}


/*
 * check to see if we got passed WEBAUTHR and WEBAUTHS, and if so
 * attempt to parse and fill in rc->subject. If we return OK,
 * caller will check to see if rc->subject got set, otherwise
 * it will return the HTTP_* code to the Apache framework.
 */
static int
check_url(MWA_REQ_CTXT *rc, int *in_url)
{
    char *subject, *wr, *ws;
    WEBAUTH_KEY *key = NULL;

    subject = NULL;

    wr = mwa_remove_note(rc->r, N_WEBAUTHR);
    if (wr == NULL) {
        *in_url = 0;
        return OK;
    } else {
        *in_url = 1;
    }

    if (rc->sconf->debug)
        ap_log_error(APLOG_MARK, APLOG_DEBUG, 0, rc->r->server,
                     "mod_webauth: check_url: found  WEBAUTHR");

    /* see if we have WEBAUTHS, which has the session key to use */
    ws = mwa_remove_note(rc->r, N_WEBAUTHS);

    if (ws != NULL) {
        /* don't have to free key, its allocated from a pool */
        key = get_session_key(ws, rc);
        if (key == NULL)
            return OK;
        return parse_returned_token(wr, key, rc);
    } else {
        MWA_SERVICE_TOKEN *st = mwa_get_service_token(rc->r->server,
                                                      rc->sconf,
                                                      rc->r->pool, 0);
        if (st != NULL)
            return parse_returned_token(wr, &st->key, rc);
    }
    return OK;
}


static char *
make_return_url(MWA_REQ_CTXT *rc, 
                int check_dconf_return_url)
{
    char *uri = rc->r->unparsed_uri;

    /* use explicit return_url if there is one */
    if (check_dconf_return_url) {
        if (rc->r->method_number == M_GET && rc->dconf->return_url) {
            if (rc->dconf->return_url[0] != '/')
                return rc->dconf->return_url;
            else 
                return ap_construct_url(rc->r->pool, 
                                        rc->dconf->return_url, rc->r);
        } else if (rc->r->method_number == M_POST && 
                   rc->dconf->post_return_url) {
            if (rc->dconf->post_return_url[0] != '/')
                return rc->dconf->post_return_url;
            else 
                return ap_construct_url(rc->r->pool, 
                                        rc->dconf->post_return_url, rc->r);
        }
    }

    /* if we are proxying or if the uri is parsed and scheme is non-null
       just use unparsed_uri */
    if ((rc->r->proxyreq == PROXYREQ_PROXY) ||
        (rc->r->parsed_uri.is_initialized && rc->r->parsed_uri.scheme != NULL)
        ) {
        /* do nothing, use uri */
    } else {
        uri = ap_construct_url(rc->r->pool, uri, rc->r);
    }

    if (rc->dconf->ssl_return && strncmp(uri, "http:", 5) == 0) {
        uri = apr_pstrcat(rc->r->pool,
                          "https:",
                          uri+5,
                          NULL);
    }

    return uri;
}


static int
redirect_request_token(MWA_REQ_CTXT *rc)
{
    MWA_SERVICE_TOKEN *st;
    WEBAUTH_ATTR_LIST *alist;
    char *redirect_url, *return_url;
    char *token, *btoken;
    size_t tlen, olen;
    int status;
    time_t curr = time(NULL);
    const char *mwa_func="redirect_request_token";

    if (rc->r->method_number != M_GET &&
        (rc->r->method_number != M_POST ||
         (rc->r->method_number == M_POST &&
          rc->dconf->post_return_url == NULL))) {
        ap_log_error(APLOG_MARK, APLOG_WARNING, 0, rc->r->server,
                     "mod_webauth: redirect_request_token: no auth during %s, "
                     "denying request",  rc->r->method);
        if (rc->r->method_number == M_POST) {
            ap_log_error(APLOG_MARK, APLOG_WARNING, 0, rc->r->server,
                         "mod_webauth: use %s to specify a return URL",
                         CD_PostReturnURL);
        }
        return HTTP_UNAUTHORIZED;
    }

    ap_discard_request_body(rc->r);

    st = mwa_get_service_token(rc->r->server, rc->sconf, rc->r->pool, 0);
    if (st == NULL) {
        ap_log_error(APLOG_MARK, APLOG_EMERG, 0, rc->r->server,
                     "mod_webauth: %s: no service token, denying request",
                     mwa_func);
        return failure_redirect(rc);
    }

    alist = webauth_attr_list_new(10);
    if (alist == NULL) {
        ap_log_error(APLOG_MARK, APLOG_EMERG, 0, rc->r->server,
                     "mod_webauth: %s: webauth_attr_list_new failed",
                     mwa_func);
        return failure_redirect(rc);
    }

    SET_TOKEN_TYPE(WA_TT_REQUEST);
    SET_CREATION_TIME(curr);
    if (rc->dconf->force_login || rc->dconf->login_canceled_url != NULL) {
        int fl = rc->dconf->force_login;
        int lc = rc->dconf->login_canceled_url != NULL;

        SET_REQUEST_OPTIONS(apr_pstrcat(rc->r->pool, 
                                        fl ? "fa" : "",
                                        fl && lc ? "," : "",
                                        lc ? "lc" : "",
                                        NULL));
    }
    if (rc->dconf->creds) {
        SET_REQUESTED_TOKEN_TYPE("proxy");

        if (rc->needed_proxy_type) {
            SET_PROXY_TYPE(rc->needed_proxy_type);
        } else {
            /* if we don't know which one we need, lets request a proxy
               token for the first one in the list. */
            MWA_WACRED *cred = (MWA_WACRED*)rc->dconf->creds->elts;
            SET_PROXY_TYPE(cred->type);
        }

        if (rc->sconf->debug)
            ap_log_error(APLOG_MARK, APLOG_DEBUG, 0, rc->r->server,
                         "mod_webauth: %s: redirecting for proxy token",
                         mwa_func);
    } else {
        SET_REQUESTED_TOKEN_TYPE("id");
        SET_SUBJECT_AUTH(rc->sconf->subject_auth_type);
        if (rc->sconf->debug)
            ap_log_error(APLOG_MARK, APLOG_DEBUG, 0, rc->r->server,
                         "mod_webauth: %s: redirecting for id token",
                         mwa_func);
    }

    if (st->app_state != NULL) {
        SET_APP_STATE(st->app_state, st->app_state_len);
    } else {
        ap_log_error(APLOG_MARK, APLOG_ERR, 0, rc->r->server,
                     "mod_webauth: %s: app state is NULL",
                     mwa_func);
    }

    return_url = make_return_url(rc, 1);

    /* never let return URL have  ?WEBAUTHR=...;;WEBUTHS=...; on the
       end of it, that could get ugly... */
    strip_end(return_url, WEBAUTHR_MAGIC);

    SET_RETURN_URL(return_url);

    if (rc->sconf->debug)
        ap_log_error(APLOG_MARK, APLOG_DEBUG, 0, rc->r->server,
                     "mod_webauth: %s: return_url(%s)", mwa_func, return_url);

    tlen = webauth_token_encoded_length(alist);
    token = apr_palloc(rc->r->pool, tlen);

    status = webauth_token_create_with_key(alist, curr, token, &olen, tlen,
                                           &st->key);

    webauth_attr_list_free(alist);

    if (status != WA_ERR_NONE) {
        mwa_log_webauth_error(rc->r->server, status, mwa_func,
                              "webauth_token_create_with_key", NULL);
        return failure_redirect(rc);
    }

    btoken = apr_palloc(rc->r->pool, apr_base64_encode_len(olen));
    apr_base64_encode(btoken, token, olen);

    redirect_url = apr_pstrcat(rc->r->pool,
                               rc->sconf->login_url,
                               "?RT=", btoken,
                               ";ST=", st->token,
                               NULL);
    
    apr_table_setn(rc->r->err_headers_out, "Location", redirect_url);

    if (rc->sconf->debug)
        ap_log_error(APLOG_MARK, APLOG_DEBUG, 0, rc->r->server,
                     "mod_webauth: redirect_requst_token: redirect(%s)",
                     redirect_url);

    set_pending_cookies(rc);
    return do_redirect(rc);
}


static int
extra_redirect(MWA_REQ_CTXT *rc)
{
    char *redirect_url;

    redirect_url = make_return_url(rc, 0);
    /* always strip extra-redirect URL */
    strip_end(redirect_url, WEBAUTHR_MAGIC);

    apr_table_setn(rc->r->err_headers_out, "Location", redirect_url);

    if (rc->sconf->debug)
        ap_log_error(APLOG_MARK, APLOG_DEBUG, 0, rc->r->server,
                     "mod_webauth: extra_redirect: redirect(%s)",
                     redirect_url);

    set_pending_cookies(rc);
    return do_redirect(rc);
}


static int
ssl_redirect(MWA_REQ_CTXT *rc)
{
    char *redirect_url;
    apr_uri_t uri;

    redirect_url = make_return_url(rc, 0);

    apr_uri_parse(rc->r->pool, redirect_url, &uri);

    if (rc->sconf->debug)
        ap_log_error(APLOG_MARK, APLOG_DEBUG, 0, rc->r->server,
                     "mod_webauth: ssl_redirect: redirect(%s)",
                     redirect_url);

    if (strcmp(uri.scheme, "http") == 0) {
        uri.scheme = (char *) "https";
        if (rc->sconf->ssl_redirect_port) {
            uri.port_str = apr_psprintf(rc->r->pool, 
                                        "%d", rc->sconf->ssl_redirect_port);
            uri.port = rc->sconf->ssl_redirect_port;
        } else {
            uri.port_str = apr_psprintf(rc->r->pool, 
                                        "%d", APR_URI_HTTPS_DEFAULT_PORT);
            uri.port = APR_URI_HTTPS_DEFAULT_PORT;
        }
        redirect_url = apr_uri_unparse(rc->r->pool, &uri, 0);
    } else {
            ap_log_error(APLOG_MARK, APLOG_WARNING, 0, rc->r->server,
                         "mod_webauth: ssl_redirect: error with "
                         "redirect url(%s) denying request", redirect_url);
            return HTTP_UNAUTHORIZED;
    }

    apr_table_setn(rc->r->err_headers_out, "Location", redirect_url);

    if (rc->sconf->debug)
        ap_log_error(APLOG_MARK, APLOG_DEBUG, 0, rc->r->server,
                     "mod_webauth: ssl_redirect: redirect(%s)",
                     redirect_url);

    set_pending_cookies(rc);
    return do_redirect(rc);
}


/*
 * check cookie for valid cred-token. If an epxired one is found,
 * do a Set-Cookie to blank it out. returns NULL on error/expired
 * cookie.
 */
static MWA_CRED_TOKEN *
parse_cred_token_cookie(MWA_REQ_CTXT *rc, MWA_WACRED *cred)
{
    char *cval;
    char *cname = cred_cookie_name(cred->type, cred->service, rc);
    MWA_CRED_TOKEN *ct;
    const char *mwa_func = "parse_cred_token_cookie";

    if (rc->sconf->ring == NULL)
        return NULL;

    cval = find_cookie(rc, cname);
    if (cval == NULL)
        return 0;

    ct =  mwa_parse_cred_token(cval, rc->sconf->ring, NULL, rc);

    if (ct == NULL) {
        /* we coudn't use the cookie, lets set it up to be nuked */
        fixup_setcookie(rc, cname, "");
    }  else {
        if (rc->sconf->debug)
            ap_log_error(APLOG_MARK, APLOG_DEBUG, 0, rc->r->server,
                         "mod_webauth: %s: found valid %s cookie for (%s)", 
                         mwa_func, cname, rc->at.subject);
    }
    return ct;
}


/*
 * add a proxy type to the array if it isn't present.
 */
static void
add_proxy_type(apr_array_header_t *a, char *type)
{
   char **ntype;
   int i;

   ntype = (char**)a->elts;
   for (i = 0; i < a->nelts; i++)
       if (strcmp(ntype[0], type) == 0)
           return;
   ntype = apr_array_push(a);
   *ntype = type;
}


/*
 * take all the creds for the given proxy_type and
 * prepare them. i.e., for krb5 this means creating
 * a credential cache file and setting KRB5CCNAME.
 */
static int 
prepare_creds(MWA_REQ_CTXT *rc, char *proxy_type,
              MWA_CRED_TOKEN **creds, size_t num_creds)
{
    const char *mwa_func="prepare_creds";

    MWA_CRED_INTERFACE *mci = 
        mwa_find_cred_interface(rc->r->server, proxy_type);

    if (mci != NULL) {
        return mci->prepare_creds(rc, creds, num_creds);
    } else {
        ap_log_error(APLOG_MARK, APLOG_ERR, 0, rc->r->server,
                     "mod_webauth: %s: unhandled proxy type: (%s)",
                     mwa_func, proxy_type);
        return 0;
    }
}


/*
 * acquire all the creds of the specified proxy_type. this
 * means making requests to the webkdc. If we don't have
 * the specified proxy_type, we'll need to do a redirect to
 * get it.
 */
static int 
acquire_creds(MWA_REQ_CTXT *rc, char *proxy_type,
              MWA_WACRED *creds, size_t num_creds,
              apr_array_header_t **acquired_creds)
{
    const char *mwa_func="acquire_creds";
    MWA_PROXY_TOKEN *pt = NULL;

    if (rc->sconf->debug) {
        ap_log_error(APLOG_MARK, APLOG_DEBUG, 0, rc->r->server,
                     "mod_webauth: %s: need this proxy type: (%s)",
                     mwa_func, proxy_type);
    }

    if (rc->pt && strcmp(rc->pt->proxy_type, proxy_type) == 0) {
        pt = rc->pt;
    } else {
        pt = parse_proxy_token_cookie(rc, proxy_type);
    }

    /* if we don't have the proxy type then redirect! */
    if (pt == NULL) {
        rc->needed_proxy_type = proxy_type;
        return redirect_request_token(rc);
    }

    if (!mwa_get_creds_from_webkdc(rc, pt, creds, num_creds, acquired_creds)) {

        /* FIXME: what do we want to do here? mwa_get_creds_from_webkdc
           will log any errors. We could either cause a failure_redirect
           or let the request continue without all the desired credentials
           and let the app cope. This might need to be a directive.
           for now, lets just continue, since that seems like the most
           reasonable choice... */

        if (rc->sconf->debug) {
            ap_log_error(APLOG_MARK, APLOG_DEBUG, 0, rc->r->server,
                         "mod_webauth: %s: mwa_get_creds_from_webkdc failed!",
                         mwa_func);
        }
    } else {
        /* need to construct new cookies for newly gathered creds */
        if (*acquired_creds) {
            MWA_CRED_TOKEN **creds = (MWA_CRED_TOKEN**)(*acquired_creds)->elts;
            int i, num_creds = (*acquired_creds)->nelts;
            for (i=0; i < num_creds; i++) {
                make_cred_cookie(creds[i], rc);
            }
        }
    }


    return OK;
}


/*
 * use_creds is on, so we need to gather creds (from cookies and/or
 * webkdc, redirecting if we don't have a proxy-token)
 */
static int 
gather_creds(MWA_REQ_CTXT *rc)
{
    int i, code;
    apr_array_header_t *needed_creds = NULL; /* (MWA_WACRED) */
    apr_array_header_t *needed_proxy_types = NULL; /* (char*) */
    apr_array_header_t *all_proxy_types = NULL; /* (char*) */
    apr_array_header_t *gathered_creds = NULL; /* (MWA_CRED_TOKEN*) */
    apr_array_header_t *acquired_creds = NULL; /* (MWA_CRED_TOKEN*) */
    MWA_WACRED *ncred, *cred;
    MWA_CRED_TOKEN *ct, **nct;
 
    cred = (MWA_WACRED*) rc->dconf->creds->elts;
    
    for (i = 0; i < rc->dconf->creds->nelts; i++) {
        if (cred[i].service) {

            if (all_proxy_types == NULL)
                all_proxy_types = apr_array_make(rc->r->pool, 2,
                                                 sizeof(char*));
            add_proxy_type(all_proxy_types, cred[i].type);

            /* check the cookie first */
            ct = parse_cred_token_cookie(rc, &cred[i]);

            if (ct != NULL) {
                /* save in gathered creds */
                if (gathered_creds == NULL)
                    gathered_creds = apr_array_make(rc->r->pool, 
                                                    rc->dconf->creds->nelts,
                                                    sizeof(MWA_CRED_TOKEN*));
                nct = apr_array_push(gathered_creds);
                *nct = ct;
            } else {
                /* keep track of the ones we need */
                if (needed_creds == NULL) {
                    needed_creds = apr_array_make(rc->r->pool, 5,
                                                  sizeof(MWA_WACRED));
                    needed_proxy_types = apr_array_make(rc->r->pool, 2,
                                                        sizeof(char*));
                }
                ncred = apr_array_push(needed_creds);
                ncred->type = cred[i].type;
                ncred->service = cred[i].service;
                add_proxy_type(needed_proxy_types, cred[i].type);
            }
        }
    }

    /* now, for each proxy type that has needed credentials, 
       try and acquire them from the webkdc. */
    if (needed_proxy_types != NULL) {
        char **proxy = (char**)needed_proxy_types->elts;
        cred = (MWA_WACRED*) needed_creds->elts;

        /* foreach proxy type, attempt to acquire the needed creds */
        for (i=0; i < needed_proxy_types->nelts; i++) {
            code = acquire_creds(rc, proxy[i], cred, 
                                 needed_creds->nelts,
                                 &acquired_creds);
            if (code != OK)
                return code;
        }
    }

    if (gathered_creds != NULL || acquired_creds != NULL) {
        if (gathered_creds == NULL)
            gathered_creds = acquired_creds;
        else if (acquired_creds != NULL)
            apr_array_cat(gathered_creds, acquired_creds);
    }

    /* now go through all_proxy_types, and for do any special
       handling for the proxy type and all its credentials.
       for example, for krb5, we'll want to create the cred file,
       dump in all the creds, and point KRB%CCNAME at it for 
       cgi programs.
    */

    if (all_proxy_types != NULL && gathered_creds != NULL) {
        char **proxy = (char**)all_proxy_types->elts;
        MWA_CRED_TOKEN **cred = (MWA_CRED_TOKEN**) gathered_creds->elts;

        /* foreach proxy type, process the creds */
        for (i=0; i < all_proxy_types->nelts; i++) {
            if (!prepare_creds(rc, proxy[i], cred, gathered_creds->nelts)) {
                /* FIXME: similar as case where we can't get
                   creds from the webkdc. prepare_creds will log
                   any errors. For now, we continue and let the
                   app cope. 
                */
            }
        }
    }

    return OK;
}


/*
 * go through cookies first. If we don't have a valid 
 * app-token and/or proxy-token cookie, check URL and
 * process the token if present.
 */
static int 
gather_tokens(MWA_REQ_CTXT *rc)
{
    int code, in_url;

    /* check the URL. this will parse the token in WEBAUTHR if there
       was one, and create the appropriate cookies, as well as fill in
       rc->{at, proxy_tokens}. */
    code = check_url(rc, &in_url);
    if (code != OK)
        return code;

    if (rc->at.subject == NULL) {
        /* not in URL, check cookie. If we have a bad/expired token in the
         cookie, parse_app_token_cookie will set it up to be expired. */
        parse_app_token_cookie(rc);

        /* if its still NULL, time for a redirect */
        if (rc->at.subject == NULL)
            return redirect_request_token(rc);
    }

    /* check if the WEBAUTHR crap was in the URL and we are configured
       to do a redirect. redirect now so we don't waste time doing saving
       creds if we are configured to saved creds for this request */
    if (in_url
        && ((rc->dconf->extra_redirect_ex && rc->dconf->extra_redirect)
            || (!rc->dconf->extra_redirect_ex && rc->sconf->extra_redirect)))
        return extra_redirect(rc);

    /* if use_creds is on, look for creds. If creds aren't found,
       see if we have a proxy-token for the creds. The proxy-token
       might already be set from check_url, if not, we need to call
       parse_proxy_token_cookie to see if we have one in a cookie.
       If we don't, time for a redirect! */
    if (rc->dconf->use_creds && rc->dconf->creds) {
        code = gather_creds(rc);
        if (code != OK)
            return code;
    }

    return OK;
}


static int
check_user_id_hook(request_rec *r)
{
    const char *at = ap_auth_type(r);
    char *wte, *wtc, *wtlu;
    MWA_REQ_CTXT rc;

    memset(&rc, 0, sizeof(rc));
    rc.r = r;

    rc.dconf = (MWA_DCONF*)ap_get_module_config(r->per_dir_config,
                                                &webauth_module);

    rc.sconf = (MWA_SCONF*)ap_get_module_config(r->server->module_config,
                                                &webauth_module);
    if (rc.sconf->debug)
        ap_log_error(APLOG_MARK, APLOG_DEBUG, 0, r->server,
                     "mod_webauth: in check_user_id hook(%s)",
                     rc.r->unparsed_uri != NULL ? 
                     rc.r->unparsed_uri : "null-uri");

    if ((at == NULL) ||
        ((strcmp(at, "WebAuth") != 0) &&
         (rc.sconf->auth_type == NULL || 
          strcmp(at, rc.sconf->auth_type) != 0))) {
        return DECLINED;
    }

    /* check to see if SSL is required */
    if (rc.sconf->require_ssl && !is_https(r)) {
        if (rc.sconf->ssl_redirect) {
            return ssl_redirect(&rc);
        } else {
            ap_log_error(APLOG_MARK, APLOG_WARNING, 0, r->server,
                         "mod_webauth: connection is not https, "
                         "denying request");
            return HTTP_UNAUTHORIZED;
        }
    }

    /* first check if we've already validated the user */
    rc.at.subject = mwa_get_note(r, N_SUBJECT); 

    if (rc.at.subject == NULL) {
        int code = gather_tokens(&rc);
        if (code != OK)
            return code;
        if (rc.at.subject != NULL) {
            /* stick it in note for future reference */
            if (rc.sconf->debug)
                ap_log_error(APLOG_MARK, APLOG_DEBUG, 0, r->server,
                             "mod_webauth: stash note, user(%s)",
                             rc.at.subject);
            mwa_setn_note(r, N_SUBJECT, NULL, "%s", rc.at.subject);
        }
    } else {
        if (rc.sconf->debug)
            ap_log_error(APLOG_MARK, APLOG_DEBUG, 0, r->server,
                         "mod_webauth: found note, user(%s)",
                         rc.at.subject);
    }

    if (rc.at.subject == NULL) {
        /* this should never get called, since gather_tokens should
         * set us up for a redirect. we put this here as a safety net.
         */
        ap_log_error(APLOG_MARK, APLOG_WARNING, 0, r->server,
                     "mod_webauth: check_user_id_hook subject still NULL!");
        return HTTP_UNAUTHORIZED;
    }

    if (rc.sconf->debug)
        ap_log_error(APLOG_MARK, APLOG_DEBUG, 0, r->server,
                     "mod_webauth: check_user_id_hook setting user(%s)",
                     rc.at.subject);

    r->user = (char*)rc.at.subject;
    r->ap_auth_type = (char*)at;

    /* set environment variables */

    wte = rc.at.expiration_time ? 
        apr_psprintf(rc.r->pool, "%d", (int)rc.at.expiration_time) : NULL;
    
    wtc = rc.at.creation_time ? 
        apr_psprintf(rc.r->pool, "%d", (int)rc.at.creation_time) : NULL;

    wtlu = rc.at.last_used_time ?
        apr_psprintf(rc.r->pool, "%d", (int)rc.at.last_used_time) : NULL;

    mwa_setenv(&rc, ENV_WEBAUTH_USER, rc.at.subject);
    if (wte != NULL) 
        mwa_setenv(&rc, ENV_WEBAUTH_TOKEN_EXPIRATION, wte);
    if (wtc != NULL)
        mwa_setenv(&rc, ENV_WEBAUTH_TOKEN_CREATION, wtc);
    if (wtlu != NULL) 
        mwa_setenv(&rc, ENV_WEBAUTH_TOKEN_LASTUSED, wtlu);

    if (rc.dconf->dont_cache_ex && rc.dconf->dont_cache)
        dont_cache(&rc);

#ifndef NO_STANFORD_SUPPORT

    if (rc.dconf->su_authgroups != NULL) {
        /* always deny access in this case */
        ap_log_error(APLOG_MARK, APLOG_WARNING, 0, r->server,
                     "mod_webauth: denying access due to use of unsupported "
                     "StanfordAuthGroups directive: %s", 
                     rc.dconf->su_authgroups);
        return HTTP_UNAUTHORIZED;
    }

    if (strcmp(at, "StanfordAuth") == 0) {
        mwa_setenv(&rc, "SU_AUTH_USER", rc.at.subject);
        if (rc.at.creation_time) {
            time_t age = time(NULL) - rc.at.creation_time;
            if (age < 0) 
                age = 0;
            mwa_setenv(&rc, "SU_AUTH_AGE", 
                       apr_psprintf(rc.r->pool, "%d", (int) age));
        }
    }
#endif 

    if (rc.sconf->debug) {
        ap_log_error(APLOG_MARK, APLOG_DEBUG, 0, r->server,
                     "mod_webauth: check_user_id_hook: no_cache(%d) dont_cache(%d) dont_cache_ex(%d)", r->no_cache, 
                     rc.dconf->dont_cache, rc.dconf->dont_cache_ex);
    }

    if (r->proxyreq != PROXYREQ_NONE) {
        /* make sure any webauth_* cookies don't end up proxied */
        /* also strip out stuff from Referer */
        strip_webauth_info(&rc);
    }

    return OK;
}


/*
 * this hook will attempt to find the returned-token and the
 * state-token in the URL (r->the_request). If we find them and stash them in 
 * the notes for the master request, and then remove them from 
 * everywhere we find them, so they 
 * don't show up in access_logs.
 *
 *  we strip them in the following places:
 *    r->the_request
 *    r->unparsed_uri
 *    r->uri
 *    r->filename
 *    r->canonical_filename
 *    r->path_info
 *    r->args
 *    r->parsed_uri.path
 *    r->parsed_uri.query
 *
 *  we'll stick the tokens in the notes table for the initial
 *  request
 *  
 */
static int 
translate_name_hook(request_rec *r)
{
    char *p, *s, *rp;
    char *wr, *ws;
    MWA_SCONF *sconf;
    static const char *rmagic = WEBAUTHR_MAGIC;
    static const char *smagic = WEBAUTHS_MAGIC;

    sconf = (MWA_SCONF*)ap_get_module_config(r->server->module_config,
                                             &webauth_module);
    if (!ap_is_initial_req(r)) {
        return DECLINED;
    }

    /* mwa_log_request(r, "before xlate"); */

    rp = ap_strstr(r->the_request, rmagic);
    if (rp == NULL) {
        /* no tokens in the request, return */
        return DECLINED;
    }

    /* we need to save the tokens for check_user_id_hook. */

    s = rp+WEBAUTHR_MAGIC_LEN;
    p = ap_strchr(s, ';');
    if (p == NULL) {
        ap_log_error(APLOG_MARK, APLOG_ERR, 0, r->server,
                     "mod_webauth: didn't find end of %s", rmagic);
        return DECLINED;
    }
    wr = apr_pstrmemdup(r->pool, s, p-s);
    /*
     * if (sconf->debug)
     * ap_log_error(APLOG_MARK, APLOG_ERR, 0, r->server, 
     * "mod_webauth: stash wr(%s)", wr);
     */
    mwa_setn_note(r, N_WEBAUTHR, NULL, "%s", wr);

    s = p+1;
    p = ap_strstr(s, smagic);
    if (p != NULL) {
        s = p+WEBAUTHS_MAGIC_LEN;
        p = ap_strchr(s, ';');
        if (p == NULL) {
            ap_log_error(APLOG_MARK, APLOG_ERR, 0, r->server,
                         "mod_webauth: didn't find end of %s", smagic);
            return DECLINED;
        }
        ws = apr_pstrmemdup(r->pool, s, p-s);
        /*if (sconf->debug)
         * ap_log_error(APLOG_MARK, APLOG_ERR, 0, r->server,
         * "mod_webauth: stash ws(%s)", ws);
         */
        mwa_setn_note(r, N_WEBAUTHS, NULL, "%s", ws);
        s = p+1;
    }

    /* Strip the WebAuth information from the internal URL unless asked. */
    if (sconf->strip_url) {
        /* move over remaining */
        strcpy(rp, s);
    
        /* these are easier, we strip rmagic and everything after it, 
           which might include smagic */

        strip_end(r->unparsed_uri, rmagic);
        strip_end(r->uri, rmagic);
        strip_end(r->filename, rmagic);
        strip_end(r->canonical_filename, rmagic);
        strip_end(r->path_info, rmagic);
        /* make sure to try rmagic and rmagic+1, since if there were
           no query args, rmagic ends up looking like query args and
           the ? gets stripped */
        strip_end(r->args, rmagic);
        strip_end(r->args, rmagic+1);
        if (r->args != NULL && *r->args == 0)
            r->args = NULL;
        strip_end(r->parsed_uri.path, rmagic);
        /* make sure to try rmagic and rmagic+1, since if there were
           no query args, rmagic ends up looking like query args and
           the ? gets stripped */
        strip_end(r->parsed_uri.query, rmagic);
        strip_end(r->parsed_uri.query, rmagic+1);
        if (r->parsed_uri.query != NULL && *r->parsed_uri.query == 0)
            r->parsed_uri.query = NULL;
    }

    /* mwa_log_request(r, "after xlate"); */

    /* still need to return DECLINED, so other modules (like mod_rerewrite)
       get a crack at things */
    return DECLINED;
}


static int 
fixups_hook(request_rec *r)
{
    MWA_REQ_CTXT rc;

    memset(&rc, 0, sizeof(rc));

    rc.r = r;
    rc.dconf = (MWA_DCONF*)ap_get_module_config(r->per_dir_config,
                                                 &webauth_module);

    rc.sconf = (MWA_SCONF*)ap_get_module_config(r->server->module_config,
                                                &webauth_module);

    if (rc.dconf->do_logout) {
        nuke_all_webauth_cookies(&rc);
        dont_cache(&rc);
    } else {
        set_pending_cookies(&rc);
    }
    return DECLINED;
}


static int
seconds(const char *value, const char **error_str)
{
    char temp[32];
    size_t mult, len;
    
    len = strlen(value);
    if (len > (sizeof(temp) - 1)) {
        *error_str = "error: value too long!";
        return 0;
    }

    strcpy(temp, value);

    switch(temp[len-1]) {
        case 's': 
            mult = 1;
            break;
        case 'm':
            mult = 60;
            break;
        case 'h': 
            mult = 60*60; 
            break;
        case 'd': 
            mult = 60*60*24; 
            break;
        case 'w': 
            mult = 60*60*24*7; 
            break;
        default:
            *error_str = "error: invalid units specified";
            return 0;
            break;
    }
    
    temp[len-1] = '\0';
    return atoi(temp) * mult;
}


static const char *
cfg_str(cmd_parms *cmd, void *mconf, const char *arg)
{
    intptr_t e = (intptr_t) cmd->info;
    const char *error_str = NULL;
    MWA_DCONF *dconf = (MWA_DCONF *)mconf;

    MWA_SCONF *sconf = (MWA_SCONF *)
        ap_get_module_config(cmd->server->module_config, &webauth_module);
    
    switch (e) {
        /* server configs */
        case E_WebKdcURL:
            sconf->webkdc_url = apr_pstrdup(cmd->pool, arg);
            break;
        case E_WebKdcPrincipal:
            sconf->webkdc_principal = apr_pstrdup(cmd->pool, arg);
            break;
        case E_LoginURL:
            sconf->login_url = apr_pstrdup(cmd->pool, arg);
            break;
        case E_AuthType:
            sconf->auth_type = apr_pstrdup(cmd->pool, arg);
            break;
        case E_FailureURL:
            dconf->failure_url = apr_pstrdup(cmd->pool, arg);
            break;
        case E_WebKdcSSLCertFile:
            sconf->webkdc_cert_file = ap_server_root_relative(cmd->pool, arg);
            break;
        case E_Keyring:
            sconf->keyring_path = ap_server_root_relative(cmd->pool, arg);
            break;
        case E_CredCacheDir:
            sconf->cred_cache_dir = ap_server_root_relative(cmd->pool, arg);
            break;
        case E_LoginCanceledURL:
            dconf->login_canceled_url = apr_pstrdup(cmd->pool, arg);
            break;
        case E_ServiceTokenCache:
            sconf->st_cache_path = ap_server_root_relative(cmd->pool, arg);
            break;
        case E_VarPrefix:
            dconf->var_prefix = apr_pstrdup(cmd->pool, arg);
            break;
        case E_SubjectAuthType:
            sconf->subject_auth_type = apr_pstrdup(cmd->pool, arg);
            sconf->subject_auth_type_ex = 1;
            /* FIXME: this check needs to be more dynamic, or done later */
            if (strcmp(arg, "krb5") && strcmp(arg,"webkdc")) {
                error_str = apr_psprintf(cmd->pool,
                                         "Invalid value directive %s: %s",
                                         cmd->directive->directive, arg);
            }
            break;
        case E_ReturnURL:
            dconf->return_url = apr_pstrdup(cmd->pool, arg);
            break;
        case E_PostReturnURL:
            dconf->post_return_url = apr_pstrdup(cmd->pool, arg);
            break;
        case E_AppTokenLifetime:
            dconf->app_token_lifetime = seconds(arg, &error_str);
            break;
        case E_TokenMaxTTL:
            sconf->token_max_ttl = seconds(arg, &error_str);
            sconf->token_max_ttl_ex = 1;
            break;
        case E_KeyringKeyLifetime:
            sconf->keyring_key_lifetime = seconds(arg, &error_str);
            sconf->keyring_key_lifetime_ex = 1;
            break;
        case E_SSLRedirectPort:
            sconf->ssl_redirect_port = atoi(arg);
            sconf->ssl_redirect_port_ex = 1;
            break;
        case E_InactiveExpire:
            dconf->inactive_expire = seconds(arg, &error_str);
            break;
        case E_LastUseUpdateInterval:
            dconf->last_use_update_interval = seconds(arg, &error_str);
            break;
#ifndef NO_STANFORD_SUPPORT
        case SE_ConfirmMsg:
            /*
            ap_log_error(APLOG_MARK, APLOG_WARNING, 0, NULL,
                         "mod_webauth: ignoring WebAuth 2.5 directive: %s",
                         cmd->directive->directive);
            */
            break;
        case SE_Life:
            dconf->app_token_lifetime = atoi(arg) * 60;
            dconf->force_login = 1;
            dconf->force_login_ex = 1;
            break;
        case SE_ReturnURL:
            dconf->return_url = apr_pstrdup(cmd->pool, arg);
            break;
        case SE_Groups:
            dconf->su_authgroups = apr_pstrdup(cmd->pool, arg);
            break;
#endif
        default:
            error_str = 
                apr_psprintf(cmd->pool,
                             "Invalid value cmd->info(%d) for directive %s",
                             (int) e,
                             cmd->directive->directive);
            break;

    }
    return error_str;
}


static const char *
cfg_flag(cmd_parms *cmd, void *mconfig, int flag)
{
    intptr_t e = (intptr_t) cmd->info;
    char *error_str = NULL;
    MWA_DCONF *dconf = (MWA_DCONF*) mconfig;

    MWA_SCONF *sconf = (MWA_SCONF *)
        ap_get_module_config(cmd->server->module_config, &webauth_module);
    
    switch (e) {
        /* server configs */
        case E_Debug:
            sconf->debug = flag;
            sconf->debug_ex = 1;
            break;
        case E_KeyringAutoUpdate:
            sconf->keyring_auto_update = flag;
            sconf->keyring_auto_update_ex = 1;
            break;
        case E_RequireSSL:
            sconf->require_ssl = flag;
            sconf->require_ssl_ex = 1;
            break;
        case E_SSLRedirect:
            sconf->ssl_redirect = flag;
            sconf->ssl_redirect_ex = 1;
            break;
        case E_WebKdcSSLCertCheck:
            sconf->webkdc_cert_check = flag;
            sconf->webkdc_cert_check_ex = 1;
            break;
        /* server config or directory config */
        case E_ExtraRedirect:
            if (cmd->path == NULL) {
                sconf->extra_redirect = flag;
                sconf->extra_redirect_ex = 1;
            } else {
                dconf->extra_redirect = flag;
                dconf->extra_redirect_ex = 1;
            }
            break;
        /* start of dconfigs */
        case E_SSLReturn:
            dconf->ssl_return = flag;
            dconf->ssl_return_ex = 1;
            break;
        case E_DoLogout:
            dconf->do_logout = flag;
            dconf->do_logout_ex = 1;
            break;
        case E_ForceLogin:
            dconf->force_login = flag;
            dconf->force_login_ex = 1;
            break;
        case E_UseCreds:
            dconf->use_creds = flag;
            dconf->use_creds_ex = 1;
            break;
        case E_StripURL:
            sconf->strip_url = flag;
            sconf->strip_url_ex = 1;
            break;
        case E_DontCache:
            dconf->dont_cache = flag;
            dconf->dont_cache_ex = 1;
            break;
#ifndef NO_STANFORD_SUPPORT
        case SE_DoConfirm:
            if (flag) {
                ap_log_error(APLOG_MARK, APLOG_WARNING, 0, NULL,
                             "mod_webauth: ignoring WebAuth 2.5 directive: %s",
                             cmd->directive->directive);
            }
            break;
        case SE_DontCache:
            dconf->dont_cache = flag;
            dconf->dont_cache_ex = 1;
            break;
        case SE_ForceReload:
            dconf->extra_redirect = flag;
            dconf->extra_redirect_ex = 1;
            break;
#endif
        default:
            error_str = 
                apr_psprintf(cmd->pool,
                             "Invalid value cmd->info(%d) for directive %s",
                             (int) e,
                             cmd->directive->directive);
            break;

    }
    return error_str;
}


static const char *
cfg_take12(cmd_parms *cmd, void *mconfig, const char *w1, const char *w2)
{
    intptr_t e = (intptr_t) cmd->info;
    char *error_str = NULL;
    MWA_DCONF *dconf = (MWA_DCONF*) mconfig;
    MWA_WACRED *cred;

    MWA_SCONF *sconf = (MWA_SCONF *)
        ap_get_module_config(cmd->server->module_config, &webauth_module);
    
    switch (e) {
        /* server configs */
        case E_Keytab:
            sconf->keytab_path = ap_server_root_relative(cmd->pool, w1);
            sconf->keytab_principal = 
                (w2 != NULL) ? apr_pstrdup(cmd->pool, w2) : NULL;
            break;
        /* start of dconfigs */
        case E_Cred:
            if (dconf->creds == NULL) {
                dconf->creds = 
                    apr_array_make(cmd->pool, 5, sizeof(MWA_WACRED));
            }
            cred = apr_array_push(dconf->creds);
            cred->type = apr_pstrdup(cmd->pool, w1);
            cred->service = (w2 == NULL) ? NULL : apr_pstrdup(cmd->pool, w2);
            break;
        default:
            error_str = 
                apr_psprintf(cmd->pool,
                             "Invalid value cmd->info(%d) for directive %s",
                             (int) e,
                             cmd->directive->directive);
            break;
    }
    return error_str;
}

#define SSTR(dir,mconfig,help) \
  {dir, (cmd_func)cfg_str,(void*)mconfig, RSRC_CONF, TAKE1, help}

#define SSTR12(dir,mconfig,help) \
  {dir, (cmd_func)cfg_take12,(void*)mconfig, RSRC_CONF, TAKE12, help}

#define SFLAG(dir,mconfig,help) \
  {dir, (cmd_func)cfg_flag,(void*)mconfig, RSRC_CONF, FLAG, help}

#define DSTR(dir,mconfig,help) \
  {dir, (cmd_func)cfg_str,(void*)mconfig, OR_AUTHCFG, TAKE1, help}

#define DFLAG(dir,mconfig,help) \
  {dir, (cmd_func)cfg_flag,(void*)mconfig, OR_AUTHCFG, FLAG, help}

#define AFLAG(dir,mconfig,help) \
  {dir, (cmd_func)cfg_flag,(void*)mconfig, OR_AUTHCFG|RSRC_CONF, FLAG, help}

/* these can only be in the server .conf file */

#define ADSTR(dir,mconfig,help) \
  {dir, (cmd_func)cfg_str,(void*)mconfig, ACCESS_CONF, TAKE1, help}

#define ADFLAG(dir,mconfig,help) \
  {dir, (cmd_func)cfg_flag,(void*)mconfig, ACCESS_CONF, FLAG, help}

#define ADTAKE12(dir,mconfig,help) \
  {dir, (cmd_func)cfg_take12,(void*)mconfig, ACCESS_CONF, TAKE12, help}

static const command_rec cmds[] = {
    /* server/vhost */

    SSTR(CD_WebKdcURL, E_WebKdcURL, CM_WebKdcURL),
    SSTR(CD_WebKdcPrincipal, E_WebKdcPrincipal, CM_WebKdcPrincipal),
    SSTR(CD_WebKdcSSLCertFile, E_WebKdcSSLCertFile, CM_WebKdcSSLCertFile),
    SSTR(CD_LoginURL, E_LoginURL, CM_LoginURL),
    SSTR(CD_AuthType, E_AuthType, CM_AuthType),
    SSTR(CD_Keyring, E_Keyring, CM_Keyring),
    SSTR(CD_CredCacheDir, E_CredCacheDir,  CM_CredCacheDir),
    SSTR(CD_ServiceTokenCache, E_ServiceTokenCache, CM_ServiceTokenCache),
    SSTR(CD_SubjectAuthType, E_SubjectAuthType, CM_SubjectAuthType),
    SSTR12(CD_Keytab, E_Keytab,  CM_Keytab),
    SFLAG(CD_StripURL, E_StripURL, CM_StripURL),
    SFLAG(CD_Debug, E_Debug, CM_Debug),
    SFLAG(CD_KeyringAutoUpdate, E_KeyringAutoUpdate, CM_KeyringAutoUpdate),
    SFLAG(CD_RequireSSL, E_RequireSSL, CM_RequireSSL),
    SFLAG(CD_SSLRedirect, E_SSLRedirect, CM_SSLRedirect),
    SFLAG(CD_WebKdcSSLCertCheck, E_WebKdcSSLCertCheck, CM_WebKdcSSLCertCheck),
    SSTR(CD_TokenMaxTTL, E_TokenMaxTTL, CM_TokenMaxTTL),
    SSTR(CD_KeyringKeyLifetime, E_KeyringKeyLifetime, CM_KeyringKeyLifetime),
    SSTR(CD_SSLRedirectPort, E_SSLRedirectPort, CM_SSLRedirectPort),

    /* directory */

    ADSTR(CD_AppTokenLifetime, E_AppTokenLifetime, CM_AppTokenLifetime),
    ADSTR(CD_InactiveExpire, E_InactiveExpire, CM_InactiveExpire),
    ADSTR(CD_LastUseUpdateInterval, E_LastUseUpdateInterval, CM_LastUseUpdateInterval),
    ADFLAG(CD_ForceLogin, E_ForceLogin, CM_ForceLogin),
    ADFLAG(CD_UseCreds, E_UseCreds, CM_UseCreds),
    ADFLAG(CD_DoLogout, E_DoLogout, CM_DoLogout),
    ADTAKE12(CD_Cred, E_Cred, CM_Cred),
    ADSTR(CD_FailureURL, E_FailureURL, CM_FailureURL),

    /* server/vhost or directory or .htaccess if override auth config */
    AFLAG(CD_ExtraRedirect, E_ExtraRedirect, CM_ExtraRedirect),

    /* directory or .htaccess if override auth config */
    DFLAG(CD_DontCache, E_DontCache, CM_DontCache),
    DFLAG(CD_SSLReturn, E_SSLReturn, CM_SSLReturn),
    DSTR(CD_ReturnURL, E_ReturnURL, CM_ReturnURL),
    DSTR(CD_PostReturnURL, E_PostReturnURL, CM_PostReturnURL),
    DSTR(CD_LoginCanceledURL, E_LoginCanceledURL, CM_LoginCanceledURL),
    DSTR(CD_VarPrefix, E_VarPrefix, CM_VarPrefix),

#ifndef NO_STANFORD_SUPPORT
    DSTR(SCD_ConfirmMsg, SE_ConfirmMsg, SCM_ConfirmMsg),
    DSTR(SCD_Groups, SE_Groups, SCM_Groups),
    DFLAG(SCD_DoConfirm, SE_DoConfirm, SCM_DoConfirm),
    DSTR(SCD_Life, SE_Life, SCM_Life),
    DSTR(SCD_ReturnURL, SE_ReturnURL, SCM_ReturnURL),
    DFLAG(SCD_DontCache, SE_DontCache, SCM_DontCache),
    DFLAG(SCD_ForceReload, SE_ForceReload, SCM_ForceReload),
#endif 
    { NULL, { NULL }, NULL, 0, 0, NULL }
};

#undef SSTR
#undef SFLAG
#undef SINT
#undef DSTR
#undef DFLAG
#undef DINT
#undef AFLAG
#undef ADSTR
#undef ADFLAG
#undef ADTAKE12


#if 0
static int webauth_auth_checker(request_rec *r)
{
    ap_log_error(APLOG_MARK, APLOG_ERR, 0, r->server,
                 "mod_webauth: in auth_checker hook");
    return DECLINED;
}


static int webauth_access_checker(request_rec *r)
{
    ap_log_error(APLOG_MARK, APLOG_ERR, 0, r->server,
                 "mod_webauth: in accesss_checker hook");
    return DECLINED;
}
#endif 


static void 
register_hooks(apr_pool_t *p UNUSED)
{
    /* get our module called before the basic authentication stuff */
    static const char * const mods[]={ "mod_access.c", "mod_auth.c", NULL };

    ap_hook_post_config(mod_webauth_init, NULL, NULL, APR_HOOK_MIDDLE);
    ap_hook_child_init(mod_webauth_child_init, NULL, NULL, APR_HOOK_MIDDLE);

    /* we need to get run before anyone else, so we can clean up the URL
       if need be */
    ap_hook_translate_name(translate_name_hook, NULL, NULL, 
                           APR_HOOK_REALLY_FIRST);

    ap_hook_check_user_id(check_user_id_hook, NULL, mods, APR_HOOK_MIDDLE);
#if 0
    ap_hook_access_checker(webauth_access_checker, NULL,NULL,APR_HOOK_FIRST);
    ap_hook_auth_checker(webauth_auth_checker, NULL, NULL, APR_HOOK_FIRST);
#endif
    ap_hook_handler(handler_hook, NULL, NULL, APR_HOOK_MIDDLE);
    ap_hook_fixups(fixups_hook, NULL,NULL,APR_HOOK_MIDDLE);
}


/* Dispatch list for API hooks */
module AP_MODULE_DECLARE_DATA webauth_module = {
    STANDARD20_MODULE_STUFF, 
    config_dir_create,     /* create per-dir    config structures */
    config_dir_merge,      /* merge  per-dir    config structures */
    config_server_create,  /* create per-server config structures */
    config_server_merge,   /* merge  per-server config structures */
    cmds,                  /* table of config file commands       */
    register_hooks         /* register hooks                      */
};