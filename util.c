/* Copyright (c) 2010 Dave Reisner
 *
 * util.c
 *
 * Permission is hereby granted, free of charge, to any person
 * obtaining a copy of this software and associated documentation
 * files (the "Software"), to deal in the Software without
 * restriction, including without limitation the rights to use,
 * copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following
 * conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES
 * OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT
 * HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY,
 * WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 */

/* standard */
#define _GNU_SOURCE
#include <ctype.h>
#include <limits.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>

/* local */
#include "aur.h"
#include "conf.h"
#include "download.h"
#include "util.h"

static char *aur_cat[] = { NULL, "None", "daemons", "devel", "editors",
                           "emulators", "games", "gnome", "i18n", "kde", "lib",
                           "modules", "multimedia", "network", "office",
                           "science", "system", "x11", "xfce", "kernels" };


static int c_vfprintf(FILE *fd, const char* fmt, va_list args) {
  const char *p;
  int color, count = 0;
  char cprefix[10] = {0};

  int i; long l; char *s;

  for (p = fmt; *p != '\0'; p++) {
    if (*p != '%') {
      fputc(*p, fd); count++;
      continue;
    }

    switch (*++p) {
    case 'c':
      i = va_arg(args, int);
      fputc(i, fd); count++;
      break;
    case 's':
      s = va_arg(args, char*);
      count += fputs(s, fd);
      break;
    case 'd':
      i = va_arg(args, int);
      if (i < 0) {
        i = -i;
        fputc('-', fd); count++;
      }
      count += fputs(itoa(i, 10), fd);
      break;
    case 'l':
      l = va_arg(args, long);
      if (l < 0) {
        l = -l;
        fputc('-', fd); count++;
      }
      count += fputs(itoa(l, 10), fd);
      break;
    case '<': /* color on */
      color = va_arg(args, int);
      snprintf(cprefix, 10, C_ON, color / 10, color % 10);
      count += fputs(cprefix, fd);
      break;
    case '>': /* color off */
      count += fputs(C_OFF, fd);
      break;
    case '%':
      fputc('%', fd); count++;
      break;
    }
  }

  return count;
}

int cfprintf(FILE *fd, const char *fmt, ...) {
  va_list args;
  int result;

  va_start(args, fmt);
  result = c_vfprintf(fd, fmt, args);
  va_end(args);

  return result;
}

int cprintf(const char *fmt, ...) {
  va_list args;
  int result;

  va_start(args, fmt);
  result = c_vfprintf(stdout, fmt, args);
  va_end(args);

  return result;
}

int file_exists(const char *filename) {
  struct stat st;

  return stat(filename, &st) == 0;
}

off_t filesize(const char *filename) {
  struct stat st;

  stat(filename, &st);

  return st.st_size;
}

static int get_screen_width(void) {
  if(!isatty(1))
    return 80;

  struct winsize win;
  if(ioctl(1, TIOCGWINSZ, &win) == 0)
    return win.ws_col;

  return 80;
}

char *get_file_as_buffer(const char *filename) {
  FILE *fd;
  char *buf;
  off_t fsize;
  size_t nread;

  fsize = filesize(filename);

  if (!fsize)
    return NULL;

  buf = calloc(1, fsize + 1);

  fd = fopen(filename, "r");
  nread = fread(buf, 1, fsize, fd);
  fclose(fd);

  return nread == 0 ? NULL : buf;
}

char *itoa(unsigned int num, int base){
   static char retbuf[33];
   char *p;

   if (base < 2 || base > 16)
     return NULL;

   p = &retbuf[sizeof(retbuf)-1];
   *p = '\0';

   do {
     *--p = "0123456789abcdef"[num % base];
     num /= base;
   } while (num != 0);

   return p;
}

void print_pkg_info(struct aur_pkg_t *pkg) {
  size_t max_line_len = get_screen_width() - INDENT - 1;

  if (config->color) {
    cprintf("Repository      : %<aur%>\n"
            "Name            : %<%s%>\n"
            "Version         : %<%s%>\n"
            "URL             : %<%s%>\n"
            "AUR Page        : %<%s%d%>\n",
            config->colors->repo,
            config->colors->pkg, pkg->name,
            pkg->ood ? config->colors->outofdate : config->colors->uptodate, pkg->ver,
            config->colors->url, pkg->url,
            config->colors->url, AUR_PKG_URL_FORMAT, pkg->id);
  } else {
    printf("%-*s: aur\n%-*s: %s\n%-*s: %s\n%-*s: %s\n%-*s: %s%d\n",
           INDENT - 2, PKG_OUT_REPO,
           INDENT - 2, PKG_OUT_NAME, pkg->name,
           INDENT - 2, PKG_OUT_VERSION, pkg->ver,
           INDENT - 2, PKG_OUT_URL, pkg->url,
           INDENT - 2, PKG_OUT_AURPAGE, AUR_PKG_URL_FORMAT, pkg->id);
  }

  if (config->moreinfo) {
    print_extinfo_list(PKG_OUT_PROVIDES, pkg->provides, max_line_len, INDENT);
    print_extinfo_list(PKG_OUT_DEPENDS, pkg->depends, max_line_len, INDENT);
    print_extinfo_list(PKG_OUT_MAKEDEPENDS, pkg->makedepends, max_line_len, INDENT);

    /* Always making excuses for optdepends... */
    if (pkg->optdepends) {
      printf("Optdepends      : ");

      printf("%s\n", (const char*)pkg->optdepends->data);

      alpm_list_t *i;
      for (i = pkg->optdepends->next; i; i = i->next)
        printf("%*s%s\n", INDENT, "", (const char*)i->data);
    }

    print_extinfo_list(PKG_OUT_CONFLICTS, pkg->conflicts, max_line_len, INDENT);
    print_extinfo_list(PKG_OUT_REPLACES, pkg->replaces, max_line_len, INDENT);
  }

  printf("%-*s: %s\n%-*s: %s\n%-*s: %d\n",
         INDENT - 2, PKG_OUT_CAT, aur_cat[pkg->cat],
         INDENT - 2, PKG_OUT_LICENSE, pkg->lic,
         INDENT - 2, PKG_OUT_NUMVOTES, pkg->votes);

  if (config->color) {
    cprintf("Out of Date     : %<%s%>\n", pkg->ood ? 
      config->colors->outofdate : config->colors->uptodate, pkg->ood ? "Yes" : "No");
  } else {
    printf("%-*s: %s\n", INDENT - 2, PKG_OUT_OOD, pkg->ood ? "Yes" : "No");
  }

  printf("%-*s: ", INDENT - 2, PKG_OUT_DESC);

  size_t desc_len = strlen(pkg->desc);
  if (desc_len < max_line_len)
    printf("%s\n", pkg->desc);
  else
    print_wrapped(pkg->desc, max_line_len, INDENT);

  putchar('\n');

}

