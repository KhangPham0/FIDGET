# FIDGET

FIDGET is a standalone program for inspecting and tuning Mesytec MDPP-32 SCP
digitizers through an Ethernet MVLC controller. It provides a desktop GUI and
a headless CLI built on the same core and hardware services, and its workflow
covers crate ownership, read-only startup auditing, configuration capture and
comparison, guarded parameter changes, deterministic startup, direct waveform
acquisition, crash recovery, and offline MVME settings export. The overall
scheme is that you always know who owns the crate, every hardware write is
planned, confirmed, and verified by readback, and the module you borrowed is
returned exactly as you found it.

## Supported hardware

The tested hardware contract is:

+ Mesytec MVLC Ethernet controller with firmware FW0046
+ Mesytec MDPP-32 in SCP mode with firmware FW2051

FIDGET checks hardware and firmware identity before banked access or
parameter writes. Unsupported identities and firmware revisions fail closed.
Other MDPP backends, including QDC mode, are displayed as unsupported and are
not silently treated as SCP hardware. Please note that the register behavior
implemented here was characterized specifically against this firmware pair,
so there is no expectation that other revisions behave the same way.

## Building FIDGET

The repository uses git submodules, so clone it together with its
dependencies:

```sh
git clone --recurse-submodules https://github.com/KhangPham0/FIDGET.git
cd FIDGET
```

If you already cloned the repository without submodules, run:

```sh
git submodule update --init --recursive
```

FIDGET requires CMake 3.24 or newer and a C++17 compiler.

### macOS desktop build

Install the Xcode command-line tools and a current CMake, then run:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

This builds the GUI, the CLI, and every test suite. The executables are
`build/src/fidget` and `build/src/cli/fidget_cli`, and you can run both
directly from the build tree; there is no separate installation step.

### Enterprise Linux 8 headless build

The headless build omits GLFW, ImGui, ImPlot, OpenGL, and X11 requirements,
which is useful on data-acquisition hosts without display libraries:

```sh
cmake -S . -B build-headless \
  -DFIDGET_BUILD_GUI=OFF \
  -DCMAKE_BUILD_TYPE=Release
cmake --build build-headless --parallel
ctest --test-dir build-headless --output-on-failure
```

GCC 8.5 is supported. Its C++17 filesystem implementation requires
`stdc++fs`; the build detects GCC versions older than 9 and adds that
standard library automatically at the final link sites. Please note that a
CMake newer than the distribution base package may be required because FIDGET
requires CMake 3.24.

## SSH bridge mode

A crate project can connect to its MVLC directly or through an SSH bridge.
Bridge mode keeps the GUI or CLI on the local machine while
`fidget_bridge` runs beside the remote MVLC and relays the command and data
UDP datagrams over SSH. All ownership checks, transaction policies,
acquisition decoding, and recovery behavior remain in the normal FIDGET
client.

Build the headless tree on the remote host, then set these fields in the
Project stage:

+ Controller connection: `SSH bridge`
+ SSH destination: an OpenSSH configuration alias, including any
  `ProxyJump` configuration
+ Remote bridge command: `fidget_bridge` or its full path on the remote host

The CLI reads the same settings from the `.mwwcrate` file and needs no SSH
flags. Authentication must be non-interactive through a key and agent.
FIDGET invokes SSH with `BatchMode=yes`, so password and host-key prompts are
refused instead of waiting invisibly behind the GUI. Establish trust with the
remote host in a terminal before using bridge mode.

## The guided workflow

The GUI presents seven ordered stages:

1. **Project:** load or edit controller endpoints and module definitions.
2. **Session:** check MVLC status, confirm the MVME handoff, and open
   ownership.
3. **Profile:** load a validated FW2051 `.mwwscp` profile or export it for
   MVME.
4. **Audit:** read and classify the 37-register startup state without a
   VME-bus write.
5. **Capture and compare:** read all eight SCP banks and compare 141 values.
   Capture writes only the bank selector and labels that fact explicitly.
6. **Prepare:** review and run deterministic startup. It repairs the approved
   startup contract, replans from a fresh capture, applies banked
   differences, verifies all 141 values, and leaves the module stopped.
