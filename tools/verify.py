#!/usr/bin/env python3
"""Regression checks and profiling for maintainers.

Leaves the two shipped targets alone and never opens an audio device. Needs a
C toolchain, EGL/OpenGL 4.5, ALSA headers and glslangValidator. Writes its
evidence to a temporary directory, or to --out.
"""
import argparse
import json
import hashlib
import os
from pathlib import Path
import re
import shlex
import shutil
import subprocess
import sys
import tempfile
import math

ROOT = Path(__file__).resolve().parents[1]

def run(args, **kw):
    return subprocess.run([str(a) for a in args], cwd=ROOT, check=True, **kw)

def shader_checks(out):
    validator = shutil.which('glslangValidator')
    if not validator:
        raise RuntimeError('glslangValidator is required for raw/minified portability checks')
    # programs.def and the manifests it includes (scene3.def, s3_*.def)
    source = ''.join(p.read_text() for p in sorted((ROOT/'demos/hello-world').glob('*.def')))
    pairs = sorted(set(re.findall(r'"([^"]+)"\s*,\s*"([^"]+)"', source)))
    # The engine's post passes: every fragment shader pairs with post.vert.
    pairs += [('post.vert', p.name) for p in sorted((ROOT/'src/shaders').glob('*.frag'))]
    raw = {p.name: p for d in ('demos/hello-world/shaders', 'src/shaders') for p in (ROOT/d).iterdir()}
    # 'min' is exactly what the headers embed: genshaders' --emit-dir output,
    # renamed with the one mapping the demo's and the engine's shaders share.
    for variant in ('raw', 'min'):
        for vs, fs in pairs:
            paths = [raw[name] if variant == 'raw' else out/'min-shaders'/name for name in (vs, fs)]
            run([validator, '-l', *dict.fromkeys(paths)], capture_output=True, text=True)   # a compute program's one file is passed once
        print(f'Shaders: {len(pairs)} {variant} program pairs linked')
    # Numerical equivalence at texel centres, including clamped edges.
    import math
    weights=[.227027,.1945946,.1216216,.054054,.016216]
    data=[math.sin(i*.37)+.1*i for i in range(37)]
    def sample(x):
        x=max(0,min(len(data)-1,x));i=int(x);f=x-i
        return data[i]*(1-f)+data[min(i+1,len(data)-1)]*f
    for i in range(len(data)):
        old=sum(sample(i+k)*weights[abs(k)] for k in range(-4,5))
        new=sample(i)*weights[0]
        for a,b in ((1,2),(3,4)):
            w=weights[a]+weights[b]; x=(a*weights[a]+b*weights[b])/w
            new+=w*(sample(i-x)+sample(i+x))
        assert abs(new-old)<1e-12
    print('Blur: five bilinear taps reproduce the nine-tap kernel including edges')

