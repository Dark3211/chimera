# Chimera Hardened — Fixes, Optimizations and Validation

Branch: `chimera-hardened`
Last updated: 2026-08-14

## Purpose

This branch keeps Chimera's existing behavior and features while hardening failure paths,
bounds handling, filesystem access, map loading, downloads, bookmark/server switching and
the x86 hook/trampoline engine.

---

# Earlier hardening

## `src/chimera/bookmark/bookmark.cpp`

- Reworked server address parsing.
- Added null/empty address validation.
- Added malformed-port validation.
- Rejects port `0` and ports above `65535`.
- Added `[IPv6]:port` parsing.
- Preserves default port `2302` when a port is omitted.
- Added password-length validation.
- Prevents null password pointers from reaching formatting routines.
- Uses `AF_UNSPEC` for server lookup.
- Added IPv4 and IPv6 DNS resolution.
- Tries all usable `getaddrinfo()` results instead of assuming a single result.
- Added socket creation, `setsockopt()`, `sendto()` and `recvfrom()` failure handling.
- Added UDP server-query timeouts.
- Added response-buffer termination and minimum-size validation.
- Added socket cleanup on failure paths.
- Sanitizes control characters in server-query responses.
- Fixed bookmark edits not being written back correctly to `bookmark.txt`.
- Hardened generated `connect` command formatting.
- Added quoting for addresses and explicit port formatting.
- Added IPv6-aware connect formatting.
- Escapes `\` and `"` in passwords before command generation.
- Added generated-command length checks.

## `src/chimera/bookmark/bookmark.hpp`

- Added safe default initialization for query result state.
- Initializes timeout/error/ping fields.
- Made query-data lookup const/noexcept.
- Added null-key handling.
- Missing keys return a safe empty string rather than a null string pointer.

## `src/chimera/chimera.cpp`

- Replaced exception-prone filesystem directory creation with `std::error_code` variants.
- Uses `create_directories()` for nested Chimera paths.
- Hardened creation of the Chimera data directory, temporary directory, map directory and
  downloaded-map directory.
- Prevents common filesystem permission/path errors from unexpectedly escaping through
  initialization code.

## `src/chimera/event/connect.cpp`

- Added null-password protection to the pre-connect hook.
- Prevents invalid memory access when Halo supplies a null password pointer.

## `src/chimera/event/tick.cpp`

- Added `VirtualProtect()` result validation.
- Initializes page-protection state.
- Corrected the memory-protection size used when modifying the tick-rate value.
- Avoids writing the target when page protection cannot safely be changed.

## `src/chimera/halo_data/port.cpp`

- Validates `halo.client_port` and `halo.server_port`.
- Rejects negative values and values above `65535`.
- Prevents silent narrowing/truncation to `uint16_t`.

## `src/chimera/map_loading/compression.cpp`

- Added compressed-file minimum-size and header-read validation.
- Supports validation/fallback between normal and demo map headers.
- Added `ZSTD_createDStream()` and `ZSTD_initDStream()` failure handling.
- Added RAII cleanup for Zstd stream state.
- Corrected streaming decompression state handling.
- Preserves unconsumed input between `ZSTD_decompressStream()` calls.
- Added truncated/incomplete stream detection.
- Added output callback failure handling.
- Ensures compressed map files close on both success and failure paths.

## `src/chimera/map_loading/fast_load.cpp`

- Replaced exception-prone filesystem checks with `std::error_code` where appropriate.
- Hardened map existence and directory iteration.
- Avoids common filesystem errors terminating Halo during map-list handling.

## `src/chimera/map_loading/map_loading.cpp`

- Added null map validation before CRC processing.
- Added minimum map-size and header validation.
- Added safer file-open/seek/read handling.
- Added bounds validation for RAM-preloaded map reads.
- Added tag-data, scenario-data, BSP-table, BSP-size and model-data bounds checks.
- Added safer virtual-address-to-tag-data conversion.
- Hardened resource-map reads and asset precaching.
- Prevented `ui.map` preload buffer underflow.
- Hardened downloaded-map rename/install handling with `std::error_code`.
- Prevents continuing a join when a required downloaded map could not be installed.

## `src/chimera/output/draw_text.cpp`

- Replaced exception-prone font-directory checks with `std::error_code`.
- Keeps filesystem errors inside the existing font-loading failure handling.

## `src/chimera/signature/hook.cpp`

- Added relocation of copied relative x86 instructions.
- Supports relocation for:
  - `CALL rel32` (`E8`)
  - `JMP rel32` (`E9`)
  - near conditional branches (`0F 80`–`0F 8F`)
  - short `JMP` (`EB`)
  - short conditional branches (`70`–`7F`)
