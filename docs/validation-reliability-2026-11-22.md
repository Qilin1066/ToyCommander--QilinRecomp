# Toy Commander 0.11.0 / Toy Racer 0.4.0 validation

This pass improves the shared launcher, persisted files and Windows path
handling. The existing widescreen/movie changes from main are included in
these versions; their rendering algorithms and lifted instructions are
unchanged by this reliability work. No disc is available in this environment,
so neither game has been played through here.

## Locally checked

Linux x64, GCC 14, Release, warnings as errors:

| Check | Result |
|---|---|
| Both complete game libraries linked with empty lifted tables | Passed |
| Recompiler/core, including synthetic Unicode CUE/track paths | Passed |
| Synthetic lifted programs and differential instruction tests | Passed |
| Decoder sweep against Capstone 5.0.7 at both instruction alignments | No unexplained differences |
| File storage, settings, save history | Passed |
| Frame timings and real SDL dummy audio callbacks | Passed |
| Renderer, Vulkan layer and gamepad integration | Skipped locally: no working graphical/Vulkan environment |
| ASan + UBSan: file storage, settings, save history | All three passed |

LeakSanitizer is disabled because this environment uses ptrace. CI builds
with warnings as errors on Linux GCC/Clang x64, Linux ARM64, macOS Intel/Apple
Silicon and Windows MSVC. Linux requires its graphical tests under Xvfb/Mesa.
Both Windows download archives and their complete links are built separately.

## Regressions and practical limits

The Windows launcher check uses real Ninja with synthetic compiler/lifter
children. It checks archive, header and compiler updates whose timestamps are
older than the installed build; CPU-identity changes; no-op reuse; explicit
clean rebuild; a locked installation followed by recovery; and native child
arguments containing Unicode, spaces, `%PATH%` and `&`. Each game's manifest
and executable name are exercised. The real recompiler is also run on a
synthetic unsupported boot file with Unicode input/output names. This checks
path handling, not a complete real-disc build or CPU performance.

Storage tests cover existing-file replacement, binary copies across multiple
64 KiB chunks, copying to self, empty files, unique temporary-file cleanup,
invalid sources/destinations and Windows sharing locks. VMU tests create a
complete 128 KiB image, update one small packet, check surrounding bytes and
refuse out-of-range packets or existing images of the wrong size. Unreadable
and oversized images are not treated as new cards. Settings tests exercise
Unicode directories, repeated saves, UTF-8 BOMs and refusal to replace an
unreadable existing settings path.

VMU writes remain small in-place updates for efficiency. They check all I/O
results but are not atomic across an entire save. The pre-burst history is
still needed for recovery after an interrupted save. Settings/flash replacement
flushes file data before rename; this is not a guarantee against every host
filesystem or power-loss failure. Backup recovery remains per file, and a
saved game's logical validity still needs real-game testing.

The renderer, GPU-specific behaviour, four physical controllers/Jump Packs,
real audio devices, loading every mission/race, save/load progression and
end-to-end completion still need real-disc validation. Full lifted-code or
instruction-test coverage is not 100% game compatibility.

## Reproduction

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DQILIN_WERROR=ON
cmake --build build --parallel
ctest --test-dir build --output-on-failure
python3 tools/check/verify_decoder.py --qilin build/qilin
cmake -S . -B build-sanitize -DCMAKE_BUILD_TYPE=Debug -DQILIN_SANITIZE=address,undefined
cmake --build build-sanitize --target qilin_file_store_test dreamcast_settings_test dreamcast_save_history_test --parallel
ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=halt_on_error=1 \
  ctest --test-dir build-sanitize --output-on-failure \
  -R '^(qilin_file_store_test|dreamcast_settings_test|dreamcast_save_history_test)$'
```