def main():
    ap=argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--profile', action='store_true')
    ap.add_argument('--sequence', action='store_true', help='profile advancing time, including simulation steps')
    ap.add_argument('--start', type=float, default=0)
    ap.add_argument('--duration', type=float, default=278.6)
    ap.add_argument('--fps', type=float, default=60)
    ap.add_argument('--scale', type=int, default=80, help='internal render percentage, 25..200')
    ap.add_argument('--legacy-order', action='store_true', help='A/B original background/seabed/batch ordering')
    ap.add_argument('--out', type=Path)
    ap.add_argument('--width', type=int, default=640)
    ap.add_argument('--height', type=int, default=360)
    ap.add_argument('--frames', type=int, default=24)
    ap.add_argument('--no-culling', action='store_true')
    ap.add_argument('--no-lod', action='store_true')
    a=ap.parse_args()
    a.profile = a.profile or a.sequence
    if min(a.width,a.height,a.frames)<=0: ap.error('dimensions and frame count must be positive')
    if not 25 <= a.scale <= 200: ap.error('scale must be 25..200')
    if not all(math.isfinite(v) for v in (a.start,a.duration,a.fps)) or a.start < 0 or a.duration <= 0 or a.fps <= 0:
        ap.error('start must be nonnegative and duration/fps finite and positive')
    if a.start + a.duration > 278.72 or a.duration*a.fps > 1000000:
        ap.error('sequence must fit the show and contain at most one million frames')
    out=a.out.resolve() if a.out else Path(tempfile.mkdtemp(prefix='microcosm-verify-'))
    out.mkdir(parents=True,exist_ok=True)
    cc=shlex.split(os.environ.get('CC','cc'))
    # Reuse the project's release compiler flags, including fast-math and LTO.
    make=(ROOT/'Makefile').read_text().replace('\\\n',' ')
    flags=re.search(r'^REL_FLAGS\s*:=\s*(.*)$',make,re.M)[1]
    flags=[flag.replace('$(OUT)',str(out)) for flag in shlex.split(flags.replace('$(INC)','-Iinclude'))]
    common=['src/math.c','src/gl_loader.c','src/platform_common.c','src/mesh.c','src/scene.c']
    identity=hashlib.sha256()
    for path in sorted([ROOT/'Makefile', * (ROOT/'demos/hello-world').glob('*.def'), * (ROOT/'src').glob('*.c'), * (ROOT/'include').rglob('*.h'), * (ROOT/'demos/hello-world').rglob('*.c'), * (ROOT/'demos/hello-world/shaders').glob('*'), * (ROOT/'src/shaders').glob('*')]):
        if path.is_file(): identity.update(str(path.relative_to(ROOT)).encode()+b'\0'+path.read_bytes())
    metadata={'source_sha256':identity.hexdigest(),'seed':'0xB16BA75 (simulation), fixed procedural scatter seeds','compiler' :run([*cc,'--version'],capture_output=True,text=True).stdout.splitlines()[0],
              'flags':flags,'width':a.width,'height':a.height,'frames':a.frames,
              'culling':not a.no_culling,'lod':not a.no_lod,'audio':'disabled; separate fake-sink checks',
              'scale':a.scale,'legacy_order':a.legacy_order,'sequence':a.sequence,
              'start':a.start,'duration':a.duration,'fps':a.fps,
              'timing':'GPU queries delayed >=8 frames; coarse means include glFinish after each frame'}
    (out/'metadata.json').write_text(json.dumps(metadata,indent=2)+'\n')
    print(f'Evidence: {out}',flush=True)
    # The predator CPU fixture includes the demo, whose release program table
    # needs the same generated headers as the rendering fixture.
    run([sys.executable,'tools/genshaders.py','demos/hello-world/shaders','-o',out/'shaders_gen.h',
         '--engine','src/shaders','--engine-output',out/'mc_engine_shaders.h','--emit-dir',out/'min-shaders'])
    if not a.profile:
        for name,sources,libs in [('cpu',['src/math.c','src/mesh.c','src/scene.c','src/platform_common.c'],['-lm']),
                                  ('predator',['src/math.c','src/mesh.c','src/scene.c','src/platform_common.c'],['-lm']),
                                  ('audio',[],['-lm','-lpthread','-lasound']),('synth',['src/synth.c','src/math.c'],['-lm'])]:
            run([*cc,*flags,ROOT/f'tools/checks/{name}.c',*sources,'-no-pie','-Wl,--gc-sections',*libs,'-o',out/name])
            run([out/name])
    run([*cc,*flags,'-DMC_HEADLESS','-DMC_PROFILE','tools/checks/render.c','tools/checks/profile.c',*common,'src/platform_surfaceless.c',
         '-no-pie','-Wl,--gc-sections','-lEGL','-lm','-o',out/'render'])
    result=run([out/'render','sequence' if a.sequence else 'profile' if a.profile else 'check',out,
                a.width,a.height,a.frames,int(a.no_culling),int(a.no_lod),a.scale,int(a.legacy_order),
                a.start,a.duration,a.fps],capture_output=True,text=True)
    (out/'render.log').write_text(result.stdout+result.stderr)
    print(result.stdout,end='');print(result.stderr,end='',file=sys.stderr)
    if not a.profile:
        shader_checks(out)
        shader_copy=out/'reload-shaders'
        shutil.copytree(ROOT/'demos/hello-world/shaders',shader_copy,dirs_exist_ok=True)
        devflags=[f for f in flags if f!='-DMC_RELEASE']
        run([*cc,*devflags,'-DMC_DEV','-DMC_HEADLESS',f'-DMC_SHADER_ROOT="{shader_copy}"',
             'tools/checks/reload.c',*common,'src/render.c','src/platform_surfaceless.c',
             '-no-pie','-Wl,--gc-sections','-lEGL','-lm','-o',out/'reload'])
        run([out/'reload'])
        run([sys.executable,'tools/checks/tooling.py'])
        for tool in ('glslmin.py','fpround.py'): run([sys.executable,ROOT/'tools'/tool,'--selftest'])
    else:
        import csv
        import statistics
        rows=list(csv.DictReader((out/'profile.csv').open()))
        summaries=[]
        def bucket(row):
            t = float(row['time'])
            return math.floor(t / 10) * 10 if a.sequence else t
        for t in sorted({bucket(r) for r in rows}):
            group=[r for r in rows if bucket(r)==t]
            wall=sorted(float(r['wall_ms']) for r in group)
            def pct(p): return wall[min(len(wall)-1, int((len(wall)-1)*p+.5))]
            record={'time':t,'samples':len(group),'wall_median_ms':statistics.median(wall),'wall_p95_ms':pct(.95),'wall_p99_ms':pct(.99),'wall_max_ms':max(wall)}
            for key in group[0]:
                if key not in ('frame','time'):record[key+'_mean']=statistics.mean(float(r[key]) for r in group)
            summaries.append(record)
        (out/'summary.json').write_text(json.dumps(summaries,indent=2)+'\n')
        print('Per-time medians, sampled tails and pass/counter means: '+str(out/'summary.json'))
    return 0

if __name__=='__main__':
    try: sys.exit(main())
    except subprocess.CalledProcessError as e:
        if e.stdout: print(e.stdout,file=sys.stderr)
        if e.stderr: print(e.stderr,file=sys.stderr)
        raise
