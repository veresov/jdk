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

#ifndef SHARE_OOPS_TRAININGDATA_HPP
#define SHARE_OOPS_TRAININGDATA_HPP

#include "compiler/compiler_globals.hpp"
#include "compiler/compilerDefinitions.hpp"
#include "memory/allocation.hpp"
#include "oops/instanceKlass.hpp"
#include "oops/symbol.hpp"
#include "runtime/handles.hpp"
#include "runtime/mutexLocker.hpp"
#include "utilities/resizeableResourceHash.hpp"

class MethodTrainingData;
class KlassTrainingData;
class TrainingData : public CHeapObj<mtCompiler>  {
public:
  class Key {
    Symbol* _klass_name;
    Symbol* _klass_loader_name;
    Symbol* _method_name;
    Symbol* _method_signature;
  public:
    Key(Symbol* klass_name, Symbol* klass_loader_name, Symbol* method_name, Symbol* method_signature) {
      _klass_name = klass_name;
      _klass_loader_name = klass_loader_name;
      _method_name = method_name;
      _method_signature = method_signature;
    }
    Key(const methodHandle& method);
    Key(const InstanceKlass* klass);
    void dump() const;

    static unsigned hash(Key const& k) {
      unsigned h =  (k.klass_name() != nullptr) ? k.klass_name()->identity_hash() : 0;
               h += (k.klass_loader_name() != nullptr) ? k.klass_loader_name()->identity_hash() : 0;
               h += (k.method_name() != nullptr) ? k.method_name()->identity_hash() : 0;
               h += (k.method_signature() != nullptr) ? k.method_signature()->identity_hash() : 0;
      return h;
    }
    static bool equals(Key const& k1, Key const& k2) {
      // We assume that all Symbols come for SymbolTable and therefore are unique.
      // Hence pointer comparison is enough to prove equality.
      return k1.klass_name() == k2.klass_name() && k1.klass_loader_name() == k2.klass_loader_name() &&
             k1.method_name() == k2.method_name() && k1.method_signature() == k2.method_signature();
    }
    Symbol* klass_name() const        { return _klass_name;        }
    Symbol* klass_loader_name() const { return _klass_loader_name; }
    Symbol* method_name() const       { return _method_name;       }
    Symbol* method_signature() const  { return _method_signature;  }
  };

private:
  typedef ResizeableResourceHashtable<Key, TrainingData*, AnyObj::C_HEAP, mtCompiler,
                                      TrainingData::Key::hash, TrainingData::Key::equals> TrainingDataSet;
  static TrainingDataSet _training_data_set;

public:
  // TODO: Fix these to mean whether we've loaded profiles and/or are collecting profiles
  static bool has_data()  { return true;  } // Going to read
  static bool need_data() { return true;  } // Going to write

  struct TrainingDataSetLock : public CHeapObj<mtCompiler> {
    virtual void lock()   { TrainingData_lock->lock_without_safepoint_check(); }
    virtual void unlock() { TrainingData_lock->unlock();                       }
  };
  struct TrainingDataSetLockNoop : public TrainingDataSetLock {
    virtual void lock()   { }
    virtual void unlock() { }
  };
  class TrainingDataSetLocker {
    static TrainingDataSetLock* lock;
  public:
    static void initialize() {
      if (need_data()) {
        lock = new TrainingDataSetLock();
      } else {
        lock = new TrainingDataSetLockNoop();
      }
    }
    TrainingDataSetLocker() {
      assert(lock != nullptr, "Forgot to call MethodTrainingDataSetLocker::initialize()");
      lock->lock();
    }
    ~TrainingDataSetLocker() {
      assert(lock != nullptr, "Forgot to call MethodTrainingDataSetLocker::initialize()");
      lock->unlock();
    }
  };
  static TrainingDataSet* training_data_set() { return &_training_data_set; }

  virtual MethodTrainingData* as_MethodTrainingData() { return nullptr; };
  virtual KlassTrainingData*  as_KlassTrainingData()  { return nullptr; };

  static void initialize();
  static void dump_all();
};

class KlassTrainingData : public TrainingData {
  Symbol* _name;
  Symbol* _loader_name;
public:
  KlassTrainingData(Symbol *name, Symbol* loader_name) :
    _name(name), _loader_name(loader_name) {
      _name->increment_refcount();
      if (_loader_name != nullptr) {
        _loader_name->increment_refcount();
      }
  }
  ~KlassTrainingData() {
    _name->decrement_refcount();
    if (_loader_name != nullptr) {
      _loader_name->decrement_refcount();
    }
  }
  Symbol* name() const { return _name; }
  Symbol* loader_name() const { return _loader_name; }
  void dump() const;
  virtual KlassTrainingData* as_KlassTrainingData() { return this; };
};

// Record information about a method at the time compilation is requested.
class MethodTrainingData : public TrainingData {
  KlassTrainingData* _klass;
  Symbol* _name;
  Symbol* _signature;
  int _level;
  bool _only_inlined;
 public:
  MethodTrainingData(KlassTrainingData* klass, Symbol* name, Symbol* signature, int level, bool only_inlined) :
    _klass(klass), _name(name), _signature(signature), _level(level), _only_inlined(only_inlined) {
    _name->increment_refcount();
    _signature->increment_refcount();
  }
  ~MethodTrainingData() {
    _name->decrement_refcount();
    _signature->decrement_refcount();
  }

  KlassTrainingData* klass() const { return _klass; }
  Symbol* name() const { return _name; }
  Symbol* signature() const { return _signature; }
  int level() const { return _level; }
  void set_level(int level) { _level = level; }
  bool only_inlined() const { return _only_inlined; }
  void set_only_inlined(bool only_inlined) { _only_inlined = only_inlined; }
  void dump() const;
  virtual MethodTrainingData* as_MethodTrainingData() { return this; };

  static MethodTrainingData* get(const methodHandle& method);
  static MethodTrainingData* get_cached(const methodHandle& method);
  static void notice_compilation(const methodHandle& method, int level, bool inlining);
};

#endif // SHARE_OOPS_TRAININGDATA_HPP
