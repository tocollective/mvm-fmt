<!--
Copyright 2026 tocollective contributors
SPDX-License-Identifier: Apache-2.0

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    https://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
-->

# mvm-fmt

`mvm-fmt` formats M source using the compiler's lexer and grammar. It preserves
the order and spelling of tokens, comments, numeric literals and string/character
literals. Imports do not need to exist. The formatted result is parsed again and
checked for identical tokens and syntax roles before it can be written.

From the common parent of `mvm/` and `mvm-fmt/`, build with CMake, Flex, Bison and libffi:

```sh
cmake -S mvm -B mvm/build -DBUILD_TESTING=ON
cmake --build mvm/build --target mvm-fmt
mvm-fmt/bin/mvm-fmt program.m                         # stdout
mvm-fmt/bin/mvm-fmt --write program.m library.m       # replace files
mvm-fmt/bin/mvm-fmt --check program.m library.m       # CI, without modification
mvm-fmt/bin/mvm-fmt --config style.toml program.m
mvm-fmt/bin/mvm-fmt --no-config program.m             # built-in defaults
cat program.m | mvm-fmt/bin/mvm-fmt
mvm-fmt/bin/mvm-fmt --stdin-filepath src/program.m < program.m
mvm-fmt/bin/mvm-fmt --write -- -filename.m
```

The executable is written to `mvm-fmt/bin/mvm-fmt` (`mvm-fmt/bin/mvm-fmt.exe` on
Windows), including with multi-configuration generators. Intermediate files
remain in `mvm/build/`.

Without filenames, input comes from stdin. Stdout mode accepts one input;
`--write` and `--check` accept multiple files. Use shell globs for file selection.
Directories are rejected. `--write` cannot be used with stdin.
`--stdin-filepath` supplies a source path for diagnostics and config discovery
without reading that path.

Exit codes: `0` on success, `1` when `--check` finds differences, `2` on an error.
Check mode prints differing paths to stdout; diagnostics go to stderr. An error
takes precedence over differences when checking several files.

## Configuration

For each source, search its containing directory and then its ancestors for
the nearest `.mformat.toml`. For stdin, start at the working directory or the
directory of `--stdin-filepath`. Configurations are not merged. `--config`
selects a file explicitly; `--no-config` disables discovery. Missing explicit
configs, invalid TOML, unknown keys and invalid values are errors.

The repository's [configuration](../mvm/.mformat.toml) contains every supported option
with its default value. The file must contain only a `[format]` table; all options
within it are optional.

| Option | Default | Accepted values / behavior |
| --- | --- | --- |
| `indent_style` | `"space"` | `"space"` or `"tab"`; one tab per level |
| `indent_width` | `4` | `1..16`; spaces per level, visual tab width for wrapping |
| `line_width` | `100` | `40..1000`; soft limit for wrapping comma lists |
| `max_blank_lines` | `1` | `0..10`; maximum consecutive blank lines |
| `brace_style` | `"same_line"` | `"same_line"` or `"next_line"` for block braces |
| `else_new_line` | `false` | Put `else` on a new line |
| `space_around_operators` | `true` | Spaces around binary/assignment/ternary operators |
| `space_after_comma` | `true` | Space after inline commas |
| `space_after_colon` | `true` | Space after declaration/type colons |
| `space_inside_braces` | `true` | Spaces inside inline import/export/record braces |
| `space_around_range` | `false` | Spaces around `..` and `...` |
| `final_newline` | `true` | End nonempty files with a newline |
| `line_ending` | `"lf"` | `"lf"` or `"crlf"` for generated newlines |

Required lexical separators remain even when optional spaces are disabled:
`a - -b` must not become `a--b`. Generics use tight angle brackets, and unary
operators remain attached to their operands where the lexer permits it.
Statements and nonempty blocks receive structural line breaks. Existing line
breaks inside expressions/lists are retained; bare control bodies retain their
inline/multiline form. The soft line limit wraps comma lists and does not split
long expressions, comments or literals. Empty input remains empty.

Bytes inside literals and block comments are preserved, including their internal
line endings and indentation. Whitespace surrounding tokens is normalized;
trailing blank lines are removed. Comment text is retained, including whitespace
inside comments.

## File replacement and verification

In-place writes use an exclusively created temporary file in the same directory,
flush its contents, preserve permission bits and atomically replace the original.
Unchanged files are left untouched. Symlinks and files with multiple hard links
are refused when replacement is needed. Concurrent changes detected before
replacement abort that file. File replacement is atomic per file; an error in a
later file does not roll back earlier files. Replacing a file creates a new inode;
extended attributes and ACLs are not copied.

```sh
cmake --build mvm/build --target mvm-fmt mfmt_test
ctest --test-dir mvm/build -R '^mfmt_' --output-on-failure
```

The C tests cover golden output and idempotence for all example, library and
benchmark sources under three configurations. If Python 3 is available, CLI
tests also check strict TOML validation, config discovery, permissions, safe
replacement, exit codes and identical compiler MIR before/after formatting.

The `mfmt_lib` C API is declared in [mfmt.h](include/mfmt.h); returned output
buffers belong to the caller and must be released with `free()`.

## License

Copyright 2026 tocollective contributors.

Licensed under the Apache License, Version 2.0. See [LICENSE](LICENSE) for the full license text.

Unless required by applicable law or agreed to in writing, software distributed
under the License is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
CONDITIONS OF ANY KIND, either express or implied. See the License for the
specific language governing permissions and limitations under the License.

Bundled third-party license texts are included in
[THIRD_PARTY_LICENSES.txt](THIRD_PARTY_LICENSES.txt). The build copies both license
files alongside the executable in `bin/`.
