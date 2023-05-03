/*
 * Copyright (c) 2019, 2023, Oracle and/or its affiliates. All rights reserved.
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
#include "cds/archiveHeapWriter.hpp"
#include "cds/archiveUtils.inline.hpp"
#include "cds/cds_globals.hpp"
#include "cds/classPrelinker.hpp"
#include "cds/dynamicArchive.hpp"
#include "cds/lambdaFormInvokers.hpp"
#include "cds/metaspaceShared.hpp"
#include "classfile/classLoaderData.inline.hpp"
#include "classfile/dictionary.hpp"
#include "classfile/symbolTable.hpp"
#include "classfile/systemDictionaryShared.hpp"
#include "classfile/vmSymbols.hpp"
#include "gc/shared/collectedHeap.hpp"
#include "gc/shared/gcVMOperations.hpp"
#include "gc/shared/gc_globals.hpp"
#include "jvm.h"
#include "logging/log.hpp"
#include "memory/metaspaceClosure.hpp"
#include "memory/resourceArea.hpp"
#include "oops/klass.inline.hpp"
#include "runtime/arguments.hpp"
#include "runtime/os.hpp"
#include "runtime/sharedRuntime.hpp"
#include "runtime/vmThread.hpp"
#include "runtime/vmOperations.hpp"
#include "utilities/align.hpp"
#include "utilities/bitMap.inline.hpp"


/*

Command-line example:

(1) Perform a trial run. At the end of the run, dump the loaded classes into foo.jsa

    For demonstration purposes, I write some a 20000 byte array that has some InstanceKlass/Method/ConstantPool
    pointers into the CDS archive.

    Note that the class "HelloWorld" is dynamically loaded in the trial run at 0x0000000801000800.

    You can add "-Xcomp", and write the nmethods into _aot_data.

$ java -cp HelloWorld.jar -XX:ArchiveClassesAtExit=foo.jsa -Xlog:cds+aot HelloWorld
Hello World
[0.634s][info][cds,aot] For java.lang.System (in static archive)
[0.634s][info][cds,aot]   k1 = 0x0000000800003290, ptr->_k1 = 0x0000000800003290 : java.lang.System
[0.634s][info][cds,aot]   m  = 0x000000080001e7a8, ptr->_m  = 0x000000080001e7a8
[0.634s][info][cds,aot] --
[0.634s][info][cds,aot] For HelloWorld (in dynamic archive)
[0.634s][info][cds,aot]   k2 = 0x0000000801000800, ptr->_k2 = 0x00007fc22b7ef008 : HelloWorld
[0.634s][info][cds,aot]   cp = 0x00007fc268400028, ptr->_cp = 0x00007fc22b7f0030


(2) This is a "production" run. HelloWorld is loaded from foo.jsa. It at a different location:
    0x0000000800d0b008.

$ java -cp HelloWorld.jar -XX:SharedArchiveFile=foo.jsa -Xlog:cds+aot HelloWorld
[0.036s][info][cds,aot] For java.lang.System (in static archive)
[0.036s][info][cds,aot]   k1 = 0x0000000800003290: java.lang.System
[0.036s][info][cds,aot]   m  = 0x000000080001e7a8: java.lang.System.<clinit>()V
[0.036s][info][cds,aot] --
[0.036s][info][cds,aot]   For HelloWorld (in dynamic archive)
[0.036s][info][cds,aot]   k2 = 0x0000000800d2c008: HelloWorld
[0.036s][info][cds,aot]   cp = 0x0000000800d2d030
Hello World


(3) CDS can also be executed in ASLR mode (with -XX:ArchiveRelocationMode=1).
    The classes will be loaded at random locations, but this is transparently handled by the CDS
    loading code. AOT doesn't need to worry about it.

$ java -cp HelloWorld.jar -XX:SharedArchiveFile=foo.jsa -Xlog:cds+aot -XX:ArchiveRelocationMode=1 HelloWorld
[0.058s][info][cds,aot] For java.lang.System (in static archive)
[0.058s][info][cds,aot]   k1 = 0x00007f724b003290: java.lang.System
[0.058s][info][cds,aot]   m  = 0x00007f724b01e7a8: java.lang.System.<clinit>()V
[0.058s][info][cds,aot] --
[0.058s][info][cds,aot]   For HelloWorld (in dynamic archive)
[0.058s][info][cds,aot]   k2 = 0x00007f724bd2c008: HelloWorld
[0.058s][info][cds,aot]   cp = 0x00007f724bd2d030
Hello World

*/

