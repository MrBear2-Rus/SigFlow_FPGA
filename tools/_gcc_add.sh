#!/usr/bin/env bash
PW='wcl66666'
OUT=/mnt/e/EDA_Race/Cangku/new/SigFlow_FPGA_Cmake/_gcc.txt
{
  echo "$PW" | sudo -S -v >/dev/null 2>&1 || { echo SUDO_FAIL; exit 1; }
  sudo apt-get install -y software-properties-common >/dev/null 2>&1
  echo "add-apt-repository:"; sudo add-apt-repository -y ppa:ubuntu-toolchain-r/test 2>&1 | tail -3
  echo "apt-get update:"; sudo apt-get update 2>&1 | tail -3
  echo "=== available gcc/g++ 1x ==="
  apt-cache search --names-only '^g\+\+-1[0-9]$' | sort
  echo "=== candidates ==="
  for v in 16 15 14 13; do echo "g++-$v: $(apt-cache policy g++-$v 2>/dev/null | grep Candidate | awk '{print $2}')"; done
  echo DONE
} > "$OUT" 2>&1
