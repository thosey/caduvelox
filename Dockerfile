FROM ubuntu:24.04 AS builder
ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y --no-install-recommends \
    cmake ninja-build build-essential git pkg-config \
    libssl-dev zlib1g-dev libgtest-dev ca-certificates wget curl libelf-dev \
 && rm -rf /var/lib/apt/lists/*

# Prefer distro liburing if recent enough, otherwise build a modern liburing from source.
RUN apt-get update && apt-get install -y --no-install-recommends liburing-dev || true
RUN bash -lc '\
  if ! pkg-config --atleast-version=2.1 liburing 2>/dev/null; then \
    echo "Building liburing from source..."; \
    cd /tmp; git clone https://github.com/axboe/liburing.git; cd liburing; \
    git checkout liburing-2.9 || true; ./configure --prefix=/usr; make -j"$(nproc)"; make install; ldconfig; \
  else \
    echo "System liburing is recent enough"; \
  fi'

WORKDIR /src
COPY . /src

# Configure and build the project
RUN cmake -S /src -B /build -G Ninja -DCMAKE_BUILD_TYPE=Release \
 && cmake --build /build -- -j"$(nproc)"

FROM ubuntu:24.04 AS runtime
ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
    ca-certificates libssl3 liburing2 || true \
 && rm -rf /var/lib/apt/lists/*

# Copy the built example executables from the builder stage.
# The static HTTPS server remains the default entrypoint.
COPY --from=builder /build/rest_api_server /usr/local/bin/rest_api_server
COPY --from=builder /build/static_https_server /usr/local/bin/static_https_server

# Optional test certs (examples). Remove if you don't want them in the image.

COPY --from=builder /src/tests/test_cert.pem /etc/ssl/certs/test_cert.pem
COPY --from=builder /src/tests/test_key.pem /etc/ssl/private/test_key.pem

# Copy entrypoint script and make executable
COPY --from=builder /src/scripts/docker-entrypoint.sh /usr/local/bin/docker-entrypoint.sh
RUN chmod +x /usr/local/bin/docker-entrypoint.sh || true

RUN apt-get update && apt-get install -y --no-install-recommends openssl || true && rm -rf /var/lib/apt/lists/*

RUN chmod +x /usr/local/bin/static_https_server || true

# Default to 8443 since this image runs the HTTPS example
EXPOSE 8443

ENTRYPOINT ["/usr/local/bin/docker-entrypoint.sh"]