struct DummyAotData {
  size_t _byte_size;
  Klass* _k1;
  Method* _m;
  int    _junk1;
  int    _junk2;
  int    _junk3;
  Klass* _k2;
  ConstantPool* _cp;
};

static DummyAotData* _aot_data = nullptr;

// FIXME - this should be added to ArchiveBuilder API
template <typename T> void relocate_src_pointer_to_buffered(T* ptr_loc) {
  T src_addr = *ptr_loc; // Points to live data that's used in the trial run
  if (src_addr != nullptr) {
    T buffered_addr = (T)ArchiveBuilder::current()->get_buffered_addr((address)src_addr);
    *ptr_loc = buffered_addr; // Points to the "buffered copy" of the live data 
    ArchivePtrMarker::mark_pointer((address*)ptr_loc);
  }
}

void dummy_aot_write_cache() {
  // Test code: just get some Klass pointers
  InstanceKlass* k1 = nullptr;
  InstanceKlass* k2 = nullptr;
  Method* m = nullptr;
  ConstantPool* cp = nullptr;

  k1 = SystemDictionary::find_instance_klass(Thread::current(), vmSymbols::java_lang_System(),
                                             Handle(), Handle());
  m = k1->class_initializer();

  {
    // Can't use SystemDictionary::find_instance_klass because we are in a safepoint and cannot
    // create a non-null Handle.
    ClassLoaderData* loader_data = ClassLoaderData::class_loader_data_or_null(SystemDictionary::java_system_loader());
    Dictionary* dictionary = loader_data->dictionary();
    k2 = dictionary->find(Thread::current(), vmSymbols::helloWorld(), Handle());
    cp = k2->constants();
  }

  // Allocate a buffer that's large enough to hold all of the AOT code
  size_t byte_size = 20000;
  DummyAotData* ptr = (DummyAotData*)ArchiveBuilder::ro_region_alloc(byte_size);

  // Copy AOT code into this buffer. Our dummy AOT code just contains some random bytes,
  // plus a few metadata pointers.
  ptr->_byte_size = byte_size;
  ptr->_k1 = k1;  // points to live data.
  ptr->_m  = m;   // points to live data.
  ptr->_junk1 = 1;
  ptr->_junk2 = 2;
  ptr->_junk3 = 3;
  ptr->_k2 = k2;  // points to live data.
  ptr->_cp = cp;  // points to live data.

  // Mark all these pointers, and relocate them to point to the "buffered copy" as necessary.
  relocate_src_pointer_to_buffered(&ptr->_k1); // now points to buffered copy
  relocate_src_pointer_to_buffered(&ptr->_m);  // now points to buffered copy
  relocate_src_pointer_to_buffered(&ptr->_k2); // now points to buffered copy
  relocate_src_pointer_to_buffered(&ptr->_cp); // now points to buffered copy

  ResourceMark rm;
  log_info(cds, aot)("For java.lang.System (in static archive)");
  log_info(cds, aot)("  k1 = " INTPTR_FORMAT ", ptr->_k1 = " INTPTR_FORMAT " : %s", p2i(k1), p2i(ptr->_k1), k1->external_name());
  log_info(cds, aot)("  m  = " INTPTR_FORMAT ", ptr->_m  = " INTPTR_FORMAT, p2i(m), p2i(ptr->_m));
  log_info(cds, aot)("--");
  log_info(cds, aot)("For HelloWorld (in dynamic archive)");
  log_info(cds, aot)("  k2 = " INTPTR_FORMAT ", ptr->_k2 = " INTPTR_FORMAT " : %s", p2i(k2), p2i(ptr->_k2), k2->external_name());
  log_info(cds, aot)("  cp = " INTPTR_FORMAT ", ptr->_cp = " INTPTR_FORMAT, p2i(cp), p2i(ptr->_cp));

  _aot_data = ptr;
}

