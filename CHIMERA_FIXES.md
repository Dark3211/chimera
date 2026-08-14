# Chimera Hardening Changes / Fixes

## `src/chimera/bookmark/bookmark.cpp`

- Reworked server address parsing.
- Added validation for null and empty addresses.
- Added validation for malformed ports.
- Rejects port `0` and ports above `65535`.
- Added proper `[IPv6]:port` parsing.
- Preserved default port `2302` when no port is specified.
- Added password length validation.
- Prevented null password pointers from being passed to formatting functions.
- Switched server lookup to `AF_UNSPEC`.
- Added IPv4 and IPv6 DNS resolution support.
- Tries all valid `getaddrinfo()` results instead of relying on a single address.
- Added socket creation error handling.
- Added `setsockopt()` error handling.
- Added `sendto()` error handling.
- Added `recvfrom()` error handling.
- Added UDP query timeout handling.
- Added safe response buffer termination.
- Added minimum response-size validation.
- Added socket cleanup on failure paths.
- Sanitizes control characters received from server query responses.
- Fixed bookmark edits not being written back correctly to `bookmark.txt`.
- Improved generated `connect` command formatting.
- Added quoting for server addresses.
- Added correct formatting for IPv6 addresses.
- Added explicit port handling.
- Added escaping for `\` in passwords.
- Added escaping for `"` in passwords.
- Added generated command length checks.

## `src/chimera/bookmark/bookmark.hpp`

- Added safe default initialization for `QueryPacketDone`.
- Initializes `timed_out`.
- Initializes `error`.
- Initializes `ping`.
- Made `get_data_for_key()` `const`.
- Made `get_data_for_key()` `noexcept`.
- Added null-key handling.
- Returns an empty string instead of `nullptr` when a key does not exist.
- Uses `c_str()` for returned string data.

## `src/chimera/chimera.cpp`

- Replaced exception-prone filesystem directory creation with `std::error_code` variants.
- Uses `create_directories()` where parent directories may not exist.
- Added safer creation of the Chimera data directory.
- Added safer creation of `chimera/tmp`.
- Added safer creation of downloaded-map directories.
- Added safer creation of map directories.
- Prevents filesystem permission/path errors from unexpectedly throwing through `noexcept` code.

## `src/chimera/event/connect.cpp`

- Added null-password protection in the pre-connect hook.
- Prevents invalid memory access when Halo provides a null password pointer.

## `src/chimera/event/tick.cpp`

- Added `VirtualProtect()` result validation.
- Initialized memory-protection state variables.
- Fixed memory protection size to use the target value size instead of pointer size.
- Prevents writing tick-rate memory when protection changes fail.

## `src/chimera/halo_data/port.cpp`

- Added validation for `halo.client_port`.
- Added validation for `halo.server_port`.
- Rejects negative values.
- Rejects values above `65535`.
- Prevents silent integer truncation to `uint16_t`.

## `src/chimera/map_loading/compression.cpp`

- Added minimum compressed-file size validation.
- Added header read validation.
- Added fallback handling for standard and demo map headers.
- Added `ZSTD_createDStream()` failure handling.
- Added RAII cleanup for `ZSTD_DStream`.
- Added `ZSTD_initDStream()` error validation.
- Fixed streaming decompression state handling.
- Preserves unconsumed compressed bytes between decompression calls.
- Correctly tracks `input_buffer.pos`.
- Added truncated-stream detection.
- Added incomplete-frame detection.
- Added EOF validation.
- Added output callback failure handling.
- Ensures the compressed file is closed on success and failure paths.
- Prevents corrupted or truncated `.map` data from being treated as valid output.

## `src/chimera/map_loading/fast_load.cpp`

- Replaced exception-prone filesystem operations with `std::error_code` variants.
- Added safer map existence checks.
- Added safer file validation.
- Added safer directory iteration.
- Prevents filesystem errors from unexpectedly terminating Halo.

## `src/chimera/map_loading/map_loading.cpp`

