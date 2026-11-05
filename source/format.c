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
#include "mc.h"
#include "parser.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>

int mc_yyparse(void *scanner, McParser *parser);
int mc_yylex_init(void **scanner);
int mc_yylex_destroy(void *scanner);
void mc_yyset_extra(McParser *parser, void *scanner);
struct yy_buffer_state;
struct yy_buffer_state *mc_yy_scan_bytes(const char *bytes, int length, void *scanner);

enum { COMMENT_BLOCK = -1, COMMENT_LINE = -2 };

typedef struct Token {
  int kind;
  size_t offset, length, match;
  size_t body_end, flat_width;
  unsigned marks, breaks;
  bool multiline, comma;
} Token;

typedef struct Syntax {
  const char *source;
  size_t length, consumed;
  Token *tokens;
  size_t count, capacity;
  size_t *lines;
  size_t line_count, line_capacity;
  size_t body_start;
  bool failed;
} Syntax;

typedef struct Output {
  char *data;
  size_t length, capacity, column;
  bool failed;
} Output;

typedef struct Frame {
  size_t token;
  bool case_body;
} Frame;

static bool reserve(void **data, size_t *capacity, size_t count, size_t size) {
  if (count <= *capacity) return true;
  size_t next = *capacity ? *capacity : 64;
  while (next < count) {
    if (next > SIZE_MAX / 2) { next = count; break; }
    next *= 2;
  }
  if (next > SIZE_MAX / size) return false;
  void *grown = realloc(*data, next * size);
  if (!grown) return false;
  *data = grown;
  *capacity = next;
  return true;
}

static void add_token(Syntax *syntax, int kind, size_t start, size_t length,
                       unsigned breaks) {
  if (syntax->failed) return;
  if (!reserve((void **)&syntax->tokens, &syntax->capacity,
               syntax->count + 1, sizeof(Token))) {
    syntax->failed = true;
    return;
  }
  syntax->tokens[syntax->count++] = (Token){
      .kind = kind, .offset = start, .length = length,
      .breaks = breaks, .match = SIZE_MAX, .body_end = SIZE_MAX,
  };
}

static void observe_token(void *context, int kind, size_t offset, size_t length) {
  Syntax *s = context;
  if (s->failed) return;
  if (offset < s->consumed || offset > s->length || length > s->length - offset) {
    s->failed = true;
    return;
  }
  unsigned breaks = 0;
  size_t i = s->consumed;
  while (i < offset) {
    char c = s->source[i];
    if (c == '\n' || c == '\r') {
      if (breaks < UINT_MAX) ++breaks;
      if (c == '\r' && i + 1 < offset && s->source[i + 1] == '\n') ++i;
      ++i;
    } else if (c == ' ' || c == '\t') {
      ++i;
    } else if (c == '/' && i + 1 < offset) {
      const size_t start = i;
      const bool line = s->source[i + 1] == '/';
      i += 2;
      if (line) {
        /* The compiler's line comment consumes a bare CR as content. */
        while (i < offset && s->source[i] != '\n') ++i;
        /* Trailing CR bytes belong to physical line endings, including EOF. */
        size_t end = i;
        while (end > start && s->source[end - 1] == '\r') --end;
        add_token(s, COMMENT_LINE, start, end - start, breaks);
        breaks = i - end > 1 ? (unsigned)(i - end - 1) : 0;
      } else {
        unsigned depth = 1;
        while (i < offset && depth) {
          if (i + 1 < offset && s->source[i] == '/' && s->source[i + 1] == '*') {
            ++depth; i += 2;
          } else if (i + 1 < offset && s->source[i] == '*' && s->source[i + 1] == '/') {
            --depth; i += 2;
          } else ++i;
        }
        add_token(s, COMMENT_BLOCK, start, i - start, breaks);
        breaks = 0;
      }
    } else {
      s->failed = true;
      return;
    }
  }
  if (kind) add_token(s, kind, offset, length, breaks);
  s->consumed = offset + length;
}

static void observe_mark(void *context, McLocation location, unsigned mark) {
  Syntax *s = context;
  if (s->failed) return;
  if (location.line < 1 || (size_t)location.line > s->line_count || location.column < 1) {
    s->failed = true;
    return;
  }
  const size_t offset = s->lines[location.line - 1] + (size_t)location.column - 1;
  size_t low = 0, high = s->count;
  while (low < high) {
    const size_t middle = low + (high - low) / 2;
    if (s->tokens[middle].offset < offset) low = middle + 1;
    else high = middle;
  }
  if (mark == MC_SYNTAX_BODY_END) {
    if (!low) s->failed = true;
    else s->tokens[s->body_start].body_end = low - 1;
  } else if (low == s->count || s->tokens[low].offset != offset) s->failed = true;
  else {
    s->tokens[low].marks |= mark;
    if (mark == MC_SYNTAX_BODY) s->body_start = low;
  }
}

