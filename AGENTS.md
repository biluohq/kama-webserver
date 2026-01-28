## AGENTS GUIDE — `kama-webserver`

This file is for AI coding agents (and humans) working in this repository.
Follow it as the single source of truth for build commands, style, and
architecture expectations.

---

### 1. Project overview

- Language: C++20 (set in top-level `CMakeLists.txt`).
- Build system: CMake (>= 3.0).
- Platform: Linux (POSIX sockets, `epoll`, `eventfd`, `pthread`).
- Architecture: muduo-style Reactor with `EventLoop`, `Channel`, `Poller`,
  `TcpServer`, `TcpConnection`, async logging, memory pool, LFU cache.

Directory layout (root = `/home/biluohq/projects/kama-webserver`):

- `CMakeLists.txt`   — top-level CMake, global flags and subdirectories.
- `src/`             — core networking + server code, main executable.
- `log/`             — logging library implementation.
- `memory/`          — memory pool / allocator utilities.
- `include/`         — public headers for all modules.
- `bin/`             — runtime binaries output (created by build).
- `lib/`             — shared libraries output (created by build).
- `build/`           — out-of-source build directory (created by scripts).
- `logs/`            — runtime log files (created by the app).

There is currently **no dedicated `tests/` directory** and no unit test
framework wired into CMake.

---

### 2. Build & run commands

Preferred build flow (uses provided script):

```bash
cd /home/biluohq/projects/kama-webserver
./build.sh
```

`build.sh` does the following:

- `rm -rf build bin logs`
- `mkdir build && cd build`
- `cmake -DCMAKE_POLICY_VERSION_MINIMUM=3.5 ..`
- `make -j $(nproc)`

Manual build (if you need more control):

```bash
cd /home/biluohq/projects/kama-webserver
mkdir -p build
cd build
cmake ..
make -j$(nproc)
```

Main executable:

- Target name: `main` (defined in `src/CMakeLists.txt`).
- Output path (after build): `bin/main` from repo root.

Run the server:

```bash
cd /home/biluohq/projects/kama-webserver
./bin/main
```

The server listens on port `8080` by default (see `main.cc`).

---

### 3. Test commands

Current status:

- There are **no `add_test()` calls** in any `CMakeLists.txt`.
- No testing framework (e.g. GTest, Catch2) is configured.
- No `tests/` or similar test directory is present.

Implications for agents:

- **Do not invent test commands** like `ctest` or `make test` — they will fail.
- When asked for a "single test" command, be explicit that there is **no
  automated test harness** in this repo right now.
- If you introduce tests in the future, prefer:
  - Add a `tests/` directory with its own `CMakeLists.txt`.
  - Use `add_executable(test_...)` plus `add_test(NAME ... COMMAND ...)`.
  - Document new test commands in this file when added.

Until tests exist, the closest thing to a "single test" is running the
appropriate executable directly, e.g. `./bin/main` or any future `test_*`
binary you add.

---

### 4. Tooling, linting, formatting

Discovered configuration:

- No `.clang-format`, `.clang-tidy`, or `.editorconfig` present.
- Style is inferred from existing `.cc` / `.h` files.

Formatting guidelines (match existing code):

- Indentation: 4 spaces, no tabs.
- Braces:
  - Function and class definitions: opening brace on the **same line**.
  - Control flow (`if`, `for`, `while`, etc.): opening brace on the same line.
- Spaces:
  - Space before `{` in control statements and function definitions.
  - No extra spaces before `;`.
- Lines:
  - Keep lines reasonably short (aim for ≤ 100–120 columns when possible).
  - Break long expressions across lines at logical boundaries.

If you auto-format, configure your tool to approximate the above and **do not
reformat unrelated code**. Keep edits minimal and localized.

---

### 5. Language level & dependencies

- C++ standard: **C++20** (`set(CMAKE_CXX_STANDARD 20)`).
- Required libraries:
  - `pthread` (set in global `LIBS` in root `CMakeLists.txt`).
- System calls and APIs:
  - POSIX sockets, `epoll`, `eventfd`, `readv`, `write`, `sendfile`, etc.

When adding new features:

- Prefer the existing STL and POSIX APIs instead of extra third-party
  dependencies.
- If a new dependency is truly required, update `CMakeLists.txt` explicitly and
  document it here.

---

### 6. Naming & file conventions

Files and classes:

- Source files: `.cc` extension (e.g., `TcpServer.cc`, `EventLoop.cc`).
- Headers: `.h` under `include/` (e.g., `TcpServer.h`, `EventLoop.h`).
- Classes / structs: `PascalCase` (e.g., `TcpConnection`, `AsyncLogging`).

