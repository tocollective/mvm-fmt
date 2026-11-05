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

#ifndef MFMT_H
#define MFMT_H

#include <stdbool.h>
#include <stddef.h>

typedef struct MfmtOptions {
  bool indent_tabs;
  unsigned indent_width;
  unsigned line_width;
  unsigned max_blank_lines;
  bool braces_next_line;
  bool else_new_line;
  bool space_around_operators;
  bool space_after_comma;
  bool space_after_colon;
  bool space_inside_braces;
  bool space_around_range;
  bool final_newline;
  bool crlf;
} MfmtOptions;

MfmtOptions mfmt_default_options(void);
/* Strict [format] table. Unspecified options retain their current values.
 * On failure options are unchanged and error receives a diagnostic. */
bool mfmt_load_config(const char *path, MfmtOptions *options,
                      char *error, size_t error_capacity);
/* Input is parsed with the M compiler grammar; imports are never loaded.
 * On success the caller owns *output (free), including for empty input.
 * Output is NUL terminated; output_length excludes the terminator.
 * Parser diagnostics are written to stderr. No partial output on failure. */
bool mfmt_format(const char *source, size_t length, const char *path,
                 const MfmtOptions *options, char **output,
                 size_t *output_length);

#endif
