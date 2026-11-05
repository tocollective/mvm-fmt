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

#define _POSIX_C_SOURCE 200809L
#define _XOPEN_SOURCE 700
#include "mfmt.h"

#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#ifdef _WIN32
#include <direct.h>
#include <fcntl.h>
#include <io.h>
#include <process.h>
#include <windows.h>
#define getpid _getpid
#define fileno _fileno
#ifndef S_ISREG
#define S_ISREG(mode) (((mode) & _S_IFMT) == _S_IFREG)
#endif
#else
#include <fcntl.h>
#include <unistd.h>
#endif

static void usage(FILE *out) {
  fputs(
      "usage: mvm-fmt [--config FILE | --no-config] [--write | --check] [--] "
      "[FILE ...]\n"
      "       mvm-fmt [--config FILE | --no-config] [--stdin-filepath FILE] "
      "[-]\n"
      "  -w, --write    Replace files atomically, preserving permissions.\n"
      "  --check        List files needing formatting; exit 1 if any differ.\n"
      "  -c, --config   Read an explicit TOML configuration.\n"
      "  --no-config    Use built-in defaults without searching.\n"
      "  --stdin-filepath FILE  Use FILE for stdin diagnostics and config "
      "discovery.\n"
      "  Without a mode, write one formatted source to stdout (default: "
      "stdin).\n"
      "  Discover the nearest .mformat.toml from each source directory "
      "upwards.\n"
      "  Exit codes: 0 success, 1 check found differences, 2 error.\n",
      out);
}

static void io_error(const char *path) {
  fprintf(stderr, "mvm-fmt: %s: %s\n", path, strerror(errno));
}

static bool slash(char c) {
#ifdef _WIN32
  return c == '/' || c == '\\';
#else
  return c == '/';
#endif
}

static bool load_options(const char *input, const char *explicit_path,
                         bool no_config, MfmtOptions *options) {
  *options = mfmt_default_options();
  char error[512];
  if (explicit_path) {
    if (mfmt_load_config(explicit_path, options, error, sizeof(error)))
      return true;
    fprintf(stderr, "mvm-fmt: %s\n", error);
    return false;
  }
  if (no_config)
    return true;
  size_t length = strlen(input);
  while (length && !slash(input[length - 1]))
    --length;
  char *directory = malloc(length + 2);
  if (!directory) {
    errno = ENOMEM;
    io_error(input);
    return false;
  }
  if (length) {
    memcpy(directory, input, length);
    directory[length] = 0;
  } else
    strcpy(directory, ".");
#ifdef _WIN32
  char *absolute = _fullpath(NULL, directory, 0);
#else
  char *absolute = realpath(directory, NULL);
#endif
  free(directory);
  if (!absolute) {
    io_error(input);
    return false;
  }
  const size_t absolute_length = strlen(absolute);
  char *candidate = malloc(absolute_length + sizeof("/.mformat.toml"));
  if (!candidate) {
    free(absolute);
    errno = ENOMEM;
    io_error(input);
    return false;
  }
  bool ok = true;
  for (;;) {
    snprintf(candidate, absolute_length + sizeof("/.mformat.toml"),
             "%s/.mformat.toml", absolute);
    struct stat st;
    if (stat(candidate, &st) == 0) {
      ok = mfmt_load_config(candidate, options, error, sizeof(error));
      if (!ok)
        fprintf(stderr, "mvm-fmt: %s\n", error);
      break;
    }
    if (errno != ENOENT) {
      io_error(candidate);
      ok = false;
      break;
    }
    size_t end = strlen(absolute);
    while (end && slash(absolute[end - 1]))
      --end;
    while (end && !slash(absolute[end - 1]))
      --end;
    if (!end)
      break;
#ifdef _WIN32
    if (end <= 3 && absolute[1] == ':' && strlen(absolute) <= 3)
      break;
    if (end == 3 && absolute[1] == ':') {
      absolute[3] = 0;
      continue;
    }
    /* Do not search above the share root of a UNC path. */
    if (slash(absolute[0]) && slash(absolute[1])) {
      size_t share = 2;
      while (absolute[share] && !slash(absolute[share]))
        ++share;
      if (absolute[share])
        ++share;
      while (absolute[share] && !slash(absolute[share]))
        ++share;
      if (end <= share)
        break;
    }
#endif
    if (end == 1 && absolute[1] == 0)
      break;
    absolute[end == 1 ? 1 : end - 1] = 0;
  }
  free(candidate);
  free(absolute);
  return ok;
}

