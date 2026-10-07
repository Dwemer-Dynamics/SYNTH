#!/usr/bin/env python3
from pathlib import Path
import os, shutil, subprocess, tempfile
root=Path(__file__).resolve().parents[2]; source=Path(__file__).with_name("presentation_tests.cpp")
cxx=os.environ.get("CXX") or next((x for x in ("clang++","g++") if shutil.which(x)),None)
if not cxx: raise SystemExit("No C++ compiler found")
with tempfile.TemporaryDirectory(prefix="synth-presentation-") as d:
 out=Path(d)/"tests"; cmd=[cxx,"-std=c++23","-Wall","-Wextra","-Wpedantic","-Werror",f"-I{root/'src'}",str(source),"-o",str(out)]
 print("+",*cmd,flush=True); subprocess.run(cmd,check=True,cwd=root); raise SystemExit(subprocess.run([out],cwd=root).returncode)
