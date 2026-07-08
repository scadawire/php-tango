/*
 * tango.cpp - PHP binding for the TANGO Controls system (cppTango)
 *
 * Exposes a PHP class hierarchy analogous to PyTango's client API. The heart
 * of it is Tango\DeviceProxy, which connects to a running Tango device and
 * lets you read/write attributes and execute commands, exactly like
 * tango.DeviceProxy in PyTango.
 *
 *   $dev = new Tango\DeviceProxy("sys/tg_test/1");
 *   echo $dev->state();                       // "RUNNING"
 *   $dev->write_attribute("double_scalar", 3.14);
 *   $v = $dev->read_attribute("double_scalar");
 *   $out = $dev->command_inout("DevDouble", 2.5);
 *
 * Errors raised by cppTango (Tango::DevFailed) surface as PHP
 * Tango\DevFailed exceptions (which extend \Exception).
 */

#include <string>
#include <vector>

/* cppTango (pure C++) is included first, before the PHP/Zend C headers.
 * config.m4 sets up a private shim include dir so that <tango/...> always
 * resolves to the header root matching the library we link against, even if a
 * second (possibly broken) Tango install shadows it in /usr/local. */
#include <tango/tango.h>

#include "php_tango.h"

extern "C" {
#include "zend_exceptions.h"
#include "zend_interfaces.h"
#include "ext/standard/info.h"
}

/* ------------------------------------------------------------------------- */
/* Class entries                                                             */
/* ------------------------------------------------------------------------- */

static zend_class_entry *tango_deviceproxy_ce = nullptr;
static zend_class_entry *tango_devfailed_ce   = nullptr;
static zend_object_handlers tango_proxy_handlers;

/* PHP object wrapping a heap-allocated Tango::DeviceProxy. */
typedef struct _tango_proxy_obj {
    Tango::DeviceProxy *proxy;
    zend_object std;
} tango_proxy_obj;

static inline tango_proxy_obj *tango_proxy_from_obj(zend_object *obj)
{
    return (tango_proxy_obj *) ((char *) obj - XtOffsetOf(tango_proxy_obj, std));
}
#define Z_TANGO_P(zv) tango_proxy_from_obj(Z_OBJ_P(zv))

/* ------------------------------------------------------------------------- */
/* Error handling                                                            */
/* ------------------------------------------------------------------------- */

/* Turn a Tango::DevFailed error stack into a readable message and throw it
 * as a Tango\DevFailed PHP exception. */
static void throw_tango_devfailed(Tango::DevFailed &e)
{
    std::string msg;
    for (CORBA::ULong i = 0; i < e.errors.length(); ++i) {
        if (i) {
            msg += "\n";
        }
        const char *reason = (const char *) e.errors[i].reason;
        const char *desc   = (const char *) e.errors[i].desc;
        if (reason && *reason) {
            msg += reason;
            msg += ": ";
        }
        if (desc) {
            msg += desc;
        }
    }
    if (msg.empty()) {
        msg = "Unknown Tango::DevFailed error";
    }
    zend_throw_exception(tango_devfailed_ce, msg.c_str(), 0);
}

static const char *tango_state_name(Tango::DevState st)
{
    int i = (int) st;
    if (i >= 0 && i <= 13) {  /* ON .. UNKNOWN */
        return Tango::DevStateName[i];
    }
    return "UNKNOWN";
}

/* ------------------------------------------------------------------------- */
/* Value marshalling helpers: C++ -> PHP                                     */
/* ------------------------------------------------------------------------- */

template <typename V>
static void php_array_from_ints(zval *arr, const V &vec)
{
    for (const auto &x : vec) {
        add_next_index_long(arr, (zend_long) x);
    }
}

template <typename V>
static void php_array_from_doubles(zval *arr, const V &vec)
{
    for (const auto &x : vec) {
        add_next_index_double(arr, (double) x);
    }
}

static void php_array_from_strings(zval *arr, const std::vector<std::string> &vec)
{
    for (const auto &s : vec) {
        add_next_index_stringl(arr, s.c_str(), s.size());
    }
}

static void php_array_from_bools(zval *arr, const std::vector<bool> &vec)
{
    for (bool b : vec) {
        add_next_index_bool(arr, b ? 1 : 0);
    }
}

static void php_array_from_states(zval *arr, const std::vector<Tango::DevState> &vec)
{
    for (const auto &st : vec) {
        add_next_index_string(arr, tango_state_name(st));
    }
}

/* Convert the value held in a DeviceData (command result) to a PHP value. */
static void devicedata_to_zval(Tango::DeviceData &dd, zval *return_value)
{
    /* By default is_empty()/extraction throw API_EmptyDeviceData on empty
     * data (e.g. a DevVoid command result). Return PHP null instead. */
    dd.reset_exceptions(Tango::DeviceData::isempty_flag);
    if (dd.is_empty()) {
        RETVAL_NULL();
        return;
    }

    switch (dd.get_type()) {
        case Tango::DEV_VOID:
            RETVAL_NULL();
            return;
        case Tango::DEV_BOOLEAN: { bool v = false; dd >> v; RETVAL_BOOL(v); return; }
        case Tango::DEV_SHORT:   { short v = 0; dd >> v; RETVAL_LONG(v); return; }
        case Tango::DEV_USHORT:  { unsigned short v = 0; dd >> v; RETVAL_LONG(v); return; }
        case Tango::DEV_LONG:    { Tango::DevLong v = 0; dd >> v; RETVAL_LONG(v); return; }
        case Tango::DEV_ULONG:   { Tango::DevULong v = 0; dd >> v; RETVAL_LONG((zend_long) v); return; }
        case Tango::DEV_LONG64:  { Tango::DevLong64 v = 0; dd >> v; RETVAL_LONG((zend_long) v); return; }
        case Tango::DEV_ULONG64: { Tango::DevULong64 v = 0; dd >> v; RETVAL_LONG((zend_long) v); return; }
        case Tango::DEV_FLOAT:   { float v = 0; dd >> v; RETVAL_DOUBLE(v); return; }
        case Tango::DEV_DOUBLE:  { double v = 0; dd >> v; RETVAL_DOUBLE(v); return; }
        case Tango::DEV_STRING:  { std::string v; dd >> v; RETVAL_STRINGL(v.c_str(), v.size()); return; }
        case Tango::DEV_STATE:   { Tango::DevState v; dd >> v; RETVAL_STRING(tango_state_name(v)); return; }

        case Tango::DEVVAR_CHARARRAY:    { std::vector<unsigned char> v; dd >> v; array_init(return_value); php_array_from_ints(return_value, v); return; }
        case Tango::DEVVAR_SHORTARRAY:   { std::vector<short> v; dd >> v; array_init(return_value); php_array_from_ints(return_value, v); return; }
        case Tango::DEVVAR_USHORTARRAY:  { std::vector<unsigned short> v; dd >> v; array_init(return_value); php_array_from_ints(return_value, v); return; }
        case Tango::DEVVAR_LONGARRAY:    { std::vector<Tango::DevLong> v; dd >> v; array_init(return_value); php_array_from_ints(return_value, v); return; }
        case Tango::DEVVAR_ULONGARRAY:   { std::vector<Tango::DevULong> v; dd >> v; array_init(return_value); php_array_from_ints(return_value, v); return; }
        case Tango::DEVVAR_LONG64ARRAY:  { std::vector<Tango::DevLong64> v; dd >> v; array_init(return_value); php_array_from_ints(return_value, v); return; }
        case Tango::DEVVAR_ULONG64ARRAY: { std::vector<Tango::DevULong64> v; dd >> v; array_init(return_value); php_array_from_ints(return_value, v); return; }
        case Tango::DEVVAR_FLOATARRAY:   { std::vector<float> v; dd >> v; array_init(return_value); php_array_from_doubles(return_value, v); return; }
        case Tango::DEVVAR_DOUBLEARRAY:  { std::vector<double> v; dd >> v; array_init(return_value); php_array_from_doubles(return_value, v); return; }
        case Tango::DEVVAR_BOOLEANARRAY: { std::vector<bool> v; dd >> v; array_init(return_value); php_array_from_bools(return_value, v); return; }
        case Tango::DEVVAR_STRINGARRAY:  { std::vector<std::string> v; dd >> v; array_init(return_value); php_array_from_strings(return_value, v); return; }

        case Tango::DEVVAR_LONGSTRINGARRAY: {
            std::vector<Tango::DevLong> vl; std::vector<std::string> vs;
            dd.extract(vl, vs);
            array_init(return_value);
            zval lval, sval;
            array_init(&lval); php_array_from_ints(&lval, vl);
            array_init(&sval); php_array_from_strings(&sval, vs);
            add_assoc_zval(return_value, "lvalue", &lval);
            add_assoc_zval(return_value, "svalue", &sval);
            return;
        }
        case Tango::DEVVAR_DOUBLESTRINGARRAY: {
            std::vector<double> vd; std::vector<std::string> vs;
            dd.extract(vd, vs);
            array_init(return_value);
            zval dval, sval;
            array_init(&dval); php_array_from_doubles(&dval, vd);
            array_init(&sval); php_array_from_strings(&sval, vs);
            add_assoc_zval(return_value, "dvalue", &dval);
            add_assoc_zval(return_value, "svalue", &sval);
            return;
        }
        default:
            zend_throw_exception(tango_devfailed_ce,
                "Unsupported Tango data type in command result", 0);
            RETVAL_NULL();
            return;
    }
}

