# Static HTTPS Server Example

A high-performance HTTPS static file server using Caduvelox with kTLS support.

## Features

- HTTPS with kernel TLS (kTLS) offload
- Zero-copy file serving
- Path traversal protection
- MIME type detection
- Graceful shutdown

## Building

```bash
# First, build the main caduvelox library
cd ../../build
cmake ..
make

# Then build this example
cd ../examples/static_https_server
mkdir build && cd build
cmake ..
make
```

## Running

```bash
# The example includes a static_site directory with sample content
# Self-signed certificates are automatically generated during build
./static_https_server

# Test with curl (use -k to accept self-signed cert)
curl -k https://localhost:8443/
curl -k https://localhost:8443/files/test.txt
```

## Configuration

- **Port**: 8443 (default)
- **Document Root**: `static_site/` (included in this example)
- **TLS**: Self-signed certificates (`test_cert.pem` and `test_key.pem`) are automatically generated during build

## Security Notes

- Path traversal attempts are blocked
- Only serves files within the document root
- Uses kTLS for hardware-accelerated encryption when available
