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
