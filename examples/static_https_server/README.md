# Static HTTPS Server Example

A high-performance HTTPS static file server using Caduvelox with kTLS support.

## Features

- HTTPS with kernel TLS (kTLS) offload
- Zero-copy file serving with splice()
- Path traversal protection
- Automatic MIME type detection
- Configurable logging (console or file)
- Log rotation support (SIGHUP)
- Graceful shutdown

## Building

### Option 1: As part of main project (recommended for development)

```bash
cd /path/to/caduvelox
mkdir build && cd build
cmake .. -DBUILD_EXAMPLES=ON
make static_https_server
```

Binary will be in `build/examples/static_https_server/static_https_server`

### Option 2: Standalone build (recommended for your own projects)

First, build the main caduvelox library:

```bash
cd /path/to/caduvelox
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
make
```

Then build this example standalone:

```bash
cd examples/static_https_server
mkdir build && cd build
cmake ..
make
```

**This demonstrates how to link your own project against libcaduvelox.a!**

## Usage

```bash
./static_https_server [document_root] [port] [log_file]
```

**Arguments:**
- `document_root` - Directory to serve files from (default: `static_site`)
- `port` - HTTPS port to listen on (default: `8443`)
- `log_file` - Path to log file (optional, uses console if omitted)

**Examples:**

```bash
# Development with console logging
./static_https_server static_site 8443

# Production with file logging
./static_https_server /var/www/html 443 /var/log/caduvelox/app.log

# Rotate logs (when using file logging)
kill -HUP $(pidof static_https_server)
```

## Testing

```bash
# Start server
./static_https_server

# In another terminal (use -k to accept self-signed cert):
curl -k https://localhost:8443/
curl -k https://localhost:8443/files/test.txt
```

## TLS Certificates

The build automatically generates self-signed certificates for testing:
- `test_cert.pem` - Test certificate  
- `test_key.pem` - Test private key

For production, set environment variables:

```bash
export CERT_PATH=/path/to/your/cert.pem
export KEY_PATH=/path/to/your/key.pem
./static_https_server
```

## Log Rotation with logrotate

Create `/etc/logrotate.d/caduvelox`:

```
/var/log/caduvelox/*.log {
    daily
    rotate 7
    compress
    delaycompress
    missingok
    notifempty
    postrotate
        killall -HUP static_https_server 2>/dev/null || true
    endscript
}
```

## Integrating into Your Own Project

This example serves as a template for building Caduvelox-based applications:

1. Copy `CMakeLists.txt` to your project
2. Build caduvelox library: `cd caduvelox/build && cmake .. && make`
3. Update `CADUVELOX_ROOT` in CMakeLists.txt if needed
4. Build your project: `mkdir build && cd build && cmake .. && make`

The CMakeLists.txt works both standalone and as part of the main build.

## Security Notes

- Path traversal attempts are blocked
- Only serves files within the document root
- kTLS for hardware-accelerated encryption when available
