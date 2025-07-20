# Caduvelox

[![Linux CI](https://github.com/thosey/caduvelox/actions/workflows/ci.yml/badge.svg)](https://github.com/thosey/caduvelox/actions/workflows/ci.yml)

<p align="center">
  <img src="logo/caduvelox-logo-full-color.png" alt="Caduvelox" width="360">
</p>

C++ HTTP/HTTPS framework built on io_uring, kernel TLS (kTLS), and lock-free architecture.

## Features

- **io_uring** with multi-shot operations for async I/O without syscall overhead
- **Kernel TLS (kTLS)** for hardware-accelerated HTTPS encryption
- **Zero-copy file serving** via splice(2) - no userspace buffer copies
- **Lock-free memory pools** - zero mutex contention in hot path
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
git submodule update --init --recursive

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
#include "caduvelox/http/HttpServer.hpp"
#include "caduvelox/Server.hpp"

int main() {
    caduvelox::Server io_server;
    io_server.init(256);
    
    caduvelox::HttpServer http(io_server);

    // Add routes
    http.addRoute("GET", R"(^/$)", [](const auto& req, auto& res) {
        res.html("<h1>Hello, Caduvelox!</h1>");
    });

    http.addRoute("GET", R"(^/api/status$)", [](const auto&, auto& res) {
        res.json(R"({"status":"ok"})");
    });

    // Listen and run
    if (!http.listen(8080)) {
        return 1;
    }
    
    io_server.run();
    return 0;
}
```

Use `res.sendFile(path)` for zero-copy file serving. Routes accept ECMAScript regular expressions.

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

- **Linux kernel**: 5.19+ (6.0+ recommended for multi-shot operations)
- **Compiler**: C++20 (GCC 10+, Clang 12+)
- **liburing**: 2.1+
- **OpenSSL**: 3.0+ with kTLS support
- **CMake**: 3.10+

## kTLS Notes

Kernel TLS requires:
- Kernel 5.15+ (6.2+ recommended for zero-copy sendfile)
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

## Acknowledgments

Uses [lock-free-memory-pool](https://github.com/thosey/lock-free-memory-pool) (MIT License).
