# FIDGET acceptance checklist

This checklist validates a FIDGET release against a crate using the supported
MVLC FW0046 and MDPP-32 SCP FW2051 hardware pair. Replace the example project,
module, channel, and profile names with the test crate's values. Preserve the
terminal transcript and project activity log as acceptance evidence.

Only a qualified operator should run the write, power-cycle, acquisition, and
crash-recovery checks. Coordinate shared-crate access before starting. Stop if
FIDGET reports foreign ownership, an identity mismatch, communication
uncertainty that does not recover, an unverified readback, or retained recovery
evidence.

## Prerequisites

- [ ] The build under test comes from the intended release commit, and both
  the GUI and headless Release builds pass every test suite. Record the
  commit hash with the acceptance evidence.
- [ ] The crate project loads with the intended endpoint, one supported target
  module, the correct VME base, and a known-good FW2051 profile.
- [ ] MVLC reports hardware ID `0x5008` and firmware FW0046.
- [ ] The target reports MDPP-32 hardware ID `0x5007` or `0x500C` and SCP
  firmware FW2051.
- [ ] A real input signal is available on a known physical channel for the
  acquisition checks.
- [ ] Another crate owner, if present, has acknowledged the test window and
  knows when MVME must be running, stopped, or fully quit.
- [ ] The known-good profile and crate project have recoverable backup copies.

Example paths used below:

```sh
PROJECT=crate.mwwcrate
PROFILE=known-good.mwwscp
MODULE=1
CHANNEL=29
```

## Tier 1: status, ownership session, and startup audit

### In-use refusal

- [ ] Start an MVME acquisition, then run
  `fidget_cli status --project "$PROJECT" --module "$MODULE"`.
  Pass if FIDGET reports `ownership: in-use`, reports nonzero DAQ mode, exits
  nonzero, and performs no VME-bus operation.
- [ ] Pass if the in-use probe stops after the minimum controller readings and
  does not read or write the target module.

### Idle session and release

- [ ] Stop the run and fully quit MVME, then rerun `status`.
  Pass if ownership is idle, the controller identity and FW0046 are reported,
  DAQ mode is zero, and the command exits 0.
- [ ] Run `fidget_cli session --project "$PROJECT" --module "$MODULE"`, answer
  the handoff prompt, observe at least five watchdog ticks, and press Enter.
  Pass if every tick remains session-open with DAQ mode zero and release is
  reported before exit.
- [ ] Restart MVME normally after release. Pass if MVME reconnects and starts a
  normal run without requiring a crate or controller reset.

### Zero-VME-bus-write audit

- [ ] Run `fidget_cli audit --project "$PROJECT" --module "$MODULE"`.
  Pass if exactly 37 ordered audit rows are printed and the summary reports
  `required=7/7`, `blocking=0`, and ready.
- [ ] Pass if the audit sends no VME-bus write. MVLC-local transient-stack
  writes used to perform VME reads do not count as VME-bus writes.
- [ ] Pass if each nonrequired live condition is classified as a warning or
  informational row rather than being changed by FIDGET.

## Tier 2: capture, compare, and guarded apply

### Capture and exact comparison

- [ ] Run
  `fidget_cli capture --project "$PROJECT" --module "$MODULE" --save baseline.mwwscp`.
  Pass if all eight quads and 141 values are printed, selector writes are
  reported honestly, and the selector is parked at quad zero.
- [ ] Pass if capture performs 149 VME operations and nine ownership gates:
  four global reads, eight selector writes, 136 banked reads, one final parking
  write, and no detector-parameter write.
- [ ] Run
  `fidget_cli compare --project "$PROJECT" --module "$MODULE" --profile baseline.mwwscp`.
  Pass if the result is comparable, reports 141/141 identical, and exits 0.

### Single-row apply

- [ ] Using an approved external tool while FIDGET is disconnected, create one
  controlled banked mismatch with a value that remains valid under the FW2051
  registry and dependency rules.
- [ ] Run `compare`. Pass if exactly the intended quad, register, setting name,
  live value, and profile value are reported.
- [ ] Run `fidget_cli apply` for that quad and register. Pass if the printed
  one-write plan is correct, closed input means No, typed `y` executes it,
  exact readback is verified, and the selector parks at quad zero.
- [ ] Pass if the post-apply output marks configuration stale and blocks another
  apply until a complete eight-quad recapture.
- [ ] Recapture and compare. Pass if the result returns to 141/141 identical.

### Bulk apply and rollback

