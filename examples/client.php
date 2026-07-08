<?php
/**
 * Example php-tango client, mirroring a typical PyTango session.
 *
 * Requires a running Tango database (TANGO_HOST) and the standard TangoTest
 * device registered as "sys/tg_test/1". Start one with:
 *
 *     export TANGO_HOST=localhost:10000
 *     /usr/lib/tango/TangoTest test    # (path may vary by distro)
 *
 * Run:
 *     php -d extension=./modules/tango.so examples/client.php
 */

use Tango\DeviceProxy;
use Tango\DevFailed;

$deviceName = $argv[1] ?? "sys/tg_test/1";

try {
    printf("php-tango linked against cppTango %s\n\n", tango_version());

    $dev = new DeviceProxy($deviceName);

    printf("device : %s\n", $dev->name());
    printf("state  : %s\n", $dev->state());
    printf("status : %s\n", $dev->status());
    printf("ping   : %d us\n", $dev->ping());

    printf("\nattributes (%d): %s ...\n",
        count($dev->get_attribute_list()),
        implode(", ", array_slice($dev->get_attribute_list(), 0, 6)));
    printf("commands  (%d): %s ...\n",
        count($dev->get_command_list()),
        implode(", ", array_slice($dev->get_command_list(), 0, 6)));

    echo "\n-- attributes --\n";
    $dev->write_attribute("double_scalar", 3.14159);
    printf("double_scalar  -> %s\n", $dev->read_attribute("double_scalar"));

    $dev->write_attribute("double_spectrum", [1.5, 2.5, 3.5]);
    printf("double_spectrum-> [%s]\n",
        implode(", ", $dev->read_attribute("double_spectrum")));

    // PyTango-style magic accessors
    $dev->long_scalar = 12345;
    printf("long_scalar    -> %s (via \$dev->long_scalar)\n", $dev->long_scalar);

    echo "\n-- commands --\n";
    printf("DevDouble(2.5)      = %s\n", $dev->command_inout("DevDouble", 2.5));
    printf("DevString('hi')     = %s\n", $dev->command_inout("DevString", "hi"));
    printf("DevVarLongArray     = [%s]\n",
        implode(", ", $dev->command_inout("DevVarLongArray", [5, 6, 7])));
    printf("DevVoid()           = %s\n",
        var_export($dev->command_inout("DevVoid"), true));

    echo "\n-- error handling --\n";
    try {
        $dev->read_attribute("nonexistent_attribute");
    } catch (DevFailed $e) {
        printf("caught DevFailed: %s\n", explode("\n", $e->getMessage())[0]);
    }

    echo "\nDone.\n";
} catch (DevFailed $e) {
    fwrite(STDERR, "Tango error: " . $e->getMessage() . "\n");
    exit(1);
}