static bool parse(Syntax *s, const char *path) {
  if (s->length > INT_MAX - 2 || memchr(s->source, '\0', s->length)) {
    fprintf(stderr, "%s: mvm-fmt: source too large or contains NUL bytes\n", path);
    return false;
  }
  /* Match the compiler's byte columns and CR, LF and CRLF line counting. */
  for (size_t i = 0;; ++i) {
    if (!reserve((void **)&s->lines, &s->line_capacity,
                 s->line_count + 1, sizeof(size_t))) return false;
    s->lines[s->line_count++] = i;
    while (i < s->length && s->source[i] != '\n' && s->source[i] != '\r') ++i;
    if (i == s->length) break;
    if (s->source[i] == '\r' && i + 1 < s->length && s->source[i + 1] == '\n') ++i;
  }
  Mc mc = {0};
  McModule module = {.path = (char *)path};
  McParser parser = {
      .mc = &mc, .module = &module, .line = 1, .column = 1,
      .syntax_context = s, .syntax_token = observe_token, .syntax_mark = observe_mark,
  };
  void *scanner = NULL;
  if (mc_yylex_init(&scanner)) return false;
  mc_yyset_extra(&parser, scanner);
  mc_yy_scan_bytes(s->source, (int)s->length, scanner);
  const int status = mc_yyparse(scanner, &parser);
  mc_yylex_destroy(scanner);
  while (mc.arena) {
    McArenaBlock *next = mc.arena->next;
    free(mc.arena);
    mc.arena = next;
  }
  return status == 0 && !mc.errors && !s->failed;
}

static bool opening(const Token *t) {
  return t->kind == LPAREN || t->kind == LBRACKET || t->kind == LBRACE ||
         t->kind == LBRACE_EXPR || (t->marks & MC_SYNTAX_GENERIC_OPEN);
}

static bool closing(const Token *t) {
  return t->kind == RPAREN || t->kind == RBRACKET || t->kind == RBRACE ||
         (t->marks & MC_SYNTAX_GENERIC_CLOSE);
}

static bool delimiters(Syntax *s, const MfmtOptions *options, Frame *stack) {
  size_t depth = 0;
  for (size_t i = 0; i < s->count; ++i) {
    Token *t = &s->tokens[i];
    if (t->breaks && depth) s->tokens[stack[depth - 1].token].multiline = true;
    if (t->kind == COMMA && depth) s->tokens[stack[depth - 1].token].comma = true;
    if (opening(t)) {
      stack[depth++] = (Frame){.token = i};
    } else if (closing(t)) {
      if (!depth) return false;
      Token *open = &s->tokens[stack[--depth].token];
      open->match = i;
      t->match = (size_t)(open - s->tokens);
      if (open->match != t->match + 1 && (open->marks & MC_SYNTAX_BLOCK))
        open->multiline = true;
      /* A soft limit: only comma lists can be wrapped automatically. */
      if (open->comma) {
        size_t width = 0;
        for (size_t j = t->match; j <= i && width <= options->line_width; ++j)
          width += s->tokens[j].length + 1;
        open->flat_width = width;
        if (width + depth * options->indent_width > options->line_width)
          open->multiline = true;
      }
    }
  }
  return depth == 0;
}

static void append(Output *out, const char *data, size_t length) {
  if (out->failed) return;
  if (length > SIZE_MAX - out->length - 1 ||
      !reserve((void **)&out->data, &out->capacity, out->length + length + 1, 1)) {
    out->failed = true;
    return;
  }
  memcpy(out->data + out->length, data, length);
  out->length += length;
  out->data[out->length] = '\0';
  for (size_t i = 0; i < length; ++i) {
    if (data[i] == '\n' || data[i] == '\r') out->column = 0;
    else ++out->column;
  }
}

static void newline(Output *out, const MfmtOptions *options) {
  append(out, options->crlf ? "\r\n" : "\n", options->crlf ? 2 : 1);
}

static bool word(const Syntax *s, const Token *t) {
  const unsigned char c = (unsigned char)s->source[t->offset];
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
         (c >= '0' && c <= '9') || c == '_';
}

static bool unary(const Token *t) {
  return (t->marks & MC_SYNTAX_UNARY) || t->kind == NOT ||
         t->kind == BNOT || t->kind == AT;
}