/* Convert the read value held in a DeviceAttribute to a PHP value. */
static void deviceattribute_to_zval(Tango::DeviceAttribute &da, zval *return_value)
{
    if (da.has_failed()) {
        Tango::DevFailed e(da.get_err_stack());
        throw_tango_devfailed(e);
        RETVAL_NULL();
        return;
    }
    da.reset_exceptions(Tango::DeviceAttribute::isempty_flag);
    if (da.is_empty()) {
        RETVAL_NULL();
        return;
    }

    Tango::AttrDataFormat fmt = da.get_data_format();
    bool is_arr = (fmt == Tango::SPECTRUM || fmt == Tango::IMAGE);
    int t = da.get_type();

    if (!is_arr) {
        switch (t) {
            case Tango::DEV_BOOLEAN: { bool v = false; da >> v; RETVAL_BOOL(v); return; }
            case Tango::DEV_SHORT:   { short v = 0; da >> v; RETVAL_LONG(v); return; }
            case Tango::DEV_USHORT:  { unsigned short v = 0; da >> v; RETVAL_LONG(v); return; }
            case Tango::DEV_UCHAR:   { unsigned char v = 0; da >> v; RETVAL_LONG(v); return; }
            case Tango::DEV_LONG:    { Tango::DevLong v = 0; da >> v; RETVAL_LONG(v); return; }
            case Tango::DEV_ULONG:   { Tango::DevULong v = 0; da >> v; RETVAL_LONG((zend_long) v); return; }
            case Tango::DEV_LONG64:  { Tango::DevLong64 v = 0; da >> v; RETVAL_LONG((zend_long) v); return; }
            case Tango::DEV_ULONG64: { Tango::DevULong64 v = 0; da >> v; RETVAL_LONG((zend_long) v); return; }
            case Tango::DEV_FLOAT:   { float v = 0; da >> v; RETVAL_DOUBLE(v); return; }
            case Tango::DEV_DOUBLE:  { double v = 0; da >> v; RETVAL_DOUBLE(v); return; }
            case Tango::DEV_STRING:  { std::string v; da >> v; RETVAL_STRINGL(v.c_str(), v.size()); return; }
            case Tango::DEV_STATE:   { Tango::DevState v; da >> v; RETVAL_STRING(tango_state_name(v)); return; }
            case Tango::DEV_ENUM:    { short v = 0; da >> v; RETVAL_LONG(v); return; }
            default:
                zend_throw_exception(tango_devfailed_ce,
                    "Unsupported Tango scalar attribute data type", 0);
                RETVAL_NULL();
                return;
        }
    }

    /* spectrum / image -> flat PHP array (row-major for images) */
    array_init(return_value);
    switch (t) {
        case Tango::DEV_BOOLEAN: { std::vector<bool> v; da.extract_read(v); php_array_from_bools(return_value, v); return; }
        case Tango::DEV_SHORT:   { std::vector<short> v; da.extract_read(v); php_array_from_ints(return_value, v); return; }
        case Tango::DEV_USHORT:  { std::vector<unsigned short> v; da.extract_read(v); php_array_from_ints(return_value, v); return; }
        case Tango::DEV_UCHAR:   { std::vector<unsigned char> v; da.extract_read(v); php_array_from_ints(return_value, v); return; }
        case Tango::DEV_LONG:    { std::vector<Tango::DevLong> v; da.extract_read(v); php_array_from_ints(return_value, v); return; }
        case Tango::DEV_ULONG:   { std::vector<Tango::DevULong> v; da.extract_read(v); php_array_from_ints(return_value, v); return; }
        case Tango::DEV_LONG64:  { std::vector<Tango::DevLong64> v; da.extract_read(v); php_array_from_ints(return_value, v); return; }
        case Tango::DEV_ULONG64: { std::vector<Tango::DevULong64> v; da.extract_read(v); php_array_from_ints(return_value, v); return; }
        case Tango::DEV_FLOAT:   { std::vector<float> v; da.extract_read(v); php_array_from_doubles(return_value, v); return; }
        case Tango::DEV_DOUBLE:  { std::vector<double> v; da.extract_read(v); php_array_from_doubles(return_value, v); return; }
        case Tango::DEV_STRING:  { std::vector<std::string> v; da.extract_read(v); php_array_from_strings(return_value, v); return; }
        case Tango::DEV_STATE:   { std::vector<Tango::DevState> v; da.extract_read(v); php_array_from_states(return_value, v); return; }
        default:
            zend_throw_exception(tango_devfailed_ce,
                "Unsupported Tango array attribute data type", 0);
            return;
    }
}

/* ------------------------------------------------------------------------- */
/* Value marshalling helpers: PHP -> C++                                     */
/* ------------------------------------------------------------------------- */

/* Insert a scalar PHP value into a DeviceData, coercing to the Tango type
 * expected by the command (tango_type is a CmdArgType). */
static bool devicedata_insert_scalar(Tango::DeviceData &dd, long tango_type, zval *v)
{
    switch (tango_type) {
        case Tango::DEV_VOID:    return true;
        case Tango::DEV_BOOLEAN: { dd << (bool) zend_is_true(v); return true; }
        case Tango::DEV_SHORT:   { dd << (short) zval_get_long(v); return true; }
        case Tango::DEV_USHORT:  { dd << (unsigned short) zval_get_long(v); return true; }
        case Tango::DEV_LONG:    { Tango::DevLong x = (Tango::DevLong) zval_get_long(v); dd << x; return true; }
        case Tango::DEV_ULONG:   { Tango::DevULong x = (Tango::DevULong) zval_get_long(v); dd << x; return true; }
        case Tango::DEV_LONG64:  { Tango::DevLong64 x = (Tango::DevLong64) zval_get_long(v); dd << x; return true; }
        case Tango::DEV_ULONG64: { Tango::DevULong64 x = (Tango::DevULong64) zval_get_long(v); dd << x; return true; }
        case Tango::DEV_FLOAT:   { dd << (float) zval_get_double(v); return true; }
        case Tango::DEV_DOUBLE:  { dd << (double) zval_get_double(v); return true; }
        case Tango::DEV_STRING: {
            zend_string *s = zval_get_string(v);
            std::string str(ZSTR_VAL(s), ZSTR_LEN(s));
            dd << str;
            zend_string_release(s);
            return true;
        }
        case Tango::DEV_STATE:   { Tango::DevState st = (Tango::DevState) zval_get_long(v); dd << st; return true; }
        default:
            return false;
    }
}

/* Insert a PHP array into a DeviceData as a Tango array (DEVVAR_*ARRAY). */
static bool devicedata_insert_array(Tango::DeviceData &dd, long tango_type, zval *arr)
{
    HashTable *ht = Z_ARRVAL_P(arr);
    zval *entry;

    switch (tango_type) {
        case Tango::DEVVAR_CHARARRAY: {
            std::vector<unsigned char> v;
            ZEND_HASH_FOREACH_VAL(ht, entry) { v.push_back((unsigned char) zval_get_long(entry)); } ZEND_HASH_FOREACH_END();
            dd << v; return true;
        }
        case Tango::DEVVAR_SHORTARRAY: {
            std::vector<short> v;
            ZEND_HASH_FOREACH_VAL(ht, entry) { v.push_back((short) zval_get_long(entry)); } ZEND_HASH_FOREACH_END();
            dd << v; return true;
        }
        case Tango::DEVVAR_USHORTARRAY: {
            std::vector<unsigned short> v;
            ZEND_HASH_FOREACH_VAL(ht, entry) { v.push_back((unsigned short) zval_get_long(entry)); } ZEND_HASH_FOREACH_END();
            dd << v; return true;
        }
        case Tango::DEVVAR_LONGARRAY: {
            std::vector<Tango::DevLong> v;
            ZEND_HASH_FOREACH_VAL(ht, entry) { v.push_back((Tango::DevLong) zval_get_long(entry)); } ZEND_HASH_FOREACH_END();
            dd << v; return true;
        }
        case Tango::DEVVAR_ULONGARRAY: {
            std::vector<Tango::DevULong> v;
            ZEND_HASH_FOREACH_VAL(ht, entry) { v.push_back((Tango::DevULong) zval_get_long(entry)); } ZEND_HASH_FOREACH_END();
            dd << v; return true;
        }
        case Tango::DEVVAR_LONG64ARRAY: {
            std::vector<Tango::DevLong64> v;
            ZEND_HASH_FOREACH_VAL(ht, entry) { v.push_back((Tango::DevLong64) zval_get_long(entry)); } ZEND_HASH_FOREACH_END();
            dd << v; return true;
        }
        case Tango::DEVVAR_ULONG64ARRAY: {
            std::vector<Tango::DevULong64> v;
            ZEND_HASH_FOREACH_VAL(ht, entry) { v.push_back((Tango::DevULong64) zval_get_long(entry)); } ZEND_HASH_FOREACH_END();
            dd << v; return true;
        }
        case Tango::DEVVAR_FLOATARRAY: {
            std::vector<float> v;
            ZEND_HASH_FOREACH_VAL(ht, entry) { v.push_back((float) zval_get_double(entry)); } ZEND_HASH_FOREACH_END();
            dd << v; return true;
        }
        case Tango::DEVVAR_DOUBLEARRAY: {
            std::vector<double> v;
            ZEND_HASH_FOREACH_VAL(ht, entry) { v.push_back((double) zval_get_double(entry)); } ZEND_HASH_FOREACH_END();
            dd << v; return true;
        }
        case Tango::DEVVAR_STRINGARRAY: {
            std::vector<std::string> v;
            ZEND_HASH_FOREACH_VAL(ht, entry) {
                zend_string *s = zval_get_string(entry);
                v.emplace_back(ZSTR_VAL(s), ZSTR_LEN(s));
                zend_string_release(s);
            } ZEND_HASH_FOREACH_END();
            dd << v; return true;
        }
        default:
            return false;
    }
}

