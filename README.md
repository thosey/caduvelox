# Caduvelox

[![Linux CI](https://github.com/thosey/caduvelox/actions/workflows/ci.yml/badge.svg)](https://github.com/thosey/caduvelox/actions/workflows/ci.yml)

<p align="center">
  <img src="logo/caduvelox-logo-full-color.png" alt="Caduvelox" width="360">
</p>

C++ HTTP/HTTPS framework built on io_uring, kernel TLS (kTLS), and thread-local memory pools.

## Features

- **io_uring** with multi-shot operations for async I/O without syscall overhead
- **Kernel TLS (kTLS)** for hardware-accelerated HTTPS encryption
- **Zero-copy file serving** via splice(2) - no userspace buffer copies
- **Thread-local memory pools** - no locking on the hot path. Pools belong to the
  ring thread that owns them, so a job must be allocated and freed on the same
  thread; nothing crosses threads
- **API**: Express-style HTTP routing + low-level Job API

## Quick Start

### 1. Install Dependencies

**Ubuntu 24.04:**
```bash
sudo apt install -y build-essential cmake pkg-config \
    liburing-dev libssl-dev libgtest-dev
```

**Fedora/RHEL:**
```bash
sudo dnf install -y gcc-c++ cmake pkg-config \
    liburing-devel openssl-devel gtest-devel
```

### 2. Clone & Build

```bash
git clone https://github.com/thosey/caduvelox.git
cd caduvelox

mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
make -j$(nproc)
```

### 3. Run Tests

```bash
./tests/caduvelox_tests
```

All tests should pass on Linux 6.0+ with kTLS support. On older kernels, kTLS tests will be skipped.

### 4. Build Examples

```bash
cd ..
cmake -S . -B build -DBUILD_EXAMPLES=ON
cmake --build build -j$(nproc)
```

**Example Static HTTPS server:**
```bash
cd build/examples/static_https_server
./static_https_server
# Visit https://localhost:8443 (accepts self-signed cert warning)
```

**Example REST API server:**
```bash
cd build/examples/rest_api_server
./rest_api_server
# Visit http://localhost:8080
```

## Usage Example

```cpp
#include "caduvelox/ServerConfig.hpp"
#include "caduvelox/http/HttpServer.hpp"

int main() {
    caduvelox::ServerConfig cfg;   // cfg.num_rings = 0 means one ring per core
    caduvelox::HttpServer http(cfg);

    // Routes must be registered before listening: each ring takes a copy of the
    // router when it starts.
    http.addRoute("GET", R"(^/$)", [](const auto& req, auto& res) {
        res.html("<h1>Hello, Caduvelox!</h1>");
    });

    http.addRoute("GET", R"(^/api/status$)", [](const auto&, auto& res) {
        res.json(R"({"status":"ok"})");
    });

    // A capture arrives percent-decoded and without the query string.
    http.addRouteWithCaptures("GET", R"(^/files/(.+)$)",
        [](const auto& req, auto& res, const std::smatch& m) {
            res.sendFile("/srv/www/" + m[1].str());
        });

    if (!http.listen(8080)) {        // listenKTLS(...) to terminate TLS here
        return 1;
    }

    http.run();                       // blocks until stop()
    return 0;
}
```

Use `res.sendFile(path)` for zero-copy file serving. Routes accept ECMAScript regular
expressions, matched against the decoded path with the query string removed.

### HTTP/1.1 feature boundary

Deliberately a subset. What is **not** implemented is worth knowing before you build on it:

| | |
|---|---|
| `Transfer-Encoding` | Not supported in any form. A request carrying it is answered `400` — this server applies no transfer codings, so honouring the header would mean framing a body two different ways |
| `Range` requests | A **single** `bytes` range is served as `206 Partial Content`, in all three forms (`bytes=0-499`, `bytes=500-`, `bytes=-500`); file responses advertise `Accept-Ranges: bytes` and carry `Last-Modified`. A multi-range request gets the whole file rather than a `multipart/byteranges` body, and `If-Range` is not honoured |
| `Expect: 100-continue` | Not implemented |
| `HEAD` | Not special-cased; a `HEAD` route is an ordinary route and its body is sent |
| Pipelining | Supported, but strictly serialised: one response in flight per connection, and request *k+1* is not parsed until response *k* is on the wire |
| Paths | Percent-decoded one segment at a time. An escape that would produce `/` or a `..` segment is refused with `400`; a literal `..` reaches the handler |
| File serving | Regular files only. Confining a path to a document root is the caller's decision — see `examples/static_https_server` |
| Request limits | 8 KiB request line, 16 KiB per header line, 200 headers, 1 GiB declared body. Over-long requests get `431`/`413`; malformed ones get `400` |
| Logging | Per-request tracing is off by default. `caduvelox::Logger::setLevel(caduvelox::LogLevel::Debug)` turns it on; it costs roughly 15% of a request's CPU |

## Architecture

**Core Components:**
- **`Server`** - io_uring event loop and job scheduler
- **`HttpServer`** - HTTP routing and request/response handling
- **Jobs** - Composable io_uring operations:
  - `AcceptJob` - Accept connections (multi-shot)
  - `MultiShotRecvJob` - Receive data (multi-shot)
  - `WriteJob` - Send data
  - `SpliceFileJob` - Zero-copy file transfer
  - `KTLSJob` - Kernel TLS setup

## Requirements

- **Linux kernel**: 6.0+. Multi-shot recv is required, not optional, and landed in
  6.0; provided buffer rings need 5.19. Developed and tested on 7.x
- **Compiler**: C++20 (GCC 10+, Clang 12+)
- **liburing**: 2.1+
- **OpenSSL**: 3.0+ with kTLS support
- **CMake**: 3.10+

## kTLS Notes

Kernel TLS requires:
- Kernel 6.0+, as above (kTLS itself is older, but the framework's floor is higher)
- `CONFIG_TLS=y` or `CONFIG_TLS=m` in kernel config
- OpenSSL built with KTLS support

Verify kTLS availability:
```bash
# Check kernel config
zgrep TLS /proc/config.gz

# Check OpenSSL support
openssl version -a | grep ktls
```

If kTLS is unavailable, the framework still works for HTTP (non-TLS) servers.

## Examples

Two complete examples demonstrate the framework:

**Static HTTPS Server** (`examples/static_https_server/`)
- Serves files with kTLS encryption
- Auto-generates self-signed certificates
- Zero-copy file transfers via splice

**REST API Server** (`examples/rest_api_server/`)
- JSON CRUD API
- HTTP routing
- In-memory data store

## Contributing

Issues and pull requests welcome! Please ensure tests pass before submitting PRs.

## License

MIT License - see [LICENSE](LICENSE) for details.
