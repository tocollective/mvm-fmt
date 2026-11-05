# Copyright 2026 tocollective contributors
# SPDX-License-Identifier: Apache-2.0
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     https://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

"""CLI contracts, config discovery, safe replacement and compiler equivalence."""
import argparse
import os
from pathlib import Path
import stat
import subprocess
import tempfile


parser = argparse.ArgumentParser()
parser.add_argument('--formatter', required=True)
parser.add_argument('--compiler', required=True)
parser.add_argument('--source-dir', type=Path, required=True)
parser.add_argument('--work-dir', type=Path, required=True)
args = parser.parse_args()
args.work_dir.mkdir(parents=True, exist_ok=True)


def run(*options, data=None, cwd=None, code=0):
    result = subprocess.run([args.formatter, *map(str, options)], input=data,
                            cwd=cwd, capture_output=True, timeout=30)
    assert result.returncode == code, (options, result.returncode, result.stderr)
    return result


with tempfile.TemporaryDirectory(dir=args.work_dir) as temporary:
    root = Path(temporary).resolve()
    nested = root / 'nested'
    nested.mkdir()
    config = root / '.mformat.toml'
    config.write_text('[format]\nindent_width = 2\n')
    source = nested / 'program.m'
    original = b'let main():Word{return 0}'
    source.write_bytes(original)
    expected = b'let main(): Word {\n  return 0\n}\n'
    assert run(source).stdout == expected
    assert run('--stdin-filepath', source, data=original).stdout == expected
    assert run(data=original, cwd=nested).stdout == expected
    assert run('--no-config', source).stdout == expected.replace(b'  return', b'    return')
    assert run('--check', source, code=1).stdout == (str(source) + '\n').encode()
    assert source.read_bytes() == original
    source.chmod(0o640)
    run('--write', source)
    assert source.read_bytes() == expected
    if os.name != 'nt':
        assert stat.S_IMODE(source.stat().st_mode) == 0o640
    previous = source.stat().st_mtime_ns
    run('--write', source)
    assert source.stat().st_mtime_ns == previous
    run('--check', source)
    local_config = nested / '.mformat.toml'
    local_config.write_text('[format]\nindent_style = "tab"\n')
    assert b'\treturn' in run(source).stdout
    assert run('--config', config, source).stdout == expected
    local_config.unlink()

    configurations = {
        'indent_style = "tab"': b'\treturn',
        'brace_style = "next_line"': b'Word\n{',
        'space_after_colon = false': b'main():Word',
        'final_newline = false': b'}',
        'line_ending = "crlf"': b'\r\n',
    }
    for option, fragment in configurations.items():
        config.write_text('[format]\n' + option + '\n')
        output = run(source).stdout
        assert fragment in output, (option, output)
        if option == 'final_newline = false':
            assert output.endswith(b'}')
        assert run(data=output, cwd=root).stdout == output
    config.write_text('''[format]
indent_style = "space"
indent_width = 2
line_width = 40
max_blank_lines = 0
brace_style = "same_line"
else_new_line = true
space_around_operators = false
space_after_comma = false
space_after_colon = false
space_inside_braces = false
space_around_range = true
final_newline = true
line_ending = "lf"
''')
    compact = run(data=b'let f():Word{let mut x:Word=1+2 if x>1{x--}else{x++}for i:Word in 0..3{x+=i}return x}', cwd=root).stdout
    assert b'x:Word=1+2' in compact and b'0 .. 3' in compact
    assert b'}\n  else {' in compact
    assert run(data=compact, cwd=root).stdout == compact
    long_source = (b'let f(first: Word, second: Word, third: Word, fourth: Word): Word '
                   b'{ return first + second + third + fourth }')
    wrapped = run(data=long_source, cwd=root).stdout
    assert b'f(\n' in wrapped and b'first:Word,\n' in wrapped
    assert run(data=wrapped, cwd=root).stdout == wrapped

    invalid_configs = (
        '[format]\nindent_width = 0', '[format]\nindent_width = 17',
        '[format]\nindent_width = true', '[format]\nindent_style = "spaces"',
        '[format]\nbrace_style = "allman"', '[format]\nline_width = 39',
        '[format]\nline_width = 1001', '[format]\nmax_blank_lines = -1',
        '[format]\nline_ending = "native"', '[format]\nfinal_newline = 1',
        '[format]\nspace_after_comma = "yes"', '[format]\ntypo = true',
        '[format]\nindent_width = 2\nindent_width = 4', '[format',
        'indent_width = 2', '[other]\nx = 1',
        '[format]\n[other]\nx = 1',
        '[format]\n"indent_width\\u0000ignored" = 2',
    )
    for content in invalid_configs:
        config.write_text(content)
        before = source.read_bytes()
        result = run('--write', source, code=2)
        assert result.stderr and not result.stdout
        assert source.read_bytes() == before
    run('--config', root / 'missing.toml', source, code=2)
    config.write_text('[format]\n')
    invalid_source = nested / 'invalid.m'
    for content in (b'let main(): Word {', b'/* unclosed', b'let s:String="unclosed',
                    b'let s:String="bad\\q"', b'let n:Word=0\0', b'\xef\xbb\xbflet n:Word=1'):
        invalid_source.write_bytes(content)
        result = run('--write', invalid_source, code=2)
        assert result.stderr and invalid_source.read_bytes() == content
        assert not list(nested.glob('*.tmp'))
    run('--check', source, invalid_source, code=2)
    run('--write', '--check', source, code=2)
    run('--write', data=original, code=2)
    run('--config', config, '--no-config', source, code=2)
    run(source, source, code=2)
    run('--stdin-filepath', source, source, code=2)
    run('--config', code=2)
    run('--unknown', code=2)
    run('--no-config', nested, code=2)
    assert b'usage:' in run('--help').stdout
    if os.name != 'nt':
        source.write_bytes(original)
        link = nested / 'link.m'
        link.symlink_to(source.name)
        run('--write', link, code=2)
        assert link.is_symlink() and source.read_bytes() == original
        hard_link = nested / 'hard.m'
        os.link(source, hard_link)
        run('--write', hard_link, code=2)
        assert hard_link.read_bytes() == original
        hard_link.unlink()
    # A literal dash-prefixed filename works after --.
    dashed = nested / '-file.m'
    dashed.write_bytes(original)
    assert run('--no-config', '--', '-file.m', cwd=nested).stdout.endswith(b'}\n')

    # Format real programs, compile both versions to MIR and compare bytes.
    # Keeping the basename and imports identical makes the comparison exact.
    for name in ('generics.m', 'statements.m', 'lexical.m', 'references.m', 'union.m'):
        original_path = args.source_dir / 'mvm-c' / 'examples' / name
        formatted_path = root / name
        formatted_path.write_bytes(run('--no-config', original_path).stdout)
        mir_paths = []
        for label, path in (('before', original_path), ('after', formatted_path)):
            mir = root / (name + '.' + label + '.mir')
            result = subprocess.run(
                [args.compiler, '-I', str(args.source_dir / 'mvm-c' / 'examples'),
                 '-I', str(args.source_dir / 'mvm-lib'), str(path), '-o', str(mir)],
                capture_output=True, timeout=30)
            assert result.returncode == 0, result.stderr
            mir_paths.append(mir)
        assert mir_paths[0].read_bytes() == mir_paths[1].read_bytes(), name

print('mvm-fmt CLI: OK')
