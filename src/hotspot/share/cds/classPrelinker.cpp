/*
 * Copyright (c) 2022, 2023, Oracle and/or its affiliates. All rights reserved.
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

#include "precompiled.hpp"
#include "cds/archiveBuilder.hpp"
#include "cds/classPrelinker.hpp"
#include "classfile/classLoader.hpp"
#include "classfile/systemDictionary.hpp"
#include "classfile/systemDictionaryShared.hpp"
#include "classfile/vmClasses.hpp"
#include "memory/resourceArea.hpp"
#include "oops/constantPool.inline.hpp"
#include "oops/instanceKlass.hpp"
#include "oops/klass.inline.hpp"
#include "runtime/handles.inline.hpp"

ClassPrelinker::ClassesTable* ClassPrelinker::_processed_classes = nullptr;
ClassPrelinker::ClassesTable* ClassPrelinker::_vm_classes = nullptr;
int ClassPrelinker::_num_vm_klasses = 0;
bool ClassPrelinker::_record_java_base_only = true;
bool ClassPrelinker::_preload_java_base_only = true;
ClassPrelinker::PreloadedKlasses ClassPrelinker::_static_preloaded_klasses;
ClassPrelinker::PreloadedKlasses ClassPrelinker::_dynamic_preloaded_klasses;

bool ClassPrelinker::is_vm_class(InstanceKlass* ik) {
  return (_vm_classes->get(ik) != nullptr);
}

void ClassPrelinker::add_one_vm_class(InstanceKlass* ik) {
  bool created;
  _vm_classes->put_if_absent(ik, &created);
  if (created) {
    ++ _num_vm_klasses;
    InstanceKlass* super = ik->java_super();
    if (super != nullptr) {
      add_one_vm_class(super);
    }
    Array<InstanceKlass*>* ifs = ik->local_interfaces();
    for (int i = 0; i < ifs->length(); i++) {
      add_one_vm_class(ifs->at(i));
    }
  }
}

void ClassPrelinker::initialize() {
  assert(_vm_classes == nullptr, "must be");
  _vm_classes = new (mtClass)ClassesTable();
  _processed_classes = new (mtClass)ClassesTable();
  for (auto id : EnumRange<vmClassID>{}) {
    add_one_vm_class(vmClasses::klass_at(id));
  }
}

void ClassPrelinker::dispose() {
  assert(_vm_classes != nullptr, "must be");
  delete _vm_classes;
  delete _processed_classes;
  _vm_classes = nullptr;
  _processed_classes = nullptr;
}

bool ClassPrelinker::can_archive_resolved_klass(ConstantPool* cp, int cp_index) {
  assert(!is_in_archivebuilder_buffer(cp), "sanity");
  assert(cp->tag_at(cp_index).is_klass(), "must be resolved");

  Klass* resolved_klass = cp->resolved_klass_at(cp_index);
  assert(resolved_klass != nullptr, "must be");

  return can_archive_resolved_klass(cp->pool_holder(), resolved_klass);
}

bool ClassPrelinker::can_archive_resolved_klass(InstanceKlass* cp_holder, Klass* resolved_klass) {
  assert(!is_in_archivebuilder_buffer(cp_holder), "sanity");
  assert(!is_in_archivebuilder_buffer(resolved_klass), "sanity");

  if (resolved_klass->is_instance_klass()) {
    InstanceKlass* ik = InstanceKlass::cast(resolved_klass);
    if (is_vm_class(ik)) { // These are safe to resolve. See is_vm_class declaration.
      assert(ik->is_shared_boot_class(), "vmClasses must be loaded by boot loader");
      if (cp_holder->is_shared_boot_class()) {
        // For now, do this for boot loader only. Other loaders
        // must go through ConstantPool::klass_at_impl at runtime
        // to put this class in their directory.

        // TODO: we can support the platform and app loaders as well, if we
        // preload the vmClasses into these two loaders during VM bootstrap.
        return true;
      }
    }

    if (cp_holder->is_subtype_of(ik)) {
      // All super types of ik will be resolved in ik->class_loader() before
      // ik is defined in this loader, so it's safe to archive the resolved klass reference.
      return true;
    }

    // TODO -- allow objArray classes, too
  }

  return false;
}

void ClassPrelinker::dumptime_resolve_constants(InstanceKlass* ik, TRAPS) {
  constantPoolHandle cp(THREAD, ik->constants());
  if (cp->cache() == nullptr || cp->reference_map() == nullptr) {
    // The cache may be null if the pool_holder klass fails verification
    // at dump time due to missing dependencies.
    return;
  }

  bool first_time;
  _processed_classes->put_if_absent(ik, &first_time);
  if (!first_time) {
    // We have already resolved the constants in class, so no need to do it again.
    return;
  }

  for (int cp_index = 1; cp_index < cp->length(); cp_index++) { // Index 0 is unused
    switch (cp->tag_at(cp_index).value()) {
    case JVM_CONSTANT_UnresolvedClass:
      maybe_resolve_class(cp, cp_index, CHECK);
      break;

    case JVM_CONSTANT_String:
      resolve_string(cp, cp_index, CHECK); // may throw OOM when interning strings.
      break;
    }
  }
}

Klass* ClassPrelinker::find_loaded_class(JavaThread* THREAD, oop class_loader, Symbol* name) {
  HandleMark hm(THREAD);
  Handle h_loader(THREAD, class_loader);
  Klass* k = SystemDictionary::find_instance_or_array_klass(THREAD, name,
                                                            h_loader,
                                                            Handle());
  if (k != nullptr) {
    return k;
  }
  if (class_loader == SystemDictionary::java_system_loader()) {
    return find_loaded_class(THREAD, SystemDictionary::java_platform_loader(), name);
  } else if (class_loader == SystemDictionary::java_platform_loader()) {
    return find_loaded_class(THREAD, nullptr, name);
  }

  return nullptr;
}

Klass* ClassPrelinker::maybe_resolve_class(constantPoolHandle cp, int cp_index, TRAPS) {
  assert(!is_in_archivebuilder_buffer(cp()), "sanity");
  InstanceKlass* cp_holder = cp->pool_holder();
  if (!cp_holder->is_shared_boot_class() &&
      !cp_holder->is_shared_platform_class() &&
      !cp_holder->is_shared_app_class()) {
    // Don't trust custom loaders, as they may not be well-behaved
    // when resolving classes.
    return nullptr;
  }

  Symbol* name = cp->klass_name_at(cp_index);
  Klass* resolved_klass = find_loaded_class(THREAD, cp_holder->class_loader(), name);
  if (resolved_klass != nullptr && can_archive_resolved_klass(cp_holder, resolved_klass)) {
    Klass* k = cp->klass_at(cp_index, CHECK_NULL); // Should fail only with OOM
    assert(k == resolved_klass, "must be");
  }

  return resolved_klass;
}

#if INCLUDE_CDS_JAVA_HEAP
void ClassPrelinker::resolve_string(constantPoolHandle cp, int cp_index, TRAPS) {
  if (!DumpSharedSpaces) {
    // The archive heap is not supported for the dynamic archive.
    return;
  }

  int cache_index = cp->cp_to_object_index(cp_index);
  ConstantPool::string_at_impl(cp, cp_index, cache_index, CHECK);
}
#endif

#ifdef ASSERT
bool ClassPrelinker::is_in_archivebuilder_buffer(address p) {
  if (!Thread::current()->is_VM_thread() || ArchiveBuilder::current() == nullptr) {
    return false;
  } else {
    return ArchiveBuilder::current()->is_in_buffer_space(p);
  }
}
#endif

class ClassPrelinker::PreloadedKlassRecorder : StackObj {
  int _loader_type;
  ResourceHashtable<InstanceKlass*, bool, 15889, AnyObj::RESOURCE_AREA, mtClassShared> _seen_klasses;
  GrowableArray<InstanceKlass*> _list;
  bool loader_type_matches(InstanceKlass* k) {
    switch (_loader_type) {
    case ClassLoader::BOOT_LOADER:     return k->is_shared_boot_class();
    case ClassLoader::PLATFORM_LOADER: return k->is_shared_platform_class();
    case ClassLoader::APP_LOADER:      return k->is_shared_app_class();
    default:
      ShouldNotReachHere();
      return false;
    }
  }

  void maybe_record(InstanceKlass* ik) {
    bool created;
    _seen_klasses.put_if_absent(ik, true, &created);
    if (!created) {
      // Already seen this class when we walked the hierarchy of a previous class
      return;
    }

    if (ClassPrelinker::is_vm_class(ik)) {
      // vmClasses are loaded in vmClasses::resolve_all() at the very beginning
      // of VM bootstrap, before ClassPrelinker::runtime_preload() is called.
      return;
    }

    if (_loader_type == ClassLoader::BOOT_LOADER) {
      bool is_java_base = (ik->module() != nullptr &&
                           ik->module()->name() != nullptr &&
                           ik->module()->name()->equals("java.base"));
      if (_record_java_base_only != is_java_base) {
        return;
      }
    }

    if (ik->is_hidden()) {
      return;
    }

    if (!loader_type_matches(ik)) {
      return;
    }
    if (MetaspaceObj::is_shared(ik)) {
      assert(DynamicDumpSharedSpaces, "must be");
      return;
    }
    InstanceKlass* s = ik->java_super();
    if (s != nullptr) {
      maybe_record(s);
    }

    Array<InstanceKlass*>* interfaces = ik->local_interfaces();
    int num_interfaces = interfaces->length();
    for (int index = 0; index < num_interfaces; index++) {
      maybe_record(interfaces->at(index));
    }

    _list.append((InstanceKlass*)ArchiveBuilder::get_buffered_klass(ik));

    if (log_is_enabled(Info, cds, preload)) {
      ResourceMark rm;
      const char* loader_name;
      if (_loader_type  == ClassLoader::BOOT_LOADER) {
        if (_record_java_base_only) {
          loader_name = "boot ";
        } else {
          loader_name = "boot2";
        }
      } else if (_loader_type  == ClassLoader::PLATFORM_LOADER) {
        loader_name = "plat ";
      } else {
        loader_name = "app  ";
      }

      log_info(cds, preload)("%s %s", loader_name, ik->external_name());
    }
  }

public:
  PreloadedKlassRecorder(int loader_type) : _loader_type(loader_type),  _seen_klasses(), _list() {}

  void iterate() {
    GrowableArray<Klass*>*  klasses = ArchiveBuilder::current()->klasses();
    for (GrowableArrayIterator<Klass*> it = klasses->begin(); it != klasses->end(); ++it) {
      Klass* k = ArchiveBuilder::current()->get_source_addr(*it);
      if (k->is_instance_klass()) {
        maybe_record(InstanceKlass::cast(k));
      }
    }
  }

  Array<InstanceKlass*>* to_array() {
    Array<InstanceKlass*>* array = ArchiveBuilder::new_ro_array<InstanceKlass*>(_list.length());
    for (int i = 0; i < _list.length(); i++) {
      array->at_put(i, _list.at(i));
      ArchivePtrMarker::mark_pointer(array->adr_at(i));
    }
    return array;
  }
};

Array<InstanceKlass*>* ClassPrelinker::record_preloaded_klasses(int loader_type) {
  ResourceMark rm;
  PreloadedKlassRecorder recorder(loader_type);
  recorder.iterate();
  return recorder.to_array();
}

void ClassPrelinker::record_preloaded_klasses(bool is_static_archive) {
  if (PreloadSharedClasses) {
    PreloadedKlasses* table = (is_static_archive) ? &_static_preloaded_klasses : &_dynamic_preloaded_klasses;

    _record_java_base_only = true;
    table->_boot     = record_preloaded_klasses(ClassLoader::BOOT_LOADER);
    _record_java_base_only = false;
    table->_boot2    = record_preloaded_klasses(ClassLoader::BOOT_LOADER);

    table->_platform = record_preloaded_klasses(ClassLoader::PLATFORM_LOADER);
    table->_app      = record_preloaded_klasses(ClassLoader::APP_LOADER);
  }
}

void ClassPrelinker::serialize(SerializeClosure* soc, bool is_static_archive) {
  PreloadedKlasses* table = (is_static_archive) ? &_static_preloaded_klasses : &_dynamic_preloaded_klasses;

  soc->do_ptr((void**)&table->_boot);
  soc->do_ptr((void**)&table->_boot2);
  soc->do_ptr((void**)&table->_platform);
  soc->do_ptr((void**)&table->_app);
}

// This function is called 4 times:
// preload only java.base classes
// preload boot classes outside of java.base
// preload classes for platform loader
// preload classes for app loader
void ClassPrelinker::runtime_preload(JavaThread* current, Handle loader) {
  if (UseSharedSpaces) {
    ExceptionMark rm(current);
    runtime_preload(&_static_preloaded_klasses, loader, current);
    if (!current->has_pending_exception()) {
      runtime_preload(&_dynamic_preloaded_klasses, loader, current);
    }
    _preload_java_base_only = false;
  }

  assert(!current->has_pending_exception(), "VM should have exited due to ExceptionMark");
}

void ClassPrelinker::runtime_preload(PreloadedKlasses* table, Handle loader, TRAPS) {
  Array<InstanceKlass*>* klasses;
  const char* loader_name;

  // ResourceMark is missing in the code below due to JDK-8307315
  ResourceMark rm(THREAD);
  if (loader() == nullptr) {
    if (_preload_java_base_only) {
      loader_name = "boot ";
      klasses = table->_boot;
    } else {
      loader_name = "boot2";
      klasses = table->_boot2;
    }
  } else if (loader() == SystemDictionary::java_platform_loader()) {
    klasses = table->_platform;
    loader_name = "plat ";
  } else {
    assert(loader() == SystemDictionary::java_system_loader(), "must be");
    loader_name = "app  ";
    klasses = table->_app;
  }

  if (klasses != nullptr) {
    for (int i = 0; i < klasses->length(); i++) {
      if (log_is_enabled(Info, cds, preload)) {
        ResourceMark rm;
        log_info(cds, preload)("%s %s", loader_name, klasses->at(i)->external_name());
      }
      if (loader() == nullptr) {
        SystemDictionary::load_instance_class(klasses->at(i)->name(), loader, CHECK);
      } else {
        SystemDictionaryShared::find_or_load_shared_class(klasses->at(i)->name(), loader, CHECK);
      }
    }
  }
}
