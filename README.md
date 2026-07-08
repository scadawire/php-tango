# php-tango

A PHP extension binding for the [TANGO Controls](https://www.tango-controls.org/)
system — the PHP analogue of [PyTango](https://pytango.readthedocs.io/).

It wraps the C++ library [cppTango](https://gitlab.com/tango-controls/cppTango)
and exposes both sides of Tango to PHP:

- a `Tango\DeviceProxy` **client** class — talk to Tango devices, read/write
  attributes and execute commands, just like `tango.DeviceProxy` in Python;
- a `Tango\Server\Server` / `Tango\Server\Device` **server** API — write a
  Tango device server entirely in PHP, the analogue of PyTango's `Device`.

```php
<?php
$dev = new Tango\DeviceProxy("sys/tg_test/1");

echo $dev->state();                       // "RUNNING"
$dev->write_attribute("double_scalar", 3.14);
$v = $dev->read_attribute("double_scalar");

// PyTango-style attribute shortcuts
$dev->double_scalar = 3.14;               // write
$v = $dev->double_scalar;                 // read

$out = $dev->command_inout("DevDouble", 2.5);
```

## Status

Both **client** and **server** sides work, verified end-to-end against a live
Tango database with cppTango 9.5.0 on PHP 8.3.

- **Client** (`Tango\DeviceProxy`): attribute reads/writes, command calls,
  state/status, timeouts and error handling — verified against `TangoTest`.
- **Server** (`Tango\Server\Server` / `Tango\Server\Device`): a device server
  written entirely in PHP, with attribute and command handlers as PHP methods —
  verified by driving it from both this extension's client **and** pytango.

## Requirements

- PHP 8.x with development headers (`phpize`, `php-config`)
- cppTango (9.3+) and its dependencies (omniORB, zmq), discoverable via
  `pkg-config --exists tango`
- A C++14 compiler

On Debian/Ubuntu:

```sh
sudo apt install php-dev libtango-dev pkg-config g++
```

## Building

```sh
phpize
./configure --enable-tango
make
```

This produces `modules/tango.so`. Try it without installing:

```sh
php -d extension=$(pwd)/modules/tango.so -r 'echo tango_version(), "\n";'
```

Install it into the PHP extension directory and enable it permanently:

```sh
sudo make install
# then add "extension=tango.so" to your php.ini (or a conf.d/ file)
```

### Choosing the right cppTango when several are installed

The build finds cppTango through `pkg-config`. If you have more than one
install (e.g. a distro package **and** a manual build under `/usr/local`),
point `pkg-config` at the one you want *before* configuring:

```sh
export PKG_CONFIG_LIBDIR=/usr/lib/x86_64-linux-gnu/pkgconfig:/usr/lib/pkgconfig:/usr/share/pkgconfig
phpize && ./configure --enable-tango && make
```

`config.m4` additionally builds a private *header shim* (`tango-shim/`) so that
`<tango/...>` includes resolve to the header root that matches the library it
links against, even when a second install would otherwise shadow it on the
compiler's default include path.

## Running the example

Needs a Tango database (`TANGO_HOST`) and a `TangoTest` device at
`sys/tg_test/1`:

```sh
export TANGO_HOST=localhost:10000
/usr/lib/tango/TangoTest test &          # path may vary
php -d extension=./modules/tango.so examples/client.php
```

## Tests

```sh
make test          # runs tests/*.phpt (does not require a live device)
```

## API

### `function tango_version(): string`
The cppTango library version the extension was built against, e.g. `"9.5.0"`.

### `class Tango\DeviceProxy`

| Method | PyTango equivalent | Notes |
| --- | --- | --- |
| `__construct(string $name)` | `DeviceProxy(name)` | Throws `Tango\DevFailed` if unreachable |
| `name(): string` | `.name()` | Fully-qualified device name |
| `state(): string` | `.state()` | e.g. `"ON"`, `"RUNNING"`, `"FAULT"` |
| `status(): string` | `.status()` | |
| `ping(): int` | `.ping()` | Round-trip time in microseconds |
| `get_attribute_list(): array` | `.get_attribute_list()` | Array of attribute names |
| `get_command_list(): array` | `.get_command_list()` | Array of command names |
| `read_attribute(string $name): mixed` | `.read_attribute(name).value` | Scalars → bool/int/float/string; spectra/images → array |
| `write_attribute(string $name, mixed $v): void` | `.write_attribute(name, v)` | Value coerced to the attribute's data type |
| `command_inout(string $cmd, mixed $arg = null): mixed` | `.command_inout(cmd, arg)` | `null` for `DevVoid`; arg coerced to the command's input type |
| `set_timeout_millis(int $ms): void` | `.set_timeout_millis(ms)` | |
| `get_timeout_millis(): int` | `.get_timeout_millis()` | |
| `$dev->attr` (via `__get`) | `dev.attr` | Shortcut for `read_attribute("attr")` |
| `$dev->attr = $v` (via `__set`) | `dev.attr = v` | Shortcut for `write_attribute("attr", $v)` |

### Type mapping

| Tango type | PHP type |
| --- | --- |
| `DevBoolean` | `bool` |
| `DevShort`, `DevLong`, `DevLong64`, `DevUShort`, `DevULong`, `DevULong64`, `DevUChar` | `int` |
| `DevFloat`, `DevDouble` | `float` |
| `DevString` | `string` |
| `DevState` | `string` (state name, e.g. `"ON"`) |
| `DevVar*Array` (spectrum/image) | `array` |
| `DevVarLongStringArray` | `["lvalue" => int[], "svalue" => string[]]` |
| `DevVarDoubleStringArray` | `["dvalue" => float[], "svalue" => string[]]` |

### Errors

Any failure reported by cppTango surfaces as a `Tango\DevFailed` exception
(extends `\Exception`); the message contains the reason and description of each
error in the Tango error stack.

## Writing a device server

The server side mirrors PyTango's high-level `Device`. Subclass
`Tango\Server\Device`, add a `read_<attr>()` / `write_<attr>($v)` method per
attribute and a method per command, then declare them on a
`Tango\Server\Server` and `run()` it.

```php
use Tango\Server\Device;
use Tango\Server\Server;

class PowerSupply extends Device
{
    private float $current = 0.0;

    public function init_device(): void      { $this->set_state("ON"); }

    public function read_current(): float     { return $this->current; }
    public function write_current(float $v): void { $this->current = $v; }
    public function read_voltage(): float     { return $this->current * 230.0; }

    public function Ramp(float $target): float { return $this->current = $target; }
    public function TurnOff(): void            { $this->set_state("OFF"); }
}

$s = new Server("PowerSupply", PowerSupply::class);
$s->attribute("current", Tango\DEV_DOUBLE, Tango\READ_WRITE);
$s->attribute("voltage", Tango\DEV_DOUBLE, Tango\READ);
$s->attribute("noise",   Tango\DEV_DOUBLE, Tango\READ, 8);   // maxX>0 => spectrum
$s->command("Ramp",    Tango\DEV_DOUBLE, Tango\DEV_DOUBLE);
$s->command("TurnOff", Tango\DEV_VOID,   Tango\DEV_VOID);
$s->run($argv);                                              // blocks
```

Register the device once in the database, then run the server with an instance
name (see `examples/server.php` for a copy-paste registration snippet):

```sh
export TANGO_HOST=localhost:10000
php -d extension=./modules/tango.so examples/server.php test
```

`Tango\Server\Device` methods: `set_state(string)`, `set_status(string)`,
`get_state(): string`, `get_name(): string`, and the overridable
`init_device(): void`. The base class automatically provides the standard
`State`, `Status` and `Init` commands.

Server-supported attribute types: `DEV_BOOLEAN`, `DEV_SHORT`, `DEV_USHORT`,
`DEV_LONG`, `DEV_ULONG`, `DEV_LONG64`, `DEV_ULONG64`, `DEV_FLOAT`, `DEV_DOUBLE`,
`DEV_STRING`, `DEV_STATE` (scalar), plus `DEV_DOUBLE`/`DEV_LONG` spectra.
Command in/out types: the scalar `DEV_*` types plus `DEVVAR_DOUBLEARRAY`,
`DEVVAR_LONGARRAY`, `DEVVAR_STRINGARRAY`.

### How it works, and the threading note

`run()` wires generic C++ bridge classes (`PhpDeviceClass` / `PhpDevice` /
`PhpCommand` / `PhpAttr`) into cppTango, and each Tango request is forwarded to
the matching PHP method.

cppTango dispatches requests on omniORB worker threads. Because the PHP
interpreter is not thread-safe (this is an NTS build), the server sets the
`BY_PROCESS` serialisation model so upcalls are serialised process-wide and
never re-enter PHP concurrently. It also clears PHP 8.3's stack-overflow guard
(`EG(stack_limit)`), which would otherwise mistake a worker thread's stack for
runaway recursion. Run **one** PHP server per process.

## Notes & limitations

- Attribute reads return the *read* value only (matching PyTango's
  `.value`); the set value of read-write attributes is not exposed yet.
- `DevEncoded` and image-attribute dimensions are not yet surfaced (images come
  back as a flat, row-major array).
- Server side supports scalar and (double/long) spectrum attributes and the
  common command types; image attributes, device/class properties, dynamic
  attributes, events and pipes are not implemented yet.
- Events, groups and pipes are not implemented on the client side.

## Layout

| File | Purpose |
| --- | --- |
| `config.m4` | Build configuration (pkg-config discovery, C++14, header shim) |
| `php_tango.h` | Extension header |
| `tango.cpp` | Extension implementation (client `DeviceProxy` + server bridge) |
| `tango.stub.php` | API stubs for IDEs/static analysis (not loaded at runtime) |
| `examples/client.php` | Runnable client example against `TangoTest` |
| `examples/server.php` | Runnable PHP device server (`PhpPowerSupply`) |
| `tests/*.phpt` | PHP test-suite cases |