- Added null map validation before CRC processing.
- Added minimum map-size validation.
- Added safer file-open handling.
- Added RAII cleanup for map file handles.
- Added validated seek operations.
- Added validated read operations.
- Added bounds checking for RAM-preloaded map reads.
- Added map-header validation.
- Rejects unknown or invalid map headers.
- Added safe virtual-address-to-tag-data offset conversion.
- Added bounds checks for tag data.
- Added bounds checks for scenario data.
- Added bounds checks for BSP tables.
- Added BSP count validation.
- Added BSP offset validation.
- Added BSP size validation.
- Added model-data offset validation.
- Added model-data size validation.
- Added safer map CRC calculation.
- Added safer resource-file reads.
- Added validation for asset precaching reads.
- Prevented `ui.map` preload buffer underflow.
- Added safer handling when `ui.map` exceeds remaining preload capacity.
- Replaced exception-prone downloaded-map rename operations with `std::error_code`.
- Added failure handling when moving downloaded maps fails.
- Removes failed temporary map files when appropriate.
- Prevents continuing to join a server when a required downloaded map could not be installed.
- Added generic map read validation.
- Added resource seek/read validation.

## `src/chimera/output/draw_text.cpp`

- Replaced exception-prone font-directory filesystem checks with `std::error_code`.
- Prevents filesystem errors from escaping before font loading error handling.

## `src/chimera/signature/hook.cpp`

- Added relocation support for copied relative x86 instructions.
- Added relocation support for `CALL rel32` (`E8`).
- Added relocation support for `JMP rel32` (`E9`).
- Added relocation support for near conditional jumps (`0F 80`–`0F 8F`).
- Added relocation support for short `JMP` (`EB`).
- Added relocation support for short conditional jumps (`70`–`7F`).
- Added source-to-trampoline instruction offset mapping.
- Preserves branches targeting instructions inside the copied block.
- Expands short `JMP` instructions when necessary.
- Expands short conditional jumps when necessary.
- Added accurate relocated trampoline-size calculation.
- Added `VirtualProtect()` failure handling when creating hooks.
- Added cleanup when executable-memory protection changes fail.
- Added `FlushInstructionCache()` after patching original code.
- Added `FlushInstructionCache()` after generating trampoline code.

## `src/chimera/signature/hook.hpp`

- Added null pointer validation to generic memory overwrite operations.
- Added null data validation.
- Added zero-length validation.
- Added `VirtualProtect()` failure handling.
- Restores the original page protection after writes.
- Added `FlushInstructionCache()` after modifying executable memory.

## `src/map_downloader/map_downloader.cpp`

- Added protection against empty delimiters in `split()`.
- Added mutex protection for shared downloader state.
- Copies download configuration under lock before worker-thread use.
- Added validation for `curl_easy_escape()`.
- Added cleanup when URL escaping fails.
- Replaced regex-based normal placeholder replacement with literal replacement.
- Prevents server/password characters from being interpreted as regex replacement syntax.
- Preserved mirror placeholder support.
- Improved per-mirror state reset.
- Closes failed mirror file handles before trying another mirror.
- Resets download counters between mirrors.
- Resets download timing between mirrors.
- Reopens the temporary file cleanly for each mirror attempt.
- Added `fopen()` failure handling.
- Added `fwrite()` failure handling.
- Detects partial/failed disk writes.
- Added `curl_easy_init()` failure handling.
- Added validation for critical `curl_easy_setopt()` calls.
- Added `CURLOPT_FAILONERROR`.
- Added redirect handling.
- Added connection timeout.
- Added total download timeout.
- Added explicit User-Agent handling.
- Added worker-thread exception handling.
- Prevents uncaught worker exceptions from reaching `std::terminate()`.
- Added cleanup of cURL resources on worker failure.
- Added cleanup of temporary file handles on worker failure.
- Added cleanup of temporary download buffers on worker failure.
- Added temporary-file removal on failed downloads when appropriate.
- Added `joinable()` validation before joining the downloader thread.
- Prevents `std::system_error` from joining a non-joinable thread.
- Added exception handling around `std::thread` creation.
- Added cleanup when thread creation fails.
- Added mutex protection to `set_server_info()`.
- Preserved server and password update functionality.
- Improved write callback capacity validation.
- Improved download failure-state handling.