- Added source-to-trampoline offset mapping.
- Preserves branch targets that point inside the copied instruction block.
- Expands short branches when required by trampoline relocation.
- Added relocated trampoline-size calculation.
- Added `VirtualProtect()` failure handling and instruction-cache flushing.

## `src/chimera/signature/hook.hpp`

- Added null target/data checks to generic overwrite helpers.
- Added zero-length checks.
- Added `VirtualProtect()` failure handling.
- Restores original page protection after writes.
- Flushes the CPU instruction cache after executable-memory modifications.

## `src/map_downloader/map_downloader.cpp`

- Protected downloader shared state.
- Hardened worker-thread configuration snapshots.
- Added URL-escaping validation and cleanup.
- Uses literal placeholder replacement where regex replacement was unsafe.
- Preserves mirror placeholder behavior.
- Hardened mirror retry state and file-handle cleanup.
- Added file open/write/partial-write failure detection.
- Added cURL initialization and critical option validation.
- Added HTTP-error handling, redirects, connection timeout and total timeout.
- Added worker-thread exception containment.
- Added temporary-resource cleanup on failed downloads.
- Added `joinable()` checks and thread-creation failure handling.
- Hardened server/password updates and download failure state.

---

# Safe bookmark server switching

Files:
- `src/chimera/bookmark/bookmark.cpp`

The reproducible failure was rapid use of `chimera_bookmark_connect`, especially:

1. connect to server A;
2. immediately request server B;
3. or repeatedly trigger the same bookmark.

Older attempts that tried to manipulate Halo's low-level connection cleanup were rejected
after runtime testing showed that forcing internal disconnect state could cause a segmentation
fault.

The validated V4 implementation instead serializes bookmark-driven connection requests:

- Adds explicit bookmark connection states:
  - `IDLE`
  - `CONNECTING`
  - `WAITING_TO_DISCONNECT`
  - `DISCONNECTING`
- Ignores repeated requests for the same bookmark while the connection is already negotiating.
- Queues a different requested bookmark rather than entering Halo's connection routine twice.
- The newest requested target wins while a switch is already in progress.
- Uses Halo's normal script-level `disconnect` path instead of the low-level forced cleanup routine.
- Waits for connected/disconnected state to settle before advancing.
- Refuses to force a second connection if disconnect does not complete safely.
- Does not automatically launch a queued connection after an unresolved connection timeout.
- V4.1 reduces the failed-connect recovery timeout from 45 seconds to 15 seconds.

Runtime result:
- rapid A -> B switching works;
- repeated bookmark use no longer reproduced the original connection-state crash in testing.

---

# Core Safety

Files:
- `src/chimera/bookmark/bookmark.cpp`
- `src/chimera/bookmark/bookmark.hpp`
- `src/chimera/command/client/server/spam_to_join.cpp`
- `src/chimera/config/ini.cpp`
- `src/chimera/halo_data/map.cpp`
- `src/chimera/halo_data/tag.cpp`
- `src/map_downloader/map_downloader.cpp`
- `src/map_downloader/map_downloader.hpp`

## Bookmark/history query concurrency

- Removed the invalid pattern where one thread locked a `std::mutex` and a detached worker
  thread unlocked it.
- Uses an atomic in-progress flag for query ownership.
- Builds query results locally in the worker.
- Publishes finished results under a mutex.
- Swaps finished results into the frame thread under the same mutex.
- Handles worker/thread-creation failures without leaving the query state permanently locked.

## Query semantics / spam-to-join compatibility

- Added an explicit key-presence helper for query packets.
- Preserves the original `sappflags` presence test used by `chimera_spam_to_join`.
- Keeps the hardened missing-key string contract without changing the intended feature behavior.

## INI correctness

- Fixed `Ini::set_value()` paths that updated an existing key and then still appended a
  duplicate key.
- Added null key/value validation.
- Existing entries are now replaced rather than duplicated.

## UTF conversion

- Corrected UTF-8 -> wide -> ANSI buffer sizing.
- Uses matching explicit source lengths in both Windows conversion calls.
- Checks conversion return values.

## Tag / map-data safety

- Fixed tag-index off-by-one checks (`index == tag_count` is no longer accepted).
- Added null checks around scenario/tag lookup paths.
- Hardened `TagBlock` address/count/index handling.
- Added defensive tag-data region/range checks before returning pointers.
- Avoids dereferencing invalid map/tag structures where the safe result is to reject them.

