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

#ifndef SHARE_COMPILER_METHODTRAININGDATA_HPP
#define SHARE_COMPILER_METHODTRAININGDATA_HPP

#include "compiler/compiler_globals.hpp"
#include "compiler/compilerDefinitions.hpp"
#include "memory/allocation.hpp"
#include "runtime/handles.hpp"
#include "runtime/mutexLocker.hpp"
#include "utilities/resizeableResourceHash.hpp"

// Record information about a method at the time compilation is requested.
// Just a name for now, full profile later.
class MethodTrainingData : public CHeapObj<mtCompiler> {
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
  static unsigned hash_name(const char* const& n) {
    return string_hash(n);
  }
  static bool equals_name(const char* const& n1, const char* const& n2) {
    return strcmp(n1, n2) == 0;
  }
  typedef ResizeableResourceHashtable<const char*, MethodTrainingData*, AnyObj::C_HEAP, mtCompiler,
                                      MethodTrainingData::hash_name, MethodTrainingData::equals_name> MethodTrainingDataSet;
  static MethodTrainingDataSet _method_training_data_set;
  struct MethodTrainingDataSetLock : public CHeapObj<mtCompiler> {
    virtual void lock()   { TrainingData_lock->lock_without_safepoint_check(); }
    virtual void unlock() { TrainingData_lock->unlock();                       }
  };
  struct MethodTrainingDataSetLockNoop : public MethodTrainingDataSetLock {
    virtual void lock()   { }
    virtual void unlock() { }
  };
  class MethodTrainingDataSetLocker {
    static MethodTrainingDataSetLock* lock;
  public:
    static void initialize() {
      if (StoreProfiles != nullptr) {
        lock = new MethodTrainingDataSetLock();
      } else {
        lock = new MethodTrainingDataSetLockNoop();
      }
    }
    MethodTrainingDataSetLocker() {
      assert(lock != nullptr, "Forgot to call MethodTrainingDataSetLocker::initialize()");
      lock->lock();
    }
    ~MethodTrainingDataSetLocker() {
      assert(lock != nullptr, "Forgot to call MethodTrainingDataSetLocker::initialize()");
      lock->unlock();
    }
  };
  static MethodTrainingDataSet* method_training_data_set() { return &_method_training_data_set; }
 public:
  MethodTrainingData(const char* method_name, int level, bool inlined) {
    _method_name = clone_string(method_name);
    _level = level;
    _only_inlined = inlined;
  }

  ~MethodTrainingData() {
    FreeHeap(_method_name);
  }

  const char* method_name() const { return _method_name; }
  int level() const { return _level; }
  void set_level(int level) { _level = level; }
  bool only_inlined() const { return _only_inlined; }
  void set_only_inlined(bool only_inlined) { _only_inlined = only_inlined; }

  static void initialize();
  static MethodTrainingData* get(const methodHandle& method);
  static MethodTrainingData* get_cached(const methodHandle& method);
  static void notice_compilation(const methodHandle& method, int level, bool inlining);
  static void dump();


  // Fix these to load/store data from another source
  static bool has_data()  { return LoadProfiles != nullptr;  }
  static bool need_data() { return StoreProfiles != nullptr; }
  static void load_profiles();
  static void store_profiles();
};

#endif // SHARE_COMPILER_METHODTRAININGDATA_HPP