7. **Acquire:** select a physical channel, start direct acquisition, view
   waveforms, switch source, preview a parameter, restore it, and stop
   through verified cleanup.

Hardware-changing actions require explicit controls and confirmation. After a
parameter apply, the configuration snapshot is stale until all eight banks
are captured again, and FIDGET will remind you of this before it lets you
apply another value.

## CLI commands

The examples below use reserved example names; replace them with your own
crate project and profile files.

Where:

+ `--project FILE` is a `.mwwcrate` crate project describing the controller
  endpoint and its modules.
+ `--module N` is a one-based project module number, defaulting to 1.
+ `--profile FILE` is a validated FW2051 `.mwwscp` profile.

### Status

Read the controller identity, firmware, and DAQ state once. This does not
claim a session, and it is safe to run while someone else owns the crate:

```sh
fidget_cli status --host mvlc.example.invalid --port 32768
fidget_cli status --project example-crate.mwwcrate --module 1
```

### Session

Check status, request explicit MVME handoff confirmation, monitor ownership,
and release on Enter, EOF, or SIGINT:

```sh
fidget_cli session --project example-crate.mwwcrate --module 1
```

### Audit

Open a session and run the 37-register startup audit. The audit performs no
VME-bus write, so it is the right first look at a module in an unknown state:

```sh
fidget_cli audit --project example-crate.mwwcrate --module 1
```

### Capture

Capture the five global values and all eight banked quads. The operation
writes only the SCP bank selector, reports every selector write honestly,
and parks the selector at quad zero when it is done. `--save` stores the
resulting profile:

```sh
fidget_cli capture --project example-crate.mwwcrate --module 1 \
  --save captured.mwwscp
```

### Compare

Capture the live state and compare all 141 values with a saved profile. The
exit status is zero only for a comparable, identical result, so you can use
this command in scripts as a strict equality check:

```sh
fidget_cli compare --project example-crate.mwwcrate --module 1 \
  --profile expected.mwwscp
```

### Apply one value

Print the plan, require typed confirmation, reject stale live state, verify
the write by exact readback, and roll back on a readable mismatch. The
transaction stops and verifies the module before the banked write, resets
its readout afterward, and deliberately leaves it stopped, matching bulk
apply. For example, to repair the quad-7 gain to its profile value, I would
use:

```sh
fidget_cli apply --project example-crate.mwwcrate --module 1 \
  --profile expected.mwwscp --register 0x611A --quad 7
```

### Apply all banked differences

Preflight with a full capture, print the ordered plan, require typed
confirmation stating the write count, verify every write, and restore
attempted values in reverse order on a readable failure:

```sh
fidget_cli apply-all --project example-crate.mwwcrate --module 1 \
  --profile expected.mwwscp
```

### Deterministic startup

Prepare the module-wide startup contract, capture, plan and apply banked
values, then recapture and require a 141/141 match. A successful run ends
ready and stopped; it does not start acquisition:

```sh
fidget_cli startup --project example-crate.mwwcrate --module 1 \
  --profile expected.mwwscp
```

### Acquire

Start direct diagnostic acquisition for one physical channel. The command
prints packet, waveform, loss, decode, channel-count, isolation,
fingerprint, and cleanup status while it runs. For example, to watch channel
29 for thirty seconds and keep its latest waveform, I would use:

```sh
fidget_cli acquire --project example-crate.mwwcrate --module 1 \
  --profile expected.mwwscp --channel 29 --seconds 30 \
  --dump-csv channel-29.csv
```

While acquisition is running, Enter stops it, `s0` through `s3` change the
waveform source, `p <register> <value>` starts a guarded parameter preview,
and `r` restores the preview. Every temporary change is journaled before it
is written and restored during verified cleanup, so a tuning session leaves
the module exactly as it found it.

### Recover

Inspect a project-adjacent recovery journal and offer journal-gated recovery.
An active tuner-owned orphan requires the complete fingerprint to match. An
idle record with pending temporary values permits only their identity-gated,
exact restoration. Closed input and a blank answer mean No:

