--TEST--
Server-side classes, constants and registration API are present
--SKIPIF--
<?php if (!extension_loaded("tango")) echo "skip tango not loaded"; ?>
--FILE--
<?php
// namespaced classes
var_dump(class_exists("Tango\\Server\\Server"));
var_dump(class_exists("Tango\\Server\\Device"));

// a few type + write-type + event constants
var_dump(Tango\DEV_VOID === 0);
var_dump(Tango\DEV_DOUBLE === 5);
var_dump(Tango\DEV_STRING === 8);
var_dump(Tango\READ_WRITE === 3);
var_dump(Tango\CHANGE_EVENT === 0);
var_dump(Tango\PIPE_EVENT === 8);

// Device base methods (incl. properties, dynamic attrs, events)
$rd = new ReflectionClass("Tango\\Server\\Device");
foreach (["set_state", "set_status", "get_state", "get_name", "init_device",
          "get_property", "add_attribute", "set_change_event",
          "push_change_event", "push_archive_event"] as $m) {
    if (!$rd->hasMethod($m)) echo "MISSING Device::$m\n";
}
// Server methods (incl. pipe)
$rs = new ReflectionClass("Tango\\Server\\Server");
foreach (["__construct", "attribute", "command", "pipe", "run"] as $m) {
    if (!$rs->hasMethod($m)) echo "MISSING Server::$m\n";
}
// client event/pipe methods + cleanup function
$rp = new ReflectionClass("Tango\\DeviceProxy");
foreach (["read_pipe", "subscribe_event", "get_events", "unsubscribe_event"] as $m) {
    if (!$rp->hasMethod($m)) echo "MISSING DeviceProxy::$m\n";
}
if (!function_exists("tango_cleanup")) echo "MISSING tango_cleanup\n";
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
$s->attribute("img", Tango\DEV_DOUBLE, Tango\READ, 4, 3);   // image
$s->command("Noop", Tango\DEV_VOID, Tango\DEV_VOID);
$s->pipe("p");
echo "registration ok\n";
?>
--EXPECT--
bool(true)
bool(true)
bool(true)
bool(true)
bool(true)
bool(true)
bool(true)
bool(true)
methods ok
rejected bad device class
registration ok