static bool binary(const Token *t) {
  if (unary(t) || (t->marks & (MC_SYNTAX_GENERIC_OPEN | MC_SYNTAX_GENERIC_CLOSE)))
    return false;
  switch (t->kind) {
  case ADD: case SUB: case MUL: case DIV: case MOD: case BAND: case BOR:
  case BXOR: case SHL: case SHR: case EQ: case NE: case LT: case LE:
  case GT: case GE: case LAND: case LOR: case ASSIGN: case ADD_ASSIGN:
  case SUB_ASSIGN: case MUL_ASSIGN: case DIV_ASSIGN: case MOD_ASSIGN:
  case AND_ASSIGN: case OR_ASSIGN: case XOR_ASSIGN: case SHL_ASSIGN:
  case SHR_ASSIGN: case QUESTION: return true;
  default: return (t->marks & MC_SYNTAX_TERNARY_COLON) != 0;
  }
}

static bool separator(const Syntax *s, const Token *a, const Token *b) {
  if (a->kind < 0 || b->kind < 0) return true;
  if (word(s, a) && word(s, b)) return true;
  if ((a->marks & MC_SYNTAX_GENERIC_CLOSE) &&
      (b->marks & MC_SYNTAX_GENERIC_CLOSE)) return false;
  char pair[3] = {s->source[a->offset + a->length - 1], s->source[b->offset], 0};
  static const char *const joined[] = {
      "++", "--", "/*", "//", "+=", "-=", "*=", "/=", "%=", "&=", "|=",
      "^=", "==", "!=", "<=", ">=", "<<", ">>", "&&", "||", "..", "::",
  };
  for (size_t i = 0; i < sizeof(joined) / sizeof(*joined); ++i)
    if (!strcmp(pair, joined[i])) return true;
  return false;
}

static bool space(const Syntax *s, const Token *a, const Token *b,
                   const MfmtOptions *o) {
  if (separator(s, a, b)) return true;
  switch (a->kind) {
  case FROM: case IMPORT: case EXPORT: case IF: case WHILE: case FOR:
  case SWITCH: case IN: case BY: case AS: case MUT: return true;
  default: break;
  }
  if (b->kind == ELSE || a->kind == ELSE || a->kind == RETURN) return true;
  if ((b->marks & MC_SYNTAX_BLOCK) && b->kind == LBRACE) return true;
  if (a->marks & MC_SYNTAX_GENERIC_OPEN) return false;
  if (b->marks & (MC_SYNTAX_GENERIC_OPEN | MC_SYNTAX_GENERIC_CLOSE)) return false;
  if ((a->kind == LBRACE || a->kind == LBRACE_EXPR) && b->kind != RBRACE)
    return o->space_inside_braces;
  if (a->kind == COMMA) return o->space_after_comma && !closing(b);
  if (a->marks & MC_SYNTAX_CASE_COLON) return true;
  if (binary(a) || binary(b)) return o->space_around_operators;
  if (a->kind == RANGE || b->kind == RANGE || a->kind == ELLIPSIS || b->kind == ELLIPSIS)
    return o->space_around_range;
  if (b->kind == COMMA || b->kind == COLON || b->kind == DOT || b->kind == DCOLON ||
      b->kind == INC || b->kind == DEC) return false;
  if (unary(a) || a->kind == DOT || a->kind == DCOLON) return false;
  if (a->kind == COLON) return o->space_after_colon;
  if (b->kind == RBRACE && a->kind != LBRACE && a->kind != LBRACE_EXPR)
    return o->space_inside_braces;
  if (b->kind == LPAREN)
    return a->kind == IF || a->kind == WHILE || a->kind == SWITCH;
  if (opening(a) || closing(b) || b->kind == LBRACKET) return false;
  if (b->kind == LBRACE || b->kind == LBRACE_EXPR) return true;
  return (closing(a) || (a->marks & MC_SYNTAX_GENERIC_CLOSE)) && word(s, b);
}

