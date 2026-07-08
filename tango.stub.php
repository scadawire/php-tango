<?php

/**
 * PHP API stubs for the php-tango extension.
 *
 * This file is NOT loaded at runtime -- the real classes/functions live in the
 * compiled C++ extension (tango.so). It exists purely so IDEs and static
 * analysers understand the API, mirroring PyTango's client interface.
 *
 * @generated hand-written to match tango.cpp
 */

namespace Tango;

/**
 * Exception raised for any error reported by cppTango (Tango::DevFailed).
 * The message contains the reason/description of each error in the stack.
 */
class DevFailed extends \Exception
{
}

/**
 * A client-side proxy to a running Tango device, analogous to
 * tango.DeviceProxy in PyTango.
 *
 *   $dev = new \Tango\DeviceProxy("sys/tg_test/1");
 *   echo $dev->state();
 *   $dev->write_attribute("double_scalar", 3.14);
 *   $value = $dev->read_attribute("double_scalar");
 *   // PyTango-style shortcuts:
 *   $dev->double_scalar = 3.14;   // write
 *   $value = $dev->double_scalar; // read
 */
class DeviceProxy
{
    /**
     * Connect to the named device (e.g. "sys/tg_test/1" or a fully-qualified
     * "tango://host:port/domain/family/member" name).
     *
     * @throws DevFailed if the device cannot be reached.
     */
    public function __construct(string $name) {}

    /** Fully-qualified device name as known by the database. */
    public function name(): string {}

    /** Current device status text. */
    public function status(): string {}

    /** Current device state as a string (e.g. "ON", "RUNNING", "FAULT"). */
    public function state(): string {}

    /** Round-trip time to the device, in microseconds. */
    public function ping(): int {}

    /** @return string[] Names of the device's attributes. */
    public function get_attribute_list(): array {}

    /** @return string[] Names of the device's commands. */
    public function get_command_list(): array {}

    /**
     * Read one attribute and return its value. Scalars come back as
     * bool/int/float/string; spectra and images come back as arrays.
     *
     * @return mixed
     * @throws DevFailed
     */
    public function read_attribute(string $name) {}

    /**
     * Write one attribute. The value is coerced to the attribute's Tango data
     * type (queried from the device). Pass an array for spectrum attributes.
     *
     * @param mixed $value
     * @throws DevFailed
     */
    public function write_attribute(string $name, $value): void {}

    /**
     * Execute a command. Pass the argument (if any) as the second parameter;
     * it is coerced to the command's input type. The command result is
     * returned as a PHP value (null for DevVoid).
     *
     * @param mixed $argument
     * @return mixed
     * @throws DevFailed
     */
    public function command_inout(string $command, $argument = null) {}

    /** Set the client-side request timeout in milliseconds. */
    public function set_timeout_millis(int $millis): void {}

    /** Get the client-side request timeout in milliseconds. */
    public function get_timeout_millis(): int {}

    /**
     * Read a pipe. Returns ['name' => rootBlobName, 'data' => [elt => value, …]];
     * nested blobs become nested associative arrays.
     *
     * @return array{name: string, data: array}
     * @throws DevFailed
     */
    public function read_pipe(string $name): array {}

    /**
     * Subscribe to an attribute event using the pull model, returning an event
     * id. Buffered events are retrieved with get_events().
     *
     * @param int  $eventType  One of the \Tango\*_EVENT constants.
     * @param int  $queueSize  Event buffer size.
     * @param bool $stateless  If true, subscription succeeds even if the device
     *                         is not yet reachable.
     * @throws DevFailed
     */
    public function subscribe_event(string $attr, int $eventType, int $queueSize = 100, bool $stateless = true): int {}

    /**
     * Pull buffered events for a subscription. Each element is
     * ['attr_name'=>string, 'event'=>string, 'err'=>bool, 'value'=>mixed|null,
     *  'error'=>string?].
     *
     * @return array<int, array>
     * @throws DevFailed
     */
    public function get_events(int $eventId): array {}

    /** Cancel an event subscription. */
    public function unsubscribe_event(int $eventId): void {}

    /** PyTango-style attribute read: $dev->attr === $dev->read_attribute("attr"). */
    public function __get(string $name) {}

    /** PyTango-style attribute write: $dev->attr = $v === write_attribute("attr", $v). */
    public function __set(string $name, $value): void {}
}

/**
 * Return the cppTango library version this extension was built against,
 * e.g. "9.5.0".
 */
function tango_version(): string {}

/**
 * Shut down the client-side Tango threads (event consumer, heartbeat, ORB).
 * Call before a script that used subscribe_event() exits, otherwise leftover
 * background threads keep the process alive.
 */
function tango_cleanup(): void {}

/* ----------------------------------------------------------------------- *
 * Data-type constants (subset of Tango::CmdArgType), used when declaring   *
 * attributes and commands on a server.                                     *
 * ----------------------------------------------------------------------- */
