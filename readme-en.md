# `redis_xnet` - A Cross-Platform Network Programming Framework
`redis_xnet` is a cross-platform network programming framework adapted from Redis's `ae` event-driven library. It integrates core functionalities such as event loops, multithreading, coroutines, and logging, and is mainly used to build high-performance network services. Below is a detailed introduction:

## I. Core Positioning
`redis_xnet` is derived from Redis's `ae` (Async Event) event loop library. It supports multiple platforms and extends features like multithreading and coroutines, making it suitable for developing high-performance, asynchronous network servers or client programs.

## II. Core Components and Features
### 1. **Event-Driven Model (Based on ae Event Loop)**
The event loop is the core of the entire library, responsible for managing I/O events and timed events, similar to Redis's event handling mechanism.
- **Core Structure**: `aeEventLoop` is the main body of the event loop, maintaining registered file events, time events, multiplexer data, etc.
- **File Events**: Supports registering/deleting read/write events (`AE_READABLE`/`AE_WRITABLE`), managed via `aeCreateFileEvent` and `aeDeleteFileEvent`, used to handle Socket I/O operations (e.g., server accepting connections, reading/writing data).
- **Time Events**: Supports registering timed tasks (`aeCreateTimeEvent`), which execute callback functions after a specified number of milliseconds, used to implement scheduled tasks, timeout detection, etc.
- **Multiplexing Adaptation**: Automatically selects the optimal multiplexing mechanism (`epoll` for Linux, `kqueue` for BSD, `IOCP` or `ws2` for Windows, with `select` as the fallback by default), ensuring cross-platform compatibility.

### 2. **Cross-Platform Support**
- The code extensively uses `_WIN32` conditional compilation with special handling for the Windows system:
  - Thread-local storage uses `_declspec(thread)` (Windows) and `__thread` (Linux).
  - Time retrieval adaptation: `GetSystemTimeAsFileTime` for Windows and `gettimeofday` for Linux.
  - Network I/O adaptation: Windows supports `IOCP` (Overlapped I/O) and `ws2`, while Linux supports `epoll`, etc.
- Ensures efficient operation of event-driven network programs in the Windows environment.

### 3. **Multithreading and Task Scheduling**
Provides thread pool and task management capabilities through the `xthread` component:
- **Task Structure**: `xthrTask` supports two task types:
  - Normal Task (`XTHR_TASK_NORMAL`): Executes a specified function with parameters.
  - Resume Task (`XTHR_TASK_RESUME`): Used for RPC callbacks or coroutine resumption, associated with a wait ID and result data.
- **Inter-thread Communication**: Transmits `xthrTask` through a task queue, supporting cross-thread function calls (e.g., `xthread_pcall` in the demo implements RPC calls).
- Suitable for offloading time-consuming operations (e.g., computation, Redis access) to worker threads to avoid blocking the event loop.

### 4. **Coroutine Support (Asynchronous Programming Enhancement)**
Provides coroutine capabilities through the `xcoroutine` component to simplify asynchronous code writing:
- **Coroutine Tasks**: `xCoroTask` is based on the C++ 20 coroutine standard, supporting the `co_await` syntax, which allows writing asynchronous operations (e.g., network I/O, RPC calls) in a synchronous style.
- **Exception Handling**: Built-in exception capture mechanism that supports handling C++ standard exceptions (e.g., `std::runtime_error`) and hardware exceptions (e.g., memory access violations, division by zero), and provides interfaces for querying and handling exception information (`has_any_exception`, `handle_exception`).
- **Lifecycle Management**: Manages coroutine states through `promise_type`, supporting coroutine creation, resumption, destruction, and exception cleanup.

### 5. **Basic Network Service Framework**
- **Server Example**: `demo/xrpc_server.cpp` demonstrates a simple RPC server implementation based on event loops and coroutines. It listens on port 8888, registers three protocol handling functions to process different requests, and includes logging, error handling, and connection closure callbacks.
- **Data Serialization**: `xpack.h` defines `XPackBuff` for data packing/unpacking, supporting secure transmission of byte streams (between threads or over the network) and avoiding memory copy issues.

### 6. **Logging System**
- Provides basic logging functionality (`xlog.c`), supporting the configuration of log file paths (`xlog_set_file_path`), log levels, thread name display, etc., for easy debugging and operation.
- Supports dual output to the console and files, with configurable timestamps and color display to enhance readability.

## III. Typical Usage Scenarios
- High-performance network servers (e.g., Redis-style cache servers, custom protocol servers).
- Asynchronous I/O-intensive applications (e.g., high-concurrency clients, proxy services).
- Complex asynchronous task scheduling that combines multithreading and coroutines (e.g., distributed task processing).

## Summary
`redis_xnet` is a network programming library centered on event-driven architecture, integrating multithreading, coroutines, and cross-platform adaptation. It retains the efficient event loop characteristics of Redis's `ae` library while extending modern asynchronous programming capabilities (coroutines, thread pools), making it suitable for developing high-performance and maintainable network applications.
