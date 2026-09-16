#!/usr/bin/env bash
OUT=/mnt/e/EDA_Race/Cangku/new/SigFlow_FPGA_Cmake/_gcc.txt
{
  echo "=== current ==="
  g++ --version | head -1
  echo "=== apt candidate ==="
  apt-cache policy gcc-16 g++-16 2>/dev/null | grep -E "gcc-16|g\+\+-16|Candidate" | head
  echo "=== ubuntu version ==="
  . /etc/os-release; echo "$PRETTY_NAME"
  echo "=== ppa reachable? ==="
  timeout 12 curl -s -o /dev/null -w "%{http_code}\n" https://ppa.launchpadcontent.net/ubuntu-toolchain-r/test/ubuntu/ 2>&1
} > "$OUT" 2>&1
