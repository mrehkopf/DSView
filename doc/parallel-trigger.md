# Parallel bus trigger entry

Configure a **Parallel** decoder, then open **Trigger Setting**. The **Parallel
bus** section lists configured parallel decoder instances, including ones set up
before the first capture. Select a bus, enter a hexadecimal value (with or without
`0x`), and press **Apply** or Enter.

Press **Clear** to set all mapped data channels to don't-care (`X`). This works
even when the hex field is empty or invalid. In simple mode it clears those
channels' trigger buttons; in advanced mode it clears only the selected pattern's
bus bits. Other channels, clock conditions, operators, and stages are preserved.
The hex field displays **Don't care** when every mapped data bit is ignored.

In simple mode, Apply updates the High/Low buttons for the mapped data channels.
In advanced mode, select an active stage tab and choose Pattern 0 or Pattern 1 in
the bus editor. The matching headings above the binary fields identify each
pattern. Apply updates that pattern's binary fields. Its comparison
operator, AND/OR setting, counter, and other stages remain unchanged. The bus
editor shows the current stage and comparison context.

D0 is the least significant bit, independently of physical channel numbers or
waveform row order. Channel assignments come from the selected decoder's
configuration. Values must fit the bus width.
Unassigned bits retain their decoder meaning of constant zero. Disabled or
unavailable mapped channels, nonzero unassigned bits, and contradictory levels
for a channel assigned to multiple bus bits prevent Apply.

The value matches one simultaneous bus item. Clock conditions are configured
separately using the existing edge controls. Decoder word assembly and word
endianness do not affect this value. Serial-trigger data entry is unchanged.

Apply replaces any edge conditions on the mapped data channels with levels. The
editor shows a short message for such replacements. Conditions on other channels are preserved
and still participate in triggering. Manual trigger edits update the hex readback;
edges or a mix of levels and don't-care bits appear as **Custom**.

Decoder mappings are used only when applying a value. Changing or deleting a
decoder does not move or remove applied triggers. Overlapping buses share the
same channel settings, so an edit through one bus can affect another's readback.
Existing session serialization saves the resulting trigger settings. The bus
selector is not persisted.

## Stacking

With analyzers stacked, only the master's channels (A1) can be trigger
sources. A parallel decoder can still span several analyzers. Bus bits on a
secondary's channels (A2, A3, ...) are left out of the trigger instead of
preventing Apply:

- Apply and Clear set only the master's channels. Any level is accepted for the
  secondaries' bits.
- The editor shows a warning naming the ignored bits, for example `D8–D15`. The
  trigger fires whenever the master's bits match, whatever the levels on the
  ignored bits, so it can also fire on bus values that differ from the one
  entered.
- The hex readback shows ignored bits as 0. For example, `0x12A5` on a 16-bit
  bus whose upper byte is on a secondary reads back as `0x00A5`.
- A bus whose data channels are all on secondaries can't be used for
  triggering.

Disabled channels are still rejected, on any analyzer.

## Verification

The conversion and stage-field regression tests run independently of the old
application test target:

```sh
cmake -S DSView/tests/trigger -B /tmp/dsview-bus-tests
cmake --build /tmp/dsview-bus-tests
ctest --test-dir /tmp/dsview-bus-tests --output-on-failure
```

For a hardware check, compare the capture trigger with an equivalent manually
entered pattern. Include a noncontiguous mapping, a clock edge outside the data
bus, both advanced patterns, and (on 32-channel hardware) a bus crossing CH15/CH16.
Verify that channel-mode changes disable unavailable mappings and that bus entry
is unavailable during acquisition. With stacking, use a bus spanning several
analyzers and check that the trigger matches the master's bits only. No firmware or trigger protocol changes are
required.