static bool read_stream(FILE *file, const char *path, char **data,
                        size_t *length) {
  size_t capacity = 8192;
  *length = 0;
  *data = malloc(capacity + 1);
  if (!*data) {
    errno = ENOMEM;
    io_error(path);
    return false;
  }
  for (;;) {
    const size_t got = fread(*data + *length, 1, capacity - *length, file);
    *length += got;
    if (ferror(file)) {
      io_error(path);
      free(*data);
      *data = NULL;
      return false;
    }
    if (feof(file))
      break;
    if (*length == capacity) {
      if (capacity > (size_t)(INT_MAX - 2) / 2) {
        fprintf(stderr, "mvm-fmt: %s: source is too large\n", path);
        free(*data);
        *data = NULL;
        return false;
      }
      capacity *= 2;
      char *grown = realloc(*data, capacity + 1);
      if (!grown) {
        free(*data);
        *data = NULL;
        errno = ENOMEM;
        io_error(path);
        return false;
      }
      *data = grown;
    }
  }
  (*data)[*length] = 0;
  return true;
}

static bool read_source(const char *path, bool standard_input, char **data,
                        size_t *length) {
  FILE *file = standard_input ? stdin : fopen(path, "rb");
  if (!file) {
    io_error(path);
    return false;
  }
  struct stat st;
  if (!standard_input &&
      (fstat(fileno(file), &st) != 0 || !S_ISREG(st.st_mode))) {
    fprintf(stderr, "mvm-fmt: %s: expected a regular file\n", path);
    fclose(file);
    return false;
  }
  bool ok = read_stream(file, path, data, length);
  if (!standard_input && fclose(file) != 0) {
    io_error(path);
    if (ok) {
      free(*data);
      *data = NULL;
    }
    ok = false;
  }
  return ok;
}

