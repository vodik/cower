/*
 *  package.h
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#ifndef _COWER_PACKAGE_H
#define _COWER_PACKAGE_H

typedef struct aur_pkg_t {
  const char* id;
  const char* name;
  const char* ver;
  const char* cat;
  const char* desc;
  const char* url;
  const char* urlpath;
  const char* lic;
  const char* votes;
  int ood;
};

int aur_pkg_cmp(void*, void*);
void aur_pkg_free(void*);

#endif /* _COWER_PACKAGE_H */
