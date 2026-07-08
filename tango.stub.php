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
