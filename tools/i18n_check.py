"""Checks the UI translations (assets/i18n/<code>.json) against the sources.

    python tools/i18n_check.py                     # overview: keys used, unwrapped Turkish literals
    python tools/i18n_check.py --lang de           # one translation: missing / unused keys, placeholders, plurals
    python tools/i18n_check.py --all [--strict]    # every translation; --strict exits 1 on any problem
    python tools/i18n_check.py --dump keys.json    # every key with where it is used and whether it is a plural

The Turkish source text is the key (see src/core/I18n.h). A key used through plural() needs the CLDR forms of
the language ("one"/"other"; ru/uk "one"/"few"/"many"; ja/ko/id only "other"); every form must keep the same
number of "{}" placeholders as the key.
"""
import json
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC = os.path.join(ROOT, 'src')
I18N = os.path.join(ROOT, 'assets', 'i18n')
LANGS = ['en', 'de', 'es', 'fr', 'pt', 'ru', 'uk', 'id', 'ja', 'ko']
PLURAL_FORMS = {
    'en': ['one', 'other'], 'de': ['one', 'other'], 'es': ['one', 'other'], 'fr': ['one', 'other'],
    'pt': ['one', 'other'], 'ru': ['one', 'few', 'many'], 'uk': ['one', 'few', 'many'],
    'id': ['other'], 'ja': ['other'], 'ko': ['other'],
}

LIT = r'L"((?:[^"\\]|\\.)*)"'
KEY_CALL = re.compile(r'\b(?:i18n::)?(tr|plural)\(\s*((?:' + LIT + r'\s*)+)')
ANY_LIT = re.compile(LIT)
TURKISH = re.compile(r'[çğıöşüÇĞİÖŞÜ]|\b(?:ve|ile|bir|için|yok|değil|şarkı|liste|ekle|kaldır|aç|kapat|sil|ara|Ara|Tamam|Vazgeç)\b')
SKIP_LINE = re.compile(r'ST_LOG_|static_assert|#include')


ESC = re.compile(r'\\(u[0-9a-fA-F]{4}|U[0-9a-fA-F]{8}|x[0-9a-fA-F]+|[0-7]{1,3}|.)')
SIMPLE = {'n': '\n', 't': '\t', 'r': '\r', '\\': '\\', '"': '"', "'": "'", '?': '?', 'a': '\a', 'b': '\b', 'f': '\f', 'v': '\v'}


def unescape(s):
    """C++ wide-literal escapes -> text (the key as the program sees it)."""
    def rep(m):
        e = m.group(1)
        if e[0] in 'uUx':
            return chr(int(e[1:], 16))
        if e[0].isdigit():
            return chr(int(e, 8))
        return SIMPLE.get(e, e)
    return ESC.sub(rep, s)


def join_literals(chunk):
    return ''.join(unescape(m.group(1)) for m in ANY_LIT.finditer(chunk))


def sources():
    for base in ('app', 'ui', 'core', 'spotify', 'youtube', 'player', 'audio', 'lyrics', 'musicbrainz', 'gfx'):
        for dirpath, _, files in os.walk(os.path.join(SRC, base)):
            for f in sorted(files):
                if f.endswith(('.cpp', '.h')):
                    yield os.path.join(dirpath, f)


def scan():
    used = {}          # key -> {"plural": bool, "where": [...]}
    unwrapped = []
    for path in sources():
        rel = os.path.relpath(path, ROOT).replace('\\', '/')
        text = open(path, encoding='utf-8-sig').read()
        lines = text.split('\n')
        starts = [0]
        for l in lines:
            starts.append(starts[-1] + len(l) + 1)
        spans = []
        for m in KEY_CALL.finditer(text):
            line_start = text.rfind('\n', 0, m.start()) + 1
            if text[line_start:m.start()].lstrip().startswith('//'):
                continue   # examples in comments
            key = join_literals(m.group(2))
            lineno = text.count('\n', 0, m.start()) + 1
            e = used.setdefault(key, {'plural': False, 'where': []})
            e['plural'] |= m.group(1) == 'plural'
            e['where'].append(f'{rel}:{lineno}')
            spans.append((m.start(2), m.end(2)))
        if not (rel.startswith('src/app/') or rel.startswith('src/ui/') or rel.startswith('src/player/')):
            continue
        for i, line in enumerate(lines):
            if SKIP_LINE.search(line) or line.lstrip().startswith('//'):
                continue
            for m in ANY_LIT.finditer(line):
                pos = starts[i] + m.start()
                if any(a <= pos < b for a, b in spans):
                    continue
                s = unescape(m.group(1))
                if TURKISH.search(s):
                    unwrapped.append((rel, i + 1, s))
    return used, unwrapped


def check_lang(code, used):
    path = os.path.join(I18N, code + '.json')
    problems = []
    if not os.path.exists(path):
        return [f'{code}: file missing'], 0
    try:
        table = json.load(open(path, encoding='utf-8'))
    except Exception as ex:
        return [f'{code}: invalid JSON: {ex}'], 0
    for key, info in sorted(used.items()):
        if key not in table:
            problems.append(f'{code}: missing {json.dumps(key, ensure_ascii=False)}')
            continue
        want = key.count('{}')
        value = table[key]
        if isinstance(value, str):
            forms = {'other': value}
            if info['plural'] and len(PLURAL_FORMS[code]) > 1:
                problems.append(f'{code}: plural key needs {PLURAL_FORMS[code]}: {json.dumps(key, ensure_ascii=False)}')
        elif isinstance(value, dict):
            forms = value
            for f in PLURAL_FORMS[code] if info['plural'] else []:
                if f not in forms and not (f == 'other' and forms):
                    problems.append(f'{code}: plural form "{f}" missing for {json.dumps(key, ensure_ascii=False)}')
        else:
            problems.append(f'{code}: bad value for {json.dumps(key, ensure_ascii=False)}')
            continue
        for form, text in forms.items():
            if not isinstance(text, str) or not text.strip():
                problems.append(f'{code}: empty {form} for {json.dumps(key, ensure_ascii=False)}')
            elif text.count('{}') != want:
                problems.append(f'{code}: {form} has {text.count("{}")} placeholders, key has {want}: '
                                f'{json.dumps(key, ensure_ascii=False)}')
    unused = [k for k in table if k not in used]
    for k in unused:
        problems.append(f'{code}: unused {json.dumps(k, ensure_ascii=False)}')
    return problems, len(table)


def main():
    args = sys.argv[1:]
    used, unwrapped = scan()
    if '--dump' in args:
        out = args[args.index('--dump') + 1]
        json.dump(used, open(out, 'w', encoding='utf-8'), ensure_ascii=False, indent=1)
        print(f'{len(used)} keys ({sum(1 for v in used.values() if v["plural"])} plural) -> {out}')
        return 0
    langs = LANGS if '--all' in args else ([args[args.index('--lang') + 1]] if '--lang' in args else [])
    if langs:
        total = 0
        for code in langs:
            problems, n = check_lang(code, used)
            total += len(problems)
            print(f'{code}: {n} texts, {len(problems)} problem(s)')
            for p in problems[:200]:
                print('   ', p)
        return 1 if ('--strict' in args and total) else 0
    print(f'{len(used)} keys used ({sum(1 for v in used.values() if v["plural"])} plural)')
    print(f'unwrapped Turkish literals ({len(unwrapped)}):')
    for f, n, s in unwrapped:
        print(f'   {f}:{n}: {s}')
    return 1 if ('--strict' in args and unwrapped) else 0


if __name__ == '__main__':
    sys.exit(main())
