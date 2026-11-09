#!/usr/bin/env python3
"""Maintainer checks for packing and minification on a controlled payload,
including that a failed pack leaves the previous output alone."""
import importlib.util
import json
import os
from pathlib import Path
import random
import shlex
import struct
import subprocess
import sys
import tempfile
from unittest import mock

sys.dont_write_bytecode = True
ROOT = Path(__file__).resolve().parents[2]

def check_first_stage_search(cc, out):
    executable = out / 's1pack'
    subprocess.run([*cc, '-std=gnu11', '-O2', str(ROOT / 'tools/s1pack.c'),
                    '-o', str(executable)], check=True)
    noisy = random.Random(1).getrandbits(8 * 256).to_bytes(256, 'little')
    # The noisy block has a valid 264-byte result with one model. Underpricing
    # model parameters selected two models and produced 269 bytes instead.
    corpus = [('empty', b'', None), ('repeated', bytes(range(16)) * 32, None),
              ('noisy', noisy, 264)]
    for name, data, ceiling in corpus:
        source = out / ('s1-' + name)
        source.write_bytes(data)
        totals = {}
        for mode, options in [('quick', ['-q']), ('searched', [])]:
            stream = out / ('s1-' + name + '-' + mode + '.bin')
            header = stream.with_suffix('.h')
            subprocess.run([str(executable), *options, str(source), str(stream), str(header)],
                           check=True, capture_output=True)
            defines = dict(line.split(maxsplit=2)[1:] for line in header.read_text().splitlines())
            masks = defines['S1_MASKS'].split(',')
            weights = defines['S1_WTS'].split(',')
            assert len(masks) == len(weights) == int(defines['S1_NM'])
            # Count the emitted .long masks and .byte weights as well as the stream.
            totals[mode] = stream.stat().st_size + struct.calcsize('<I') * len(masks) + len(weights)
        assert totals['searched'] <= totals['quick'], (name, totals)
        if ceiling is not None:
            assert totals['searched'] <= ceiling, (name, totals, ceiling)

def main():
    spec=importlib.util.spec_from_file_location('genshaders',ROOT/'tools/genshaders.py')
    generator=importlib.util.module_from_spec(spec);spec.loader.exec_module(generator)
    cc=shlex.split(os.environ.get('CC','cc'))
    with tempfile.TemporaryDirectory(prefix='microcosm-tools-') as dirname:
        out=Path(dirname);shader=out/'test.frag';shader.write_text('shader source')
        with mock.patch.object(generator.subprocess,'run',side_effect=FileNotFoundError('injected')):
            try: generator.minify(str(shader))
            except RuntimeError: pass
            else: raise AssertionError('strict minification accepted a failure')
            assert generator.minify(str(shader),strict=False)=='shader source'
        with mock.patch.object(generator.subprocess,'run',side_effect=subprocess.CalledProcessError(1,['minifier'],stderr='specific minifier failure')):
            try: generator.minify(str(shader))
            except RuntimeError as e: assert 'specific minifier failure' in str(e)
            else: raise AssertionError('strict minification accepted a failure')
        # Overloaded helpers: a call keeps only the overload with its argument count.
        text=('float a(float b){return b;}float a(vec2 b,float c){return c;}float d(){return 1.;}'
              'void main(){float e=a(.5);}')
        kept=generator.drop_dead_functions(text)
        assert 'a(float b)' in kept and 'a(vec2' not in kept and 'd()' not in kept,kept
        generated=out/'shaders.h';generated.write_text('obsolete shader data')
        subprocess.run([sys.executable,str(ROOT/'tools/genshaders.py'),str(out/'no-shaders'),
                        '-o',str(generated),'--allow-empty'],check=True,capture_output=True)
        assert 'obsolete' not in generated.read_text() and 'return "";' in generated.read_text()
        check_first_stage_search(cc, out)
        subprocess.run([*cc,str(ROOT/'tools/checks/cmx86.c'),'-o',str(out/'cmx86')],check=True)
        print(subprocess.run([str(out/'cmx86')],check=True,capture_output=True,text=True).stdout.strip())
        # The shipped asm second stage must decode bit for bit like the cm.h model
        # in every configuration, not just on the one payload pack.py checks per build.
        print(subprocess.run([sys.executable,str(ROOT/'tools/checks/s2equiv.py')],check=True,capture_output=True,text=True).stdout.strip())
        # A real program, packed with its code range so the x86 transform and models run end to end.
        subprocess.run([*cc,str(ROOT/'tools/checks/launcher.c'),'-o',str(out/'payload')],check=True)
        (out/'range').write_text(subprocess.run([sys.executable,str(ROOT/'tools/textrange.py'),str(out/'payload')],
                                                check=True,capture_output=True,text=True).stdout)
        assert int((out/'range').read_text().split()[1],0)>int((out/'range').read_text().split()[0],0)
        command=[sys.executable,str(ROOT/'tools/pack.py'),str(out/'payload'),str(out/'packed'),'--quick','--code-range',str(out/'range')]
        subprocess.run([*command,'--manifest',str(out/'pack.json')],check=True,capture_output=True)
        packed=(out/'packed').read_bytes();manifest=json.loads((out/'pack.json').read_text())
        phoff,=struct.unpack_from('<Q',packed,32);phentsize,phnum=struct.unpack_from('<HH',packed,54)
        kinds=[struct.unpack_from('<I',packed,phoff+i*phentsize)[0] for i in range(phnum)]
        assert kinds==[1],f'launcher is not a single static PT_LOAD: {kinds}'
        assert manifest['packer']=='cm' and manifest['packed_bytes']==len(packed)==manifest['payload_bytes']+manifest['launcher_bytes']
        result=subprocess.run([str(out/'packed'),'forwarded'],env={**os.environ,'MC_PACK_CHECK':'present'},capture_output=True,text=True)
        assert result.returncode==17 and 'arguments/environment preserved' in result.stdout
        previous=(out/'packed').read_bytes()
        failure=subprocess.run([*command,'--budget','1'],capture_output=True,text=True)
        assert failure.returncode and (out/'packed').read_bytes()==previous
        assert 'no candidate published' in failure.stderr
        (out/'payload').write_bytes(b'not an ELF')
        failure=subprocess.run(command,capture_output=True,text=True)
        assert failure.returncode and (out/'packed').read_bytes()==previous
        print('Tooling: dead overloads dropped by argument count, strict minification, first-stage search size accounting, static single-segment launcher, packed launch/argv/env/status (x86 coder on), and failed-pack output preservation passed')

if __name__=='__main__': main()