static void render(Syntax *s, const MfmtOptions *o, Frame *stack,
                    size_t *body_ends, Output *out) {
  size_t depth = 0, indentation = 0;
  for (size_t i = 0; i < s->count && !out->failed; ++i) {
    Token *t = &s->tokens[i];
    Token *previous = i ? &s->tokens[i - 1] : NULL;
    Token *next = i + 1 < s->count ? &s->tokens[i + 1] : NULL;
    if (closing(t)) {
      Frame frame = stack[--depth];
      Token *open = &s->tokens[frame.token];
      if (open->multiline) --indentation;
      if (frame.case_body) --indentation;
    }
    if ((t->marks & MC_SYNTAX_CASE) && depth && stack[depth - 1].case_body) {
      --indentation;
      stack[depth - 1].case_body = false;
    }
    bool line = false;
    if (previous) {
      line = t->breaks != 0 || previous->kind == COMMENT_LINE;
      if ((t->marks & MC_SYNTAX_STATEMENT) && !(t->marks & MC_SYNTAX_BODY))
        line = true;
      if (t->marks & MC_SYNTAX_CASE) line = true;
      if (opening(previous) && previous->multiline && previous->match != i) line = true;
      if (closing(t) && s->tokens[t->match].multiline && t->match != i - 1) line = true;
      if (previous->kind == COMMA && depth && s->tokens[stack[depth - 1].token].multiline)
        line = true;
      if ((previous->marks & MC_SYNTAX_CASE_COLON) && !(t->marks & MC_SYNTAX_BLOCK))
        line = true;
      if (t->marks & MC_SYNTAX_BLOCK) {
        if (o->braces_next_line) line = true;
        else if (!(t->marks & MC_SYNTAX_STATEMENT) || (t->marks & MC_SYNTAX_BODY))
          line = previous->kind == COMMENT_LINE;
      }
      if (previous->marks & MC_SYNTAX_CASE_COLON && t->marks & MC_SYNTAX_BLOCK)
        line = o->braces_next_line;
      if (t->kind == ELSE) {
        if (o->else_new_line) line = true;
        else if (previous->kind == RBRACE &&
                 (s->tokens[previous->match].marks & MC_SYNTAX_BLOCK)) line = false;
      }
      if (line) {
        newline(out, o);
        unsigned blank = t->breaks > 1 ? t->breaks - 1 : 0;
        if (blank > o->max_blank_lines) blank = o->max_blank_lines;
        for (unsigned j = 0; j < blank; ++j) newline(out, o);
      } else if (space(s, previous, t, o)) {
        append(out, " ", 1);
      }
    }
    if ((t->marks & MC_SYNTAX_BODY) && !(t->marks & MC_SYNTAX_BLOCK) && line) {
      ++indentation;
      ++body_ends[t->body_end];
    }
    if (!out->column) {
      for (size_t j = 0; j < indentation; ++j) {
        if (o->indent_tabs) {
          append(out, "\t", 1);
          out->column += o->indent_width - 1;
        }
        else for (unsigned k = 0; k < o->indent_width; ++k) append(out, " ", 1);
      }
    }
    append(out, s->source + t->offset, t->length);
    if (opening(t)) {
      if (t->comma && out->column + t->flat_width > o->line_width) t->multiline = true;
      stack[depth++] = (Frame){.token = i};
      if (t->multiline) ++indentation;
    }
    if ((t->marks & MC_SYNTAX_CASE_COLON) && depth &&
        (!next || !(next->marks & (MC_SYNTAX_BLOCK | MC_SYNTAX_CASE)))) {
      stack[depth - 1].case_body = true;
      ++indentation;
    }
    indentation -= body_ends[i];
  }
  if (s->count && o->final_newline) newline(out, o);
  if (!out->data) append(out, "", 0);
}

static void dispose(Syntax *s) {
  free(s->tokens);
  free(s->lines);
}

bool mfmt_format(const char *source, size_t length, const char *path,
                 const MfmtOptions *options, char **output, size_t *output_length) {
  *output = NULL;
  *output_length = 0;
  if (!path) path = "<stdin>";
  if (!options || !source || !options->indent_width || options->indent_width > 16 ||
      options->line_width < 40 || options->line_width > 1000 || options->max_blank_lines > 10) {
    fprintf(stderr, "%s: mvm-fmt: invalid formatting options\n", path);
    return false;
  }
  Syntax input = {.source = source, .length = length};
  Syntax verification = {0};
  Frame *stack = NULL;
  size_t *body_ends = NULL;
  Output result = {0};
  bool ok = false;
  if (!parse(&input, path)) goto done;
  if (input.count > SIZE_MAX / sizeof(*stack)) goto done;
  stack = malloc((input.count ? input.count : 1) * sizeof(*stack));
  body_ends = calloc(input.count ? input.count : 1, sizeof(*body_ends));
  if (!stack || !body_ends || !delimiters(&input, options, stack)) goto done;
  render(&input, options, stack, body_ends, &result);
  if (result.failed) goto done;
  /* Check both lexemes and grammar roles before exposing a rewrite. This
   * catches accidental token merging and changes to statement boundaries. */
  verification.source = result.data;
  verification.length = result.length;
  if (!parse(&verification, path) || verification.count != input.count) goto done;
  for (size_t i = 0; i < input.count; ++i) {
    const Token *a = &input.tokens[i], *b = &verification.tokens[i];
    if (a->kind != b->kind || a->length != b->length || a->marks != b->marks ||
        a->body_end != b->body_end ||
        memcmp(source + a->offset, result.data + b->offset, a->length)) goto done;
  }
  *output = result.data;
  *output_length = result.length;
  result.data = NULL;
  ok = true;
done:
  if (!ok) fprintf(stderr, "%s: mvm-fmt: formatting failed; source was not changed\n", path);
  free(stack);
  free(body_ends);
  free(result.data);
  dispose(&input);
  dispose(&verification);
  return ok;
}