/* ------------------------------------------------------------------------- */
/* Object create / free handlers                                             */
/* ------------------------------------------------------------------------- */

static zend_object *tango_proxy_create(zend_class_entry *ce)
{
    tango_proxy_obj *intern =
        (tango_proxy_obj *) zend_object_alloc(sizeof(tango_proxy_obj), ce);

    zend_object_std_init(&intern->std, ce);
    object_properties_init(&intern->std, ce);
    intern->std.handlers = &tango_proxy_handlers;
    intern->proxy = nullptr;
    return &intern->std;
}

static void tango_proxy_free(zend_object *object)
{
    tango_proxy_obj *intern = tango_proxy_from_obj(object);
    if (intern->proxy) {
        delete intern->proxy;
        intern->proxy = nullptr;
    }
    zend_object_std_dtor(&intern->std);
}

/* Fetch a connected proxy or throw. Returns nullptr (and sets an exception)
 * when the object was never successfully constructed. */
static Tango::DeviceProxy *tango_get_proxy(zval *zthis)
{
    tango_proxy_obj *obj = Z_TANGO_P(zthis);
    if (!obj->proxy) {
        zend_throw_exception(tango_devfailed_ce,
            "DeviceProxy is not connected (constructor failed)", 0);
        return nullptr;
    }
    return obj->proxy;
}

/* ------------------------------------------------------------------------- */
/* Argument info                                                             */
/* ------------------------------------------------------------------------- */

