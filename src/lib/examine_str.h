/*
 * Examine - a set of tools for memory leak detection on Windows and
 * PE file reader
 *
 * Copyright (C) 2012-2015 Vincent Torri.
 * All rights reserved.
 *
 * This program is free software: you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public License
 * as published by the Free Software Foundation, either version 3 of
 * the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
 */

#ifndef EXM_STR_H
#define EXM_STR_H

void exm_str_append(char *buf, const char *str);
void exm_str_append_with_quotes(char *buf, const char *str);

#endif /* EXM_STR_H */