- [ ] Create at least two approved banked mismatches, including one coupled
  timing or sampling value when practical.
- [ ] Run `fidget_cli apply-all`. Pass if the full preflight capture occurs
  before the first parameter write, the printed write count and order match the
  differences, and typed confirmation states the same count.
- [ ] Pass if the module is stopped with verified zero readback before applying,
  every write has exact readback, FIFO and readout resets are sent, and the
  module remains stopped.
- [ ] In an emulated or otherwise controlled failure test, fail the second
  readback. Pass if every attempted value is restored in reverse order with
  exact rollback readback.
- [ ] Recapture and compare. Pass if all 141 values match the known-good profile.

## Tier 3: deterministic startup

### Standard startup tier

- [ ] Create approved module-wide and banked differences while FIDGET is
  disconnected.
- [ ] Run
  `fidget_cli startup --project "$PROJECT" --module "$MODULE" --profile "$PROFILE"`.
  Pass if the recipe names every preparation mismatch, the banked write count,
  and the planner decision before asking for typed confirmation.
- [ ] Pass if preparation stops the module, writes only mismatched allowlisted
  values, verifies every readback, sends both resets, and verifies the full
  preparation contract while stopped.
- [ ] Pass if FIDGET captures after preparation, plans only from that fresh
  capture, applies the banked plan, captures again, and reports 141/141 exact.
- [ ] Pass if the final message is `Ready and stopped`, acquisition-enable is
  zero, and MVLC DAQ mode is zero.

### Cold-start tier, required before the v1 tag

- [ ] In a coordinated window, power-cycle the crate and do not start MVME.
- [ ] Run status and the zero-VME-bus-write audit. Pass if the supported
  hardware pair is identified and any cold-state blockers are reported without
  a write.
- [ ] Run deterministic startup from the known-good profile. Pass if the module
  is rebuilt without prior MVME initialization, final comparison is 141/141,
  and the module ends stopped.
- [ ] Start a diagnostic acquisition after cold startup. Pass if the expected
  instrumented channels become active without first running MVME.

## Tier 4: acquisition and tune loop

### Direct acquisition and isolation

- [ ] If the project contains another MDPP, start that non-target module before
  the test. Run `acquire` on the target signal channel.
- [ ] Pass if startup reports every non-target module with its base, identity,
  IRQ, prior acquisition state, verified stop, FIFO reset, and readout reset.
- [ ] Pass if the recovery journal exists before the first readout-stack write
  and changes from Prepared to Active only after acquisition starts.
- [ ] Observe at least 30 status intervals. Pass if packet and waveform counts
  advance, the requested channel appears in the per-channel breakdown, packet
  loss and decode errors remain zero, and the fingerprint remains matching.
- [ ] Pass if the GUI live trace and history show the same requested physical
  channel as the CLI counters and an optional CSV dump contains sample index
  and sample value columns.

### Source and preview controls

- [ ] During acquisition, issue `s0`, `s1`, `s2`, and `s3` as appropriate.
  Pass if each source change preserves all non-source bits in `0x614A`, verifies
  readback, resumes acquisition, and clears histories for only the selected
  quad.
- [ ] Issue `p <register> <value>` with a valid value. Pass if the live value and
  dependency are captured first, the journal arms before the write, readback is
  exact, the apply duration is reported, and old and preview waveforms do not
  mix.
- [ ] Issue `r`. Pass if the original value is restored with exact readback, the
  restore duration is reported, and the journal disarms the preview only after
  verification.
- [ ] Apply another preview and leave it active, then stop with Enter or SIGINT.
  Pass if cleanup restores preview first and source second, reports both
  results, stops the target and isolated modules, disables MVLC DAQ mode, zeros
  trigger, offset, stack, and token registers, removes the journal, and closes
  the data socket.
- [ ] Recapture and compare. Pass if the module remains 141/141 identical to the
  pre-acquisition profile.

## Tier 5: crash recovery

Run these checks only with the tested target disconnected from other owners.
Use a deliberate process termination only after confirming the recovery
journal exists and contains the expected endpoint and fingerprint.

### Plain active orphan

- [ ] Start acquisition with no active source deviation or parameter preview,
  then terminate FIDGET without cleanup. Pass if the project recovery journal
  remains and normal `status`, `session`, and acquisition commands refuse the
  active crate.
- [ ] Run `fidget_cli recover --project "$PROJECT" --module "$MODULE"` with
  closed input. Pass if the journal summary and live controller state are shown,
  recovery is not run, and no cleanup write occurs.
