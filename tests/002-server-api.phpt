--TEST--
Server-side classes, constants and registration API are present
--SKIPIF--
<?php if (!extension_loaded("tango")) echo "skip tango not loaded"; ?>
--FILE--
<?php
// namespaced classes
var_dump(class_exists("Tango\\Server\\Server"));
var_dump(class_exists("Tango\\Server\\Device"));

// a few type + write-type constants
var_dump(Tango\DEV_VOID === 0);
var_dump(Tango\DEV_DOUBLE === 5);
var_dump(Tango\DEV_STRING === 8);
var_dump(Tango\READ_WRITE === 3);

// Device base methods
$rd = new ReflectionClass("Tango\\Server\\Device");
foreach (["set_state", "set_status", "get_state", "get_name", "init_device"] as $m) {
    if (!$rd->hasMethod($m)) echo "MISSING Device::$m\n";
}
// Server methods
$rs = new ReflectionClass("Tango\\Server\\Server");
foreach (["__construct", "attribute", "command", "run"] as $m) {
    if (!$rs->hasMethod($m)) echo "MISSING Server::$m\n";
}
echo "methods ok\n";

// registration must reject a class that does not extend Tango\Server\Device
try {
    new Tango\Server\Server("Foo", stdClass::class);
    echo "no exception?!\n";
} catch (Tango\DevFailed $e) {
    echo "rejected bad device class\n";
}

// a proper subclass registers without error, and attribute()/command() work
class MyDev extends Tango\Server\Device {
    public function read_x(): float { return 1.0; }
}
$s = new Tango\Server\Server("MyServer", MyDev::class);
$s->attribute("x", Tango\DEV_DOUBLE, Tango\READ);
$s->command("Noop", Tango\DEV_VOID, Tango\DEV_VOID);
echo "registration ok\n";
?>
--EXPECT--
bool(true)
bool(true)
bool(true)
bool(true)
bool(true)
bool(true)
methods ok
rejected bad device class
registration ok