void dummy_aot_serialize_data(SerializeClosure* soc) {
  soc->do_ptr((void**)&_aot_data);

  // The pointers inside _aot_data have been relocated to point to the latest addresses of the archived
  // metadata objects.
  //
  // The AOT code can be restored at any time after this point.

  if (soc->reading()) {
    log_info(cds, aot)("_aot_data = " INTPTR_FORMAT ":", p2i(_aot_data));
    if (_aot_data != nullptr) {
      ResourceMark rm;
      log_info(cds, aot)("For java.lang.System (in static archive)");
      log_info(cds, aot)("  k1 = " INTPTR_FORMAT ": %s", p2i(_aot_data->_k1), _aot_data->_k1->external_name());
      log_info(cds, aot)("  m  = " INTPTR_FORMAT ": %s", p2i(_aot_data->_m),  _aot_data->_m->name_and_sig_as_C_string());
      log_info(cds, aot)("--");
      log_info(cds, aot)("  For HelloWorld (in dynamic archive)");
      log_info(cds, aot)("  k2 = " INTPTR_FORMAT ": %s", p2i(_aot_data->_k2), _aot_data->_k2->external_name());
      log_info(cds, aot)("  cp = " INTPTR_FORMAT, p2i(_aot_data->_cp));
    }
  }
}

class DynamicArchiveBuilder : public ArchiveBuilder {
  const char* _archive_name;
public:
  DynamicArchiveBuilder(const char* archive_name) : _archive_name(archive_name) {}
  void mark_pointer(address* ptr_loc) {
    ArchivePtrMarker::mark_pointer(ptr_loc);
  }

  static int dynamic_dump_method_comparator(Method* a, Method* b) {
    Symbol* a_name = a->name();
    Symbol* b_name = b->name();

    if (a_name == b_name) {
      return 0;
    }

    u4 a_offset = ArchiveBuilder::current()->any_to_offset_u4(a_name);
    u4 b_offset = ArchiveBuilder::current()->any_to_offset_u4(b_name);

    if (a_offset < b_offset) {
      return -1;
    } else {
      assert(a_offset > b_offset, "must be");
      return 1;
    }
  }

public:
  DynamicArchiveHeader *_header;

  void init_header();
  void release_header();
  void post_dump();
  void sort_methods();
  void sort_methods(InstanceKlass* ik) const;
  void remark_pointers_for_instance_klass(InstanceKlass* k, bool should_mark) const;
  void write_archive(char* serialized_data);

public:
  DynamicArchiveBuilder() : ArchiveBuilder() { }

  // Do this before and after the archive dump to see if any corruption
  // is caused by dynamic dumping.
  void verify_universe(const char* info) {
    if (VerifyBeforeExit) {
      log_info(cds)("Verify %s", info);
      // Among other things, this ensures that Eden top is correct.
      Universe::heap()->prepare_for_verify();
      Universe::verify(info);
    }
  }

  void doit() {
    verify_universe("Before CDS dynamic dump");
    DEBUG_ONLY(SystemDictionaryShared::NoClassLoadingMark nclm);

    // Block concurrent class unloading from changing the _dumptime_table
    MutexLocker ml(DumpTimeTable_lock, Mutex::_no_safepoint_check_flag);
    SystemDictionaryShared::check_excluded_classes();

    if (SystemDictionaryShared::is_dumptime_table_empty()) {
      log_warning(cds, dynamic)("There is no class to be included in the dynamic archive.");
      return;
    }

    // save dumptime tables
    SystemDictionaryShared::clone_dumptime_tables();

    init_header();
    gather_source_objs();
    reserve_buffer();

    log_info(cds, dynamic)("Copying %d klasses and %d symbols",
                           klasses()->length(), symbols()->length());
    dump_rw_metadata();
    dump_ro_metadata();
    relocate_metaspaceobj_embedded_pointers();
    relocate_roots();

    verify_estimate_size(_estimated_metaspaceobj_bytes, "MetaspaceObjs");

    char* serialized_data;
    {
      // Write the symbol table and system dictionaries to the RO space.
      // Note that these tables still point to the *original* objects, so
      // they would need to call DynamicArchive::original_to_target() to
      // get the correct addresses.
      assert(current_dump_space() == ro_region(), "Must be RO space");
      SymbolTable::write_to_archive(symbols());

      ArchiveBuilder::OtherROAllocMark mark;
      SystemDictionaryShared::write_to_archive(false);
      ClassPrelinker::record_preloaded_klasses(false);
      TrainingData::dump_training_data();
      dummy_aot_write_cache();

      serialized_data = ro_region()->top();
      WriteClosure wc(ro_region());
      SymbolTable::serialize_shared_table_header(&wc, false);
      SystemDictionaryShared::serialize_dictionary_headers(&wc, false);
      dummy_aot_serialize_data(&wc);
      ClassPrelinker::serialize(&wc, false);
      TrainingData::serialize_training_data(&wc);
    }

    verify_estimate_size(_estimated_hashtable_bytes, "Hashtables");

    sort_methods();

    log_info(cds)("Make classes shareable");
    make_klasses_shareable();

    log_info(cds)("Adjust lambda proxy class dictionary");
    SystemDictionaryShared::adjust_lambda_proxy_class_dictionary();

    log_info(cds)("Adjust method info dictionary");
    SystemDictionaryShared::adjust_method_info_dictionary();

    log_info(cds)("Adjust training data dictionary");
    TrainingData::adjust_training_data_dictionary();

    relocate_to_requested();

    write_archive(serialized_data);
    release_header();

    post_dump();

    // Restore dumptime tables
    SystemDictionaryShared::restore_dumptime_tables();

    assert(_num_dump_regions_used == _total_dump_regions, "must be");
    verify_universe("After CDS dynamic dump");
  }

