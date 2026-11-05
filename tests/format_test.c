/*
 * Copyright 2026 tocollective contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     https://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "mfmt.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define REQUIRE(condition) do { \
  if (!(condition)) { \
    fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #condition); \
    exit(1); \
  } \
} while (0)

static void check(const char *source, const char *expected, MfmtOptions options) {
  char *formatted = NULL, *again = NULL;
  size_t length = 0, again_length = 0;
  REQUIRE(mfmt_format(source, strlen(source), "test.m", &options, &formatted, &length));
  if (expected && strcmp(formatted, expected)) {
    fprintf(stderr, "expected:\n%s\nactual:\n%s\n", expected, formatted);
    exit(1);
  }
  REQUIRE(length == strlen(formatted));
  REQUIRE(mfmt_format(formatted, length, "test.m", &options, &again, &again_length));
  REQUIRE(length == again_length && memcmp(formatted, again, length) == 0);
  free(formatted);
  free(again);
}

static char *read_file(const char *path) {
  FILE *file = fopen(path, "rb");
  REQUIRE(file != NULL);
  REQUIRE(fseek(file, 0, SEEK_END) == 0);
  long size = ftell(file);
  REQUIRE(size >= 0 && fseek(file, 0, SEEK_SET) == 0);
  char *data = malloc((size_t)size + 1);
  REQUIRE(data != NULL);
  REQUIRE(fread(data, 1, (size_t)size, file) == (size_t)size);
  data[size] = 0;
  REQUIRE(fclose(file) == 0);
  return data;
}

int main(int argc, char **argv) {
  MfmtOptions options = mfmt_default_options();
  check("let main():Word{let mut x:Word=1 if x>0{x+=1}else{x=- -1}return x}",
        "let main(): Word {\n"
        "    let mut x: Word = 1\n"
        "    if x > 0 {\n        x += 1\n    } else {\n        x = - -1\n    }\n"
        "    return x\n}\n", options);
  check("import{a,b as c}from\"library.m\"\nlet value:Word=1",
        "import { a, b as c } from \"library.m\"\nlet value: Word = 1\n", options);
  check("let p:Point={.x=1,.y=2}",
        "let p: Point = { .x = 1, .y = 2 }\n", options);
  check("// head\r\nlet x:Word=1//tail\r\n\r\nlet y:Word=2\r\n",
        "// head\nlet x: Word = 1 //tail\n\nlet y: Word = 2\n", options);
  check("/* outer /* nested */ end */\nlet s:String=\"one\n  two\"",
        "/* outer /* nested */ end */\nlet s: String = \"one\n  two\"\n", options);
  check("let f():Void{switch 1{case 0:case 1:return default:{return}}}",
        "let f(): Void {\n    switch 1 {\n        case 0:\n        case 1:\n"
        "            return\n        default: {\n            return\n        }\n    }\n}\n", options);
  check("", "", options);
  check(" \r\n\t ", "", options);
  check("// only", "// only\n", options);
  check("// only\r\r", "// only\n", options);
  check("// head\r\r\nlet n:Word=1", "// head\n\nlet n: Word = 1\n", options);
  check("let n:Word=true?1:2", "let n: Word = true ? 1 : 2\n", options);
  check("let f():Word{if true\nreturn 1\nreturn 2}",
        "let f(): Word {\n    if true\n        return 1\n    return 2\n}\n", options);
  check("let f():Word{while true\nif false\nreturn 1\nelse\nreturn 2\nreturn 3}",
        "let f(): Word {\n    while true\n        if false\n            return 1\n"
        "        else\n            return 2\n    return 3\n}\n", options);
  check("type Box<T>{value:T,}\nlet x:Box<Box<Word>> = {.value={.value=1}}", NULL, options);
  check("let f(x:String?[]):String?{let y:((word:Word):Void)?=null if x[0]!=null{return x[0]}return null}",
        "let f(x: String?[]): String? {\n    let y: ((word: Word): Void)? = null\n"
        "    if x[0] != null {\n        return x[0]\n    }\n    return null\n}\n", options);
  options.space_around_operators = false;
  options.space_after_colon = false;
  options.space_after_comma = false;
  options.space_inside_braces = false;
  check("let f():Word{let n:Word=1 - -1 if n < -1{return - -n}return n}",
        "let f():Word {\n    let n:Word=1- -1\n    if n<-1 {\n"
        "        return - -n\n    }\n    return n\n}\n", options);
  check("let f():Word{return 1 / /* comment */ 2}", NULL, options);
  options = mfmt_default_options();
  options.indent_tabs = true;
  options.braces_next_line = true;
  options.else_new_line = true;
  options.crlf = true;
  options.final_newline = false;
  check("let f():Word{if true{return 1}else{return 2}}",
        "let f(): Word\r\n{\r\n\tif true\r\n\t{\r\n\t\treturn 1\r\n\t}\r\n"
        "\telse\r\n\t{\r\n\t\treturn 2\r\n\t}\r\n}", options);
  /* Literal bytes are independent of the requested line-ending style. */
  check("let s:String=\"a\nb\"", "let s: String = \"a\nb\"", options);
  char *failed = (char *)1;
  size_t failed_length = 1;
  REQUIRE(!mfmt_format("let x:Word=", 11, "invalid.m", &options, &failed, &failed_length));
  REQUIRE(!failed && !failed_length);
  REQUIRE(!mfmt_format("\0", 1, "invalid.m", &options, &failed, &failed_length));
  for (int i = 1; i < argc; ++i) {
    char *source = read_file(argv[i]);
    for (unsigned style = 0; style < 3; ++style) {
      options = mfmt_default_options();
      if (style == 1) {
        options.indent_width = 2;
        options.line_width = 40;
        options.braces_next_line = true;
        options.else_new_line = true;
        options.max_blank_lines = 0;
      } else if (style == 2) {
        options.indent_tabs = true;
        options.space_around_operators = false;
        options.space_inside_braces = false;
        options.space_after_comma = false;
        options.space_after_colon = false;
        options.crlf = true;
        options.final_newline = false;
      }
      fprintf(stderr, "corpus: %s (style %u)\n", argv[i], style);
      check(source, NULL, options);
    }
    free(source);
  }
  return 0;
}
