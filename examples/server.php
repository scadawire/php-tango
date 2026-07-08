<?php
/**
 * Example Tango device server written in PHP — the analogue of a PyTango
 * high-level Device server. It exposes a simple "power supply":
 *
 *   - attribute  current     (double, read-write)
 *   - attribute  voltage     (double, read-only, computed)
 *   - attribute  noise       (double spectrum, read-only)
 *   - command    Ramp(double)->double
 *   - command    TurnOff()->void
 *
 * Register it in the database, then run it, e.g.:
 *
 *   export TANGO_HOST=localhost:10000
 *   /usr/bin/python3 - <<'PY'
 *   import tango
 *   db = tango.Database()
 *   di = tango.DbDevInfo(); di.name="test/php_ps/1"; di._class="PhpPowerSupply"; di.server="PhpPowerSupply/test"
 *   db.add_device(di)
 *   PY
 *   php -d extension=./modules/tango.so examples/server.php test
 */

use Tango\Server\Device;
use Tango\Server\Server;

class PhpPowerSupply extends Device
{
    private float $current = 0.0;

    // called once when the device starts (override of the base no-op)
    public function init_device(): void
    {
        $this->current = 1.5;
        $this->set_state("ON");
        $this->set_status("PhpPowerSupply ready; current = {$this->current} A");
    }

    // attribute: current (read-write) -> read_current / write_current
    public function read_current(): float
    {
        return $this->current;
    }
    public function write_current(float $v): void
    {
        $this->current = $v;
        $this->set_status("current set to {$v} A");
    }

    // attribute: voltage (read-only), computed from current
    public function read_voltage(): float
    {
        return $this->current * 230.0;
    }

    // attribute: noise (read-only double spectrum)
    public function read_noise(): array
    {
        $n = [];
        for ($i = 0; $i < 8; $i++) {
            $n[] = round(mt_rand() / mt_getrandmax(), 4);
        }
        return $n;
    }

    // command: Ramp(double) -> double
    public function Ramp(float $target): float
    {
        $this->current = $target;
        return $this->current;
    }

    // command: TurnOff() -> void
    public function TurnOff(): void
    {
        $this->current = 0.0;
        $this->set_state("OFF");
        $this->set_status("Powered off");
    }
}

$server = new Server("PhpPowerSupply", PhpPowerSupply::class);

$server->attribute("current", Tango\DEV_DOUBLE, Tango\READ_WRITE);
$server->attribute("voltage", Tango\DEV_DOUBLE, Tango\READ);
$server->attribute("noise",   Tango\DEV_DOUBLE, Tango\READ, 8);   // maxX>0 => spectrum

$server->command("Ramp",    Tango\DEV_DOUBLE, Tango\DEV_DOUBLE);
$server->command("TurnOff", Tango\DEV_VOID,   Tango\DEV_VOID);

// Blocks running the Tango event loop until the server is stopped.
$server->run($argv);
