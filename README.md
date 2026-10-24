# WebSocket messages

RFC 6455 §5.4 messages, including the byte stream. The codec is an internal
dependency; do not call it. API in [include/wsm.h](include/wsm.h).

No UTF-8, close handshake, RSV or opcode policy, auto-pong, HTTP, or sockets.

## Build

Hosted (`-DWSM_HOSTED=ON` required on every configure; the cache keeps the last value otherwise):

```sh
cmake -B build -DWSM_HOSTED=ON && cmake --build build && ctest --test-dir build
```

Freestanding:

```sh
cmake -B build -DWSM_HOSTED=OFF && cmake --build build && ctest --test-dir build
```

See [CMakeLists.txt](CMakeLists.txt) and [tests/CMakeLists.txt](tests/CMakeLists.txt) for
options and dependencies.

## Style

Run the [build](#build) step first when `compile_commands.json` is missing or stale.

`--experimental-custom-checks` is required for `CustomChecks` in `.clang-tidy`.

To fix formatting, rerun the clang-format command below with `-i` in place of
`--dry-run --Werror`.

```sh
clang-format-23 --dry-run --Werror $(find include src examples tests -type f -name '*.[ch]' | sort)
clang-tidy-23 --experimental-custom-checks $(jq -r '.[].file' compile_commands.json | grep -E '/(src|include|tests|examples)/' | grep -v '/_deps/' | sort -u)
```