const DEV_VOID    = 0;
const DEV_BOOLEAN = 1;
const DEV_SHORT   = 2;
const DEV_LONG    = 3;
const DEV_FLOAT   = 4;
const DEV_DOUBLE  = 5;
const DEV_USHORT  = 6;
const DEV_ULONG   = 7;
const DEV_STRING  = 8;
const DEV_STATE   = 19;
const DEV_UCHAR   = 22;
const DEV_LONG64  = 23;
const DEV_ULONG64 = 24;
const DEVVAR_DOUBLEARRAY = 13;
const DEVVAR_LONGARRAY   = 11;
const DEVVAR_STRINGARRAY = 16;

/* Attribute write types (Tango::AttrWriteType). */
const READ            = 0;
const READ_WITH_WRITE = 1;
const WRITE           = 2;
const READ_WRITE      = 3;

/* Event types (Tango::EventType), for DeviceProxy::subscribe_event(). */
const CHANGE_EVENT           = 0;
const PERIODIC_EVENT         = 2;
const ARCHIVE_EVENT          = 3;
const USER_EVENT             = 4;
const ATTR_CONF_EVENT        = 5;
const DATA_READY_EVENT       = 6;
const INTERFACE_CHANGE_EVENT = 7;
const PIPE_EVENT             = 8;

namespace Tango\Server;

/**
 * Base class for a Tango device implemented in PHP -- the analogue of
 * PyTango's Device. Subclass it, override init_device(), and add
 * read_<attr>()/write_<attr>($v) methods plus a method per command.
 *
 *   class PowerSupply extends \Tango\Server\Device {
 *       private float $current = 0.0;
 *       public function init_device(): void { $this->set_state("ON"); }
 *       public function read_current(): float { return $this->current; }
 *       public function write_current(float $v): void { $this->current = $v; }
 *       public function Ramp(float $t): float { return $this->current = $t; }
 *   }
 */
class Device
{
    /** Set the device state from a state name, e.g. "ON", "FAULT". */
    public function set_state(string $state): void {}

    /** Set the device status text. */
    public function set_status(string $status): void {}

    /** Current device state as a string. */
    public function get_state(): string {}

    /** Fully-qualified device name. */
    public function get_name(): string {}

    /** Called once when the device starts. Override to initialise. */
    public function init_device(): void {}

    /**
     * Read a device property from the Tango database. Returns a string for a
     * single value, an array of strings for several, or $default when unset.
     *
     * @return string|array|mixed
     */
    public function get_property(string $name, mixed $default = null): mixed {}

    /**
     * Add a dynamic attribute at runtime (e.g. from init_device()). Handled by
     * read_<name>()/write_<name>() like a declared attribute. A positive $maxX
     * makes a spectrum; a positive $maxY makes an image.
     */
    public function add_attribute(string $name, int $type, int $writeType = \Tango\READ, int $maxX = 0, int $maxY = 0): void {}

    /** Enable/declare change events for an attribute (usually in init_device). */
    public function set_change_event(string $attr, bool $implemented = true, bool $detect = false): void {}

    /** Push a change event for an attribute with the given value. */
    public function push_change_event(string $attr, mixed $value): void {}

    /** Push an archive event for an attribute with the given value. */
    public function push_archive_event(string $attr, mixed $value): void {}
}

/**
 * Declares and runs a PHP Tango device server, the analogue of PyTango's
 * server::run(). Register the attributes and commands your Device class
 * implements, then call run().
 *
 *   $s = new \Tango\Server\Server("PowerSupply", PowerSupply::class);
 *   $s->attribute("current", \Tango\DEV_DOUBLE, \Tango\READ_WRITE);
 *   $s->command("Ramp", \Tango\DEV_DOUBLE, \Tango\DEV_DOUBLE);
 *   $s->run($argv);
 */
class Server
{
    /**
     * @param string $className   Tango class/server name (also argv[0]).
     * @param string $deviceClass FQCN of a class extending Tango\Server\Device.
     */
    public function __construct(string $className, string $deviceClass) {}

    /**
     * Declare an attribute. Handlers are read_<name>() and (for writable
     * attributes) write_<name>($value). A positive $maxX makes it a spectrum;
     * a positive $maxY makes it an image (read handler returns an array of rows).
     *
     * @param int $type      One of the \Tango\DEV_* constants.
     * @param int $writeType One of \Tango\READ, WRITE, READ_WRITE, READ_WITH_WRITE.
     * @param int $maxX      Max X dimension; >0 declares a spectrum attribute.
     * @param int $maxY      Max Y dimension; >0 declares an image attribute.
     */
    public function attribute(string $name, int $type, int $writeType = \Tango\READ, int $maxX = 0, int $maxY = 0): void {}

    /**
     * Declare a command, handled by a method of the same name on the device.
     *
     * @param int $inType  \Tango\DEV_* type of the argument (DEV_VOID for none).
     * @param int $outType \Tango\DEV_* type of the result (DEV_VOID for none).
     */
    public function command(string $name, int $inType = \Tango\DEV_VOID, int $outType = \Tango\DEV_VOID): void {}

    /**
     * Declare a pipe, handled by read_<name>() returning an associative array
     * (element name => value; associative sub-arrays become nested blobs).
     */
    public function pipe(string $name): void {}

    /**
     * Run the Tango event loop (blocks until the server is stopped).
     * $argv[0] is replaced with the server name; pass the instance name as
     * $argv[1] (i.e. run `php srv.php <instance>`).
     */
    public function run(array $argv): void {}
}
