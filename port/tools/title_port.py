"""Title mods (docs/TITLE_MODS.md): a championship's name, belt textures and menu picture replaced.

    py tools/title_port.py titles
    py tools/title_port.py make --game <SvR 2011 game folder or ISO> --title <key> --out <dir> [--name <name>]
        [--short <name>] [--champion <name>] [--picture <png>] [--art <png>] [--set <belt texture>=<picture> ...] [--id <folder>] [--author <name>] [--version <v>] [--svrmod]

titles lists the keys (IC, US, WWE, WHC, WWE_TAG, WORLD_TAG, ECW, DIVAS, WOMENS, HARDCORE, CRUISER, MILLION, WCW)
with their names, belt textures (data/belt_weapon_textures.json) and menu pictures (data/title_pictures.json).
make writes <out>/<id>/ - manifest.txt (type=title) and textures/ named as in texture packs
(<owner>_<texture>_<hash16>.dds / .png: the game serves them in place of the stock ones) - and with --svrmod
<out>/<id>.svrmod as well; <out>/report.json. The picture (--picture) goes in for the title's menu picture in
both menu sizes (menuHD / menuSD), any size. stdout is JSON only; exit 0 ok / 2 bad input / 3 failed.
"""
import argparse
import io
import json
import os
import re
import sys
import zipfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import discfs  # noqa: E402
import reskin_port  # noqa: E402

KEYS = ['IC', 'US', 'WWE', 'WHC', 'WWE_TAG', 'WORLD_TAG', 'ECW', 'DIVAS', 'WOMENS', 'HARDCORE', 'CRUISER', 'MILLION', 'WCW']


NAMES = {'WCW': 'WCW Championship'}  # (no belt table)


class BadInput(Exception):
    pass


def pictures(part='pictures'):
    with open(os.path.join(HERE, 'data', 'title_pictures.json')) as f:
        return json.load(f).get(part, {})


def titles():
    belts = {b['key']: b for b in reskin_port.table('belt')}
    pics = pictures()
    out = []
    for k in KEYS:
        b = belts.get(k, {})
        used = {}
        for o in belts.values():
            if o['key'] != k:
                for t in o['textures']:
                    used[t['hash16']] = True
        out.append({'key': k, 'name': b.get('name', NAMES.get(k, k)), 'belt_textures': [t['name'] for t in b.get('textures', [])],
                    'belt': [{'name': t['name'], 'width': t['width'], 'height': t['height'], 'shared': t['hash16'] in used}
                             for t in b.get('textures', [])],
                    'pictures': [{x: p[x] for x in ('name', 'owner', 'width', 'height')} for p in pics.get(k, [])],
                    'art': [{x: p[x] for x in ('name', 'owner')} for p in pictures('art').get(k, [])]})
    return out


