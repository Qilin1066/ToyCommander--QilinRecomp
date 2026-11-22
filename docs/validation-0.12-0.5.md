# Toy Commander 0.12.0 / Toy Racer 0.5.0 validation

This pass addresses controller admission, vibration lifetime, audio availability
and save recovery in the runtime shared by both games. It follows the
[previous storage/path pass](validation-reliability-2026-11-22.md). No game disc
or end-to-end playthrough is available in this environment.

## Changes and regressions

A fifth gamepad's earlier press now keeps it eligible for a port after a
controller disconnects. Previously its first-press flag was consumed while
all ports were occupied and it was never reconsidered. The graphics-backed
SDL virtual-pad test fills four ports, presses/releases a fifth, disconnects
port A and checks that the waiting pad takes A while B/C/D remain connected.

Vibration now uses a 300 ms command renewed every 100 ms. The test observes
real virtual-device rumble callbacks, renews a constant strength for longer
than one lease, then stops renewing and checks expiry. Disconnect and player
order reset stop motors; non-finite strength is treated as zero. This does
not replace checks on physical controllers or Jump Packs.

The display releases only the SDL video/gamepad references it acquired.
The pad test owns an audio-subsystem reference across display destruction
and verifies that it remains initialised. Global SDL shutdown previously
released other components' subsystems too.

Audio output reports an unavailable backend and retries at most once a second.
Public operations serialise stream-pointer changes; callbacks retain their
SDL stream lock and never take the lifecycle mutex, avoiding inverted locks
during destruction. Failed sample/format/volume/rate operations schedule a
reopen on the next push. Reopening discards stale samples, restores gain and
resumes output. Diagnostic counters distinguish unavailable submissions,
open failures and recoveries. Invalid producer buffers/rates remain errors.

The SDL audio test deliberately selects a nonexistent driver, creates output,
submits samples and changes volume/mute, selects SDL's dummy driver and verifies
that the same object recovers with callbacks. It checks subsystem ownership,
then runs concurrent producers, format changes and volume updates. This tests
initialisation failure/recovery and concurrency; physical audio-device removal,
Bluetooth transitions, every driver error path and subjective sound quality
remain unverified.

Save-history operations now report inspection, iteration and pruning errors.
They reject linked history folders before making a snapshot that their restore
policy could not use. Tests create a directory link where the platform permits
it, and verify refusal without changing the current save or link target.
Restore requests edited with a UTF-8 BOM and CRLF are accepted; oversized,
multiple-line and embedded-NUL requests are refused without deleting the request
or changing the save. The new request tests cover BOM/CRLF and oversize refusal;
prior tests retain ordinary/invalid-path restoration and five-copy retention.
This remains per-file recovery, not a whole-game save transaction.

## Validation

Linux x64/GCC 14, Release, warnings as errors:

| Check | Result |
|---|---|
| Both complete game libraries/empty-table links | Passed |
| Core, lifted programs and instruction differential tests | Passed |
| Storage, settings, save history and frame-time tests | Passed |
| Real SDL audio outage/recovery and concurrent callbacks | Passed |
| Vulkan layer, renderer and virtual-pad integration | Skipped locally: no graphics/Vulkan environment |
| ASan + UBSan: storage, settings, history and audio callback suites | All four passed |

LeakSanitizer is disabled because the execution environment uses ptrace.
CI builds with warnings as errors on six compiler/architecture/platform targets,
requires Linux graphical tests under Xvfb/Mesa, checks both Windows launchers,
and builds both Windows packages. Reproduction uses the
[previous commands](validation-reliability-2026-11-22.md#reproduction), adding
`qilin_audio_callback_test` to the sanitizer targets and test filter.

The graphics algorithms, guest instructions and timing are unchanged. Full-game
compatibility, real mission/race progression and hardware behaviour still need
real-disc testing.
