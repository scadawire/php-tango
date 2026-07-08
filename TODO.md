# php-tango — open points

Tracking what's not yet implemented. The client (`Tango\DeviceProxy`) and server
(`Tango\Server\*`) both work today; the items below are the remaining gaps
against full PyTango / cppTango parity.

## Client (`Tango\DeviceProxy`)

- [ ] **Async event model** — only the pull model (`subscribe_event` +
      `get_events`) is exposed. A callback/push model would deliver events on
      cppTango's ZMQ consumer threads, which reintroduces the NTS threading
      hazard (see Cross-cutting). Needs a safe hand-off (e.g. a thread-safe
      queue drained from the main thread) or a ZTS build.
- [ ] **`write_pipe()`** — only `read_pipe()` exists.
- [ ] **Read-back / set value** — attribute reads return the *read* value only
      (like PyTango `.value`); expose the write/set value of READ_WRITE
      attributes (PyTango `.w_value`).
- [ ] **`DevEncoded`** attributes/commands (e.g. images as JPEG/GRAY8).
- [ ] **Groups** (`Tango::Group`) — parallel read/write/command over many
      devices.
- [ ] **Batch ops** — `write_attributes()` (multiple attrs in one call),
      `read_attributes()`.
- [ ] **Attribute metadata** — expose quality, timestamp and
      `get_attribute_config()` (min/max, unit, format, …) to PHP.
- [ ] **Async command/attribute calls** (`command_inout_asynch`, etc.).

## Server (`Tango\Server\Server` / `Tango\Server\Device`)

- [ ] **Wider element types for arrays/events** — server spectrum/image
      attributes and `push_change_event`/`push_archive_event` currently marshal
      `DEV_DOUBLE` and `DEV_LONG` element types (scalars already cover the full
      `DEV_*` set). Extend to short/bool/string/… spectra and images.
- [ ] **Wider command array types** — command in/out arrays cover
      `DEVVAR_DOUBLEARRAY` / `DEVVAR_LONGARRAY` / `DEVVAR_STRINGARRAY`; add the
      remaining `DEVVAR_*ARRAY` types (long64, bool, ushort, …) and the
      long/double-string array types.
- [ ] **Class properties** — only device properties are implemented
      (`Device::get_property`). Add class-level property access and
      `put_property()` / property defaults.
- [ ] **`remove_attribute()`** — dynamic attributes can be added but not removed.
- [ ] **`write_pipe` handler** — pipes are read-only on the server side.
- [ ] **More event kinds** — data-ready, user and interface-change events (only
      change/archive pushes are implemented). Periodic events / auto change
      detection (criteria-based) vs the current manual push.
- [ ] **Attribute properties** — `set_default_properties()` (min/max, unit,
      label, format, alarms) and per-read quality/timestamp control.
- [ ] **`is_allowed` hooks** — let PHP veto a command/attribute access (bridge
      currently always allows).
- [ ] **`always_executed_hook` / `read_attr_hardware`** — optional PHP hooks
      called before each request.

## Cross-cutting

- [ ] **One PHP server per process** — enforced because the interpreter is NTS
      and callbacks run on omniORB worker threads (serialised via `BY_PROCESS`,
      stack guard cleared). A ZTS PHP build could lift this and enable the async
      client event model.
- [ ] **Build ergonomics** — when multiple cppTango installs exist, the build
      needs `PKG_CONFIG_LIBDIR` pointed at the working one (documented in
      README). Consider a `--with-tango=<prefix>` configure option.
- [ ] **Packaging** — no `make install` docs beyond the basics; no PECL/package
      metadata; CI not set up.

## Notes

- Nothing here is a known bug — the implemented surface is verified end-to-end
  against a live server from both this extension's client and pytango.
- Async client events and wider server element-type coverage are the two most
  useful next steps.
