dnl
dnl APACHE_MODULE(modname [, shared])
dnl
dnl Includes an extension in the build.
dnl
dnl "modname" is the name of the modules/ subdir where the extension resides
dnl "shared" can be set to "shared" or "yes" to build the extension as
dnl a dynamically loadable library.
dnl
dnl XXX - for now, all modules using this function are in modules/standard
AC_DEFUN(APACHE_MODULE,[
  if test -d "$cwd/$srcdir/modules/standard" ; then
dnl    MOD_SUBDIRS="$MOD_SUBDIRS $1"
    if test "$2" != "shared" -a "$2" != "yes"; then
      libname=$(basename $1)
      _extlib="libapachemod_${libname}.a"
      MOD_LTLIBS="$MOD_LTLIBS modules/standard/libapachemod_${libname}.la"
      MOD_LIBS="$MOD_LIBS standard/$_extlib"
      MOD_STATIC="$MOD_STATIC $1"
    else
      MOD_SHARED="$MOD_SHARED $1"
    fi
dnl    APACHE_OUTPUT(modules/$1/Makefile)
  fi
])

AC_SUBST(MOD_LTLIBS)

dnl ## APACHE_OUTPUT(file)
dnl ## adds "file" to the list of files generated by AC_OUTPUT
dnl ## This macro can be used several times.
AC_DEFUN(APACHE_OUTPUT, [
  APACHE_OUTPUT_FILES="$APACHE_OUTPUT_FILES $1"
])

dnl
dnl AC_ADD_LIBRARY(library)
dnl
dnl add a library to the link line
dnl
AC_DEFUN(AC_ADD_LIBRARY,[
  APACHE_ONCE(LIBRARY, $1, [
    EXTRA_LIBS="$EXTRA_LIBS -l$1"
  ])
])

dnl
dnl AC_CHECK_DEFINE(macro, headerfile)
dnl
dnl checks for the macro in the header file
dnl
AC_DEFUN(AC_CHECK_DEFINE,[
  AC_CACHE_CHECK(for $1 in $2, ac_cv_define_$1,
  AC_EGREP_CPP([YES_IS_DEFINED], [
#include <$2>
#ifdef $1
YES_IS_DEFINED
#endif
  ], ac_cv_define_$1=yes, ac_cv_define_$1=no))
  if test "$ac_cv_define_$1" = "yes" ; then
      AC_DEFINE(HAVE_$1,,
          [Define if the macro "$1" is defined on this system])
  fi
])

dnl
dnl AC_TYPE_RLIM_T
dnl
dnl If rlim_t is not defined, define it to int
dnl
AC_DEFUN(AC_TYPE_RLIM_T, [
  AC_CACHE_CHECK([for rlim_t], ac_cv_type_rlim_t, [
    AC_TRY_COMPILE([#include <sys/resource.h>], [rlim_t spoon;], [
      ac_cv_type_rlim_t=yes
    ],[ac_cv_type_rlim_t=no
    ])
  ])
  if test "$ac_ac_type_rlim_t" = "no" ; then
      AC_DEFINE(rlim_t, int,
          [Define to 'int' if <sys/resource.h> doesn't define it for us])
  fi
])

dnl
dnl APACHE_ONCE(namespace, variable, code)
dnl
dnl execute code, if variable is not set in namespace
dnl
AC_DEFUN(APACHE_ONCE,[
  unique=`echo $ac_n "$2$ac_c" | tr -c -d a-zA-Z0-9`
  cmd="echo $ac_n \"\$$1$unique$ac_c\""
  if test -n "$unique" && test "`eval $cmd`" = "" ; then
    eval "$1$unique=set"
    $3
  fi
])

dnl
dnl APACHE_CHECK_THREADS()
dnl
dnl Determine the best flags for linking against a threading library.
dnl
AC_DEFUN(THREAD_TEST, [
AC_TRY_RUN( [
#include <pthread.h>

void *thread_routine(void *data) {
    return data;
}

int main() {
    pthread_t thd;
    int data = 1;
    return pthread_create(&thd, NULL, thread_routine, &data);
} ], [ 
  THREADS_WORKING="yes"
  ], [
  THREADS_WORKING="no"
  ], THREADS_WORKING="no" ) ] )

define(APACHE_CHECK_THREADS, [dnl
  cflags_orig="$CFLAGS"
  ldflags_orig="$LDFLAGS"
  for test_cflag in $1; do
    for test_ldflag in $2; do
      CFLAGS="$test_cflag $cflags_orig"
      LDFLAGS="$test_ldflag $ldflags_orig"
      THREAD_TEST()
      if test "$THREADS_WORKING" = "yes"; then
        break
      fi
    done
    if test "$THREADS_WORKING" = "yes"; then
      threads_result="Updating CFLAGS and LDFLAGS"
      break
    fi
      threads_result="Threads not found"
  done
] )
        