  virtual void iterate_roots(MetaspaceClosure* it, bool is_relocating_pointers) {
    FileMapInfo::metaspace_pointers_do(it);
    SystemDictionaryShared::dumptime_classes_do(it);
    TrainingData::iterate_roots(it);
  }
};

void DynamicArchiveBuilder::init_header() {
  FileMapInfo* mapinfo = new FileMapInfo(_archive_name, false);
  assert(FileMapInfo::dynamic_info() == mapinfo, "must be");
  FileMapInfo* base_info = FileMapInfo::current_info();
  // header only be available after populate_header
  mapinfo->populate_header(base_info->core_region_alignment());
  _header = mapinfo->dynamic_header();

  _header->set_base_header_crc(base_info->crc());
  for (int i = 0; i < MetaspaceShared::n_regions; i++) {
    _header->set_base_region_crc(i, base_info->region_crc(i));
  }
}

void DynamicArchiveBuilder::release_header() {
  // We temporarily allocated a dynamic FileMapInfo for dumping, which makes it appear we
  // have mapped a dynamic archive, but we actually have not. We are in a safepoint now.
  // Let's free it so that if class loading happens after we leave the safepoint, nothing
  // bad will happen.
  assert(SafepointSynchronize::is_at_safepoint(), "must be");
  FileMapInfo *mapinfo = FileMapInfo::dynamic_info();
  assert(mapinfo != nullptr && _header == mapinfo->dynamic_header(), "must be");
  delete mapinfo;
  assert(!DynamicArchive::is_mapped(), "must be");
  _header = nullptr;
}

void DynamicArchiveBuilder::post_dump() {
  ArchivePtrMarker::reset_map_and_vs();
  ClassPrelinker::dispose();
}

void DynamicArchiveBuilder::sort_methods() {
  InstanceKlass::disable_method_binary_search();
  for (int i = 0; i < klasses()->length(); i++) {
    Klass* k = klasses()->at(i);
    if (k->is_instance_klass()) {
      sort_methods(InstanceKlass::cast(k));
    }
  }
}

