#!/bin/bash
set -e
API="https://gh-proxy.com/https://api.github.com/repos/bluenviron/mediamtx/releases/latest"
V=$(curl -s -m 30 "$API" | grep -oP '"tag_name":\s*"v[0-9.]+"' | head -1 | grep -oP 'v[0-9.]+')
echo "VERSION=$V"
cd ~ || exit 1
curl -sL -m 300 -o mediamtx.tar.gz "https://gh-proxy.com/https://github.com/bluenviron/mediamtx/releases/download/$V/mediamtx_${V}_linux_amd64.tar.gz"
echo "DOWNLOAD_EXIT=$?"
ls -la mediamtx.tar.gz
tar xzf mediamtx.tar.gz
chmod +x mediamtx
echo EXTRACT_DONE
ls -la ~/mediamtx*
./mediamtx --version || true
