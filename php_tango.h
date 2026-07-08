/*
 * php_tango.h - PHP binding for the TANGO Controls system (cppTango)
 *
 * Provides a PHP class hierarchy analogous to PyTango's client API,
 * centred on Tango\DeviceProxy.
 */
#ifndef PHP_TANGO_H
#define PHP_TANGO_H

extern "C" {
#include "php.h"
}

#define PHP_TANGO_VERSION "0.1.0"
#define PHP_TANGO_EXTNAME "tango"

extern zend_module_entry tango_module_entry;
#define phpext_tango_ptr &tango_module_entry

#if defined(ZTS) && defined(COMPILE_DL_TANGO)
ZEND_TSRMLS_CACHE_EXTERN()
#endif

#endif /* PHP_TANGO_H */