// The address order of the copied Symbols may be different than when the original
// klasses were created. Re-sort all the tables. See Method::sort_methods().
void DynamicArchiveBuilder::sort_methods(InstanceKlass* ik) const {
  assert(ik != nullptr, "DynamicArchiveBuilder currently doesn't support dumping the base archive");
  if (MetaspaceShared::is_in_shared_metaspace(ik)) {
    // We have reached a supertype that's already in the base archive
    return;
  }

  if (ik->java_mirror() == nullptr) {
    // null mirror means this class has already been visited and methods are already sorted
    return;
  }
  ik->remove_java_mirror();

  if (log_is_enabled(Debug, cds, dynamic)) {
    ResourceMark rm;
    log_debug(cds, dynamic)("sorting methods for " PTR_FORMAT " (" PTR_FORMAT ") %s",
                            p2i(ik), p2i(to_requested(ik)), ik->external_name());
  }

  // Method sorting may re-layout the [iv]tables, which would change the offset(s)
  // of the locations in an InstanceKlass that would contain pointers. Let's clear
  // all the existing pointer marking bits, and re-mark the pointers after sorting.
  remark_pointers_for_instance_klass(ik, false);

  // Make sure all supertypes have been sorted
  sort_methods(ik->java_super());
  Array<InstanceKlass*>* interfaces = ik->local_interfaces();
  int len = interfaces->length();
  for (int i = 0; i < len; i++) {
    sort_methods(interfaces->at(i));
  }

#ifdef ASSERT
  if (ik->methods() != nullptr) {
    for (int m = 0; m < ik->methods()->length(); m++) {
      Symbol* name = ik->methods()->at(m)->name();
      assert(MetaspaceShared::is_in_shared_metaspace(name) || is_in_buffer_space(name), "must be");
    }
  }
  if (ik->default_methods() != nullptr) {
    for (int m = 0; m < ik->default_methods()->length(); m++) {
      Symbol* name = ik->default_methods()->at(m)->name();
      assert(MetaspaceShared::is_in_shared_metaspace(name) || is_in_buffer_space(name), "must be");
    }
  }
#endif

  Method::sort_methods(ik->methods(), /*set_idnums=*/true, dynamic_dump_method_comparator);
  if (ik->default_methods() != nullptr) {
    Method::sort_methods(ik->default_methods(), /*set_idnums=*/false, dynamic_dump_method_comparator);
  }
  if (ik->is_linked()) {
    // If the class has already been linked, we must relayout the i/v tables, whose order depends
    // on the method sorting order.
    // If the class is unlinked, we cannot layout the i/v tables yet. This is OK, as the
    // i/v tables will be initialized at runtime after bytecode verification.
    ik->vtable().initialize_vtable();
    ik->itable().initialize_itable();
  }

  // Set all the pointer marking bits after sorting.
  remark_pointers_for_instance_klass(ik, true);
}

template<bool should_mark>
class PointerRemarker: public MetaspaceClosure {
public:
  virtual bool do_ref(Ref* ref, bool read_only) {
    if (should_mark) {
      ArchivePtrMarker::mark_pointer(ref->addr());
    } else {
      ArchivePtrMarker::clear_pointer(ref->addr());
    }
    return false; // don't recurse
  }
};

void DynamicArchiveBuilder::remark_pointers_for_instance_klass(InstanceKlass* k, bool should_mark) const {
  if (should_mark) {
    PointerRemarker<true> marker;
    k->metaspace_pointers_do(&marker);
    marker.finish();
  } else {
    PointerRemarker<false> marker;
    k->metaspace_pointers_do(&marker);
    marker.finish();
  }
}

void DynamicArchiveBuilder::write_archive(char* serialized_data) {
  Array<u8>* table = FileMapInfo::saved_shared_path_table().table();
  SharedPathTable runtime_table(table, FileMapInfo::shared_path_table().size());
  _header->set_shared_path_table(runtime_table);
  _header->set_serialized_data(serialized_data);

  FileMapInfo* dynamic_info = FileMapInfo::dynamic_info();
  assert(dynamic_info != nullptr, "Sanity");

  dynamic_info->open_for_write();
  ArchiveHeapInfo no_heap_for_dynamic_dump;
  ArchiveBuilder::write_archive(dynamic_info, &no_heap_for_dynamic_dump);

  address base = _requested_dynamic_archive_bottom;
  address top  = _requested_dynamic_archive_top;
  size_t file_size = pointer_delta(top, base, sizeof(char));

  log_info(cds, dynamic)("Written dynamic archive " PTR_FORMAT " - " PTR_FORMAT
                         " [" UINT32_FORMAT " bytes header, " SIZE_FORMAT " bytes total]",
                         p2i(base), p2i(top), _header->header_size(), file_size);

  log_info(cds, dynamic)("%d klasses; %d symbols", klasses()->length(), symbols()->length());
}

class VM_PopulateDynamicDumpSharedSpace: public VM_GC_Sync_Operation {
  DynamicArchiveBuilder _builder;
public:
  VM_PopulateDynamicDumpSharedSpace(const char* archive_name)
  : VM_GC_Sync_Operation(), _builder(archive_name) {}
  VMOp_Type type() const { return VMOp_PopulateDumpSharedSpace; }
  void doit() {
    ResourceMark rm;
    if (AllowArchivingWithJavaAgent) {
      log_warning(cds)("This archive was created with AllowArchivingWithJavaAgent. It should be used "
              "for testing purposes only and should not be used in a production environment");
    }
    FileMapInfo::check_nonempty_dir_in_shared_path_table();

    _builder.doit();
  }
  ~VM_PopulateDynamicDumpSharedSpace() {
    LambdaFormInvokers::cleanup_regenerated_classes();
  }
};

