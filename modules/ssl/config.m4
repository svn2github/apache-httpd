dnl Licensed to the Apache Software Foundation (ASF) under one or more
dnl contributor license agreements.  See the NOTICE file distributed with
dnl this work for additional information regarding copyright ownership.
dnl The ASF licenses this file to You under the Apache License, Version 2.0
dnl (the "License"); you may not use this file except in compliance with
dnl the License.  You may obtain a copy of the License at
dnl
dnl      http://www.apache.org/licenses/LICENSE-2.0
dnl
dnl Unless required by applicable law or agreed to in writing, software
dnl distributed under the License is distributed on an "AS IS" BASIS,
dnl WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
dnl See the License for the specific language governing permissions and
dnl limitations under the License.

AC_DEFUN([CHECK_OCSP], [
AC_CHECK_HEADERS(openssl/ocsp.h, 
  [AC_DEFINE([HAVE_OCSP], 1, [Define if OCSP is supported by OpenSSL])]
)
])

dnl #  start of module specific part
APACHE_MODPATH_INIT(ssl)

dnl #  list of module object files
ssl_objs="dnl
mod_ssl.lo dnl
ssl_engine_config.lo dnl
ssl_engine_dh.lo dnl
ssl_engine_init.lo dnl
ssl_engine_io.lo dnl
ssl_engine_kernel.lo dnl
ssl_engine_log.lo dnl
ssl_engine_mutex.lo dnl
ssl_engine_pphrase.lo dnl
ssl_engine_rand.lo dnl
ssl_engine_vars.lo dnl
ssl_expr.lo dnl
ssl_expr_eval.lo dnl
ssl_expr_parse.lo dnl
ssl_expr_scan.lo dnl
ssl_scache.lo dnl
ssl_util.lo dnl
ssl_util_ssl.lo dnl
ssl_engine_ocsp.lo dnl
ssl_util_ocsp.lo dnl
"
dnl #  hook module into the Autoconf mechanism (--enable-ssl option)
APACHE_MODULE(ssl, [SSL/TLS support (mod_ssl)], $ssl_objs, , no, [
    APACHE_CHECK_SSL_TOOLKIT
    APR_SETVAR(MOD_SSL_LDADD, [\$(SSL_LIBS)])
    CHECK_OCSP
    if test "x$enable_ssl" = "xshared"; then
       # The only symbol which needs to be exported is the module
       # structure, so ask libtool to hide everything else:
       APR_ADDTO(MOD_SSL_LDADD, [-export-symbols-regex ssl_module])
    fi
])

# Ensure that other modules can pick up mod_ssl.h
APR_ADDTO(INCLUDES, [-I\$(top_srcdir)/$modpath_current])

dnl #  end of module specific part
APACHE_MODPATH_FINISH

