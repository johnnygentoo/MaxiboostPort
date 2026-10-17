"""Compile selected shadPS4/GoldHEN XML patches into out/patches.bin for the loader.

Patch addresses are PS4 virtual addresses (eboot base 0x400000); the loader's
image places eboot vaddr 0 at image offset 0. Only literal writes are supported
(bytes, bytes16/32/64, float32/64, utf8, utf16); pattern ("mask") patches are rejected.

The built-in patches are patches/Gundam.xml (GUNDAM VERSUS 1.13). Frame rate and effect switches
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
APP_VERSION='01.13'
BUILTIN_XML=Path(__file__).resolve().parent.parent/'patches/Gundam.xml'
# BB_FPS presets: patch names from patches/Gundam.xml. The game runs at 60 FPS (flip rate 0).
FPS_PRESETS={'60':[]}
# The game renders at 1920x1080. The places in the 1.13 executable that set its frame size are not
# mapped yet (RESOLUTION_SITES): until they are, every output size shows the 1080p frame scaled
# by the presenter, and no resolution patch is made.
OUTPUT_SIZE=(1920,1080)
RESOLUTION_SITES=[]
RESOLUTION_CLAMP=[]


def boot_image(out):
    """The prepared memory image (out/boot.bin, BBPROBE2), to check bytes before patching."""
    raw=(Path(out)/'boot.bin').read_bytes()
    magic,size,entry,ns,nr,ni,flags=struct.unpack_from('<8s6Q',raw,0)
    if magic!=b'BBPROBE2':
        raise ValueError('out/boot.bin is not a prepared image')
    pos=56+ns*24+ni*128+nr*32
    return raw[pos:pos+size]


def resolution_writes(size,image):
    """Writes that make the game render at size (w, h); the original bytes are checked first."""
    w,h=size
    if not (256<=w<=7680 and 144<=h<=4320) or w%2 or h%2:
        raise ValueError(f'unsupported render size {w}x{h}')
    writes=[]
    for wa,ha in RESOLUTION_SITES:
        if struct.unpack_from('<I',image,wa)[0]!=1920 or struct.unpack_from('<I',image,ha)[0]!=1080:
            raise ValueError(f'the executable has no 1920x1080 at {wa:#x}: not GUNDAM VERSUS 1.13')
        writes+=[(wa,struct.pack('<I',w)),(ha,struct.pack('<I',h))]
    for at,original,patched in RESOLUTION_CLAMP:
        if image[at:at+len(original)]!=original:
            raise ValueError(f'unexpected display clamp at {at:#x}: not GUNDAM VERSUS 1.13')
        writes.append((at,patched))
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
    if not RESOLUTION_SITES:
        return None  # the frame size is not mapped yet: the presenter scales the 1080p frame
    out=output_size(settings)
    scale=1.0
    if settings.get('upscaler','off') in TEMPORAL_UPSCALERS:
        preset=int(settings.get('preset','0') or 0)
        scale=PRESET_SCALES[max(0,min(preset,len(PRESET_SCALES)-1))]
    render=tuple(max(2,round(v/scale/2)*2) for v in out)
    if render==out==OUTPUT_SIZE:
        return None
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
        if name not in have: print(f'Patches: {name!r} not available for GUNDAM VERSUS {app_version} yet',file=sys.stderr)
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
GAME_IDS={'CUSA08790'}


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
    p.add_argument('--game-dir',type=Path,default=Path(os.environ.get('BB_GAME_DIR','CUSA08790-full')))
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
    names=FPS_PRESETS[a.fps]+[n.strip() for n in a.extra.split(';') if n.strip()]
    names+=[n for n in effect_patches(read_settings(a.settings)) if n not in names]
    names=available(a.xml,names,a.app_version)
    segments=eboot_segments((a.out/'eboot.elf').read_bytes())
    writes=compile_patches(a.xml,names,a.app_version,segments)
    if a.render_res:
        w,h=(int(v) for v in a.render_res.lower().split('x'))
        if (w,h)!=OUTPUT_SIZE:
            writes+=resolution_writes((w,h),boot_image(a.out))
            print(f'Patches: the game renders at {w}x{h}')
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
