# Search

The search finds events and states in captured logic data, with conditions on
what happens around them: what to look for (an edge, a bus value at a strobe, a
pulse), plus what must or must not happen before, after or during it. Matches
are listed with their timing, so the search also measures: setup and hold
times, pulse widths, counts.

Press **Search** in the toolbar (or **R**) to show the search bar under the
waveforms.

## The search bar

- **The search** (the wide button): what is searched for. Click it to edit.
  After you press OK, the search runs at once if there is data.
- **◀ / ▶**: the previous or next match before or after the search cursor.
  Without results yet, they start a search and go to the first match found.
- **Search / Stop**: runs the search again, or stops it. A search runs in the
  background over the whole capture; ◀ and ▶ work while it runs.
- **Status**: the number of matches and which one the cursor is on.
- **Results**: the list of matches. Click a row to go there, and a column
  header to sort by it, for example to find the shortest pulse. The line under
  the list gives the minimum, mean and maximum of each column.

| Column | Shows |
| --- | --- |
| # | The match number, in time order |
| Time | Where the match starts, from the start of the capture |
| Duration | How long a state lasted (only when searching for a state) |
| Δ n | For condition n: the time from the match to the nearest occurrence of its event (negative: before) |
| Count n | For condition n with a count: how many occurrences its window holds |

The results stay until the data changes (a new capture, another file or
device); the search itself is kept and saved with the session (`.dsl` files and
device profiles). At most 1,000,000 matches are listed; the search stops
there. Searching is not available during a capture.

## What to find

Choose **An event** or **A state**, then enter one or more patterns. A pattern
is a list of signals and the value each must have; all of them must match at
once. **Add alternative (or)** adds another pattern; then any of them may
match.

Signals are logic channels and buses. A bus is a configured **Parallel**
decoder: its D0 is the least significant bit, and unassigned bits are 0. When
analyzers are stacked, every analyzer's channels can be searched (unlike
triggers).

### Channel values

| Value | Means |
| --- | --- |
| `0`, `1` | Low, high |
| `X` | Don't care |
| `R`, `F` | Rising, falling edge |
| `C` | Either edge |

### Bus values

| Value | Means |
| --- | --- |
| `0x21XX`, `$21XX`, `21XX` | Hexadecimal; an `X` digit is four don't-care bits |
| `0b0010_0001_XXXX_XXRF` | Binary, with `0`, `1`, `X`, `R`, `F` or `C` per bit |
| `!= 0xFF`, `!= 0x1X` | Not this value (X digits are left out of the comparison) |
| `< 0x8000`, `<=`, `>`, `>=` | Compares the bus value |
| `0x2100..0x213F` | In this range, both ends included |
| `changes` | At least one bit changes |

Digits left of the value are 0, as in a number: `0x1X` on a 16-bit bus is
`0x001X`. Spaces and `_` are ignored. A value that starts with `0b` is binary;
write `0x0B12` for a hexadecimal value starting with 0B.

`C` on every bit of a bus means all bits change at the same sample; `changes`
means any one of them does.

### Events and states

- **Edges** happen at the sample where the new level is first seen. A pattern
  with edges matches where all its edges happen and its levels hold at that
  sample, so `/RD = F` with `A = 0x2100` finds reads of $2100, sampling the
  address as /RD falls.
- **Levels alone** match where they become true, once per stretch where they
  hold. The first sample of the capture can't hold an event.
- **A state** is a stretch of samples where the levels hold. A match lasts from
  its first sample to the first sample where the levels no longer hold.
  **Lasting at least / and at most** limit its length, for pulse-width and
  timeout searches. A state may not contain edges.
- A state that already holds when the capture starts, or still holds when it
  ends, has an unknown length. It matches only if its length bounds are certain
  to hold: with only a minimum, and at least that long within the capture.

## Conditions

Each condition looks at a window around the match, and all conditions must
hold. A condition is about:

- **An event**, which **occurs**, **does not occur**, or **occurs at least /
  at most / exactly N times** in the window.
