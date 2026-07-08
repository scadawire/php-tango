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
    private float $maxCurrent = 10.0;   // from a device property

    // called once when the device starts (override of the base no-op)
    public function init_device(): void
    {
        // device property (from the Tango DB), with a default
        $this->maxCurrent = (float) $this->get_property("MaxCurrent", "10.0");

        $this->current = 1.5;
        $this->set_state("ON");
        $this->set_status("PhpPowerSupply ready; current = {$this->current} A, "
            . "max = {$this->maxCurrent} A");

        // enable manual change events on 'current', then add a dynamic attribute
        $this->set_change_event("current", true, false);
        $this->add_attribute("headroom", Tango\DEV_DOUBLE, Tango\READ);   // dynamic
    }

    // attribute: current (read-write) -> read_current / write_current
    public function read_current(): float
    {
        return $this->current;
    }
    public function write_current(float $v): void
    {
        $this->current = min($v, $this->maxCurrent);
        $this->set_status("current set to {$this->current} A");
        $this->push_change_event("current", $this->current);   // notify subscribers
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

    // attribute: ramp_profile (read-only double IMAGE, 4x3)
    public function read_ramp_profile(): array
    {
        $rows = [];
        for ($y = 0; $y < 3; $y++) {
            $row = [];
            for ($x = 0; $x < 4; $x++) {
                $row[] = round($this->current + $y * 4 + $x, 2);
            }
            $rows[] = $row;
        }
        return $rows;   // array of rows (row-major)
    }

    // dynamic attribute: headroom (added in init_device)
    public function read_headroom(): float
    {
        return $this->maxCurrent - $this->current;
    }

    // pipe: status_pipe -> associative array (element name => value)
    public function read_status_pipe(): array
    {
        return [
            "current"  => $this->current,
            "voltage"  => $this->current * 230.0,
            "labels"   => ["I", "U"],
            "readings" => [$this->current, $this->current * 230.0],
        ];
    }

    // command: Ramp(double) -> double
    public function Ramp(float $target): float
    {
        $this->current = min($target, $this->maxCurrent);
        $this->push_change_event("current", $this->current);
        return $this->current;
    }

    // command: TurnOff() -> void
    public function TurnOff(): void
    {
        $this->current = 0.0;
        $this->set_state("OFF");
        $this->set_status("Powered off");
        $this->push_change_event("current", $this->current);
    }
}

$server = new Server("PhpPowerSupply", PhpPowerSupply::class);

$server->attribute("current",      Tango\DEV_DOUBLE, Tango\READ_WRITE);
$server->attribute("voltage",      Tango\DEV_DOUBLE, Tango\READ);
$server->attribute("noise",        Tango\DEV_DOUBLE, Tango\READ, 8);      // spectrum
$server->attribute("ramp_profile", Tango\DEV_DOUBLE, Tango\READ, 4, 3);   // image 4x3

$server->command("Ramp",    Tango\DEV_DOUBLE, Tango\DEV_DOUBLE);
$server->command("TurnOff", Tango\DEV_VOID,   Tango\DEV_VOID);

$server->pipe("status_pipe");

// Blocks running the Tango event loop until the server is stopped.
$server->run($argv);
