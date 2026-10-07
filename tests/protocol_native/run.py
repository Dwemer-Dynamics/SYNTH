#!/usr/bin/env python3
from pathlib import Path
import os,shutil,subprocess,tempfile
root=Path(__file__).resolve().parents[2]; compiler=os.environ.get('CXX') or shutil.which('clang++') or shutil.which('c++')
if not compiler: raise SystemExit('no C++ compiler')
with tempfile.TemporaryDirectory(prefix='synth-protocol-native-') as d:
 out=str(Path(d)/('tests.exe' if os.name=='nt' else 'tests'))
 cmd=[compiler,'-std=c++23','-Wall','-Wextra','-Wpedantic','-Werror',f'-I{root}/src',str(root/'tests/protocol_native/protocol_native_tests.cpp'),'-o',out]
 print('+',' '.join(cmd),flush=True); subprocess.run(cmd,check=True,cwd=root); raise SystemExit(subprocess.run([out,str(root)],cwd=root).returncode)