- **A state**, which **holds at some point**, **never holds** or **holds
  throughout** the window.

The window is one of:

| Window | Covers |
| --- | --- |
| relative to the match start, from *t1* to *t2* | The samples from start + t1 to start + t2. Negative times are before the match |
| relative to the match end, from *t1* to *t2* | The same around the match end: for a state, the first sample after it; for an event, the event itself |
| during the match | The state's samples (only when searching for a state) |
| until the next boundary event | From the sample after the match up to, not including, the next occurrence of the boundary event |
| since the previous boundary event | From the sample after the last occurrence of the boundary event before the match, up to the sample before the match |

Times take a unit: `s`, `ms`, `us` (or `µs`), `ns`, `ps`, or `sa` for samples
(`0` needs none). Windows include both ends, and an event counts when its
sample is in the window. The window is the samples between the times, so at
250 MHz (4 ns per sample) `-10ns` to `10ns` is samples -2 to +2. When the
condition's event is like the one searched for, start the window at `1sa` to
leave the match itself out.

**Δ** is measured from the match start (or, for windows relative to the match
end or until the next boundary event, from the match end) to the occurrence in
the window nearest to it. Of two at the same distance, the earlier one counts.

**Windows past the ends of the capture** are decided only when the part inside
the capture decides them. *Occurs* and *at least N* can still be true there;
*does not occur*, *at most*, *exactly*, *never holds* and *holds throughout*
can't be confirmed, so such a match isn't listed. The same goes for "since the
previous boundary event" when there is none before the match.

## Examples

| To find | Find | Condition |
| --- | --- | --- |
| Reads of the BRK vector | event: `/RD = F`, `A = 0xXXFFE6` | |
| Writes to the PPU registers | event: `/PAWR = F`, `PA = 0x00..0x3F` | |
| A write to $420B with no /ROMSEL activity for 2 µs | event: `/WR = F`, `A = 0xXX420B` | event `/ROMSEL = C` does not occur, relative to the match start, from `0` to `2us` |
| /WE pulses shorter than 45 ns | state: `/WE = 0`, at most `45ns` | |
| No NMI for 20 ms (/ROMSEL high that long) | state: `/ROMSEL = 1`, at least `20ms` | |
| Data hold after writes | state: `/WR = 0` | event `D = changes` occurs, relative to the match end, from `0` to `300ns`; sort by Δ |
| Address setup before /WE falls | event: `/WE = F` | event `A = changes` occurs, relative to the match start, from `-200ns` to `0`; Δ is the setup time |
| A changing data bus around a clock edge | event: `CPU_CLK = R` | event `D = changes` occurs, from `-10ns` to `5ns` |
| Reads of $3060 after many reads without a write | event: `/RD = F`, `A = 0xXX3060` | event `/RD = F`, `A = 0xXX3060` occurs at least `1000` times, since the previous boundary event `/WR = F` |
| Gaps longer than 10 µs in data bus activity | event: `D = changes` | event `D = changes` does not occur, from `1sa` to `10us` |
| Low pulses (glitches) of at most two samples | state: `CLK = 0`, at most `2sa` | |
| A0–A5 all falling at once | event: `A = 0bXXXX_XXXX_XXXX_XXXX_XXFF_FFFF` | |

## Limits

- Logic data only.
- Sequences of three or more events (A, then B, then C) can't be expressed;
  conditions all relate to the one match.
- There is no bus-cycle decoding: a "read of an address" is the strobe edge
  with the address bus value at that sample.
- A search stops when one condition's windows would need more than 16 million
  events at once.

The control interface runs the same search: `data.search`, or `dsview-ctl
search` (see [control-interface.md](control-interface.md#search)).

## Verification

The engine is tested against a sample-by-sample reference implementation of
these rules, with thousands of random signals and queries:

```sh
cmake -S DSView/tests/search -B /tmp/search-tests
cmake --build /tmp/search-tests
ctest --test-dir /tmp/search-tests --output-on-failure
```