## Downloader callback/cancellation correctness

- Uses the complete cURL write size (`size * nmemb`) with overflow checks.
- Hardened shared state with RAII locking.
- Improved write-buffer capacity validation.
- The transfer callback can abort when a real cancel state is requested instead of always
  returning success.
- Keeps normal downloads and mirror behavior intact.

Runtime result:
- Stage A compiled successfully and passed bookmark/history, server switching and map
  download/runtime testing.

---

# Map / Memory Safety and Optimization

Files:
- `src/chimera/map_loading/compression.cpp`
- `src/chimera/map_loading/fast_load.cpp`
- `src/chimera/map_loading/map_loading.cpp`

## Zstd / compressed map validation

- Validates null/empty input/output parameters.
- Uses non-throwing filesystem size lookup.
- Tracks the decompressed size declared by the compressed map header.
- Rejects output that exceeds the declared map size.
- Requires final output size to match the declared decompressed size.
- Preserves streaming and multi-frame Zstd handling.
- Rejects truncated/incomplete streams.
- Protects output-position arithmetic from overflow.
- Ensures file cleanup on all exception paths.

## Fast map-list safety

- Case-insensitive string comparison is null-safe.
- Stock-map CRC lookup is null-safe.
- Custom Edition CRC callback validates map entry, CRC availability and map-list pointers.
- Map entries require a regular file, not merely a filesystem object that exists.
- Directory entry checks use `std::error_code`.

## CRC memory optimization

The map CRC processing order remains:

1. BSP data
2. model vertex data
3. tag data

The BSP/model portions are now read through a fixed 64 KiB working buffer instead of allocating
an entire BSP and an entire model-vertex section at once.

Benefits:
- lower transient memory usage;
- no change to the incremental CRC byte order;
- large corrupted size fields are rejected before reading.

Tag data remains available as a validated working region because scenario/BSP metadata requires
random access while constructing the CRC ranges.

## Map-name / read bounds

- Map names are validated before normalization/copying.
- Map names must be NUL-terminated inside Halo's 32-byte name field.
- Lowercase map-name buffers are zero initialized.
- File seek offsets are checked before narrowing to `long`.
- Map RAM reads are bounded by `decompressed_size`.
- `OVERLAPPED`, output pointer and path retrieval are validated in the map-read hook.
- Preload cursor arithmetic is checked before advancing or writing.

## Map memory-buffer correctness

- `LoadedMap::buffer_size` remains the capacity of the map-memory region.
- `loaded_size` tracks the amount actually occupied.
- Prevents repeated preloading from treating already-reduced remaining capacity as a new endpoint.

## Configuration correction

- `memory.max_tmp_files` is treated as a count of files.
- It no longer passes through the MiB conversion helper, which previously could turn a default
  count of `3` into `3 * 1024 * 1024`.

## Miscellaneous

- Guards download percentage calculation when total size is still zero.
- Uses safer filesystem size/error handling when loading maps.

Runtime result:
- Stage B compiled successfully and passed stock/custom map loading, server switching and
  map/runtime testing.

---

# Hook Engine Safety

Files:
- `src/chimera/signature/hook.cpp`
- `src/chimera/signature/hook.hpp`

The x86 decoder is intentionally limited to encodings Chimera knows how to copy safely.
Stage C does not guess the size of unknown x86 instructions.

## Fail-safe decoder

- Replaced decoder `std::terminate()` paths with clean decode failure.
- An unsupported instruction now prevents that individual hook from being installed instead of
  terminating the Halo process.
- Added decoder no-progress detection so an unhandled sub-form cannot repeatedly decode the same
  address forever.
- Clears decoder output before each decode attempt.

## Hook input/state validation

- Initializes `Hook::address` to `nullptr`.
- Adds null checks for hook targets/functions/output pointers.
- Rollback only writes original bytes when there is a valid saved address.

## Trampoline allocation / construction

- Uses `new(std::nothrow)` for trampoline allocation.
- Handles temporary vector/allocation failures.
- Cleans hook state on failed allocation or protection changes.
- Builds/validates relocated original instructions before overwriting Halo's target bytes.
- Prevents a relocation failure from leaving a partially installed target patch.

## Memory protection

- Keeps `VirtualProtect()` validation around generated executable code and Halo target bytes.
- Retains instruction-cache flushing after generated or patched executable code.
- Existing working relative-branch relocation support remains unchanged.

Runtime result:
- Stage C compiled successfully and Halo CE startup, maps, server joins, bookmarks and normal
  hooked functionality passed runtime testing.

---