```sh
fidget_cli recover --project example-crate.mwwcrate --module 1
```

### Export for MVME

Generate a validated MVME settings block without constructing a transport or
contacting crate hardware. Overwriting an existing output requires typed
confirmation:

```sh
fidget_cli export --profile expected.mwwscp --out expected.mvme
```

## Shared-crate ownership and crash recovery

FIDGET assumes the controller and digitizers may also be used by MVME or
another operator, so a normal session begins with a read-only status check.
The session is refused while MVLC DAQ mode is active, and opening one
requires you to explicitly confirm that MVME has stopped and quit.

While FIDGET owns a session, a serialized command worker prevents
overlapping transactions. Idle checks and a pre-write gate watch DAQ mode.
During direct acquisition, an 11-register fingerprint covers DAQ state, the
uploaded stack, trigger, offset, and a per-session random token. Missing
replies temporarily freeze writes while the receiver continues draining
data. A proven fingerprint mismatch causes passive detach: FIDGET closes its
sockets and does not write cleanup into a run that is no longer its own.

Before any non-target stop or reset and before installing the diagnostic
stack, FIDGET atomically writes `<project-file>.recovery`. The record contains
the endpoint, identities, readout fingerprint, isolated modules, and any
parameter or source value that must be restored, and it is removed only after
verified cleanup. If the process crashes, `fidget_cli recover` compares the
live controller state with the journal before writing anything. An active
orphan requires the complete fingerprint, including the random token, to
match. A foreign or mismatched
fingerprint produces zero cleanup writes and retains the journal as evidence.
If DAQ mode is already zero but a preview or source restoration remains, the
MVLC and MDPP identities, exact FW2051 firmware, stopped module state, live
temporary value, and DAQ-mode gates must all pass before FIDGET restores only
that journaled value. An idle journal with no pending restoration is stale and
may be removed without a hardware write. A version-3 journal can identify an
original source value but not the applied value, so source restoration from
that legacy evidence fails closed in either DAQ state and requires manual
resolution.

Operation history is appended to `<project-file>.activity`. It records
session, audit, capture, apply, startup, acquisition, source, preview,
recovery, and export outcomes, including register-level before and after
values for parameter changes. Please note that a persistence error is
reported but never prevents a required hardware cleanup.

## Exporting settings to MVME

An export contains a comment-delimited manifest followed by the two writable
global settings, eight selector-delimited quad blocks, and a final selector
park at quad zero. Hardware identity, firmware, VME base, source checksum,
timestamp, and value count remain comments. FIDGET validates the profile,
every value, and all coupled dependencies before producing the block.

To use an export, open the target module's initialization script in MVME,
paste the complete text between the `BEGIN FIDGET MVME EXPORT` and
`END FIDGET MVME EXPORT` fences into the module settings section, review it,
and let MVME apply it through its normal initialization path. FIDGET never
locates or edits an MVME installation. After MVME has applied the script,
quit MVME, recapture with FIDGET, and require a 141/141 comparison against
the source profile.

## License

FIDGET is available under the MIT license; see the `LICENSE` file. Vendored
third-party components keep their own licenses, which are collected in the
`LICENSES` directory and beside the vendored sources.

## Acknowledgements

+ Most of the implementation code in this repository was written with the
  help of AI coding assistants (Anthropic's Claude and OpenAI's models). I
  directed the design, decided every behavior that touches shared hardware,
  and performed all of the hardware validation myself on a live MVLC +
  MDPP-32 crate. Development proceeded in reviewed phases behind
  real-hardware acceptance gates, and the criteria a release must pass are
  recorded in `docs/acceptance-checklist.md`.
+ The register-level behavior implemented here is specific to the Mesytec
  MVLC (FW0046) and MDPP-32 SCP (FW2051) firmware pair and was characterized
  against Mesytec's documentation and live hardware. FIDGET contains no
  Mesytec source code.
+ The GUI is built on Dear ImGui, ImPlot, and GLFW, the tests use doctest,
  and the embedded typefaces are Cascadia Code and Font Awesome. Their
  license notices are preserved in this repository.
