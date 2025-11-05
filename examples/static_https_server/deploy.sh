#!/bin/bash
set -e

echo "Deploying Caduvelox systemd service..."

# Fix SELinux context on binary (required on Fedora/RHEL)
if command -v chcon &> /dev/null && [ -f /usr/sbin/getenforce ]; then
    if [ "$(/usr/sbin/getenforce)" != "Disabled" ]; then
        echo "Setting SELinux context on binary..."
        sudo chcon -t bin_t build/static_https_server
    fi
fi

# Copy service file to systemd directory
sudo cp caduvelox.service /etc/systemd/system/

# Reload systemd to recognize the new service
sudo systemctl daemon-reload

# Reset any previous failures
sudo systemctl reset-failed caduvelox 2>/dev/null || true

# Enable service to start on boot
sudo systemctl enable caduvelox

# Start the service
sudo systemctl start caduvelox

echo "✅ Service deployed and started!"
echo ""
echo "Useful commands:"
echo "  sudo systemctl status caduvelox   # Check status"
echo "  sudo systemctl restart caduvelox  # Restart service"
echo "  sudo systemctl stop caduvelox     # Stop service"
echo "  sudo journalctl -u caduvelox -f   # Follow logs"
echo "  sudo journalctl -u caduvelox -n 100  # Last 100 log lines"
echo "  tail -f /var/log/caduvelox.log    # Follow application logs"
