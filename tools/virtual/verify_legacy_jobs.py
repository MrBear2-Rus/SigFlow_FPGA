"""Run actual legacy plugin build/run providers, with no fake process host."""
import argparse,shutil,sys
from pathlib import Path
from generated_runner_test_support import ROOT,Harness
parser=argparse.ArgumentParser();parser.add_argument('--build-dir',type=Path,default=ROOT/'build-m0');args=parser.parse_args()
h=Harness(args.build_dir,ROOT/'.work/k45/legacy')
tools=h.work/'posix-tools';shutil.copy2(h.mingw/'bin/mingw32-make.exe',tools/'make.exe')
h.env['SIGFLOW_PYTHON3']=sys.executable;h.env['SIGFLOW_SH']=h.shell.as_posix()
# This locked Windows Verilator runtime was configured with an unavailable
# build-machine MSVC path. Select this workspace's GCC in the child environment;
# -e lets GNU Make honor these explicit compiler settings over that old default.
h.env.update(CXX=(h.mingw/'bin/g++.exe').as_posix(),LINK=(h.mingw/'bin/g++.exe').as_posix(),
             AR=(h.mingw/'bin/ar.exe').as_posix(),PYTHON3=Path(sys.executable).as_posix(),MAKEFLAGS='-e')
r=h.run([h.build/'tests/virtual/k4_k5/k45_legacy_jobs.exe',ROOT,h.work,h.verilator],'legacy-jobs',timeout=180)
print(r.stdout,end='')