def make(a):
    key = a.title.upper()
    if key not in KEYS:
        raise BadInput('--title %r: one of %s' % (a.title, ', '.join(KEYS)))
    if not (a.name or a.picture or a.art or a.set):
        raise BadInput('nothing to replace: give --name, --picture, --art or --set')
    from PIL import Image
    folder_id = re.sub(r'[^a-z0-9]+', '_', (a.id or ('%s_%s' % (a.name or 'title', key))).lower()).strip('_')[:40]
    mod = os.path.join(a.out, folder_id)
    tex = os.path.join(mod, 'textures')
    os.makedirs(tex, exist_ok=True)
    notes, warnings = [], []
    if a.picture:
        if not os.path.exists(a.picture):
            raise BadInput('%s: not found' % a.picture)
        img = Image.open(a.picture).convert('RGBA')
        for p in pictures()[key]:
            if img.width * p['height'] != img.height * p['width']:
                warnings.append('the picture is %dx%d, the menu\'s is %dx%d (other proportions stretch)'
                                % (img.width, img.height, p['width'], p['height']))
            img.save(os.path.join(tex, '%s_%s_%s.png' % (p['owner'], p['name'], p['hash16'])))
        notes.append('menu picture: title select, championship screens (menuHD and menuSD)')
    if a.art:  # (the championship screens' art, BG_<title>: Championship Scramble's picture beside the holder)
        if not os.path.exists(a.art):
            raise BadInput('%s: not found' % a.art)
        art = pictures('art').get(key, [])
        if not art:
            raise BadInput('the %s title has no championship art in the game (the tag titles)' % key)
        img = Image.open(a.art).convert('RGBA')
        for p in art:
            img.save(os.path.join(tex, '%s_%s_%s.png' % (p['owner'], p['name'], p['hash16'])))
        notes.append('championship art: the picture beside the holder (Championship Scramble; menuHD and menuSD)')
    if a.set:
        belt = next((b for b in reskin_port.table('belt') if b['key'] == key), None)
        if not belt:
            raise BadInput('no belt texture table for %s: give --name / --picture only' % key)
        for k, s in enumerate(a.set):
            if '=' not in s:
                raise BadInput('--set %r: <texture>=<picture>' % s)
            tname, pic = s.split('=', 1)
            t = next((t for t in belt['textures'] if t['name'].lower() == tname.lower()), None)
            if not t:
                raise BadInput('the %s belt has no texture %r (%s)' % (key, tname, ', '.join(x['name'] for x in belt['textures'])))
            if not os.path.exists(pic):
                raise BadInput('%s: not found' % pic)
            reskin_port.progress(10 + 80 * k // len(a.set), t['name'])
            with open(os.path.join(tex, '%s_%s_%s.dds' % (t['owner'], reskin_port.safe(t['name']), t['hash16'])), 'wb') as f:
                f.write(reskin_port.encode(Image.open(pic), t['format']))
        notes.append('belt textures: %d (entrances, celebrations, cut scenes, the belt picked up in a match)' % len(a.set))
    lines = ['type=title', 'id=' + folder_id, 'title=' + key]
    if a.name:
        lines.append('name=' + a.name)
        notes.append('name: "%s" wherever the game names the %s title' % (a.name, key))
    if a.champion:
        lines.append('champion=' + a.champion)
    if a.short:
        lines.append('short=' + a.short)
    if a.author:
        lines.append('author=' + a.author)
    lines += ['made_with=' + (a.made_with or 'Port tools (title_port)'), 'version=' + (a.version or '1.0')]
    with open(os.path.join(mod, 'manifest.txt'), 'w', encoding='utf-8', newline='\n') as f:
        f.write('\n'.join(lines) + '\n')
    rep = {'ok': True, 'out': mod, 'opens': 'title', 'warnings': warnings, 'dropped': [], 'notes': notes}
    if a.svrmod:
        path = os.path.join(a.out, folder_id + '.svrmod')
        with zipfile.ZipFile(path, 'w', zipfile.ZIP_DEFLATED) as z:
            for root, _, files in os.walk(mod):
                for fn in files:
                    full = os.path.join(root, fn)
                    z.write(full, os.path.relpath(full, mod).replace(os.sep, '/'))
        rep['svrmod'] = path
    with open(os.path.join(a.out, 'report.json'), 'w') as f:
        json.dump(rep, f, indent=1)
    reskin_port.progress(100, 'done')
    return rep


def main(argv):
    ap = argparse.ArgumentParser(prog='title_port.py')
    sub = ap.add_subparsers(dest='cmd', required=True)
    sub.add_parser('titles')
    p = sub.add_parser('make')
    p.add_argument('--game')
    p.add_argument('--title', required=True)
    p.add_argument('--out', required=True)
    p.add_argument('--name')
    p.add_argument('--short')
    p.add_argument('--champion')
    p.add_argument('--picture')
    p.add_argument('--art')
    p.add_argument('--set', action='append', default=[])
    p.add_argument('--id')
    p.add_argument('--author')
    p.add_argument('--version')
    p.add_argument('--made-with')
    p.add_argument('--svrmod', action='store_true')
    try:
        a = ap.parse_args(argv)
    except SystemExit:
        print(json.dumps({'ok': False, 'error': 'bad arguments'}))
        return 2
    try:
        print(json.dumps(titles() if a.cmd == 'titles' else make(a)))
        return 0
    except BadInput as e:
        print(json.dumps({'ok': False, 'error': str(e)}))
        return 2
    except Exception as e:  # noqa: BLE001
        print('failed: %r' % e, file=sys.stderr)
        print(json.dumps({'ok': False, 'error': str(e)}))
        return 3


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
