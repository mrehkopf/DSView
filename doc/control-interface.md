# Control interface

Scripts and AI agents can drive DSView through a local socket: select a
device, set the sample rate, threshold voltage and other options, choose
channels, set the trigger, run captures and export the data as VCD or CSV.
They can also save and open DSView data files (`.dsl`), and save and load
configuration files (`.dsc`). Commands go through the same code paths as the
toolbar, so a capture started this way behaves like one started with the Start
button, and the GUI shows it.

There are two ways in:

- **`dsview-ctl`**, a command line client that prints JSON. This is the easy
  route for shell scripts and for agents that can run commands.
- **JSON-RPC 2.0** directly on the socket, for programs. See
  [Protocol](#protocol).

Logic analyzer mode only, for now. See [Limitations](#limitations).

## Starting DSView

| Command | What you get |
| --- | --- |
| `DSView --control` | The normal window, plus the control socket. Dialogs still appear. While one is open, commands that change something fail with error 7. |
| `DSView --control --non-interactive` | No dialogs. Each one is answered with No/Cancel, and its text is passed on to the client in `messages`. |
| `DSView --headless` | No window at all (Qt's offscreen platform). Implies `--control` and `--non-interactive`. |

`--control=NAME` picks the socket. The default is `dsview-control` in the
per-user runtime directory on Linux (usually `/run/user/<uid>/dsview-control`),
and the named pipe `dsview-control-<user>` on Windows. Only your user can
connect. On Linux the path must stay under about 100 bytes.

If another DSView already serves that socket, the new one doesn't start a server;
in headless mode it exits with status 2. A socket file left behind by a
crashed DSView is replaced. An analyzer can only be open in one DSView at a
time.

To stop a headless DSView, run `dsview-ctl quit` or send it Ctrl-C or SIGTERM.
These take the normal exit path, which saves the device profile and releases the
analyzer. A headless DSView doesn't save the window layout.

## dsview-ctl

```bash
DSView --headless &
dsview-ctl devices
dsview-ctl select "DSLogic U3Pro32"
dsview-ctl set operation_mode="Buffer Mode" samplerate=100M duration=1ms vth=1.8
dsview-ctl enable 0-7
dsview-ctl trigger 0=rising --position 10
dsview-ctl capture --timeout 5 -o capture.vcd
dsview-ctl quit
```

| Command | Does |
| --- | --- |
| `info` | DSView version, protocol version, socket, current device, state |
| `quit` | Close DSView |
| `devices` | List devices |
| `select INDEX\|NAME` | Switch device. NAME can be the device name, its unique id, or `name [id]` as the GUI shows it |
| `config` | All options with their values and choices |
| `config load FILE` | Load a configuration file. `--default` instead of FILE loads the device's default configuration |
| `config save FILE` | Save the configuration to a file |
| `get [KEY...]` | Current values |
| `set KEY=VALUE...` | Change options |
| `channels` | List channels |
| `enable CH...` | Enable exactly these channels, e.g. `enable 0-7 12` |
| `rename CH NAME` | Rename a channel, as in the channel header. The name appears in exports and saved files |
| `trigger` | Show the trigger |
| `trigger CH=COND...` | Set the simple trigger to exactly these conditions, e.g. `trigger 0=rising 3=high`. `--clear` removes all conditions, `--position PCT` sets the trigger position, `--mode MODE` switches between `simple`, `advanced` and `serial` |
| `capture` | Capture and wait for the end. `--instant` ignores the trigger, `--no-wait` returns once it runs, `--timeout SEC` stops it after that long, `-o FILE` exports afterwards (`--format vcd\|csv\|gnuplot\|dsl`) |
| `status` | Capture state and data size |
| `wait [--timeout SEC]` | Wait for a running capture to end |
| `stop` | Stop the running capture |
| `export FILE` | Export the data. `--format`, `--start N`, `--end N`. A `.dsl` file is DSView's own data file |
| `open FILE` | Open a data file (`.dsl`) and load its data |
| `close` | Close the open data file |
| `call METHOD [JSON]` | Send any request |

Global options go before the command: `-s NAME` (`--socket`) picks the socket and
`--compact` prints one-line JSON.

Results are printed as JSON on stdout. Errors are printed as JSON on stderr
(`code`, `message` and sometimes `data`), with these exit codes:

| Exit code | Meaning |
| --- | --- |
| 0 | Success |
| 1 | DSView reported an error |
| 2 | Bad command line |
| 3 | Can't reach DSView |

Relative file names are made absolute before they are sent, because DSView's
working directory isn't the client's.

## Options

`config` (`config.list`) lists what the current device offers. A DSLogic
U3Pro32 has:

| Key | Type | Notes |
| --- | --- | --- |
| `samplerate` | Hz | One of the listed choices |
| `duration` | seconds | Rounded up to the next choice the toolbar offers |
| `samples` | count | Sets the shortest duration that holds this many samples |
| `operation_mode` | choice | `Buffer Mode`, `Stream Mode`, `Internal Test` |
| `channel_mode` | choice | The choices depend on `operation_mode` |
| `stop_option` | choice | What a stop in buffer mode does |
| `vth` | volts | Threshold, 0 to 5 V |
| `filter` | choice | `None` or `1 Sample Clock` |
| `rle_compress` | bool | Offers longer durations that only fit with RLE |
| `external_clock` | bool | Sample on an external clock |
| `clock_negedge` | bool | External clock on the falling edge |
| `ext_clock_rate` | Hz | Frequency of the external clock |
| `max_height` | choice | Display scaling |

Older DSLogic models have `threshold` (a choice) instead of `vth`. The demo
device reports `pattern`, which can't be changed here yet. While stacking, only
`vth`, `filter`, `external_clock`, `clock_negedge` and `ext_clock_rate` are
offered; they are set on both analyzers.

Values can be numbers or strings:

- **Units and prefixes:** strings may carry SI prefixes (`p n u m k M G`, so
  `m` is milli and `M` is mega) and a unit, e.g. `100M`, `100MHz`, `10ms`,
  `1.8V`.
- **Bare numbers** are in the base unit: Hz, seconds, volts, samples.
- **Choices** are given by name (case-insensitive), as `config` lists them.
- **Booleans** accept `true`/`false`, `on`/`off`, `yes`/`no` or `1`/`0`.

`config.set` applies values in a fixed order, because some options change what
the others accept:

1. `operation_mode`, then `channel_mode`.
2. The other device options.
3. `samplerate`.
4. `duration` or `samples`.

The sample count is a multiple of 1024. If one value is rejected, the ones
before it stay applied, and the error's `data` holds `applied` and the current
`values`.

## Triggers

`trigger.get` shows the trigger and `trigger.set` changes it. The trigger is
the one in DSView's trigger panel and channel headers, so the GUI shows what
was set, and DSView keeps it in the device profile.

```bash
dsview-ctl trigger 0=rising 3=high --position 10   # simple trigger
dsview-ctl trigger --clear                          # no trigger
dsview-ctl call trigger.set '{"stages": [{"pattern0": {"15": "rising"}},
                                         {"pattern0": {"12": "rising"}, "count": 2}]}'
```

A pattern is an object of channel and condition, such as
`{"0": "rising", "3": "high"}`:

- **Channels** are given by index, or by name when the key is not a number.
- **Usable channels:** only enabled channels that the hardware trigger covers
  (`usable_channels`). While stacking, the secondary's channels are left out.
- **Conditions** are words or the letters the GUI shows:

| Condition | Letter | Meaning |
| --- | --- | --- |
| `none` | `X` | Don't care |
| `low` | `0` | Low level |
| `high` | `1` | High level |
| `rising` | `R` | Rising edge |
| `falling` | `F` | Falling edge |
| `edge` | `C` | Rising or falling edge |

### Modes

- **`simple`:** the `channels` pattern. All its conditions must hold at the
  same sample. Setting `channels` replaces the whole simple trigger. With no
  conditions (`{}`, or `trigger --clear`), there is no trigger and captures
  start at once.
- **`advanced`:** `stages`, a list of 1 to 16 stages. The trigger fires once
  each stage has matched, one after the other. Setting `stages` replaces all of
  them; fields left out take their defaults. Needs DSLogic hardware, not in
  stream mode. A stage has these fields:

| Field | Default | Meaning |
| --- | --- | --- |
| `pattern0`, `pattern1` | `{}` | Two patterns |
| `negate0`, `negate1` | `false` | Match when the pattern doesn't hold (`!=` in the GUI) |
| `logic` | `and` | How the two patterns combine: `and` or `or` |
| `count` | 1 | How many matches the stage needs |
| `contiguous` | `false` | The matches must be on consecutive samples |

- **`serial`:** can be selected with `mode`, but its settings can only be
  changed in DSView's Serial Trigger tab so far. `trigger.get` reports them.
  The serial trigger also uses the logic settings of the first stages.

Without `mode`, `channels` selects the simple trigger and `stages` the advanced
one. To give both, add `mode`. Changing only `position` keeps the mode.

`position` is the share of the capture before the trigger, in percent, from 1
to `position_max` (up to 90 in buffer mode, 1 in stream mode). The analyzer
doesn't wait for the part before the trigger to fill: a trigger that comes
sooner sits earlier in the data. `trigger_sample` in the capture status says
where.

### Notes

- **Set the trigger last:** device options and channels can change it, as they
  do in the GUI. Stream mode switches to the simple trigger and sets the position
  to 1, and neither comes back when you leave stream mode. A channel that is
  switched off loses its simple trigger condition. `trigger.set` replies with
  the trigger as DSView then holds it, the same as `trigger.get`.
- **Multi-channel warning:** DSView normally asks before it captures with a
  simple trigger on several channels. It doesn't for a trigger set through the
  control interface, as long as that DSView keeps running and the trigger
  stays as it was set. The same goes for a trigger from a configuration file
  loaded with `config.load`. A trigger that comes from the saved device profile
  still brings up the warning; with `--non-interactive` it ends up in
  `messages`.
- **Demo device:** it accepts a simple trigger but ignores it, so `triggered`
  stays `false`.
- **Measured on a DSLogic U3Pro32 at 100 MHz, using its internal test pattern:**
  `trigger_sample` is the sample where the condition is met. The exception is a
  stage `count` above 1 on a condition that holds on consecutive samples: there
  the reported position can be one sample before the last match. The part
  before the trigger came out a few samples short of `position` (rounded down to
  a multiple of 64 samples).

## Capturing

`capture.start` uses the trigger (see [Triggers](#triggers)). With `instant` it
starts at once and ignores the trigger, like the Instant button. Repeat and loop mode are
switched to single mode, since only single captures end on their own.

| Call | Timeout behaviour |
| --- | --- |
| `capture.start` with `wait` and a `timeout` | Stops the capture when time runs out, then reports error 4 together with whatever data arrived |
| `capture.wait` with a `timeout` | Leaves the capture running, so you can wait again or stop it |

A client that disconnects doesn't stop the capture.

The status object (`capture.status`, and the result of a finished capture) has
these fields:

| Field | Meaning |
| --- | --- |
| `state` | `idle`, `running`, `loading` (a data file), `exporting` or `saving` |
| `has_data` | Whether there is captured data |
| `samples`, `samplerate`, `duration` | Size of the data |
| `trigger_armed` | The capture waited for a trigger (not instant, and a trigger was set). Left out for a data file |
| `triggered`, `trigger_sample` | Whether and where the trigger hit; only with an armed trigger, or from a data file |
| `stopped` | The capture was stopped before it was complete |
| `error` | Why the last capture failed, if it did |
| `progress` | Hardware only, while running: percentage |

## Exporting

`export` writes the captured data in one of these formats:

| Format | Written as |
| --- | --- |
| `vcd` | Value Change Dump. The default, also chosen for files ending in `.vcd` |
| `csv` | CSV. Chosen by a `.csv` ending |
| `gnuplot` | gnuplot data. Chosen by a `.gnuplot` ending |
| `dsl` | DSView's own data file, which DSView can open again. Chosen by a `.dsl` ending. See [Data files](#data-files) |

Only enabled channels are written, under their names. `start` and `end` select
the samples `start` to `end - 1`, except for `dsl`. The range is widened to whole multiples of 8
samples, and the result's `start` and `samples` give the actual range. The
result also lists the channels and the file size.

Timestamps in the file count from the start of the capture, also when only a
range is exported, so they line up with `trigger_sample`. A VCD file ends with a
timestamp at the end of the exported data. That tells you how long the last
levels lasted.

Fast signals make big files. 67 million samples of a 32-bit counter take
1.5 GB as VCD. Export a range when you only need part of the capture.

CSV output follows the **Original data** / **Compressed data** choice last made
in the GUI's export dialog. Compressed data has one row per change; original data
has one row per sample.

## Data files

DSView's data files (`.dsl`, File > Save and File > Open in the GUI) hold the
captured data together with the device options, channels, trigger and
protocol decoders of the capture.

```bash
dsview-ctl capture -o capture.dsl   # capture and save
dsview-ctl open capture.dsl         # open it again later
dsview-ctl export capture.vcd       # export it
dsview-ctl close
```

- **Saving:** `export` with the format `dsl`, which a `.dsl` ending picks.
  A data file always holds the whole capture, so `start` and `end` are refused.
  Only enabled channels are saved.
- **Opening:** `file.open` makes the file the current device, as File > Open
  does, and replies once its data is loaded, with the device and the status.
  Until then, commands that read or change the device fail with error 1.
  `capture.status` gives the state `loading`, and `capture.stop` cuts the
  loading short, which leaves part of the data and `stopped` in the status. Captured data that wasn't saved is dropped; DSView
  doesn't ask, as it does in the GUI.
- **The file as a device:** it shows up in `device.list` under its file name.
  Its data can be exported, and its options, channels and trigger read but not
  changed. It can't capture (error 3). Selecting it again with `device.select`
  loads the data again, and the reply waits for that too.
- **Trigger:** the status gives `triggered` and `trigger_sample` from the file
  when the capture had triggered.
- **Closing:** `file.close` closes the current data file and removes it from
  the device list. DSView then selects the last device in the list, as when the
  file is closed in the GUI. That may be another data file (the reply waits
  until it has loaded) or an analyzer. Use `device.select` to go elsewhere.

Every `file.open` adds a device, also for a file that is already open, so
close files you are done with.

## Configuration files

Configuration files (`.dsc`, File > Config in the GUI, where its dialogs call
them sessions) hold an analyzer's device options, channels, trigger and protocol
decoders, but no data. DSView also keeps the configuration of each analyzer in
its device profile and restores it when the analyzer is selected.

```bash
dsview-ctl config save i2c-setup.dsc
dsview-ctl config load i2c-setup.dsc
dsview-ctl config load --default    # DSView's default configuration for the device
```

- **Analyzers only:** as in the GUI, not for the demo device or a data file
  (error 3).
- **Checked first:** `config.load` refuses a file that isn't a DSView
  configuration file, is in DSView's oldest format, or was saved for another
  driver or mode, with error 9.
  `data` then gives the file's `file_device` and `file_mode`. The driver is
  `DSLogic` for every DSLogic model, so a configuration from another model is
  accepted, as in the GUI.
- **Loading** replaces the protocol decoders and sets the options, channels and
  trigger from the file. The reply gives `values`, as `config.get` does. Set
  options or the trigger afterwards to change them.
- **Saving** writes what the GUI's File > Config > Store writes. The reply gives
  `path` and `bytes`.

## Protocol

JSON-RPC 2.0 over the socket (a Unix domain socket, or a named pipe on
Windows). Each request and each response is one JSON object on one line, in
UTF-8. Several requests may be in flight on one connection; match replies by
`id`. A request without `id` is a notification and gets no reply. Batches are
not supported.

```text
→ {"jsonrpc":"2.0","id":1,"method":"config.set","params":{"samplerate":"100M","duration":"1ms"}}
← {"jsonrpc":"2.0","id":1,"result":{"applied":{"samplerate":100000000,"duration":0.001,"samples":100352},"values":{...}}}
```

Any reply may carry `messages`: dialogs that weren't shown because DSView runs
non-interactively, such as a hardware error. In an error reply they are in
`error.data.messages`.

| Method | Params | Result |
| --- | --- | --- |
| `server.info` | | `name`, `version`, `protocol` (1), `pid`, `socket`, `headless`, `non_interactive`, `state`, `device` |
| `server.quit` | | `quitting`. DSView exits after replying |
| `device.list` | | `devices`: `index`, `name`, `unique_id`, `active` |
| `device.select` | `index` or `name` | `device`: `name`, `driver`, `type` (`hardware`, `demo`, `file`), `mode`, `stacking`, `path` for a file. For a data file also `status`, once its data is loaded |
| `config.list` | | `options`: `key`, `label`, `type`, `value`, `choices`, `unit`, `min`, `max`, `read_only` |
| `config.get` | `keys` (optional list) | `values`: key → value |
| `config.set` | key → value | `applied` (what was set, after rounding), `values` |
| `config.load` | `path` (absolute), or `default`: `true` | `path`, `values` |
| `config.save` | `path` (absolute) | `path`, `bytes` |
| `channel.list` | | `channels`: `index`, `name`, `enabled`; `max_enabled`; `read_only` while stacking |
| `channel.set` | `enabled`: list of indexes, or `index` with `enabled` and/or `name` | As `channel.list` |
| `capture.start` | `instant`, `wait`, `timeout` (seconds) | Status |
| `capture.wait` | `timeout` | Status |
| `capture.status` | | Status |
| `capture.stop` | | Status once the capture has ended |
| `export` | `path` (absolute), `format` (`vcd`, `csv`, `gnuplot`, `dsl`), `start`, `end` | `path`, `format`, `bytes`, `start`, `samples`, `samplerate`, `trigger_sample`, `channels` |
| `file.open` | `path` (absolute) | `device`, `status`, once the data is loaded |
| `file.close` | | `closed` (the file's path), `device`. Also `status` when the new device is a data file |
| `trigger.get` | | `mode`, `active` (a capture waits for it), `position`, `position_max`, `modes` (available here), `usable_channels`, `channels`, `stages`, `serial` |
| `trigger.set` | `mode`, `position`, `channels`, `stages` | As `trigger.get` |

| Code | Meaning |
| --- | --- |
| -32700 | Not valid JSON |
| -32600 | Not a request object |
| -32601 | Unknown method; `data.methods` lists them |
| -32602 | Bad parameters; often with `data.keys`, `data.formats` or `data.applied` |
| -32603 | Internal error, e.g. the device could not be opened |
| 1 | Busy: a capture, export or save is running, a data file is loading, or DSView is in the middle of something |
| 2 | No device |
| 3 | Not supported: not logic analyzer mode, a data file instead of a device, channels while stacking, an advanced or serial trigger here, serial trigger settings, configuration files without an analyzer, or `file.close` without a data file |
| 4 | Timed out; `data` is the status |
| 5 | Capture failed; `data` is the status |
| 6 | Export failed |
| 7 | A dialog is open in DSView |
| 8 | No captured data |
| 9 | File: missing or unreadable, not a DSView file or in a format too old, a configuration for another device or mode, or writing it failed. `data.path` names the file |

## Limitations

- **Modes:** logic analyzer mode only. With an oscilloscope, the info,
  device and status calls work and the rest return error 3.
- **Not yet available:** serial trigger settings, protocol decoders (though
  data and configuration files carry them), and switching the device mode.
- **Persistence:** settings changed through the control interface are kept
  in the device profile, as GUI changes are.
- **AppImage:** `dsview-ctl` is not yet reachable from the AppImage. Use a
  client built from source, or talk to the socket directly.