static bool replace_source(const char *path, const char *original,
                           size_t original_length, const char *formatted,
                           size_t length) {
  struct stat st;
#ifdef _WIN32
  DWORD attributes = GetFileAttributesA(path);
  if (attributes == INVALID_FILE_ATTRIBUTES ||
      (attributes & FILE_ATTRIBUTE_REPARSE_POINT) || stat(path, &st) != 0 ||
      !S_ISREG(st.st_mode)) {
#else
  if (lstat(path, &st) != 0 || !S_ISREG(st.st_mode)) {
#endif
    fprintf(
        stderr,
        "mvm-fmt: %s: --write requires a regular file, not a symbolic link\n",
        path);
    return false;
  }
  if (st.st_nlink != 1) {
    fprintf(
        stderr,
        "mvm-fmt: %s: refusing to replace a file with multiple hard links\n",
        path);
    return false;
  }
  const size_t size = strlen(path) + 80;
  char *temporary = malloc(size);
  if (!temporary) {
    errno = ENOMEM;
    io_error(path);
    return false;
  }
  FILE *file = NULL;
  static unsigned serial;
  for (unsigned attempt = 0; attempt < 100; ++attempt) {
    snprintf(temporary, size, "%s.mformat-%lu-%u.tmp", path,
             (unsigned long)getpid(), ++serial);
#ifdef _WIN32
    file = fopen(temporary, "wbx");
#else
    int descriptor = open(temporary, O_WRONLY | O_CREAT | O_EXCL, 0600);
    if (descriptor >= 0) {
      file = fdopen(descriptor, "wb");
      if (!file) {
        const int saved = errno;
        close(descriptor);
        remove(temporary);
        errno = saved;
      }
    }
#endif
    if (file || errno != EEXIST)
      break;
  }
  if (!file) {
    io_error(temporary);
    free(temporary);
    return false;
  }
  bool ok = true;
  if (length && fwrite(formatted, 1, length, file) != length)
    ok = false;
  if (ok && fflush(file) != 0)
    ok = false;
#ifdef _WIN32
  if (ok && _commit(fileno(file)) != 0)
    ok = false;
  if (ok && _chmod(temporary, st.st_mode & (_S_IREAD | _S_IWRITE)) != 0)
    ok = false;
#else
  if (ok && fchmod(fileno(file), st.st_mode & 07777) != 0)
    ok = false;
  if (ok && fsync(fileno(file)) != 0)
    ok = false;
#endif
  if (!ok)
    io_error(temporary);
  if (fclose(file) != 0) {
    io_error(temporary);
    ok = false;
  }
  /* Detect edits made since the initial read before replacing the file. */
  char *current = NULL;
  size_t current_length = 0;
  if (ok) {
#ifndef _WIN32
    struct stat current_stat;
    if (lstat(path, &current_stat) != 0 || current_stat.st_dev != st.st_dev ||
        current_stat.st_ino != st.st_ino ||
        current_stat.st_mode != st.st_mode ||
        current_stat.st_nlink != st.st_nlink) {
      fprintf(stderr, "mvm-fmt: %s: file changed while formatting\n", path);
      ok = false;
    }
#endif
  }
  if (ok) {
    ok = read_source(path, false, &current, &current_length);
    if (ok && (current_length != original_length ||
               memcmp(current, original, original_length))) {
      fprintf(stderr, "mvm-fmt: %s: file changed while formatting\n", path);
      ok = false;
    }
    free(current);
  }
  if (ok) {
#ifdef _WIN32
    if (!MoveFileExA(temporary, path,
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
      fprintf(stderr, "mvm-fmt: %s: replacement failed (Windows error %lu)\n",
              path, GetLastError());
      ok = false;
    }
#else
    if (rename(temporary, path) != 0) {
      io_error(path);
      ok = false;
    }
#endif
  }
  if (!ok)
    remove(temporary);
  free(temporary);
  return ok;
}

int main(int argc, char **argv) {
  bool write = false, check = false, no_config = false, literal = false;
  const char *config = NULL, *stdin_path = NULL;
  const char **files = calloc((size_t)argc, sizeof(*files));
  if (!files) {
    fputs("mvm-fmt: out of memory\n", stderr);
    return 2;
  }
  size_t count = 0;
  int result = 0;
  for (int i = 1; i < argc; ++i) {
    const char *arg = argv[i];
    if (!literal && !strcmp(arg, "--")) {
      literal = true;
      continue;
    }
    if (!literal && (!strcmp(arg, "--help") || !strcmp(arg, "-h"))) {
      usage(stdout);
      free(files);
      return 0;
    }
    if (!literal && (!strcmp(arg, "--write") || !strcmp(arg, "-w")))
      write = true;
    else if (!literal && !strcmp(arg, "--check"))
      check = true;
    else if (!literal && !strcmp(arg, "--no-config"))
      no_config = true;
    else if (!literal && (!strcmp(arg, "--config") || !strcmp(arg, "-c") ||
                          !strcmp(arg, "--stdin-filepath"))) {
      if (++i == argc) {
        result = 2;
        goto usage_error;
      }
      if (!strcmp(arg, "--stdin-filepath"))
        stdin_path = argv[i];
      else
        config = argv[i];
    } else if (!literal && arg[0] == '-' && strcmp(arg, "-")) {
      result = 2;
      goto usage_error;
    } else
      files[count++] = arg;
  }
  if (!count)
    files[count++] = "-";
  bool has_stdin = false;
  for (size_t i = 0; i < count; ++i)
    if (!strcmp(files[i], "-"))
      has_stdin = true;
  if ((write && check) || (config && no_config) ||
      (!write && !check && count != 1) ||
      (has_stdin && (write || count != 1)) || (stdin_path && !has_stdin)) {
    result = 2;
    goto usage_error;
  }
#ifdef _WIN32
  _setmode(_fileno(stdin), _O_BINARY);
  _setmode(_fileno(stdout), _O_BINARY);
#endif
  for (size_t i = 0; i < count; ++i) {
    const bool standard_input = !strcmp(files[i], "-");
    const char *path =
        standard_input ? (stdin_path ? stdin_path : "<stdin>") : files[i];
    MfmtOptions options;
    char *source = NULL, *formatted = NULL;
    size_t length = 0, formatted_length = 0;
    if (!load_options(standard_input && !stdin_path ? "stdin.m" : path, config,
                      no_config, &options) ||
        !read_source(path, standard_input, &source, &length)) {
      result = 2;
      continue;
    }
    if (!mfmt_format(source, length, path, &options, &formatted,
                     &formatted_length)) {
      free(source);
      result = 2;
      continue;
    }
    const bool different =
        length != formatted_length || memcmp(source, formatted, length);
    if (check && different) {
      if (printf("%s\n", path) < 0)
        result = 2;
      else if (result == 0)
        result = 1;
    } else if (write && different) {
      if (!replace_source(path, source, length, formatted, formatted_length))
        result = 2;
    } else if (!write && !check && formatted_length &&
               fwrite(formatted, 1, formatted_length, stdout) !=
                   formatted_length) {
      io_error("stdout");
      result = 2;
    }
    free(source);
    free(formatted);
  }
  if (fflush(stdout) != 0) {
    io_error("stdout");
    result = 2;
  }
  free(files);
  return result;
usage_error:
  usage(stderr);
  free(files);
  return result;
}
