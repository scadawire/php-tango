# php-tango

A PHP extension binding for the [TANGO Controls](https://www.tango-controls.org/)
system — the PHP analogue of [PyTango](https://pytango.readthedocs.io/).

It wraps the C++ client library [cppTango](https://gitlab.com/tango-controls/cppTango)
and exposes a `Tango\DeviceProxy` class, letting PHP scripts talk to Tango
device servers: read/write attributes and execute commands, just like
`tango.DeviceProxy` in Python.

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

Client side, verified end-to-end against a live `TangoTest` device with
cppTango 9.5.0 on PHP 8.3. Attribute reads/writes, command calls, state/status,
timeouts and error handling all work. Writing a device *server* in PHP (the
`Tango::DeviceClass`/`DeviceImpl` side of PyTango) is out of scope for now.

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

## Notes & limitations

- Attribute reads return the *read* value only (matching PyTango's
  `.value`); the set value of read-write attributes is not exposed yet.
- `DevEncoded` and image-attribute dimensions are not yet surfaced (images come
  back as a flat, row-major array).
- Events, groups, pipes, and device-server (server-side) APIs are not
  implemented.

## Layout

| File | Purpose |
| --- | --- |
| `config.m4` | Build configuration (pkg-config discovery, C++14, header shim) |
| `php_tango.h` | Extension header |
| `tango.cpp` | Extension implementation (all marshalling + `DeviceProxy`) |
| `tango.stub.php` | API stubs for IDEs/static analysis (not loaded at runtime) |
| `examples/client.php` | Runnable example against `TangoTest` |
| `tests/*.phpt` | PHP test-suite cases |