void print_pkg_search(alpm_list_t *search) {
  alpm_list_t *i;
  struct aur_pkg_t *pkg;

  for (i = search; i; i = i->next) {
    pkg = i->data;

    if (config->quiet) {
      if (config->color)
        cprintf("%<%s%>\n", config->colors->pkg, pkg->name);
      else
        printf("%s\n", pkg->name);
    } else {
      if (config->color)
        cprintf("%<aur/%>%<%s%> %<%s%>\n",
          config->colors->repo, config->colors->pkg, pkg->name, pkg->ood ?
            config->colors->outofdate : config->colors->uptodate, pkg->ver);
      else
        printf("aur/%s %s\n", pkg->name, pkg->ver);
      printf("    %s\n", pkg->desc);
    }
  }
}

void print_pkg_update(const char *pkg, const char *local_ver, const char *remote_ver) {
  if (config->color) {
    if (! config->quiet)
      cprintf("%<%s%> %<%s%> -> %<%s%>\n", config->colors->pkg, pkg, 
        config->colors->outofdate, local_ver, config->colors->uptodate, remote_ver);
    else
      cprintf("%<%s%>\n", config->colors->pkg, pkg);
  } else {
    if (! config->quiet)
      printf("%s %s -> %s\n", pkg, local_ver, remote_ver);
    else
      printf("%s\n", pkg);
  }
}

void print_extinfo_list(const char *field, alpm_list_t *list, size_t max_line_len, int indent) {
  if (!list)
    return;

  printf("%-*s: ", indent - 2, field);

  int count = 0;
  size_t deplen;
  alpm_list_t *i;
  for (i = list; i; i = i->next) {
    deplen = strlen(i->data);
    if (count + deplen >= max_line_len) {
      printf("%-*s", indent + 1, "\n");
      count = 0;
    }
    count += printf("%s  ", (const char*)i->data);
  }
  putchar('\n');
}

void print_wrapped(const char* buffer, size_t maxlength, int indent) {
  unsigned pos, lastSpace;

  pos = lastSpace = 0;
  while(buffer[pos] != 0) {
    int isLf = (buffer[pos] == '\n');

    if (isLf || pos == maxlength) {
      if (isLf || lastSpace == 0)
        lastSpace = pos;

      while(*buffer != 0 && lastSpace-- > 0)
        putchar(*buffer++);

      putchar('\n');
      if (indent)
        printf("%*s", indent, "");

      if (isLf) /* newline in the stream, skip it */
        buffer++;

      while (*buffer && isspace(*buffer))
        buffer++;

      lastSpace = pos = 0;
    } else {
      if (isspace(buffer[pos]))
        lastSpace = pos;

      pos++;
    }
  }
  printf("%s\n", buffer);
}

char *ltrim(char *str) {
  char *pch = str;

  if (str == NULL || *str == '\0')
    return str;

  while (isspace(*pch))
    pch++;

  if (pch != str)
    memmove(str, pch, (strlen(pch) + 1));

  return str;
}

char *strtrim(char *str) {
  char *pch = str;

  if (str == NULL || *str == '\0')
    return str;

  while (isspace(*pch))
    pch++;

  if (pch != str)
    memmove(str, pch, (strlen(pch) + 1));

  if (*str == '\0')
    return(str);

  pch = (str + strlen(str) - 1);

  while (isspace(*pch))
    pch--;

  *++pch = '\0';

  return str;
}

alpm_list_t *strsplit(const char *str, const char splitchar) {
  alpm_list_t *list = NULL;
  const char *prev = str;
  char *dup = NULL;

  while((str = strchr(str, splitchar))) {
    dup = strndup(prev, str - prev);
    if(dup == NULL) {
      return(NULL);
    }
    list = alpm_list_add(list, dup);

    str++;
    prev = str;
  }

  dup = strdup(prev);
  if(dup == NULL) {
    return(NULL);
  }
  list = alpm_list_add(list, dup);

  return(list);
}

char *relative_to_absolute_path(const char *relpath) {
  if (*relpath == '/') { /* already absolute */
    return strdup(relpath);
  }

  char *abspath = NULL;

  abspath = getcwd(abspath, PATH_MAX + 1);
  abspath = strncat(abspath, "/", PATH_MAX - strlen(abspath));
  abspath = strncat(abspath, relpath, PATH_MAX - strlen(abspath));

  return abspath;

}

/* vim: set ts=2 sw=2 et: */
