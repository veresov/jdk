/*
 * Copyright (c) 2023, Oracle and/or its affiliates. All rights reserved.
 * DO NOT ALTER OR REMOVE COPYRIGHT NOTICES OR THIS FILE HEADER.
 *
 * This code is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 only, as
 * published by the Free Software Foundation.
 *
 * This code is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
 * version 2 for more details (a copy is included in the LICENSE file that
 * accompanied this code).
 *
 * You should have received a copy of the GNU General Public License version
 * 2 along with this work; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301 USA.
 *
 * Please contact Oracle, 500 Oracle Parkway, Redwood Shores, CA 94065 USA
 * or visit www.oracle.com if you need additional information or have any
 * questions.
 *
 */

#ifndef SHARE_COMPILER_COMPILATIONRECORD_HPP
#define SHARE_COMPILER_COMPILATIONRECORD_HPP

// Record information about a method at the time compilation is requested.
// Just a name for now, full profile later.
class CompilationRecord : public CHeapObj<mtCompiler> {
  char* _method_name;
  int _level;
  bool _only_inlined;

  static unsigned string_hash(const char* s) {
    unsigned h = 0;
    const char* p = s;
    while (*p != '\0') {
    h = 31 * h + *p;
    p++;
    }
    return h;
  }

  static char* clone_string(const char* s) {
    char* c = AllocateHeap(strlen(s) + 1, mtCompiler);
    strcpy(c, s);
    return c;
  }
public:
  CompilationRecord(const CompilationRecord& cr) {
    _method_name = clone_string(cr.method_name());
    _level = cr.level();
    _only_inlined = cr.only_inlined();
  }

  CompilationRecord(const char* method_name, int level, bool inlined) {
    _method_name = clone_string(method_name);
    _level = level;
    _only_inlined = inlined;
  }

  ~CompilationRecord() {
    FreeHeap(_method_name);
  }

  const char* method_name() const { return _method_name; }
  int level() const { return _level; }
  void set_level(int level) { _level = level; }
  bool only_inlined() const { return _only_inlined; }
  void set_only_inlined(bool only_inlined) { _only_inlined = only_inlined; }

  static unsigned hash_name(const char* const& n) {
    return string_hash(n);
  }
  static bool equals_name(const char* const& n1, const char* const& n2) {
    return strcmp(n1, n2) == 0;
  }
};

#endif // SHARE_COMPILER_COMPILATIONRECORD_HPP
