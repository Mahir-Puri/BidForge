# Dependencies

CMake FetchContent downloads dependencies into `build/_deps/`. Generated build
directories are ignored by Git, so dependency source does not appear as project changes.
Versions and SHA-256 hashes are pinned in `cmake/Dependencies.cmake`.

| Dependency | Version | License |
|---|---|---|
| [cpp-httplib](https://github.com/yhirose/cpp-httplib/tree/v0.18.3) | 0.18.3 | [MIT](https://github.com/yhirose/cpp-httplib/blob/v0.18.3/LICENSE) |
| [nlohmann/json](https://github.com/nlohmann/json/tree/v3.11.3) | 3.11.3 | [MIT](https://github.com/nlohmann/json/blob/v3.11.3/LICENSE.MIT) |
| [GoogleTest](https://github.com/google/googletest/tree/v1.15.2) | 1.15.2 | [BSD-3-Clause](https://github.com/google/googletest/blob/v1.15.2/LICENSE) |

The first configure needs network access. Existing build directories reuse their
downloaded sources; a new build directory downloads its own copies. GoogleTest is
downloaded only when `BUILD_TESTING` is enabled (the default). Go has no third-party
dependencies.

For offline configuration with existing dependency sources, CMake supports
`FETCHCONTENT_SOURCE_DIR_HTTPLIB`, `FETCHCONTENT_SOURCE_DIR_JSON`, and
`FETCHCONTENT_SOURCE_DIR_GOOGLETEST`. The first two directories must contain
`httplib.h` and `json.hpp`, respectively. Local source overrides bypass download
hash verification; use sources you have already verified.

The inspected cpp-httplib API uses `ThreadPool(worker_count, max_queued_requests)`.
Its queue contains accepted connections, not individual keep-alive requests.
On enqueue failure it closes the socket; there is no overload-response callback
in this version. Dependency pins provide reproducibility, not a security audit.