- [ ] Rerun recovery and type `y`. Pass if the complete 11-register fingerprint,
  including the random token, matches before the first write; every cleanup
  rung reports success; all four MVLC cleanup registers read zero; isolated
  modules are stopped and reset; and the journal is removed last.

### Armed preview and source recovery

- [ ] Start acquisition, change source, apply a parameter preview, confirm both
  restore fields are armed in the journal, then terminate without cleanup.
- [ ] Run confirmed recovery. Pass if preview restore occurs before source
  restore, each live value must equal either the journaled applied value or the
  original value, every necessary restore is readback-verified, and the journal
  is resaved after each disarmed restore.
- [ ] Pass if a third, unexpected live value causes refusal with no overwrite of
  that value and retains the journal.

### Armed restoration while DAQ is idle

- [ ] Terminate FIDGET while a preview or source deviation is armed and MVLC
  DAQ mode is zero. Pass if recovery retains the journal and offers restoration
  rather than classifying it as stale.
- [ ] Pass if recovery reverifies MVLC identity and DAQ mode, MDPP identity,
  exact FW2051 firmware, and the stopped module state before the first write.
- [ ] Pass if a live value equal to the original clears its restoration flag
  without rewriting the parameter, a live value equal to the recorded applied
  value is restored with exact readback, and any third value is not overwritten.
- [ ] Pass if multiple pending restorations are cleared and journaled one at a
  time, and the journal is removed only after no restoration remains.
- [ ] Pass if a version-3 source record without an applied value is retained
  with a manual-resolution message and causes zero VME-bus recovery writes,
  whether DAQ mode is active or idle.

### Stale journal

- [ ] Present a valid recovery journal with DAQ mode zero and no pending preview
  or source restoration.
- [ ] Pass if recovery reports the journal already clean, deletes it, performs
  zero cleanup writes, and recommends a fresh comparison.

### Foreign fingerprint

- [ ] In an emulator or controlled test, perturb one saved fingerprint field or
  token while DAQ mode remains active.
- [ ] Pass if recovery names the first mismatched field, performs zero cleanup
  writes, and retains the journal.

## Tier 6: MVME export import-back

- [ ] With no crate connection available, run
  `fidget_cli export --profile "$PROFILE" --out exported.mvme`.
  Pass if the command exits 0, reports the source checksum, identity, base,
  timestamp, and 141-value count, and no transport is constructed.
- [ ] Pass if the export contains the complete begin/end fences, identity lines
  only as comments, two writable global settings, eight 17-register quad
  blocks, and a final `0x6100 0` selector park.
- [ ] Paste the complete fenced block into a scratch MVME module initialization
  script, review it, let MVME apply it, then stop and fully quit MVME.
- [ ] Recapture with FIDGET and compare against the source profile. Pass if the
  result is comparable and 141/141 identical.

## Tier 7: GUI walkthrough

- [ ] Launch `fidget`. Pass if the FIDGET window opens with the workflow
  rail, stage area, status strip, and activity panel rendered.
- [ ] Create or load a project. Pass if endpoint fields, module rows, active
  module selection, and Save/Load/Use actions behave consistently; unsupported
  backend rows remain visibly disabled.
- [ ] Walk through Session, Profile, Audit, Capture, Prepare, and Acquire. Pass
  if the workflow rail, stage title, status strip, and single accent action all
  derive from the same current snapshot and do not contradict one another.
- [ ] Pass if the audit and comparison tables show blockers and differences with
  consistent status colors, stale state is prominent, and every hardware write
  control is disabled with a visible reason when its gate is false.
- [ ] Pass if the acquisition plot shows the live trace, age-faded history,
  frozen gold reference, channel lock while running, rates, loss, decode state,
  per-channel counts, and verified stop result.
- [ ] Pass if the activity panel shows oldest-to-newest entries, follows new
  entries when auto-scroll is enabled, filters by category and severity, and
  highlights the active operation's related entry in gold.
- [ ] Pass if an existing recovery journal blocks the normal workflow and the
  recovery card presents the same decoded gate and result as the CLI.
- [ ] Pass if Profile export defaults beside the loaded profile, refuses silent
  replacement, records an export activity entry, and never contacts hardware.

## Final evidence

- [ ] GUI and headless Release builds pass every test suite.
- [ ] The project activity file contains session closure and register-level
  entries for every accepted parameter change.
- [ ] No recovery journal remains after the final verified cleanup.
- [ ] The final capture is 141/141 identical to the accepted profile.
- [ ] The crate's normal owner can reconnect and run after FIDGET releases it.