Members, variables, and constants:

- Data members: `camelCase_` with trailing underscore (e.g., `loop_`,
  `outputBuffer_`).
- Local variables: lower `camelCase` or descriptive lower-case names.
- Constants: either `kCamelCase` (e.g., `kRollSize`, `kPollTimeMs`) or
  `ALL_CAPS` for macro-style constants.
- Thread-local helpers: `t_name` (e.g., `t_timer`, `t_lastSecond`).

Includes:

- System / standard headers: angle brackets, e.g. `#include <string>`.
- Project headers: this codebase often uses angle brackets as well, e.g.
  `#include <TcpServer.h>`, because `include/` is added to the include path.
  **Follow the existing pattern in the file you are editing.**

Namespaces:

- Minimal use of custom namespaces; most code is in the global namespace.
- If you introduce namespaces, keep them small and avoid deep nesting.

---

### 7. Error handling & logging

Logging is central. Key points:

- Use the existing `Logger` macros:
  - `LOG_TRACE`, `LOG_DEBUG`, `LOG_INFO`, `LOG_WARN`, `LOG_ERROR`, `LOG_FATAL`.
- `LOG_FATAL` will flush and abort the process; reserve it for truly unrecoverable
  conditions (e.g., failing to create the main listening socket).
- Prefer `LOG_ERROR` / `LOG_WARN` for recoverable failures.

Patterns to follow:

- Always log context with errors (e.g., file descriptor, address, errno).
- Functions that interact with system calls often take an `int *saveErrno`
  out-parameter; populate it exactly as existing code does.
- Avoid throwing exceptions through event-loop or callback boundaries; existing
  code uses return values and logging instead. If you must use exceptions,
  catch them at the boundary and log via `LOG_ERROR`.

Example style (from existing code):

```cpp
if (sockfd < 0)
{
    LOG_FATAL << "listen socket create err " << errno;
}
```

---

### 8. Concurrency, event loops, and callbacks

The server is built around a Reactor model:

- `EventLoop` wraps the poller (`epoll`) and pending functors.
- `TcpServer` manages listening sockets and connection distribution.
- `TcpConnection` wraps per-connection state and I/O buffers.
- `EventLoopThreadPool` creates sub-loops for multi-threaded I/O.

Guidelines for agents:

- **Never block inside callbacks** (e.g., connection or message callbacks).
  - Do not perform long-running or blocking IO there.
  - Offload heavy work to separate threads or use existing async patterns.
- Use `runInLoop` / `queueInLoop` to schedule work on a specific `EventLoop`
  instead of cross-thread direct calls.
- Preserve the high-water mark handling and backpressure logic in
  `TcpConnection` when modifying send / buffer code.

There is coroutine-based logic in `main.cc` (`Task sessionHandler(...)`). If you
extend it, keep the contract:

- Use `co_await asyncRead(conn)` and `co_await asyncDrain(conn)` rather than
  direct blocking reads/writes.
- Ensure the connection remains alive while the coroutine is active.

---

### 9. Memory management

- Custom memory pool and LFU cache live under `memory/` and are used from
  `main.cc` and elsewhere.
- Initialize and tear down memory-related components as shown in `main.cc`:
  - `memoryPool::HashBucket::initMemoryPool();`
  - LFU cache construction near startup.

Guidelines:

- When using the memory pool or cache, follow existing call patterns.
- Avoid bypassing the pool with ad-hoc `new`/`delete` if the module already has
  dedicated allocation helpers.

---

### 10. AI-specific notes (Cursor / Copilot / agents)

- Current repository state:
  - No `.cursor/rules/**` directory found.
  - No `.cursorrules` file found.
  - No `.github/copilot-instructions.md` file found.
  - This `AGENTS.md` is the primary guidance file for AI agents.

Agent behavior expectations:

- Obey the build and style rules in this file.
- When adding new tooling (formatters, linters, test frameworks), **also**
  update this file with the new commands and conventions.
- Prefer minimal, surgical changes that respect existing patterns over
  large-scale rewrites.

---

### 11. Checklist before you send a PR or patch

For any non-trivial change, agents should verify:

1. **Builds cleanly** using `./build.sh` or the manual CMake + `make` flow.
2. **No new warnings** are introduced if compiler warnings are enabled.
3. **Logging**: important branches and error paths log at appropriate levels.
4. **Reactor safety**: no new blocking operations in hot paths or callbacks.
5. **Style**: indentation, brace placement, and naming match nearby code.
6. **Docs**: if you add new commands, modules, or conventions, update this
   `AGENTS.md` accordingly.

If any of the above cannot be satisfied (e.g., build fails for unrelated
reasons), clearly document the limitation in your response.
