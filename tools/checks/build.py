#!/usr/bin/env python3
"""Check real incremental builds in a disposable copy, never in the user's tree."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile

ROOT=Path(__file__).resolve().parents[2]

def main():
    with tempfile.TemporaryDirectory(prefix='microcosm-build-') as dirname:
        root=Path(dirname)
        for name in ('src','include','tools','demos'):
            shutil.copytree(ROOT/name,root/name,ignore=shutil.ignore_patterns('__pycache__'))
        shutil.copy2(ROOT/'Makefile',root/'Makefile')
        def make(*args):
            p=subprocess.run(['make','-j2','PACK=quick',*args],cwd=root,capture_output=True,text=True)
            if p.returncode: raise RuntimeError(p.stdout+p.stderr)
            return p.stdout+p.stderr
        make('hello-world')
        out=root/'build/hello-world'
        def state(): return {str(p.relative_to(out)):p.stat().st_mtime_ns for p in out.rglob('*') if p.is_file()}
        original=state();log=make('hello-world');assert state()==original,log
        shader=root/'demos/hello-world/shaders/object.frag'
        shader.write_text(shader.read_text()+'\n// no semantic shader change\n')
        make('hello-world');after_comment=state()
        assert after_comment['hello-world.bin']==original['hello-world.bin']
        assert after_comment['shaders_gen.h']==original['shaders_gen.h']
        assert state()==after_comment
        log=make('hello-world');assert state()==after_comment and 'genshaders:' not in log
        text=shader.read_text();assert 'hemi) * 0.65' in text   # the edit below must find its text, or it changes nothing
        shader.write_text(text.replace('hemi) * 0.65','hemi) * 0.66',1))   # a real change (an edit to an unused #define would be minified away)
        make('hello-world');after_shader=state()
        assert after_shader['.release/demos/hello-world/main.o']>after_comment['.release/demos/hello-world/main.o']
        assert after_shader['.release/src/math.o']==after_comment['.release/src/math.o']
        assert after_shader['.dev/demos/hello-world/main.o']==after_comment['.dev/demos/hello-world/main.o']
        header=root/'include/microcosm/mc_render.h';header.write_text(header.read_text()+'\n/* dependency check */\n')
        make('dev');after_header=state()
        assert after_header['.dev/src/render.o']>after_shader['.dev/src/render.o']
        assert after_header['.dev/src/math.o']==after_shader['.dev/src/math.o']
        make('dev','CC='+os.environ.get('CC','cc')+' -DMC_CONFIG_CHECK=1');after_flags=state()
        assert after_flags['.dev/src/math.o']>after_header['.dev/src/math.o']
        print('Build: unchanged no-op, comment-only shader edits, shader-only rebuilds, header dependencies and toolchain changes passed')

if __name__=='__main__': main()