ZEND_BEGIN_ARG_INFO_EX(arginfo_ctor, 0, 0, 1)
    ZEND_ARG_TYPE_INFO(0, name, IS_STRING, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(arginfo_none, 0, 0, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(arginfo_name_only, 0, 0, 1)
    ZEND_ARG_TYPE_INFO(0, name, IS_STRING, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(arginfo_write_attr, 0, 0, 2)
    ZEND_ARG_TYPE_INFO(0, name, IS_STRING, 0)
    ZEND_ARG_INFO(0, value)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(arginfo_command, 0, 0, 1)
    ZEND_ARG_TYPE_INFO(0, command, IS_STRING, 0)
    ZEND_ARG_INFO(0, argument)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(arginfo_set_timeout, 0, 0, 1)
    ZEND_ARG_TYPE_INFO(0, millis, IS_LONG, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(arginfo_get, 0, 0, 1)
    ZEND_ARG_TYPE_INFO(0, name, IS_STRING, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(arginfo_set, 0, 0, 2)
    ZEND_ARG_TYPE_INFO(0, name, IS_STRING, 0)
    ZEND_ARG_INFO(0, value)
ZEND_END_ARG_INFO()

/* ------------------------------------------------------------------------- */
/* Tango\DeviceProxy methods                                                 */
/* ------------------------------------------------------------------------- */

PHP_METHOD(DeviceProxy, __construct)
{
    char *name;
    size_t name_len;

    ZEND_PARSE_PARAMETERS_START(1, 1)
        Z_PARAM_STRING(name, name_len)
    ZEND_PARSE_PARAMETERS_END();

    tango_proxy_obj *obj = Z_TANGO_P(getThis());
    try {
        std::string dev_name(name, name_len);
        obj->proxy = new Tango::DeviceProxy(dev_name);
    } catch (Tango::DevFailed &e) {
        obj->proxy = nullptr;
        throw_tango_devfailed(e);
        RETURN_THROWS();
    }
}

PHP_METHOD(DeviceProxy, name)
{
    if (zend_parse_parameters_none() == FAILURE) RETURN_THROWS();
    Tango::DeviceProxy *dev = tango_get_proxy(getThis());
    if (!dev) RETURN_THROWS();
    try {
        std::string s = dev->name();
        RETURN_STRINGL(s.c_str(), s.size());
    } catch (Tango::DevFailed &e) { throw_tango_devfailed(e); RETURN_THROWS(); }
}

PHP_METHOD(DeviceProxy, status)
{
    if (zend_parse_parameters_none() == FAILURE) RETURN_THROWS();
    Tango::DeviceProxy *dev = tango_get_proxy(getThis());
    if (!dev) RETURN_THROWS();
    try {
        std::string s = dev->status();
        RETURN_STRINGL(s.c_str(), s.size());
    } catch (Tango::DevFailed &e) { throw_tango_devfailed(e); RETURN_THROWS(); }
}

PHP_METHOD(DeviceProxy, state)
{
    if (zend_parse_parameters_none() == FAILURE) RETURN_THROWS();
    Tango::DeviceProxy *dev = tango_get_proxy(getThis());
    if (!dev) RETURN_THROWS();
    try {
        Tango::DevState st = dev->state();
        RETURN_STRING(tango_state_name(st));
    } catch (Tango::DevFailed &e) { throw_tango_devfailed(e); RETURN_THROWS(); }
}

PHP_METHOD(DeviceProxy, ping)
{
    if (zend_parse_parameters_none() == FAILURE) RETURN_THROWS();
    Tango::DeviceProxy *dev = tango_get_proxy(getThis());
    if (!dev) RETURN_THROWS();
    try {
        RETURN_LONG(dev->ping());  /* round-trip time in microseconds */
    } catch (Tango::DevFailed &e) { throw_tango_devfailed(e); RETURN_THROWS(); }
}

PHP_METHOD(DeviceProxy, get_attribute_list)
{
    if (zend_parse_parameters_none() == FAILURE) RETURN_THROWS();
    Tango::DeviceProxy *dev = tango_get_proxy(getThis());
    if (!dev) RETURN_THROWS();
    try {
        std::vector<std::string> *lst = dev->get_attribute_list();
        array_init(return_value);
        if (lst) {
            php_array_from_strings(return_value, *lst);
            delete lst;
        }
    } catch (Tango::DevFailed &e) { throw_tango_devfailed(e); RETURN_THROWS(); }
}

PHP_METHOD(DeviceProxy, get_command_list)
{
    if (zend_parse_parameters_none() == FAILURE) RETURN_THROWS();
    Tango::DeviceProxy *dev = tango_get_proxy(getThis());
    if (!dev) RETURN_THROWS();
    try {
        std::vector<std::string> *lst = dev->get_command_list();
        array_init(return_value);
        if (lst) {
            php_array_from_strings(return_value, *lst);
            delete lst;
        }
    } catch (Tango::DevFailed &e) { throw_tango_devfailed(e); RETURN_THROWS(); }
}

PHP_METHOD(DeviceProxy, read_attribute)
{
    char *name;
    size_t name_len;
    ZEND_PARSE_PARAMETERS_START(1, 1)
        Z_PARAM_STRING(name, name_len)
    ZEND_PARSE_PARAMETERS_END();

    Tango::DeviceProxy *dev = tango_get_proxy(getThis());
    if (!dev) RETURN_THROWS();
    try {
        std::string attr_name(name, name_len);
        Tango::DeviceAttribute da = dev->read_attribute(attr_name);
        deviceattribute_to_zval(da, return_value);
    } catch (Tango::DevFailed &e) { throw_tango_devfailed(e); RETURN_THROWS(); }
}

PHP_METHOD(DeviceProxy, write_attribute)
{
    char *name;
    size_t name_len;
    zval *value;
    ZEND_PARSE_PARAMETERS_START(2, 2)
        Z_PARAM_STRING(name, name_len)
        Z_PARAM_ZVAL(value)
    ZEND_PARSE_PARAMETERS_END();

    Tango::DeviceProxy *dev = tango_get_proxy(getThis());
    if (!dev) RETURN_THROWS();

    std::string attr_name(name, name_len);
    try {
        Tango::AttributeInfoEx cfg = dev->get_attribute_config(attr_name);
        long dtype = cfg.data_type;

        Tango::DeviceAttribute da;
        da.set_name(attr_name.c_str());

        if (Z_TYPE_P(value) == IS_ARRAY) {
            /* spectrum write */
            HashTable *ht = Z_ARRVAL_P(value);
            zval *entry;
            int dim_x = (int) zend_hash_num_elements(ht);
            switch (dtype) {
                case Tango::DEV_DOUBLE: case Tango::DEV_FLOAT: {
                    std::vector<double> v;
                    ZEND_HASH_FOREACH_VAL(ht, entry) { v.push_back(zval_get_double(entry)); } ZEND_HASH_FOREACH_END();
                    da.insert(v, dim_x, 0);
                    break;
                }
                case Tango::DEV_STRING: {
                    std::vector<std::string> v;
                    ZEND_HASH_FOREACH_VAL(ht, entry) {
                        zend_string *s = zval_get_string(entry);
                        v.emplace_back(ZSTR_VAL(s), ZSTR_LEN(s));
                        zend_string_release(s);
                    } ZEND_HASH_FOREACH_END();
                    da.insert(v, dim_x, 0);
                    break;
                }
                default: {
                    std::vector<Tango::DevLong> v;
                    ZEND_HASH_FOREACH_VAL(ht, entry) { v.push_back((Tango::DevLong) zval_get_long(entry)); } ZEND_HASH_FOREACH_END();
                    da.insert(v, dim_x, 0);
                    break;
                }
            }
        } else {
            /* scalar write, coerced to the attribute's data type */
            switch (dtype) {
                case Tango::DEV_BOOLEAN: { da << (bool) zend_is_true(value); break; }
                case Tango::DEV_SHORT:   { da << (short) zval_get_long(value); break; }
                case Tango::DEV_USHORT:  { da << (unsigned short) zval_get_long(value); break; }
                case Tango::DEV_UCHAR:   { da << (unsigned char) zval_get_long(value); break; }
                case Tango::DEV_LONG:    { Tango::DevLong x = (Tango::DevLong) zval_get_long(value); da << x; break; }
                case Tango::DEV_ULONG:   { Tango::DevULong x = (Tango::DevULong) zval_get_long(value); da << x; break; }
                case Tango::DEV_LONG64:  { Tango::DevLong64 x = (Tango::DevLong64) zval_get_long(value); da << x; break; }
                case Tango::DEV_ULONG64: { Tango::DevULong64 x = (Tango::DevULong64) zval_get_long(value); da << x; break; }
                case Tango::DEV_FLOAT:   { da << (float) zval_get_double(value); break; }
                case Tango::DEV_DOUBLE:  { da << (double) zval_get_double(value); break; }
                case Tango::DEV_STRING: {
                    zend_string *s = zval_get_string(value);
                    std::string str(ZSTR_VAL(s), ZSTR_LEN(s));
                    da << str;
                    zend_string_release(s);
                    break;
                }
                case Tango::DEV_STATE:   { Tango::DevState st = (Tango::DevState) zval_get_long(value); da << st; break; }
                default:
                    zend_throw_exception(tango_devfailed_ce,
                        "Unsupported attribute data type for write", 0);
                    RETURN_THROWS();
            }
        }

        dev->write_attribute(da);
    } catch (Tango::DevFailed &e) { throw_tango_devfailed(e); RETURN_THROWS(); }
}

PHP_METHOD(DeviceProxy, command_inout)
{
    char *cmd;
    size_t cmd_len;
    zval *arg = nullptr;
    ZEND_PARSE_PARAMETERS_START(1, 2)
        Z_PARAM_STRING(cmd, cmd_len)
        Z_PARAM_OPTIONAL
        Z_PARAM_ZVAL(arg)
    ZEND_PARSE_PARAMETERS_END();

    Tango::DeviceProxy *dev = tango_get_proxy(getThis());
    if (!dev) RETURN_THROWS();

    std::string cmd_name(cmd, cmd_len);
    try {
        Tango::DeviceData dout;

        if (arg == nullptr || Z_TYPE_P(arg) == IS_NULL) {
            dout = dev->command_inout(cmd_name);
        } else {
            Tango::CommandInfo ci = dev->command_query(cmd_name);
            if (ci.in_type == Tango::DEV_VOID) {
                dout = dev->command_inout(cmd_name);
            } else {
                Tango::DeviceData din;
                bool ok;
                if (Z_TYPE_P(arg) == IS_ARRAY) {
                    ok = devicedata_insert_array(din, ci.in_type, arg);
                } else {
                    ok = devicedata_insert_scalar(din, ci.in_type, arg);
                }
                if (!ok) {
                    zend_throw_exception(tango_devfailed_ce,
                        "Unsupported argument type for this command", 0);
                    RETURN_THROWS();
                }
                dout = dev->command_inout(cmd_name, din);
            }
        }
        devicedata_to_zval(dout, return_value);
    } catch (Tango::DevFailed &e) { throw_tango_devfailed(e); RETURN_THROWS(); }
}

PHP_METHOD(DeviceProxy, set_timeout_millis)
{
    zend_long millis;
    ZEND_PARSE_PARAMETERS_START(1, 1)
        Z_PARAM_LONG(millis)
    ZEND_PARSE_PARAMETERS_END();

    Tango::DeviceProxy *dev = tango_get_proxy(getThis());
    if (!dev) RETURN_THROWS();
    try {
        dev->set_timeout_millis((int) millis);
    } catch (Tango::DevFailed &e) { throw_tango_devfailed(e); RETURN_THROWS(); }
}

PHP_METHOD(DeviceProxy, get_timeout_millis)
{
    if (zend_parse_parameters_none() == FAILURE) RETURN_THROWS();
    Tango::DeviceProxy *dev = tango_get_proxy(getThis());
    if (!dev) RETURN_THROWS();
    try {
        RETURN_LONG(dev->get_timeout_millis());
    } catch (Tango::DevFailed &e) { throw_tango_devfailed(e); RETURN_THROWS(); }
}

/* PyTango-style attribute access: $dev->voltage and $dev->voltage = 5 */
PHP_METHOD(DeviceProxy, __get)
{
    char *name;
    size_t name_len;
    ZEND_PARSE_PARAMETERS_START(1, 1)
        Z_PARAM_STRING(name, name_len)
    ZEND_PARSE_PARAMETERS_END();

    Tango::DeviceProxy *dev = tango_get_proxy(getThis());
    if (!dev) RETURN_THROWS();
    try {
        std::string attr_name(name, name_len);
        Tango::DeviceAttribute da = dev->read_attribute(attr_name);
        deviceattribute_to_zval(da, return_value);
    } catch (Tango::DevFailed &e) { throw_tango_devfailed(e); RETURN_THROWS(); }
}

PHP_METHOD(DeviceProxy, __set)
{
    char *name;
    size_t name_len;
    zval *value;
    ZEND_PARSE_PARAMETERS_START(2, 2)
        Z_PARAM_STRING(name, name_len)
        Z_PARAM_ZVAL(value)
    ZEND_PARSE_PARAMETERS_END();

    /* Delegate to write_attribute() so both entry points share logic. */
    zval fn, args[2], retval;
    ZVAL_STRING(&fn, "write_attribute");
    ZVAL_STRINGL(&args[0], name, name_len);
    ZVAL_COPY(&args[1], value);
    call_user_function(NULL, getThis(), &fn, &retval, 2, args);
    zval_ptr_dtor(&fn);
    zval_ptr_dtor(&args[0]);
    zval_ptr_dtor(&args[1]);
    zval_ptr_dtor(&retval);
}

static const zend_function_entry tango_deviceproxy_methods[] = {
    PHP_ME(DeviceProxy, __construct,        arginfo_ctor,        ZEND_ACC_PUBLIC | ZEND_ACC_CTOR)
    PHP_ME(DeviceProxy, name,               arginfo_none,        ZEND_ACC_PUBLIC)
    PHP_ME(DeviceProxy, status,             arginfo_none,        ZEND_ACC_PUBLIC)
    PHP_ME(DeviceProxy, state,              arginfo_none,        ZEND_ACC_PUBLIC)
    PHP_ME(DeviceProxy, ping,               arginfo_none,        ZEND_ACC_PUBLIC)
    PHP_ME(DeviceProxy, get_attribute_list, arginfo_none,        ZEND_ACC_PUBLIC)
    PHP_ME(DeviceProxy, get_command_list,   arginfo_none,        ZEND_ACC_PUBLIC)
    PHP_ME(DeviceProxy, read_attribute,     arginfo_name_only,   ZEND_ACC_PUBLIC)
    PHP_ME(DeviceProxy, write_attribute,    arginfo_write_attr,  ZEND_ACC_PUBLIC)
    PHP_ME(DeviceProxy, command_inout,      arginfo_command,     ZEND_ACC_PUBLIC)
    PHP_ME(DeviceProxy, set_timeout_millis, arginfo_set_timeout, ZEND_ACC_PUBLIC)
    PHP_ME(DeviceProxy, get_timeout_millis, arginfo_none,        ZEND_ACC_PUBLIC)
    PHP_ME(DeviceProxy, __get,              arginfo_get,         ZEND_ACC_PUBLIC)
    PHP_ME(DeviceProxy, __set,              arginfo_set,         ZEND_ACC_PUBLIC)
    PHP_FE_END
};

/* ========================================================================= */
/* SERVER SIDE                                                               */
/*                                                                           */
/* The PHP analogue of PyTango's device-server API. A PHP author subclasses  */
/* Tango\Server\Device, describes attributes/commands on a Tango\Server\     */
/* Server object, and calls run(). Underneath, generic C++ bridge classes    */
/* (PhpDeviceClass / PhpDevice / PhpCommand / PhpAttr) forward every Tango    */
/* request to the corresponding PHP method.                                  */
/*                                                                           */
/* NOTE on threading: cppTango dispatches requests on omniORB worker         */
/* threads. We set the BY_PROCESS serialisation model so those upcalls are   */
/* serialised process-wide -- never re-entrant -- which is what makes it     */
/* safe to enter the (non-thread-safe) PHP interpreter from them.            */
/* ========================================================================= */

/* ----- registration specs, filled in from PHP ----- */
struct AttrSpec {
    std::string name;
    long data_type;                 /* CmdArgType */
    Tango::AttrWriteType w_type;
    Tango::AttrDataFormat format;    /* SCALAR or SPECTRUM */
    long max_x;
};
struct CmdSpec {
    std::string name;
    long in_type;
    long out_type;
};
struct ServerReg {
    std::string class_name;
    zend_class_entry *php_ce;        /* the user's PHP device class */
    std::vector<AttrSpec> attrs;
    std::vector<CmdSpec> cmds;
};

/* The single active server registration (a process hosts one PHP server). */
static ServerReg *g_server_reg = nullptr;

static zend_class_entry *tango_server_ce = nullptr;
static zend_class_entry *tango_serverdevice_ce = nullptr;
static zend_object_handlers tango_device_handlers;

/* Internal object backing a Tango\Server\Device PHP instance. Holds a back
 * pointer to the C++ device so set_state()/get_name() etc. can reach it. */
typedef struct _tango_device_obj {
    Tango::DeviceImpl *dev;          /* owned by Tango, not freed here */
    zend_object std;
} tango_device_obj;

static inline tango_device_obj *tango_device_from_obj(zend_object *obj)
{
    return (tango_device_obj *) ((char *) obj - XtOffsetOf(tango_device_obj, std));
}

static Tango::DevState state_from_name(const char *name)
{
    for (int i = 0; i <= 13; ++i) {
        if (strcasecmp(name, Tango::DevStateName[i]) == 0) {
            return (Tango::DevState) i;
        }
    }
    return Tango::UNKNOWN;
}

/* If a PHP handler left an exception pending, convert it to a Tango::DevFailed
 * so the client receives a proper error (and clear it on the PHP side). */
static void rethrow_php_exception_as_devfailed(const char *origin)
{
    if (!EG(exception)) {
        return;
    }
    zend_object *ex = EG(exception);
    zval rv;
    zval *msg = zend_read_property(ex->ce, ex, "message", sizeof("message") - 1, 1, &rv);
    std::string desc = "PHP exception in device server";
    if (msg && Z_TYPE_P(msg) == IS_STRING) {
        desc = ZSTR_VAL(Z_STR_P(msg));
    }
    zend_clear_exception();
    Tango::Except::throw_exception(std::string("PHP_Exception"), desc, std::string(origin));
}

/* Call $obj->$method(...args), returning success. On a PHP exception, converts
 * it into a Tango::DevFailed (thrown). */
static bool php_call_method(zend_object *obj, const char *method,
                            uint32_t argc, zval *args, zval *retval,
                            const char *origin)
{
    zval zobj, fn;
    ZVAL_OBJ(&zobj, obj);
    ZVAL_STRING(&fn, method);

    ZVAL_UNDEF(retval);
    int rc = call_user_function(NULL, &zobj, &fn, retval, argc, args);
    zval_ptr_dtor(&fn);

    if (rc == FAILURE) {
        Tango::Except::throw_exception(std::string("PHP_CallFailed"),
            std::string("Could not call method ") + method,
            std::string(origin));
    }
    rethrow_php_exception_as_devfailed(origin);
    return true;
}

static bool php_method_exists(zend_class_entry *ce, const char *lc_name, size_t len)
{
    return zend_hash_str_find_ptr(&ce->function_table, lc_name, len) != nullptr;
}

/* ----- CORBA::Any (command payload) <-> zval ----- */

static void any_to_zval(const CORBA::Any &in, long type, zval *out)
{
    switch (type) {
        case Tango::DEV_VOID: ZVAL_NULL(out); return;
        case Tango::DEV_BOOLEAN: { CORBA::Boolean v = 0; in >>= CORBA::Any::to_boolean(v); ZVAL_BOOL(out, v); return; }
        case Tango::DEV_SHORT:   { CORBA::Short v = 0; in >>= v; ZVAL_LONG(out, v); return; }
        case Tango::DEV_USHORT:  { CORBA::UShort v = 0; in >>= v; ZVAL_LONG(out, v); return; }
        case Tango::DEV_LONG:    { Tango::DevLong v = 0; in >>= v; ZVAL_LONG(out, v); return; }
        case Tango::DEV_ULONG:   { Tango::DevULong v = 0; in >>= v; ZVAL_LONG(out, (zend_long) v); return; }
        case Tango::DEV_LONG64:  { Tango::DevLong64 v = 0; in >>= v; ZVAL_LONG(out, (zend_long) v); return; }
        case Tango::DEV_ULONG64: { Tango::DevULong64 v = 0; in >>= v; ZVAL_LONG(out, (zend_long) v); return; }
        case Tango::DEV_FLOAT:   { Tango::DevFloat v = 0; in >>= v; ZVAL_DOUBLE(out, v); return; }
        case Tango::DEV_DOUBLE:  { Tango::DevDouble v = 0; in >>= v; ZVAL_DOUBLE(out, v); return; }
        case Tango::DEV_STRING:  { const char *s = nullptr; in >>= s; ZVAL_STRING(out, s ? s : ""); return; }
        case Tango::DEVVAR_DOUBLEARRAY: {
            const Tango::DevVarDoubleArray *p = nullptr; in >>= p;
            array_init(out);
            if (p) for (CORBA::ULong i = 0; i < p->length(); ++i) add_next_index_double(out, (*p)[i]);
            return;
        }
        case Tango::DEVVAR_LONGARRAY: {
            const Tango::DevVarLongArray *p = nullptr; in >>= p;
            array_init(out);
            if (p) for (CORBA::ULong i = 0; i < p->length(); ++i) add_next_index_long(out, (*p)[i]);
            return;
        }
        case Tango::DEVVAR_STRINGARRAY: {
            const Tango::DevVarStringArray *p = nullptr; in >>= p;
            array_init(out);
            if (p) for (CORBA::ULong i = 0; i < p->length(); ++i) add_next_index_string(out, (const char *) (*p)[i]);
            return;
        }
        default:
            ZVAL_NULL(out);
            return;
    }
}

static CORBA::Any *zval_to_any(zval *v, long type, const char *origin)
{
    CORBA::Any *any = new CORBA::Any();
    switch (type) {
        case Tango::DEV_VOID: break;
        case Tango::DEV_BOOLEAN: (*any) <<= CORBA::Any::from_boolean(zend_is_true(v) ? 1 : 0); break;
        case Tango::DEV_SHORT:   (*any) <<= (CORBA::Short) zval_get_long(v); break;
        case Tango::DEV_USHORT:  (*any) <<= (CORBA::UShort) zval_get_long(v); break;
        case Tango::DEV_LONG:    (*any) <<= (Tango::DevLong) zval_get_long(v); break;
        case Tango::DEV_ULONG:   (*any) <<= (Tango::DevULong) zval_get_long(v); break;
        case Tango::DEV_LONG64:  (*any) <<= (Tango::DevLong64) zval_get_long(v); break;
        case Tango::DEV_ULONG64: (*any) <<= (Tango::DevULong64) zval_get_long(v); break;
        case Tango::DEV_FLOAT:   (*any) <<= (Tango::DevFloat) zval_get_double(v); break;
        case Tango::DEV_DOUBLE:  (*any) <<= (Tango::DevDouble) zval_get_double(v); break;
        case Tango::DEV_STRING: {
            zend_string *s = zval_get_string(v);
            (*any) <<= (const char *) ZSTR_VAL(s);
            zend_string_release(s);
            break;
        }
        case Tango::DEVVAR_DOUBLEARRAY: {
            Tango::DevVarDoubleArray *arr = new Tango::DevVarDoubleArray();
            if (Z_TYPE_P(v) == IS_ARRAY) {
                HashTable *ht = Z_ARRVAL_P(v); zval *e; CORBA::ULong i = 0;
                arr->length(zend_hash_num_elements(ht));
                ZEND_HASH_FOREACH_VAL(ht, e) { (*arr)[i++] = zval_get_double(e); } ZEND_HASH_FOREACH_END();
            }
            (*any) <<= arr;
            break;
        }
        case Tango::DEVVAR_LONGARRAY: {
            Tango::DevVarLongArray *arr = new Tango::DevVarLongArray();
            if (Z_TYPE_P(v) == IS_ARRAY) {
                HashTable *ht = Z_ARRVAL_P(v); zval *e; CORBA::ULong i = 0;
                arr->length(zend_hash_num_elements(ht));
                ZEND_HASH_FOREACH_VAL(ht, e) { (*arr)[i++] = (Tango::DevLong) zval_get_long(e); } ZEND_HASH_FOREACH_END();
            }
            (*any) <<= arr;
            break;
        }
        case Tango::DEVVAR_STRINGARRAY: {
            Tango::DevVarStringArray *arr = new Tango::DevVarStringArray();
            if (Z_TYPE_P(v) == IS_ARRAY) {
                HashTable *ht = Z_ARRVAL_P(v); zval *e; CORBA::ULong i = 0;
                arr->length(zend_hash_num_elements(ht));
                ZEND_HASH_FOREACH_VAL(ht, e) {
                    zend_string *s = zval_get_string(e);
                    (*arr)[i++] = CORBA::string_dup(ZSTR_VAL(s));
                    zend_string_release(s);
                } ZEND_HASH_FOREACH_END();
            }
            (*any) <<= arr;
            break;
        }
        default:
            delete any;
            Tango::Except::throw_exception(std::string("PHP_UnsupportedType"),
                std::string("Unsupported command output type"), std::string(origin));
    }
    return any;
}

/* ----- attribute value: zval -> Attribute (read) ----- */

template <typename T>
static void set_attr_scalar(Tango::Attribute &att, T val)
{
    T *p = new T[1];
    p[0] = val;
    att.set_value(p, 1, 0, true);
}

template <typename T, typename ConvFn>
static void set_attr_spectrum(Tango::Attribute &att, zval *arr, ConvFn conv)
{
    HashTable *ht = Z_ARRVAL_P(arr);
    long n = (long) zend_hash_num_elements(ht);
    T *p = new T[n > 0 ? n : 1];
    zval *e; long i = 0;
    ZEND_HASH_FOREACH_VAL(ht, e) { p[i++] = conv(e); } ZEND_HASH_FOREACH_END();
    att.set_value(p, n, 0, true);
}

static void zval_into_attribute(Tango::Attribute &att, const AttrSpec &spec, zval *v)
{
    if (spec.format == Tango::SPECTRUM) {
        if (Z_TYPE_P(v) != IS_ARRAY) {
            Tango::Except::throw_exception(std::string("PHP_WrongType"),
                std::string("Spectrum attribute handler must return an array: ") + spec.name,
                std::string("PhpAttr::read"));
        }
        switch (spec.data_type) {
            case Tango::DEV_DOUBLE:
                set_attr_spectrum<Tango::DevDouble>(att, v, [](zval *e){ return (Tango::DevDouble) zval_get_double(e); }); return;
            case Tango::DEV_LONG:
                set_attr_spectrum<Tango::DevLong>(att, v, [](zval *e){ return (Tango::DevLong) zval_get_long(e); }); return;
            default:
                Tango::Except::throw_exception(std::string("PHP_UnsupportedType"),
                    std::string("Unsupported spectrum data type for ") + spec.name,
                    std::string("PhpAttr::read"));
        }
    }

    switch (spec.data_type) {
        case Tango::DEV_BOOLEAN: set_attr_scalar<Tango::DevBoolean>(att, zend_is_true(v) ? 1 : 0); return;
        case Tango::DEV_SHORT:   set_attr_scalar<Tango::DevShort>(att, (Tango::DevShort) zval_get_long(v)); return;
        case Tango::DEV_USHORT:  set_attr_scalar<Tango::DevUShort>(att, (Tango::DevUShort) zval_get_long(v)); return;
        case Tango::DEV_LONG:    set_attr_scalar<Tango::DevLong>(att, (Tango::DevLong) zval_get_long(v)); return;
        case Tango::DEV_ULONG:   set_attr_scalar<Tango::DevULong>(att, (Tango::DevULong) zval_get_long(v)); return;
        case Tango::DEV_LONG64:  set_attr_scalar<Tango::DevLong64>(att, (Tango::DevLong64) zval_get_long(v)); return;
        case Tango::DEV_ULONG64: set_attr_scalar<Tango::DevULong64>(att, (Tango::DevULong64) zval_get_long(v)); return;
        case Tango::DEV_FLOAT:   set_attr_scalar<Tango::DevFloat>(att, (Tango::DevFloat) zval_get_double(v)); return;
        case Tango::DEV_DOUBLE:  set_attr_scalar<Tango::DevDouble>(att, (Tango::DevDouble) zval_get_double(v)); return;
        case Tango::DEV_STATE:   set_attr_scalar<Tango::DevState>(att, state_from_name(ZSTR_VAL(zval_get_string(v)))); return;
        case Tango::DEV_STRING: {
            zend_string *s = zval_get_string(v);
            Tango::DevString *p = new Tango::DevString[1];
            p[0] = CORBA::string_dup(ZSTR_VAL(s));
            zend_string_release(s);
            att.set_value(p, 1, 0, true);
            return;
        }
        default:
            Tango::Except::throw_exception(std::string("PHP_UnsupportedType"),
                std::string("Unsupported scalar data type for ") + spec.name,
                std::string("PhpAttr::read"));
    }
}

/* ----- attribute value: WAttribute (write) -> zval ----- */

static void write_value_to_zval(Tango::WAttribute &att, const AttrSpec &spec, zval *out)
{
    switch (spec.data_type) {
        case Tango::DEV_BOOLEAN: { Tango::DevBoolean v; att.get_write_value(v); ZVAL_BOOL(out, v); return; }
        case Tango::DEV_SHORT:   { Tango::DevShort v; att.get_write_value(v); ZVAL_LONG(out, v); return; }
        case Tango::DEV_USHORT:  { Tango::DevUShort v; att.get_write_value(v); ZVAL_LONG(out, v); return; }
        case Tango::DEV_LONG:    { Tango::DevLong v; att.get_write_value(v); ZVAL_LONG(out, v); return; }
        case Tango::DEV_ULONG:   { Tango::DevULong v; att.get_write_value(v); ZVAL_LONG(out, (zend_long) v); return; }
        case Tango::DEV_LONG64:  { Tango::DevLong64 v; att.get_write_value(v); ZVAL_LONG(out, (zend_long) v); return; }
        case Tango::DEV_ULONG64: { Tango::DevULong64 v; att.get_write_value(v); ZVAL_LONG(out, (zend_long) v); return; }
        case Tango::DEV_FLOAT:   { Tango::DevFloat v; att.get_write_value(v); ZVAL_DOUBLE(out, v); return; }
        case Tango::DEV_DOUBLE:  { Tango::DevDouble v; att.get_write_value(v); ZVAL_DOUBLE(out, v); return; }
        case Tango::DEV_STRING:  { Tango::DevString v; att.get_write_value(v); ZVAL_STRING(out, v ? v : ""); return; }
        default:
            ZVAL_NULL(out);
            return;
    }
}

/* ----- bridge classes ----- */

class PhpDevice;   /* fwd */

/* Retrieve the PHP device object stored on a PhpDevice (see below). */
static zend_object *php_obj_of(Tango::DeviceImpl *dev);

class PhpCommand : public Tango::Command
{
    CmdSpec spec;
public:
    PhpCommand(const CmdSpec &s)
        : Tango::Command(s.name, (Tango::CmdArgType) s.in_type, (Tango::CmdArgType) s.out_type),
          spec(s) {}

    CORBA::Any *execute(Tango::DeviceImpl *dev, const CORBA::Any &in_any) override
    {
        zend_object *obj = php_obj_of(dev);
        zval arg, retval;
        uint32_t argc = 0;
        if (spec.in_type != Tango::DEV_VOID) {
            any_to_zval(in_any, spec.in_type, &arg);
            argc = 1;
        }
        php_call_method(obj, spec.name.c_str(), argc, argc ? &arg : nullptr, &retval,
                        (std::string("PhpCommand::") + spec.name).c_str());
        if (argc) {
            zval_ptr_dtor(&arg);
        }
        CORBA::Any *out = zval_to_any(&retval, spec.out_type,
                                      (std::string("PhpCommand::") + spec.name).c_str());
        zval_ptr_dtor(&retval);
        return out;
    }
};

class PhpAttr : public Tango::Attr
{
public:
    AttrSpec spec;
    PhpAttr(const AttrSpec &s)
        : Tango::Attr(s.name.c_str(), s.data_type, s.w_type), spec(s) {}

    void read(Tango::DeviceImpl *dev, Tango::Attribute &att) override
    {
        zend_object *obj = php_obj_of(dev);
        std::string method = "read_" + spec.name;
        zval retval;
        php_call_method(obj, method.c_str(), 0, nullptr, &retval, method.c_str());
        zval_into_attribute(att, spec, &retval);
        zval_ptr_dtor(&retval);
    }

    void write(Tango::DeviceImpl *dev, Tango::WAttribute &att) override
    {
        zend_object *obj = php_obj_of(dev);
        std::string method = "write_" + spec.name;
        zval arg, retval;
        write_value_to_zval(att, spec, &arg);
        php_call_method(obj, method.c_str(), 1, &arg, &retval, method.c_str());
        zval_ptr_dtor(&arg);
        zval_ptr_dtor(&retval);
    }

    bool is_allowed(Tango::DeviceImpl *, Tango::AttReqType) override { return true; }
};

class PhpSpectrumAttr : public Tango::SpectrumAttr
{
public:
    AttrSpec spec;
    PhpSpectrumAttr(const AttrSpec &s)
        : Tango::SpectrumAttr(s.name.c_str(), s.data_type, s.w_type, s.max_x), spec(s) {}

    void read(Tango::DeviceImpl *dev, Tango::Attribute &att) override
    {
        zend_object *obj = php_obj_of(dev);
        std::string method = "read_" + spec.name;
        zval retval;
        php_call_method(obj, method.c_str(), 0, nullptr, &retval, method.c_str());
        zval_into_attribute(att, spec, &retval);
        zval_ptr_dtor(&retval);
    }

    bool is_allowed(Tango::DeviceImpl *, Tango::AttReqType) override { return true; }
};

class PhpDeviceClass;

class PhpDevice : public Tango::Device_5Impl
{
public:
    ServerReg *reg;
    zend_object *php_obj;    /* the PHP device instance (owns a ref) */

    PhpDevice(Tango::DeviceClass *cl, const std::string &name, ServerReg *r)
        : Tango::Device_5Impl(cl, name), reg(r), php_obj(nullptr)
    {
        zval zobj;
        if (object_init_ex(&zobj, reg->php_ce) == SUCCESS) {
            tango_device_obj *io = tango_device_from_obj(Z_OBJ(zobj));
            io->dev = this;
            php_obj = Z_OBJ(zobj);     /* keep the ref created by object_init_ex */
        }
        PhpDevice::init_device();
    }

    ~PhpDevice() override
    {
        if (php_obj) {
            zval z; ZVAL_OBJ(&z, php_obj);
            zval_ptr_dtor(&z);
            php_obj = nullptr;
        }
    }

    void init_device() override
    {
        set_state(Tango::ON);
        set_status("Device is ON");
        if (php_obj && php_method_exists(php_obj->ce, "init_device", sizeof("init_device") - 1)) {
            zval retval;
            php_call_method(php_obj, "init_device", 0, nullptr, &retval, "PhpDevice::init_device");
            zval_ptr_dtor(&retval);
        }
    }
};

static zend_object *php_obj_of(Tango::DeviceImpl *dev)
{
    return static_cast<PhpDevice *>(dev)->php_obj;
}

class PhpDeviceClass : public Tango::DeviceClass
{
public:
    ServerReg *reg;
    PhpDeviceClass(const std::string &name, ServerReg *r)
        : Tango::DeviceClass(const_cast<std::string &>(name)), reg(r) {}

    void command_factory() override
    {
        for (auto &c : reg->cmds) {
            command_list.push_back(new PhpCommand(c));
        }
    }

    void attribute_factory(std::vector<Tango::Attr *> &att_list) override
    {
        for (auto &a : reg->attrs) {
            if (a.format == Tango::SPECTRUM) {
                att_list.push_back(new PhpSpectrumAttr(a));
            } else {
                att_list.push_back(new PhpAttr(a));
            }
        }
    }

    void device_factory(const Tango::DevVarStringArray *devlist) override
    {
        for (CORBA::ULong i = 0; i < devlist->length(); ++i) {
            std::string dname = (const char *) (*devlist)[i];
            PhpDevice *dev = new PhpDevice(this, dname, reg);
            device_list.push_back(dev);
            if (Tango::Util::instance()->use_db()) {
                export_device(dev);
            } else {
                export_device(dev, dname.c_str());
            }
        }
    }
};

/* class_factory hook: cppTango calls this to populate the class list. */
static PhpDeviceClass *g_php_device_class = nullptr;

static void php_class_factory(Tango::DServer *ds)
{
    if (g_server_reg && !g_php_device_class) {
        g_php_device_class = new PhpDeviceClass(g_server_reg->class_name, g_server_reg);
    }
    if (g_php_device_class) {
        ds->_add_class(g_php_device_class);
    }
}

/* ------------------------------------------------------------------------- */
/* Tango\Server\Device (base class for PHP device implementations)           */
/* ------------------------------------------------------------------------- */

static zend_object *tango_device_create(zend_class_entry *ce)
{
    tango_device_obj *intern =
        (tango_device_obj *) zend_object_alloc(sizeof(tango_device_obj), ce);
    zend_object_std_init(&intern->std, ce);
    object_properties_init(&intern->std, ce);
    intern->std.handlers = &tango_device_handlers;
    intern->dev = nullptr;
    return &intern->std;
}

static Tango::DeviceImpl *device_of_this(zval *zthis)
{
    tango_device_obj *o = (tango_device_obj *) ((char *) Z_OBJ_P(zthis) - XtOffsetOf(tango_device_obj, std));
    return o->dev;
}

PHP_METHOD(ServerDevice, set_state)
{
    char *name; size_t len;
    ZEND_PARSE_PARAMETERS_START(1, 1) Z_PARAM_STRING(name, len) ZEND_PARSE_PARAMETERS_END();
    Tango::DeviceImpl *d = device_of_this(getThis());
    if (d) d->set_state(state_from_name(name));
}

PHP_METHOD(ServerDevice, set_status)
{
    char *s; size_t len;
    ZEND_PARSE_PARAMETERS_START(1, 1) Z_PARAM_STRING(s, len) ZEND_PARSE_PARAMETERS_END();
    Tango::DeviceImpl *d = device_of_this(getThis());
    if (d) d->set_status(std::string(s, len));
}

PHP_METHOD(ServerDevice, get_state)
{
    if (zend_parse_parameters_none() == FAILURE) RETURN_THROWS();
    Tango::DeviceImpl *d = device_of_this(getThis());
    if (!d) RETURN_NULL();
    RETURN_STRING(tango_state_name(d->get_state()));
}

PHP_METHOD(ServerDevice, get_name)
{
    if (zend_parse_parameters_none() == FAILURE) RETURN_THROWS();
    Tango::DeviceImpl *d = device_of_this(getThis());
    if (!d) RETURN_NULL();
    std::string n = d->get_name();
    RETURN_STRINGL(n.c_str(), n.size());
}

/* Overridable no-op hook. */
PHP_METHOD(ServerDevice, init_device)
{
    ZEND_PARSE_PARAMETERS_NONE();
}

ZEND_BEGIN_ARG_INFO_EX(arginfo_sd_str, 0, 0, 1)
    ZEND_ARG_TYPE_INFO(0, value, IS_STRING, 0)
ZEND_END_ARG_INFO()

static const zend_function_entry tango_serverdevice_methods[] = {
    PHP_ME(ServerDevice, set_state,   arginfo_sd_str, ZEND_ACC_PUBLIC)
    PHP_ME(ServerDevice, set_status,  arginfo_sd_str, ZEND_ACC_PUBLIC)
    PHP_ME(ServerDevice, get_state,   arginfo_none,   ZEND_ACC_PUBLIC)
    PHP_ME(ServerDevice, get_name,    arginfo_none,   ZEND_ACC_PUBLIC)
    PHP_ME(ServerDevice, init_device, arginfo_none,   ZEND_ACC_PUBLIC)
    PHP_FE_END
};

/* ------------------------------------------------------------------------- */
/* Tango\Server\Server (registration + run)                                  */
/* ------------------------------------------------------------------------- */

typedef struct _tango_server_obj {
    ServerReg *reg;
    zend_object std;
} tango_server_obj;

static inline tango_server_obj *tango_server_from_obj(zend_object *obj)
{
    return (tango_server_obj *) ((char *) obj - XtOffsetOf(tango_server_obj, std));
}

static zend_object_handlers tango_server_handlers;

static zend_object *tango_server_create(zend_class_entry *ce)
{
    tango_server_obj *intern =
        (tango_server_obj *) zend_object_alloc(sizeof(tango_server_obj), ce);
    zend_object_std_init(&intern->std, ce);
    object_properties_init(&intern->std, ce);
    intern->std.handlers = &tango_server_handlers;
    intern->reg = new ServerReg();
    intern->reg->php_ce = nullptr;
    return &intern->std;
}

static void tango_server_free(zend_object *object)
{
    tango_server_obj *intern = tango_server_from_obj(object);
    delete intern->reg;
    zend_object_std_dtor(&intern->std);
}

/* __construct(string $className, string $deviceClass) */
PHP_METHOD(Server, __construct)
{
    char *cls; size_t cls_len;
    char *dev_cls; size_t dev_cls_len;
    ZEND_PARSE_PARAMETERS_START(2, 2)
        Z_PARAM_STRING(cls, cls_len)
        Z_PARAM_STRING(dev_cls, dev_cls_len)
    ZEND_PARSE_PARAMETERS_END();

    tango_server_obj *o = tango_server_from_obj(Z_OBJ_P(getThis()));
    o->reg->class_name = std::string(cls, cls_len);

    zend_class_entry *ce = zend_lookup_class(zend_string_init(dev_cls, dev_cls_len, 0));
    if (!ce) {
        zend_throw_exception_ex(tango_devfailed_ce, 0,
            "Device class '%s' not found", dev_cls);
        RETURN_THROWS();
    }
    if (!instanceof_function(ce, tango_serverdevice_ce)) {
        zend_throw_exception_ex(tango_devfailed_ce, 0,
            "Device class '%s' must extend Tango\\Server\\Device", dev_cls);
        RETURN_THROWS();
    }
    o->reg->php_ce = ce;
}

/* attribute(string $name, int $type, int $writeType = READ, int $maxX = 0) */
PHP_METHOD(Server, attribute)
{
    char *name; size_t name_len;
    zend_long type;
    zend_long wtype = Tango::READ;
    zend_long max_x = 0;
    ZEND_PARSE_PARAMETERS_START(2, 4)
        Z_PARAM_STRING(name, name_len)
        Z_PARAM_LONG(type)
        Z_PARAM_OPTIONAL
        Z_PARAM_LONG(wtype)
        Z_PARAM_LONG(max_x)
    ZEND_PARSE_PARAMETERS_END();

    tango_server_obj *o = tango_server_from_obj(Z_OBJ_P(getThis()));
    AttrSpec a;
    a.name = std::string(name, name_len);
    a.data_type = (long) type;
    a.w_type = (Tango::AttrWriteType) wtype;
    a.max_x = (long) max_x;
    a.format = (max_x > 0) ? Tango::SPECTRUM : Tango::SCALAR;
    o->reg->attrs.push_back(a);
}

/* command(string $name, int $inType = DEV_VOID, int $outType = DEV_VOID) */
PHP_METHOD(Server, command)
{
    char *name; size_t name_len;
    zend_long in_type = Tango::DEV_VOID;
    zend_long out_type = Tango::DEV_VOID;
    ZEND_PARSE_PARAMETERS_START(1, 3)
        Z_PARAM_STRING(name, name_len)
        Z_PARAM_OPTIONAL
        Z_PARAM_LONG(in_type)
        Z_PARAM_LONG(out_type)
    ZEND_PARSE_PARAMETERS_END();

    tango_server_obj *o = tango_server_from_obj(Z_OBJ_P(getThis()));
    CmdSpec c;
    c.name = std::string(name, name_len);
    c.in_type = (long) in_type;
    c.out_type = (long) out_type;
    o->reg->cmds.push_back(c);
}

/* run(array $argv): void  -- blocks running the Tango event loop.
 * argv[0] is replaced with the server (class) name; argv[1] should be the
 * instance name, e.g. running `php srv.php test` => server "ClassName/test". */
PHP_METHOD(Server, run)
{
    zval *argv_arr;
    ZEND_PARSE_PARAMETERS_START(1, 1)
        Z_PARAM_ARRAY(argv_arr)
    ZEND_PARSE_PARAMETERS_END();

    tango_server_obj *o = tango_server_from_obj(Z_OBJ_P(getThis()));
    if (!o->reg->php_ce) {
        zend_throw_exception(tango_devfailed_ce, "Server has no device class", 0);
        RETURN_THROWS();
    }

    /* Build a C argv: argv[0] = server name, then the user's args from [1..]. */
    std::vector<std::string> args;
    args.push_back(o->reg->class_name);
    HashTable *ht = Z_ARRVAL_P(argv_arr);
    zval *e;
    zend_ulong idx = 0;
    ZEND_HASH_FOREACH_NUM_KEY_VAL(ht, idx, e) {
        if (idx == 0) continue;   /* skip the script path */
        zend_string *s = zval_get_string(e);
        args.emplace_back(ZSTR_VAL(s), ZSTR_LEN(s));
        zend_string_release(s);
    } ZEND_HASH_FOREACH_END();

    int argc = (int) args.size();
    char **argv = (char **) emalloc(sizeof(char *) * (argc + 1));
    for (int i = 0; i < argc; ++i) argv[i] = estrdup(args[i].c_str());
    argv[argc] = nullptr;

    g_server_reg = o->reg;
    Tango::DServer::register_class_factory(php_class_factory);

    /* cppTango services requests on omniORB worker threads, whereas PHP 8.3's
     * stack-overflow guard captured the *main* thread's stack bounds at
     * startup. Left as-is, callbacks on a worker thread look like runaway
     * recursion. Disable the guard for this (trusted, server) process by
     * clearing the recorded stack limit -- PHP treats NULL as "no limit". */
#if PHP_VERSION_ID >= 80300
    EG(stack_base)  = NULL;
    EG(stack_limit) = NULL;
#endif

    try {
        Tango::Util *tg = Tango::Util::init(argc, argv);
        /* Serialise all upcalls process-wide so re-entrancy never reaches the
         * (non-thread-safe) PHP interpreter concurrently. */
        tg->set_serial_model(Tango::BY_PROCESS);
        tg->server_init();
        tg->server_run();          /* blocks until the server is shut down */
    } catch (Tango::DevFailed &e) {
        throw_tango_devfailed(e);
        RETURN_THROWS();
    } catch (...) {
        zend_throw_exception(tango_devfailed_ce, "Unknown fatal error running Tango server", 0);
        RETURN_THROWS();
    }
}

ZEND_BEGIN_ARG_INFO_EX(arginfo_srv_ctor, 0, 0, 2)
    ZEND_ARG_TYPE_INFO(0, className, IS_STRING, 0)
    ZEND_ARG_TYPE_INFO(0, deviceClass, IS_STRING, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(arginfo_srv_attr, 0, 0, 2)
    ZEND_ARG_TYPE_INFO(0, name, IS_STRING, 0)
    ZEND_ARG_TYPE_INFO(0, type, IS_LONG, 0)
    ZEND_ARG_TYPE_INFO(0, writeType, IS_LONG, 0)
    ZEND_ARG_TYPE_INFO(0, maxX, IS_LONG, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(arginfo_srv_cmd, 0, 0, 1)
    ZEND_ARG_TYPE_INFO(0, name, IS_STRING, 0)
    ZEND_ARG_TYPE_INFO(0, inType, IS_LONG, 0)
    ZEND_ARG_TYPE_INFO(0, outType, IS_LONG, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(arginfo_srv_run, 0, 0, 1)
    ZEND_ARG_TYPE_INFO(0, argv, IS_ARRAY, 0)
ZEND_END_ARG_INFO()

static const zend_function_entry tango_server_methods[] = {
    PHP_ME(Server, __construct, arginfo_srv_ctor, ZEND_ACC_PUBLIC | ZEND_ACC_CTOR)
    PHP_ME(Server, attribute,   arginfo_srv_attr, ZEND_ACC_PUBLIC)
    PHP_ME(Server, command,     arginfo_srv_cmd,  ZEND_ACC_PUBLIC)
    PHP_ME(Server, run,         arginfo_srv_run,  ZEND_ACC_PUBLIC)
    PHP_FE_END
};

/* ------------------------------------------------------------------------- */
/* Global function: tango_version()                                          */
/* ------------------------------------------------------------------------- */

PHP_FUNCTION(tango_version)
{
    if (zend_parse_parameters_none() == FAILURE) RETURN_THROWS();
    RETURN_STRING(Tango::TgLibVers);
}

ZEND_BEGIN_ARG_INFO_EX(arginfo_tango_version, 0, 0, 0)
ZEND_END_ARG_INFO()

static const zend_function_entry tango_functions[] = {
    PHP_FE(tango_version, arginfo_tango_version)
    PHP_FE_END
};

/* ------------------------------------------------------------------------- */
/* Module lifecycle                                                          */
/* ------------------------------------------------------------------------- */

PHP_MINIT_FUNCTION(tango)
{
    zend_class_entry ce;

    /* Tango\DevFailed extends \Exception */
    INIT_NS_CLASS_ENTRY(ce, "Tango", "DevFailed", NULL);
    tango_devfailed_ce = zend_register_internal_class_ex(&ce, zend_ce_exception);

    /* Tango\DeviceProxy */
    INIT_NS_CLASS_ENTRY(ce, "Tango", "DeviceProxy", tango_deviceproxy_methods);
    tango_deviceproxy_ce = zend_register_internal_class(&ce);
    tango_deviceproxy_ce->create_object = tango_proxy_create;

    memcpy(&tango_proxy_handlers, zend_get_std_object_handlers(),
           sizeof(zend_object_handlers));
    tango_proxy_handlers.offset   = XtOffsetOf(tango_proxy_obj, std);
    tango_proxy_handlers.free_obj = tango_proxy_free;
    tango_proxy_handlers.clone_obj = NULL;

    /* ----- server side ----- */

    /* Tango\Server\Device: base class PHP device implementations extend. */
    INIT_NS_CLASS_ENTRY(ce, "Tango\\Server", "Device", tango_serverdevice_methods);
    tango_serverdevice_ce = zend_register_internal_class(&ce);
    tango_serverdevice_ce->create_object = tango_device_create;
    memcpy(&tango_device_handlers, zend_get_std_object_handlers(),
           sizeof(zend_object_handlers));
    tango_device_handlers.offset    = XtOffsetOf(tango_device_obj, std);
    tango_device_handlers.clone_obj = NULL;

    /* Tango\Server\Server: registration + run(). */
    INIT_NS_CLASS_ENTRY(ce, "Tango\\Server", "Server", tango_server_methods);
    tango_server_ce = zend_register_internal_class(&ce);
    tango_server_ce->create_object = tango_server_create;
    memcpy(&tango_server_handlers, zend_get_std_object_handlers(),
           sizeof(zend_object_handlers));
    tango_server_handlers.offset    = XtOffsetOf(tango_server_obj, std);
    tango_server_handlers.free_obj  = tango_server_free;
    tango_server_handlers.clone_obj = NULL;

    /* ----- constants: Tango\DEV_*, Tango\READ etc. ----- */
#define TANGO_NS_LONG_CONST(name, val) \
    zend_register_long_constant("Tango\\" name, sizeof("Tango\\" name) - 1, \
        (val), CONST_CS | CONST_PERSISTENT, module_number)

    TANGO_NS_LONG_CONST("DEV_VOID",    Tango::DEV_VOID);
    TANGO_NS_LONG_CONST("DEV_BOOLEAN", Tango::DEV_BOOLEAN);
    TANGO_NS_LONG_CONST("DEV_SHORT",   Tango::DEV_SHORT);
    TANGO_NS_LONG_CONST("DEV_LONG",    Tango::DEV_LONG);
    TANGO_NS_LONG_CONST("DEV_LONG64",  Tango::DEV_LONG64);
    TANGO_NS_LONG_CONST("DEV_FLOAT",   Tango::DEV_FLOAT);
    TANGO_NS_LONG_CONST("DEV_DOUBLE",  Tango::DEV_DOUBLE);
    TANGO_NS_LONG_CONST("DEV_USHORT",  Tango::DEV_USHORT);
    TANGO_NS_LONG_CONST("DEV_ULONG",   Tango::DEV_ULONG);
    TANGO_NS_LONG_CONST("DEV_ULONG64", Tango::DEV_ULONG64);
    TANGO_NS_LONG_CONST("DEV_UCHAR",   Tango::DEV_UCHAR);
    TANGO_NS_LONG_CONST("DEV_STRING",  Tango::DEV_STRING);
    TANGO_NS_LONG_CONST("DEV_STATE",   Tango::DEV_STATE);
    TANGO_NS_LONG_CONST("DEVVAR_DOUBLEARRAY", Tango::DEVVAR_DOUBLEARRAY);
    TANGO_NS_LONG_CONST("DEVVAR_LONGARRAY",   Tango::DEVVAR_LONGARRAY);
    TANGO_NS_LONG_CONST("DEVVAR_STRINGARRAY", Tango::DEVVAR_STRINGARRAY);

    TANGO_NS_LONG_CONST("READ",             Tango::READ);
    TANGO_NS_LONG_CONST("WRITE",            Tango::WRITE);
    TANGO_NS_LONG_CONST("READ_WRITE",       Tango::READ_WRITE);
    TANGO_NS_LONG_CONST("READ_WITH_WRITE",  Tango::READ_WITH_WRITE);
#undef TANGO_NS_LONG_CONST

    return SUCCESS;
}

PHP_MINFO_FUNCTION(tango)
{
    php_info_print_table_start();
    php_info_print_table_row(2, "TANGO Controls support", "enabled");
    php_info_print_table_row(2, "php-tango version", PHP_TANGO_VERSION);
    php_info_print_table_row(2, "cppTango version", Tango::TgLibVers);
    php_info_print_table_end();
}

zend_module_entry tango_module_entry = {
    STANDARD_MODULE_HEADER,
    PHP_TANGO_EXTNAME,
    tango_functions,
    PHP_MINIT(tango),
    NULL,               /* MSHUTDOWN */
    NULL,               /* RINIT */
    NULL,               /* RSHUTDOWN */
    PHP_MINFO(tango),
    PHP_TANGO_VERSION,
    STANDARD_MODULE_PROPERTIES
};

/* Emit the get_module() entry point for shared builds. Older PHP defines
 * COMPILE_DL_TANGO; PHP 8.x marks shared builds with ZEND_COMPILE_DL_EXT. */
#if defined(COMPILE_DL_TANGO) || defined(ZEND_COMPILE_DL_EXT)
#ifdef ZTS
ZEND_TSRMLS_CACHE_DEFINE()
#endif
ZEND_GET_MODULE(tango)
#endif