void DynamicArchive::check_for_dynamic_dump() {
  if (DynamicDumpSharedSpaces && !UseSharedSpaces) {
    // This could happen if SharedArchiveFile has failed to load:
    // - -Xshare:off was specified
    // - SharedArchiveFile points to an non-existent file.
    // - SharedArchiveFile points to an archive that has failed CRC check
    // - SharedArchiveFile is not specified and the VM doesn't have a compatible default archive

#define __THEMSG " is unsupported when base CDS archive is not loaded. Run with -Xlog:cds for more info."
    if (RecordDynamicDumpInfo) {
      log_error(cds)("-XX:+RecordDynamicDumpInfo%s", __THEMSG);
      MetaspaceShared::unrecoverable_loading_error();
    } else {
      assert(ArchiveClassesAtExit != nullptr, "sanity");
      log_warning(cds)("-XX:ArchiveClassesAtExit" __THEMSG);
    }
#undef __THEMSG
    DynamicDumpSharedSpaces = false;
  }
}

void DynamicArchive::dump_at_exit(JavaThread* current, const char* archive_name) {
  ExceptionMark em(current);
  ResourceMark rm(current);

  if (!DynamicDumpSharedSpaces || archive_name == nullptr) {
    return;
  }

  log_info(cds, dynamic)("Preparing for dynamic dump at exit in thread %s", current->name());

  JavaThread* THREAD = current; // For TRAPS processing related to link_shared_classes
  MetaspaceShared::link_shared_classes(false/*not from jcmd*/, THREAD);
  if (!HAS_PENDING_EXCEPTION) {
    // copy shared path table to saved.
    FileMapInfo::clone_shared_path_table(current);
    TrainingData::init_dumptime_table(CHECK); // captures TrainingDataSetLocker
    if (!HAS_PENDING_EXCEPTION) {
      VM_PopulateDynamicDumpSharedSpace op(archive_name);
      VMThread::execute(&op);
      return;
    }
  }

  // One of the prepatory steps failed
  oop ex = current->pending_exception();
  log_error(cds)("Dynamic dump has failed");
  log_error(cds)("%s: %s", ex->klass()->external_name(),
                 java_lang_String::as_utf8_string(java_lang_Throwable::message(ex)));
  CLEAR_PENDING_EXCEPTION;
  DynamicDumpSharedSpaces = false;  // Just for good measure
}

// This is called by "jcmd VM.cds dynamic_dump"
void DynamicArchive::dump_for_jcmd(const char* archive_name, TRAPS) {
  assert(UseSharedSpaces && RecordDynamicDumpInfo, "already checked in arguments.cpp");
  assert(ArchiveClassesAtExit == nullptr, "already checked in arguments.cpp");
  assert(DynamicDumpSharedSpaces, "already checked by check_for_dynamic_dump() during VM startup");
  MetaspaceShared::link_shared_classes(true/*from jcmd*/, CHECK);
  // copy shared path table to saved.
  FileMapInfo::clone_shared_path_table(CHECK);
  TrainingData::init_dumptime_table(CHECK); // captures TrainingDataSetLocker

  VM_PopulateDynamicDumpSharedSpace op(archive_name);
  VMThread::execute(&op);
}

bool DynamicArchive::validate(FileMapInfo* dynamic_info) {
  assert(!dynamic_info->is_static(), "must be");
  // Check if the recorded base archive matches with the current one
  FileMapInfo* base_info = FileMapInfo::current_info();
  DynamicArchiveHeader* dynamic_header = dynamic_info->dynamic_header();

  // Check the header crc
  if (dynamic_header->base_header_crc() != base_info->crc()) {
    log_warning(cds)("Dynamic archive cannot be used: static archive header checksum verification failed.");
    return false;
  }

  // Check each space's crc
  for (int i = 0; i < MetaspaceShared::n_regions; i++) {
    if (dynamic_header->base_region_crc(i) != base_info->region_crc(i)) {
      log_warning(cds)("Dynamic archive cannot be used: static archive region #%d checksum verification failed.", i);
      return false;
    }
  }

  return true;
}
