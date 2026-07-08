--TEST--
Extension loads and registers the Tango API surface
--SKIPIF--
<?php if (!extension_loaded("tango")) echo "skip tango not loaded"; ?>
--FILE--
<?php
// cppTango version string, e.g. "9.5.0"
var_dump(preg_match('/^\d+\.\d+\.\d+/', tango_version()) === 1);

// classes exist under the Tango\ namespace
var_dump(class_exists("Tango\\DeviceProxy"));
var_dump(class_exists("Tango\\DevFailed"));
var_dump(is_subclass_of("Tango\\DevFailed", "Exception"));

// expected public methods on DeviceProxy
$rc = new ReflectionClass("Tango\\DeviceProxy");
$expected = [
    "__construct", "name", "status", "state", "ping",
    "get_attribute_list", "get_command_list",
    "read_attribute", "write_attribute", "command_inout",
    "set_timeout_millis", "get_timeout_millis", "__get", "__set",
];
foreach ($expected as $m) {
    if (!$rc->hasMethod($m)) {
        echo "MISSING METHOD: $m\n";
    }
}
echo "methods ok\n";

// constructing with a bogus device raises Tango\DevFailed (offline is fine:
// either "no database" or "device not defined" -- both are DevFailed).
try {
    new Tango\DeviceProxy("no/such/device");
    echo "no exception?!\n";
} catch (Tango\DevFailed $e) {
    echo "caught DevFailed\n";
}
?>
--EXPECT--
bool(true)
bool(true)
bool(true)
bool(true)
methods ok
caught DevFailed
