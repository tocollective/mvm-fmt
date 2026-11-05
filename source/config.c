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
#include "tomlc17.h"

#include <stdio.h>
#include <string.h>

MfmtOptions mfmt_default_options(void) {
  return (MfmtOptions){
      .indent_width = 4,
      .line_width = 100,
      .max_blank_lines = 1,
      .space_around_operators = true,
      .space_after_comma = true,
      .space_after_colon = true,
      .space_inside_braces = true,
      .final_newline = true,
  };
}

static bool string_is(toml_datum_t value, const char *text) {
  return value.type == TOML_STRING &&
         (size_t)value.u.str.len == strlen(text) &&
         memcmp(value.u.str.ptr, text, strlen(text)) == 0;
}

bool mfmt_load_config(const char *path, MfmtOptions *options,
                      char *error, size_t error_capacity) {
  toml_result_t document = toml_parse_file_ex(path);
  bool ok = false;
  const char *reason = NULL;
  const char *key = "format";
  MfmtOptions next = *options;
  if (!document.ok) {
    snprintf(error, error_capacity, "%s: %s", path, document.errmsg);
    toml_free(document);
    return false;
  }
  toml_datum_t table = toml_get(document.toptab, "format");
  if (document.toptab.u.tab.size != 1 || table.type != TOML_TABLE) {
    reason = "expected only a [format] table";
    goto done;
  }
  for (int32_t i = 0; i < table.u.tab.size; ++i) {
    key = table.u.tab.key[i];
    toml_datum_t value = table.u.tab.value[i];
    /* Length checks also reject TOML keys containing embedded NUL bytes. */
#define KEY(name) ((size_t)table.u.tab.len[i] == sizeof(name) - 1 && strcmp(key, name) == 0)
#define BOOL_OPTION(name, field) \
    if (KEY(name)) { \
      if (value.type != TOML_BOOLEAN) { reason = "expected a boolean"; goto done; } \
      next.field = value.u.boolean; continue; \
    }
    BOOL_OPTION("else_new_line", else_new_line)
    BOOL_OPTION("space_around_operators", space_around_operators)
    BOOL_OPTION("space_after_comma", space_after_comma)
    BOOL_OPTION("space_after_colon", space_after_colon)
    BOOL_OPTION("space_inside_braces", space_inside_braces)
    BOOL_OPTION("space_around_range", space_around_range)
    BOOL_OPTION("final_newline", final_newline)
#undef BOOL_OPTION
    if (KEY("indent_width") || KEY("line_width") || KEY("max_blank_lines")) {
      const int64_t minimum = KEY("indent_width") ? 1 : KEY("line_width") ? 40 : 0;
      const int64_t maximum = KEY("indent_width") ? 16 : KEY("line_width") ? 1000 : 10;
      if (value.type != TOML_INT64 || value.u.int64 < minimum || value.u.int64 > maximum) {
        reason = "expected indent_width in 1..16, line_width in 40..1000, or max_blank_lines in 0..10";
        goto done;
      }
      if (KEY("indent_width")) next.indent_width = (unsigned)value.u.int64;
      else if (KEY("line_width")) next.line_width = (unsigned)value.u.int64;
      else next.max_blank_lines = (unsigned)value.u.int64;
      continue;
    }
    if (KEY("indent_style")) {
      if (!string_is(value, "space") && !string_is(value, "tab")) {
        reason = "expected 'space' or 'tab'"; goto done;
      }
      next.indent_tabs = string_is(value, "tab");
    } else if (KEY("brace_style")) {
      if (!string_is(value, "same_line") && !string_is(value, "next_line")) {
        reason = "expected 'same_line' or 'next_line'"; goto done;
      }
      next.braces_next_line = string_is(value, "next_line");
    } else if (KEY("line_ending")) {
      if (!string_is(value, "lf") && !string_is(value, "crlf")) {
        reason = "expected 'lf' or 'crlf'"; goto done;
      }
      next.crlf = string_is(value, "crlf");
    } else {
      reason = "unknown option"; goto done;
    }
#undef KEY
  }
  *options = next;
  ok = true;
done:
  if (!ok)
    snprintf(error, error_capacity, "%s: format.%s: %s", path, key, reason);
  toml_free(document);
  return ok;
}
