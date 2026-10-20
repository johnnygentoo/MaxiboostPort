"""Compile selected shadPS4/GoldHEN XML patches into out/patches.bin for the loader.

Patch addresses are PS4 virtual addresses (eboot base 0x400000); the loader's
image places eboot vaddr 0 at image offset 0. Only literal writes are supported
(bytes, bytes16/32/64, float32/64, utf8, utf16); pattern ("mask") patches are rejected.

The built-in patches are patches/Maxiboost.xml (MAXIBOOST ON 1.15). Frame rate and effect switches
name patches in it; a name the file does not have yet is reported and skipped, so the port runs
the game as it is until the patch is written.
"""
import argparse
import json
import os
import struct
import sys
import xml.etree.ElementTree as ET
from pathlib import Path

EBOOT_BASE=0x400000
APP_VERSION='01.15'
BUILTIN_XML=Path(__file__).resolve().parent.parent/'patches/Maxiboost.xml'
# BB_FPS presets: patch names from patches/Maxiboost.xml. The game runs at 60 FPS (flip rate 0).
FPS_PRESETS={'60':[]}
# Built-in patches applied on every start (patches/Maxiboost.xml): installed add-ons count as owned.
ALWAYS=['Installed add-ons']
# Sizes (MAXIBOOST ON 1.15, image offsets). The game renders its 3D scene and draws its 2D layer
# (menus, HUD) at 1920x1080 and makes its render targets with the sizes as immediates
# (SCENE_SITES, the scene size); the display buffers' size is a 16-byte constant (DISPLAY_SITES) and
# the 2D layer's viewports are floats (VIEWPORT_FLOAT_SITES), both the output size. Found with a
# frame capture and BB_CPU_WATCH: the 3D pass's descriptor got its size from a movabs into r12
# (guest+0x687e92) that a search for rax-only encodings had missed. Above 1080p the game's GPU
# memory pools grow (POOL_SITES, POOL1_SITES), or a 4K start fails loading a texture.
OUTPUT_SIZE=(1920,1080)
RESOLUTION_SITES=[]  # GUNDAM VERSUS' kind (one movabs): MAXIBOOST ON has SCENE_SITES
RESOLUTION_CLAMP=[]
# MAXIBOOST ON 1.15: the display (VideoOut) buffers' size, a 16-byte constant 1920,1080,1920,1080.
DISPLAY_SITES=[(0xba1d30,0xba1d34),(0xba1d38,0xba1d3c)]
# The renderer's targets (guest+0x687e92 to +0x689fe9): 1920 (w), 1080 (h) and the half-size 960 (w2),
# 540 (h2); (image offset, kind).
SCENE_SITES=[
    (0x687e94,'w'),(0x687e98,'h'),(0x689feb,'w'),(0x689fef,'h'),
    (0x6880eb,'w'),
    (0x6880ef,'h'),
    (0x688400,'w'),
    (0x688408,'h'),
    (0x68840e,'w'),
    (0x688414,'h'),
    (0x688447,'w'),
    (0x68844d,'h'),
    (0x688453,'w'),
    (0x688462,'h'),
    (0x6884d6,'w'),
    (0x6884da,'h'),
    (0x68864a,'w'),
    (0x68864f,'h'),
    (0x68867b,'w2'),
    (0x688680,'h2'),
    (0x688692,'w2'),
    (0x688697,'h2'),
    (0x6886b0,'w'),
    (0x6886b5,'h'),
    (0x6886e9,'w'),
    (0x6886ee,'h'),
    (0x6887f7,'w'),
    (0x688812,'w'),
    (0x688817,'h'),
    (0x68882d,'w'),
    (0x688832,'h'),
    (0x688848,'w'),
    (0x68884d,'h'),
    (0x688863,'w'),
    (0x688868,'h'),
    (0x68890e,'w'),
    (0x688913,'h'),
    (0x68892c,'w'),
    (0x688931,'h'),
    (0x6889ab,'w'),
    (0x6889b0,'h'),
    (0x6889e7,'w'),
    (0x6889ec,'h')]
SCENE_VALUES={'w':1920,'h':1080,'w2':960,'h2':540}
# BB_SCENE_SITES_ONLY=i,j,... (diagnostics): only these entries of SCENE_SITES are patched.
if os.environ.get('BB_SCENE_SITES_ONLY'):
    SCENE_SITES=[SCENE_SITES[int(i)] for i in os.environ['BB_SCENE_SITES_ONLY'].split(',')]
