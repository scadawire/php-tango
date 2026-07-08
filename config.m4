dnl config.m4 for the php-tango extension
dnl A PHP binding for the TANGO Controls system (cppTango), analogous to PyTango.

PHP_ARG_ENABLE([tango],
  [whether to enable TANGO Controls support],
  [AS_HELP_STRING([--enable-tango],
    [Enable TANGO Controls (cppTango) support])],
  [no])

if test "$PHP_TANGO" != "no"; then

  dnl --- locate cppTango via pkg-config --------------------------------------
  AC_PATH_PROG(PKG_CONFIG, pkg-config, no)
  if test "$PKG_CONFIG" = "no"; then
    AC_MSG_ERROR([pkg-config is required to build php-tango but was not found])
  fi

  if ! $PKG_CONFIG --exists tango; then
    AC_MSG_ERROR([the 'tango' pkg-config module was not found. Install cppTango, or set PKG_CONFIG_PATH to point at tango.pc])
  fi

  TANGO_VERSION=`$PKG_CONFIG --modversion tango`
  AC_MSG_RESULT([found cppTango version $TANGO_VERSION])

  TANGO_CFLAGS=`$PKG_CONFIG --cflags tango`
  TANGO_LIBS=`$PKG_CONFIG --libs tango`

  dnl feed include/lib lines to the PHP build system
  PHP_EVAL_INCLINE([$TANGO_CFLAGS])
  PHP_EVAL_LIBLINE([$TANGO_LIBS], [TANGO_SHARED_LIBADD])

  dnl this is a C++ extension
  PHP_REQUIRE_CXX()
  PHP_SUBST([TANGO_SHARED_LIBADD])

  dnl cppTango 9.x requires at least C++14. Silence its deprecation noise.
  TANGO_EXTRA_CXXFLAGS="-std=c++14 -Wno-deprecated-declarations -Wno-deprecated -DZEND_ENABLE_STATIC_TSRMLS_CACHE=1"

  dnl -----------------------------------------------------------------------
  dnl Resolve the cppTango header root that matches the linked library.
  dnl
  dnl A second Tango install (e.g. an incomplete source build in
  dnl /usr/local) can shadow the real headers: because /usr/local/include is
  dnl searched before /usr/include for <tango/...>, and -I/usr/include cannot
  dnl re-order a standard system dir, the wrong headers get pulled in -- even
  dnl from *within* cppTango's own headers. We sidestep this by pointing a
  dnl private shim directory's "tango" entry at the correct root and adding
  dnl -I<shim> (a non-standard dir, so it wins over every standard dir).
  dnl -----------------------------------------------------------------------
  TANGO_SHIM="`pwd`/tango-shim"
  rm -rf "$TANGO_SHIM"
  mkdir -p "$TANGO_SHIM"

  tango_root=""
  tango_cand_dirs=`echo "$TANGO_CFLAGS" | tr ' ' '\n' | sed -n 's/^-I//p'`
  for cand in $tango_cand_dirs /usr/include /usr/local/include; do
    if test -f "$cand/tango/tango.h"; then
      rm -f "$TANGO_SHIM/tango"
      ln -s "$cand/tango" "$TANGO_SHIM/tango"
      echo '#include <tango/tango.h>' > conftest-tango.cpp
      echo 'const char *php_tango_v = Tango::TgLibVers;' >> conftest-tango.cpp
      if $CXX $CXXFLAGS -std=c++14 -I"$TANGO_SHIM" $TANGO_CFLAGS \
           -c conftest-tango.cpp -o conftest-tango.o >/dev/null 2>&1; then
        tango_root="$cand"
        rm -f conftest-tango.cpp conftest-tango.o
        break
      fi
      rm -f conftest-tango.cpp conftest-tango.o
    fi
  done

  if test -z "$tango_root"; then
    AC_MSG_ERROR([could not find a usable cppTango header root that both provides <tango/tango.h> and compiles against the linked library])
  fi
  AC_MSG_RESULT([using cppTango headers from $tango_root (via shim $TANGO_SHIM)])

  dnl shim -I must come first so it beats any shadowing install
  TANGO_EXTRA_CXXFLAGS="-I$TANGO_SHIM $TANGO_EXTRA_CXXFLAGS"

  PHP_NEW_EXTENSION([tango],
    [tango.cpp],
    [$ext_shared],,
    [$TANGO_CFLAGS $TANGO_EXTRA_CXXFLAGS],
    [cxx])
fi