# Float sites of the scene size: none needed so far (kept for other builds' patches).
SCENE_HALF_FLOAT_SITES=[]
# MAXIBOOST ON 1.15: its 2 GiB pool of GPU memory (render targets and textures; guest+0x685a66
# allocates, +0x685aa6 maps it) is too small for its targets above 1080p (a 4K start failed in a
# texture load). Its allocator keeps the size too (guest+0x68615a, +0x68620c, +0x686246). Above
# 1080p it gets POOL_LARGE (run.sh gives the game 9 GiB of direct memory then).
POOL_SITES=[0x685a67,0x685aa7,0x686160,0x686212,0x686247]
POOL_SIZE=0x80000000
POOL_LARGE=0xf0000000
# Its 1 GiB pool (guest+0x685982 allocates, +0x6859bf maps).
POOL1_SITES=[0x685983,0x6859c0,0x68605b]
POOL1_SIZE=0x40000000
POOL1_LARGE=0x80000000
SCENE_FLOAT_SITES=[]
# MAXIBOOST ON 1.15: viewports of the 2D layer (menus, HUD) as floats {x, y, 1920, 1080, 0, 1}, and a
# 1920.0f|1080.0f movabs (guest+0x6dfa19): (width offset, height offset).
VIEWPORT_FLOAT_SITES=[(0xba1f00,0xba1f04),(0xbc1c88,0xbc1c8c),(0x6dfa1b,0x6dfa1f),(0xbcd400,0xbcd404)]


def boot_image(out):
    """The prepared memory image (out/boot.bin, BBPROBE2), to check bytes before patching."""
    raw=(Path(out)/'boot.bin').read_bytes()
    magic,size,entry,ns,nr,ni,flags=struct.unpack_from('<8s6Q',raw,0)
    if magic!=b'BBPROBE2':
        raise ValueError('out/boot.bin is not a prepared image')
    pos=56+ns*24+ni*128+nr*32
    return raw[pos:pos+size]


def original_bytes():
    """[(image offset, bytes)]: what every size site holds in the supported executable (tests build
    a stand-in image from it)."""
    out=[]
    for wa,ha in RESOLUTION_SITES+DISPLAY_SITES:
        out+=[(wa,struct.pack('<I',1920)),(ha,struct.pack('<I',1080))]
    for at,kind in SCENE_SITES:
        out.append((at,struct.pack('<I',SCENE_VALUES[kind])))
    for at,kind in SCENE_FLOAT_SITES:
        out.append((at,struct.pack('<f',{'w':1920.0,'h':1080.0}[kind])))
    for wa,ha in SCENE_HALF_FLOAT_SITES:
        out+=[(wa,struct.pack('<f',960.0)),(ha,struct.pack('<f',540.0))]
    for wa,ha in VIEWPORT_FLOAT_SITES:
        out+=[(wa,struct.pack('<f',1920.0)),(ha,struct.pack('<f',1080.0))]
    out+=[(at,struct.pack('<I',POOL_SIZE)) for at in POOL_SITES]
    out+=[(at,struct.pack('<I',POOL1_SIZE)) for at in POOL1_SITES]
    out.append((0x68606a,struct.pack('<I',POOL1_SIZE-0x40)))
    for at,original,_ in RESOLUTION_CLAMP:
        out.append((at,original))
    return out


def resolution_writes(size,image):
    """Writes that make the game render at size (w, h); the original bytes are checked first."""
    w,h=size
    if not (256<=w<=7680 and 144<=h<=4320) or w%2 or h%2:
        raise ValueError(f'unsupported render size {w}x{h}')
    writes=[]
    for wa,ha in RESOLUTION_SITES:
        if struct.unpack_from('<I',image,wa)[0]!=1920 or struct.unpack_from('<I',image,ha)[0]!=1080:
            raise ValueError(f'the executable has no 1920x1080 at {wa:#x}: not MAXIBOOST ON 1.15')
        writes+=[(wa,struct.pack('<I',w)),(ha,struct.pack('<I',h))]
    for at,kind in SCENE_FLOAT_SITES:
        if struct.unpack_from('<f',image,at)[0]!={'w':1920.0,'h':1080.0}[kind]:
            raise ValueError(f'unexpected float at {at:#x}: not MAXIBOOST ON 1.15')
        writes.append((at,struct.pack('<f',float(w if kind=='w' else h))))
    for wa,ha in SCENE_HALF_FLOAT_SITES:
        if struct.unpack_from('<ff',image,wa)!=(960.0,540.0):
            raise ValueError(f'the executable has no 960.0, 540.0 at {wa:#x}: not MAXIBOOST ON 1.15')
        writes+=[(wa,struct.pack('<f',w/2)),(ha,struct.pack('<f',h/2))]
    if w*h>1920*1080:
        for at in POOL_SITES:
            if struct.unpack_from('<I',image,at)[0]!=POOL_SIZE:
                raise ValueError(f'the executable has no {POOL_SIZE:#x} at {at:#x}: not MAXIBOOST ON 1.15')
            writes.append((at,struct.pack('<I',POOL_LARGE)))
        # Pool 1's first free block: its size less a 0x40 header (guest+0x686067).
        if struct.unpack_from('<I',image,0x68606a)[0]!=POOL1_SIZE-0x40:
            raise ValueError('the executable has no pool block size at 0x68606a: not MAXIBOOST ON 1.15')
        writes.append((0x68606a,struct.pack('<I',POOL1_LARGE-0x40)))
        for at in POOL1_SITES:
            if struct.unpack_from('<I',image,at)[0]!=POOL1_SIZE:
                raise ValueError(f'the executable has no {POOL1_SIZE:#x} at {at:#x}: not MAXIBOOST ON 1.15')
            writes.append((at,struct.pack('<I',POOL1_LARGE)))
    for at,kind in SCENE_SITES:
        if struct.unpack_from('<I',image,at)[0]!=SCENE_VALUES[kind]:
            raise ValueError(f'the executable has no {SCENE_VALUES[kind]} at {at:#x}: not MAXIBOOST ON 1.15')
        value={'w':w,'h':h,'w2':w//2,'h2':h//2}[kind]
        writes.append((at,struct.pack('<I',value)))
    for at,original,patched in RESOLUTION_CLAMP:
        if image[at:at+len(original)]!=original:
            raise ValueError(f'unexpected display clamp at {at:#x}: not MAXIBOOST ON 1.15')
        writes.append((at,patched))
    return writes


def display_writes(size,image):
    """Writes that make the game's display buffer (and the HUD drawn into it) size (w, h)."""
    w,h=size
    if not (256<=w<=7680 and 144<=h<=4320) or w%2 or h%2:
        raise ValueError(f'unsupported display size {w}x{h}')
    writes=[]
    for wa,ha in VIEWPORT_FLOAT_SITES:
        if struct.unpack_from('<f',image,wa)[0]!=1920.0:
            raise ValueError(f'the executable has no 1920.0 at {wa:#x}: not MAXIBOOST ON 1.15')
        if struct.unpack_from('<f',image,ha)[0]!=1080.0:
            raise ValueError(f'the executable has no 1080.0 at {ha:#x}: not MAXIBOOST ON 1.15')
        writes+=[(wa,struct.pack('<f',float(w))),(ha,struct.pack('<f',float(h)))]
    for wa,ha in DISPLAY_SITES:
        if struct.unpack_from('<I',image,wa)[0]!=1920 or struct.unpack_from('<I',image,ha)[0]!=1080:
            raise ValueError(f'the executable has no 1920x1080 at {wa:#x}: not MAXIBOOST ON 1.15')
        writes+=[(wa,struct.pack('<I',w)),(ha,struct.pack('<I',h))]
    return writes


# Upscaler presets (bbport.ini "preset": Native AA, Quality, Balanced, Performance, Ultra
# Performance): output / render size ratio. The temporal upscalers take the scene at the reduced
# size, the HUD is drawn at the output size (the GPU library's UI composition).
PRESET_SCALES=[1.0,1.5,1.7,2.0,3.0]
TEMPORAL_UPSCALERS={'fsr3','fsr4','fsr411','dlss'}


def scaled_sizes(settings):
    """(render, output): the size the game renders at (the output, divided by the upscaler
    preset's scale with a temporal upscaler) and the output. None when both are 1920x1080 (the
    game's own size, no patch)."""
    if not RESOLUTION_SITES and not SCENE_SITES:
        return None  # the frame size is not mapped yet: the presenter scales the 1080p frame
    out=output_size(settings)
    scale=1.0
    if settings.get('upscaler','off') in TEMPORAL_UPSCALERS:
        preset=int(settings.get('preset','0') or 0)
        scale=PRESET_SCALES[max(0,min(preset,len(PRESET_SCALES)-1))]
    render=tuple(max(2,round(v/scale/2)*2) for v in out)
    if render==out==OUTPUT_SIZE and settings.get('upscaler','off') not in TEMPORAL_UPSCALERS|{'taa'}:
        return None
    # With a temporal upscaler or TAA the GPU library takes the scene from the game's pass scaling
    # it into the display buffer, also at 1080p (Native AA): a scaled session (BB_RENDER_RES).
    return render,out


# Effect switches (bbport.ini, in-game menu, launcher): key -> (patch when the key is 0, patch
# when it is 1). The game reads these at start; a change applies after a restart.
EFFECTS={
    'skip_intro':(None,'Skip Intro'),
}


def effect_patches(settings):
    names=[]
    for key,(off,on) in EFFECTS.items():
        if key not in settings: continue
        name=on if settings.get(key,'1')=='1' else off
        if name: names.append(name)
    return names


def read_settings(path):
    settings={}
    if path.exists():
        for line in path.read_text(encoding='utf-8').splitlines():
            key,sep,value=line.partition('=')
            if sep and not line.startswith('#'): settings[key.strip()]=value.strip()
    return settings


def output_size(settings):
    """Output (UI) size from bbport.ini output_res, e.g. 3840x2160; 1920x1080 by default."""
    try:
        w,h=(int(v) for v in settings.get('output_res','').lower().split('x'))
        if w>0 and h>0: return (w,h)
    except ValueError:
        pass
    return OUTPUT_SIZE


def available(xml, names, app_version):
    """The names the XML has for this version; the others are reported."""
    have={meta.get('Name') for meta in ET.parse(xml).getroot().iter('Metadata')
          if meta.get('AppVer')==app_version}
    for name in names:
        if name not in have: print(f'Patches: {name!r} not available for MAXIBOOST ON {app_version} yet',file=sys.stderr)
    return [n for n in names if n in have]


def eboot_segments(elf):
    phoff,=struct.unpack_from('<Q',elf,0x20)
    phentsize,phnum=struct.unpack_from('<HH',elf,0x36)
    segments=[]
    for i in range(phnum):
        kind,_,_,vaddr,_,_,memsz,_=struct.unpack_from('<IIQQQQQQ',elf,phoff+i*phentsize)
        if kind==1: segments.append((vaddr,vaddr+memsz))
    return segments


def encode(line):
    kind,value=line.get('Type'),line.get('Value')
    if kind=='bytes': return bytes.fromhex(value.replace(' ',''))
    if kind in ('bytes16','bytes32','bytes64'):
        return int(value,0).to_bytes(int(kind[5:])//8,'little')
    if kind=='float32': return struct.pack('<f',float(value))
    if kind=='float64': return struct.pack('<d',float(value))
    if kind=='utf8': return value.encode()+b'\0'
    if kind=='utf16': return value.encode('utf-16-le')+b'\0\0'
    raise ValueError(f'unsupported patch type {kind!r}')


def compile_patches(xml, names, app_version, segments):
    found={}
    for meta in ET.parse(xml).getroot().iter('Metadata'):
        if meta.get('Name') in names and meta.get('AppVer')==app_version and meta.get('AppElf','eboot.bin')=='eboot.bin':
            found[meta.get('Name')]=meta
    missing=[n for n in names if n not in found]
    if missing: raise ValueError(f'patches not found for app version {app_version}: {missing}')
    writes=[]
    for name in names:
        for line in found[name].iter('Line'):
            offset=int(line.get('Address'),0)-EBOOT_BASE
            data=encode(line)
            if not any(start<=offset and offset+len(data)<=end for start,end in segments):
                raise ValueError(f'{name}: address {line.get("Address")} is outside the eboot')
            writes.append((offset,data))
    return writes


# Third-party patch files (shadPS4/GoldHEN XML) in the data directory's patches/ folder.
# A file for any retail release applies when it has the same executable (game_check.py).
GAME_IDS={'CUSA18758'}


def same_app_version(value, app_version):
    """'1.09' is '01.09' (patch files write either)."""
    try:
        return value is not None and [int(p) for p in value.strip().split('.')]==[int(p) for p in app_version.split('.')]
    except ValueError:
        return False


def external_patches(directory, app_version=APP_VERSION, exclude=BUILTIN_XML):
    """[(key, file, metadata)] of eboot patches for this version in directory/*.xml.
    key is "<file name>/<patch name>" (the launcher's selection, patches.json)."""
    found=[]
    directory=Path(directory)
    for path in sorted(directory.glob('*.xml')) if directory.is_dir() else []:
        # The built-in file, and copies of it in another patches folder (an older package's): those
        # applied its frame-rate patches over the port's own (from Awaken's BloodBorneButPc).
        if path.resolve()==Path(exclude).resolve() or path.name.lower()==Path(exclude).name.lower(): continue
        try:
            root=ET.parse(path).getroot()
        except ET.ParseError as error:
            print(f'Patches: {path.name}: {error}',file=sys.stderr)
            continue
        ids={e.text.strip() for e in root.iter('ID') if e.text}
        if ids and not ids&GAME_IDS: continue
        for meta in root.iter('Metadata'):
            if same_app_version(meta.get('AppVer'),app_version) and meta.get('AppElf','eboot.bin')=='eboot.bin':
                found.append((f'{path.name}/{meta.get("Name")}',path,meta))
    return found


def external_selection(found, config):
    """Selected external patches: patches.json {"enabled": [...], "disabled": [...]} overrides
    each file's isEnabled."""
    settings={}
    if config and Path(config).is_file():
        settings=json.loads(Path(config).read_text(encoding='utf-8'))
    enabled,disabled=set(settings.get('enabled',[])),set(settings.get('disabled',[]))
    return [(key,path,meta) for key,path,meta in found
            if key in enabled or (key not in disabled and meta.get('isEnabled','false').lower()=='true')]


def compile_external(selected, segments):
    """Writes of the selected external patches; a patch with unsupported lines is skipped whole."""
    writes=[]
    for key,_,meta in selected:
        try:
            ours=[]
            for line in meta.iter('Line'):
                offset=int(line.get('Address') or '',0)-EBOOT_BASE
                data=encode(line)
                if not any(start<=offset and offset+len(data)<=end for start,end in segments):
                    raise ValueError(f'address {line.get("Address")} is outside the eboot')
                ours.append((offset,data))
        except ValueError as error:
            print(f'Patches: skipped {key}: {error}',file=sys.stderr)
            continue
        writes+=ours
        print(f'Patches: external {key} ({len(ours)} writes)')
    return writes


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--patches-dir',type=Path,help='third-party patch XML files (shadPS4 format)')
    p.add_argument('--patches-config',type=Path,help='patches.json: enabled/disabled external patches')
    p.add_argument('--xml',type=Path,default=BUILTIN_XML)
    p.add_argument('--fps',choices=sorted(FPS_PRESETS),default='60')
    p.add_argument('--extra',default='',help='additional patch names, separated by ";"')
    p.add_argument('--app-version',default=APP_VERSION)
    p.add_argument('--out',type=Path,default=Path(__file__).resolve().parent.parent/'out')
    p.add_argument('--settings',type=Path,default=Path(__file__).resolve().parent.parent/'bbport.ini')
    p.add_argument('--game-dir',type=Path,default=Path(os.environ.get('BB_GAME_DIR','CUSA18758-full')))
    p.add_argument('--render-res',default='',help='render resolution WxH (the game renders natively at it)')
    p.add_argument('--output-res',default='',help='output resolution WxH')
    p.add_argument('--print-preset-size',action='store_true',help='print the selected preset size, if reduced')
    p.add_argument('--print-scaled',action='store_true',
                   help='print "RENDER OUTPUT" when a resolution patch renders the game at another size')
    a=p.parse_args()
    if a.print_scaled:
        sizes=scaled_sizes(read_settings(a.settings))
        if sizes: print(f'{sizes[0][0]}x{sizes[0][1]} {sizes[1][0]}x{sizes[1][1]}')
        return
    if a.print_preset_size:
        return  # no upscaler presets for this game yet
    names=ALWAYS+FPS_PRESETS[a.fps]+[n.strip() for n in a.extra.split(';') if n.strip()]
    names+=[n for n in effect_patches(read_settings(a.settings)) if n not in names]
    names=available(a.xml,names,a.app_version)
    segments=eboot_segments((a.out/'eboot.elf').read_bytes())
    writes=compile_patches(a.xml,names,a.app_version,segments)
    if a.render_res:
        w,h=(int(v) for v in a.render_res.lower().split('x'))
        if (w,h)!=OUTPUT_SIZE:
            writes+=resolution_writes((w,h),boot_image(a.out))
            print(f'Patches: the game renders at {w}x{h}')
    if a.output_res and DISPLAY_SITES:
        w,h=(int(v) for v in a.output_res.lower().split('x'))
        if (w,h)!=OUTPUT_SIZE:
            writes+=display_writes((w,h),boot_image(a.out))
            print(f'Patches: the game displays at {w}x{h}')
    if a.patches_dir:
        # After the built-in ones: an external patch of the same bytes wins.
        writes+=compile_external(external_selection(external_patches(a.patches_dir,a.app_version,a.xml),
                                                    a.patches_config),segments)
    # BBPATCH2: the patch base, so the loader can rebase pointers the patches write into
    # relocated slots.
    blob=struct.pack('<8sQQ',b'BBPATCH2',EBOOT_BASE,len(writes))
    for offset,data in writes: blob+=struct.pack('<QQ',offset,len(data))+data
    (a.out/'patches.bin').write_bytes(blob)
    print(f'Patches: FPS preset {a.fps}; {len(writes)} writes from {names or "none"}')


if __name__=='__main__':
    main()
