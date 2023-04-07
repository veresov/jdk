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

#include "ci/ciEnv.hpp"
#include "ci/ciMetadata.hpp"
#include "ci/ciObject.hpp"
#include "classfile/classLoaderData.hpp"
#include "classfile/javaClasses.hpp"
#include "compiler/compileTask.hpp"
#include "memory/metadataFactory.hpp"
#include "memory/resourceArea.hpp"
#include "oops/fieldStreams.inline.hpp"
#include "oops/method.hpp"
#include "oops/methodCounters.hpp"
#include "oops/trainingData.hpp"
#include "precompiled.hpp"
#include "runtime/arguments.hpp"
#include "runtime/fieldDescriptor.inline.hpp"
#include "runtime/javaThread.inline.hpp"
#include "runtime/jniHandles.inline.hpp"
#include "runtime/os.hpp"
#include "utilities/growableArray.hpp"
#include "utilities/xmlstream.hpp"

TrainingData::TrainingDataSet TrainingData::_training_data_set(1024);

int TrainingData::TrainingDataSet::_lock_mode;

void TrainingData::initialize() {
  // this is a nop if training modes are not enabled
  if (have_data() || need_data()) {
    TrainingDataSet::initialize();
  }
}

TrainingData::Key::Key(const KlassTrainingData* klass,
                       Symbol* method_name, Symbol* signature)
  : Key(klass->name(), klass->loader_name(),
        method_name, signature) {
}

TrainingData::Key::Key(const InstanceKlass* klass,
                       Symbol* method_name, Symbol* signature)
  : Key(klass->name(), klass->class_loader_name_and_id(),
        method_name, signature) {
}

TrainingData::Key::Key(const methodHandle& method)
  : Key(method->method_holder(),
        method->name(), method->signature()) {
}

MethodTrainingData* MethodTrainingData::make(const methodHandle& method,
                                             bool null_if_not_found) {
  MethodTrainingData* mtd = nullptr;
  if (!have_data() && !need_data()) {
    return mtd;
  }
  // Try grabbing the cached value first.
  MethodCounters* mcs = method->method_counters();
  if (mcs != nullptr) {
    mtd = mcs->method_training_data();
    if (mtd != nullptr)  return mtd;
  }

  // No cached value.  Slow path looks in the central hash table.
  if (null_if_not_found) {
    Key mkey(method);
    TrainingDataSetLocker l;
    auto v = training_data_set()->find(&mkey);
    if (v == nullptr)  return nullptr;
    mtd = v->as_MethodTrainingData();
    mtd->refresh_from(method());
    method->init_training_data(mtd);  // Cache the pointer for next time.
    return mtd;
  }

  // Make a place to cache the pointer for next time, if we can.
  if (mcs == nullptr) {
    mcs = Method::build_method_counters(Thread::current(), method());
  }

  KlassTrainingData* ktd = KlassTrainingData::make(method->method_holder());
  // FIXME: metadata should be allocated in CLD of holder
  mtd = new MethodTrainingData(ktd, method->name(), method()->signature());
  {
    TrainingDataSetLocker l;
    mtd = training_data_set()->install(mtd)->as_MethodTrainingData();
    mtd->refresh_from(method());
    // cache for next time
    method->init_training_data(mtd);  // Cache the pointer for next time.
  }

  return mtd;
}

void MethodTrainingData::refresh_from(const Method* method) {
  if (method == nullptr || method == _holder)  return;
  _holder = method;
}

CompileTrainingData* CompileTrainingData::make(CompileTask* task,
                                               Method* inlined_method) {
  int cpl = task->comp_level();
  int cid = task->compile_id();
  Thread* thread = Thread::current();
  methodHandle top_method(thread, task->method());
  methodHandle this_method;
  if (inlined_method == nullptr || inlined_method == top_method()) {
    this_method = top_method;
  } else {
    this_method = methodHandle(thread, inlined_method);
  }
  MethodTrainingData* topm = MethodTrainingData::make(top_method);
  topm->notice_compilation(cpl);
  MethodTrainingData* thism = topm;
  if (inlined_method != top_method()) {
    thism = MethodTrainingData::make(this_method);
    thism->notice_compilation(cpl, true);
  }

  // Find the insertion point.  Also check for duplicate records.
  CompileTrainingData* *insp = &thism->_compile;
  while ((*insp) != nullptr && (*insp)->compile_id() > cid) {
    insp = &(*insp)->_next;
  }
  while ((*insp) != nullptr && (*insp)->compile_id() == cid) {
    if ((*insp)->method() == thism &&
        (*insp)->top_method() == topm) {
      break;
    }
  }

  // FIXME: metadata should be allocated in CLD of a method holder
  CompileTrainingData* ctd = new CompileTrainingData(thism, topm, cpl, cid);

  // Link it into the method, under a lock.
  TrainingDataSetLocker l;
  while ((*insp) != nullptr && (*insp)->compile_id() == cid) {
    if ((*insp)->method() == thism &&
        (*insp)->top_method() == topm) {
      delete ctd;
      return (*insp);
    }
  }
  ctd->_next = (*insp);
  (*insp) = ctd;
  return ctd;
}

void CompileTrainingData::record_compilation_queued(CompileTask* task) {
  _qtime = tty->time_stamp().seconds();
}
void CompileTrainingData::record_compilation_start(CompileTask* task) {
  _stime = tty->time_stamp().seconds();
}
void CompileTrainingData::record_compilation_end(CompileTask* task) {
  _etime = tty->time_stamp().seconds();
  if (task->is_success()) {   // record something about the nmethod output
    _nm_total_size = task->nm_total_size();
  }
}
void CompileTrainingData::notice_inlined_method(CompileTask* task,
                                                const methodHandle& method) {
  //CompileTrainingData::make(task, method);
  // all this does is put a mark on the method:
  auto mtd = MethodTrainingData::make(method);
  if (mtd != nullptr)  mtd->notice_compilation(task->comp_level(), true);
}

void CompileTrainingData::notice_jit_observation(ciEnv* env, ciBaseObject* what) {
  // A JIT is starting to look at class k.
  // We could follow the queries that it is making, but it is
  // simpler to assume, conservatively, that the JIT will
  // eventually depend on the initialization state of k.
  CompileTask* task = env->task();
  assert(task != nullptr, "");
  Method* method = task->method();
  InstanceKlass* compiling_klass = method->method_holder();
  if (what->is_metadata()) {
    ciMetadata* md = what->as_metadata();
    if (md->is_instance_klass()) {
      InstanceKlass* ik = md->as_instance_klass()->get_instanceKlass();
      KlassTrainingData* ktd = ik->training_data_or_null();
      if (ktd == nullptr)  return;
      ktd->record_touch_common(env->log(), "jit", task,
                               compiling_klass, nullptr,
                               method->name(), method->signature(),
                               nullptr);
      // This JIT task is (probably) requesting that ik be initialized,
      // so add him to my _init_deps list.
      _init_deps.append_if_missing(ktd);
    }
  }
}




static int cmp_zeroes_to_end(int id1, int id2) {
  int cmp = id1 - id2;
  // sort zeroes to the end, not the start
  return (id1 == 0 || id2 == 0) ? -cmp : cmp;
}

int MethodTrainingData::cmp(const TrainingData* tdata) const {
  if (this == tdata)  return 0;
  if (tdata->is_MethodTrainingData()) {
    const MethodTrainingData* that = tdata->as_MethodTrainingData();
    if (this->klass() == that->klass()) {
      int cmp = cmp_zeroes_to_end(this->last_compile_id(),
                                  that->last_compile_id());
      return cmp != 0 ? cmp : this->key()->cmp(that->key());
    }
    tdata = that->klass();
  }
  return this->klass()->cmp(tdata);
}

int KlassTrainingData::cmp(const TrainingData* tdata) const {
  if (this == tdata)  return 0;
  if (tdata->is_KlassTrainingData()) {
    const KlassTrainingData* that = tdata->as_KlassTrainingData();
    int cmp = cmp_zeroes_to_end(this->clinit_sequence_index_or_zero(),
                                that->clinit_sequence_index_or_zero());
    return cmp != 0 ? cmp : this->key()->cmp(that->key());
  }
  assert(tdata->is_MethodTrainingData(), "");
  return 0 - tdata->cmp(this);
}

class TrainingDataDumper {
  xmlStream* _out;
  GrowableArray<const TrainingData*> _index;

 public:
  TrainingDataDumper() {
    _out = nullptr;
  }

  void set_out(xmlStream* out) {
    assert(_out == nullptr && out != nullptr, "");
    _out = out;
    _out->head("training_data");
  }

  void close() {
    if (_out != nullptr) {
      _out->tail("training_data");
      _out->flush();
      _out = nullptr;
    }
  }

  ~TrainingDataDumper() { close(); }

  xmlStream* out() { return _out; }

  // Return -1 if not yet dumped, else index it was dumped under.
  // Second argument enables allocation of a new index if needed.
  // Yes, this is quadratic.  No, we don't care about that at the
  // end of a training run.  When deserializing, the corresponding
  // operation uses a similar temporary GrowableArray but is O(1).
  int id_of(const TrainingData* tdata) {
    return _index.find(tdata);
  }

  // Make sure this guy get line printed with id='%d'.
  int identify(TrainingData* tdata) {
    if (tdata == nullptr)  return -1;
    int len = _index.length();
    int id = id_of(tdata);
    if (id >= 0)  return id;  // already assigned
    id = _index.append(tdata);
    if (tdata->dump(*this, TrainingData::DP_identify))  return id;
    // this tdata refused to identify itself
    if (id == _index.length() - 1) {
      _index.remove_at(id);
    } else {
      _index.at_put(id, nullptr);
    }
    return -1;
  }

  template<typename DAG, typename ELP>
  int dump_id_list(int depc, DAG dep_at_get, ELP elem_print) {
    for (int pass = 0; ; pass++) {
      int idc = 0;
      for (int i = 0; i < depc; i++) {
        auto dep = dep_at_get(i);
        int id = identify(dep);
        if (id < 0)  continue;
        if (pass == 1) {
          if (idc == 0)
                out()->print("%d", id);
          else  out()->print(" %d", id);
        }
        idc += 1;
      }
      if (pass == 1 || idc == 0) {
        return idc;
      }
      // second pass and idc > 0, so we will print something
      elem_print();
    }
  }
};

using ClassState = InstanceKlass::ClassState;
#define EACH_CLASS_STATE(FN) \
    FN(allocated, "A") \
    FN(loaded, "O") \
    FN(being_linked, "BL") \
    FN(linked, "L") \
    FN(being_initialized, "BI") \
    FN(fully_initialized, "I") \
    FN(initialization_error, "IE") \
    /**/
static const char* ClassState_to_name(ClassState state) {
  #define SWITCH_CASE(x, y) \
    case ClassState::x: return y;
  switch (state) { EACH_CLASS_STATE(SWITCH_CASE) }
  #undef SWITCH_CASE
  return "?";
}
static int name_to_ClassState(const char* n) {
  #define NAME_CASE(x, y) \
    if (!strcmp(n, y))  return (int)ClassState::x;
  EACH_CLASS_STATE(NAME_CASE);
  #undef NAME_CASE
  return -1;
}

bool KlassTrainingData::dump(TrainingDataDumper& tdd, DumpPhase dp) {
  if (_do_not_dump) {
    return false;
  }
  if (dp == DP_prepare) {
    return true;
  }
  auto out = tdd.out();
  int kid = tdd.id_of(this);
  if (dp == DP_identify) {
    out->begin_elem("klass id='%d'", kid);
    out->name(this->name());
    Symbol* ln = this->loader_name();
    if (ln != nullptr)  out->name(this->name(), "loader_");
    ClassState state = ClassState::allocated;
    if (has_holder())  state = holder()->init_state();
    out->print(" state='%s'", ClassState_to_name(state));
    out->end_elem();
    return true;
  }
  assert(dp == DP_detail, "");
  int depc = init_dep_count();
  if (depc > 0) {
    auto begin = [&]{
      out->begin_elem("klass_deps klass='%d' ids='", kid);
    };
    int idc = tdd.dump_id_list(depc, [&](int i){ return init_dep(i); }, begin);
    if (idc > 0) {
      out->print("'");
      out->end_elem();
    }
  }
  return true;
}

bool MethodTrainingData::dump(TrainingDataDumper& tdd, DumpPhase dp) {
  if (_do_not_dump) {
    return false;
  }
  if (dp == DP_prepare) {
    if (has_holder()) {
      // FIXME: we might need to clone these two things
      _final_counters = holder()->method_counters();
      _final_profile  = holder()->method_data();
    }
    return true;
  }
  int mid = tdd.id_of(this);
  auto out = tdd.out();
  if (dp == DP_identify) {
    int kid = tdd.identify(this->klass());
    out->begin_elem("method id='%d' klass='%d'", mid, kid);
    out->name(this->name());
    out->signature(this->signature());
    out->print(" level_mask='%d'", _level_mask);
    if (last_compile_id() != 0) out->print(" compile_id='%d'", last_compile_id());
    // FIXME: dump counters, MDO, list of classes depended on
    out->end_elem();
    return true;
  }
  assert(dp == DP_detail, "");
  for (auto ctd = _compile; ctd != nullptr; ctd = ctd->next()) {
    int cid = ctd->compile_id();
    out->begin_elem("compile compile_id='%d' level='%d' method='%d'",
                    cid, ctd->level(), mid);
    if (ctd->is_inlined())  out->print(" is_inlined='1'");
    out->end_elem();
    int depc = ctd->init_dep_count();
    if (depc > 0) {
      auto begin = [&]{
        out->begin_elem("compile_deps compile_id='%d' ids='", cid);
      };
      int idc = tdd.dump_id_list(depc, [&](int i){ return ctd->init_dep(i); },
                                 begin);
      if (idc > 0) {
        out->print("'");
        out->end_elem();
      }
    }
  }
  return true;
}

static int qsort_compare_tdata(TrainingData** p1, TrainingData** p2) {
  return (*p1)->cmp(*p2);
}

void TrainingData::store_results() {
  if (!need_data() && !have_data())  return;

  ResourceMark rm;
  TrainingDataDumper tdd;

  // collect all the training data and prepare to dump or archive
  GrowableArray<TrainingData*> tda;
  int prev_len = -1, len = 0;
  while (prev_len != len) {
    assert(prev_len < len, "must not shrink the worklist");
    prev_len = len; len = tda.length();
    for (int i = 0; i < len; i++) {
      tda.at(i)->prepare_to_dump(tdd);
    }
    tda.clear();
    // Since prepare_to_dump might have entered new items into the
    // global TD table, we need to enumerate again from scratch.
    {
      TrainingDataSetLocker l;
      training_data_set()->iterate_all([&](const Key* k, TrainingData* td) {
        tda.append(td);
      });
    }
  }
  tda.sort(qsort_compare_tdata);
  // Data is ready to dump now.

  const char* file_name = TrainingFile;
  if (file_name == nullptr)  file_name = "hs_training_%p.log";
  if (strstr(file_name, "%p")) {
    const char* tmplt = file_name;
    size_t buf_len = strlen(tmplt) + 100;
    char* buf = NEW_RESOURCE_ARRAY(char, buf_len);
    if (buf != nullptr) {
      Arguments::copy_expand_pid(tmplt, strlen(tmplt), buf, buf_len);
      // (if copy_expand_pid fails, we will be OK with its partial output)
      file_name = buf;
    }
  }
  fileStream file(file_name);
  if (!file.is_open()) {
    warning("Training data failed: cannot open file %s", file_name);
    return;
  }
  xmlStream out(&file);
  tdd.set_out(&out);
  for (int i = 0; i < tda.length(); i++) {
    tda.at(i)->dump(tdd, DP_prepare);
  }
  for (int i = 0; i < tda.length(); i++) {
    tdd.identify(tda.at(i));
    tda.at(i)->dump(tdd, DP_detail);
  }
  tdd.close();
}

using FieldData = KlassTrainingData::FieldData;
int KlassTrainingData::_clinit_count;  //number <clinit> events in RecordTraining
GrowableArrayCHeap<FieldData, mtCompiler>* KlassTrainingData::_no_static_fields;

KlassTrainingData* KlassTrainingData::make(Symbol* name, Symbol* loader_name) {
  // FIXME: metadata should be allocated in default CLD
  KlassTrainingData* tdata = new KlassTrainingData(name, loader_name);
  if (tdata != nullptr) {
    TrainingDataSetLocker l;
    tdata = training_data_set()->install(tdata)->as_KlassTrainingData();
  }
  return tdata;
}

KlassTrainingData* KlassTrainingData::make(InstanceKlass* holder) {
  // FIXME: metadata should be allocated in CLD of holder
  KlassTrainingData* tdata = new KlassTrainingData(holder);
  if (tdata != nullptr) {
    TrainingDataSetLocker l;
    tdata = training_data_set()->install(tdata)->as_KlassTrainingData();
    tdata->refresh_from(holder);
    bool ok = holder->init_training_data(tdata);
    assert(ok, "CAS under mutex cannot fail");
  }
  return tdata;
}

void KlassTrainingData::refresh_from(const InstanceKlass* klass) {
  if (!has_holder()) {
    init_holder(klass);
  }
  if (holder() == klass) {
    if (klass->is_initialized() && !_clinit_is_done) {
      _clinit_is_done = true;
    }
  }
}

void KlassTrainingData::init_holder(const InstanceKlass* klass) {
  if (holder() == klass) {
    return;   // no change to make
  }

  jobject hmj = _holder_mirror;
  if (hmj != nullptr) {   // clear out previous handle, if any
    _holder_mirror = nullptr;
    assert(JNIHandles::is_global_handle(hmj), "");
    JNIHandles::destroy_global(hmj);
  }

  // reset state derived from any previous klass
  _static_fields = nullptr;
  _fieldinit_count = 0;
  _clinit_is_done = false;
  _clinit_sequence_index = 0;

  // Keep the klass alive during the training run, unconditionally.
  //
  // FIXME: Revisit this decision; we could allow training runs to
  // unload classes in the normal way.  We might use make_weak_global
  // instead of make_global.
  //
  // The data from the training run would mention the name of the
  // unloaded class (and of its loader).  Is it worth the complexity
  // to track and then unload classes, remembering just their names?

  if (klass != nullptr) {
    Handle hm(JavaThread::current(), klass->java_mirror());
    hmj = JNIHandles::make_global(hm);
    Atomic::release_store(&_holder_mirror, hmj);
  }

  Atomic::release_store(&_holder, const_cast<InstanceKlass*>(klass));
  assert(holder() == klass, "");

  if (klass == nullptr)  return;

  bool is_init = klass->is_initialized();
  _clinit_is_done = is_init;
  if (is_init) {
    // if <clinit> is in the past, do not bother tracking fields
    _static_fields = no_static_fields();
  } else {
    setup_static_fields(klass);
  }
}

void KlassTrainingData::record_initialization_start() {
  assert(_clinit_sequence_index == 0, "set this under mutex");
  _clinit_sequence_index = next_clinit_count();
  log_initialization(true);
}

bool KlassTrainingData::add_initialization_touch(Klass* requester) {
  _has_initialization_touch = true;
  if (requester == nullptr || !requester->is_instance_klass())
    return false;
  auto rtd = KlassTrainingData::make(InstanceKlass::cast(requester));
  if (rtd != nullptr) {
    // The requester is asking that I be initialized; this means
    // that I should be added to his _init_deps list.up
    rtd->_init_deps.append_if_missing(this);
  }
  return true;
}

void KlassTrainingData::record_initialization_end() {
  _clinit_is_done = true;  // we know this now
  log_initialization(false);
}

GrowableArrayCHeap<FieldData, mtCompiler>*
KlassTrainingData::no_static_fields() {
  GrowableArrayCHeap<FieldData, mtCompiler>* nsf = _no_static_fields;
  if (nsf != nullptr) {
    return nsf;
  }
  nsf = new GrowableArrayCHeap<FieldData, mtCompiler>(0);
  if (nsf != nullptr && !Atomic::replace_if_null(&_no_static_fields, nsf)) {
    delete nsf;
    nsf = _no_static_fields;
  }
  return nsf;
}

// Note:  Racers may do this more than once.
// So, make no externally visible side effects.
void KlassTrainingData::setup_static_fields(const InstanceKlass* holder) {
  auto fda = Atomic::load_acquire(&_static_fields);
  if (fda != nullptr)  return;
  fda = new GrowableArrayCHeap<FieldData, mtCompiler>();
  int num_statics = 0;
  for (JavaFieldStream fs(holder); !fs.done(); fs.next()) {
    if (!fs.access_flags().is_static())
      continue;  // only tracking static fields
    if (fs.access_flags().is_final() && fs.initval_index() != 0)
      continue;  // skip constants initialized directly by the JVM
    fda->append(FieldData());
    // set up tracking data for the field
    FieldData& data = fda->adr_at(num_statics)[0];
    data.init_from(fs.field_descriptor());
    if (!field_state_is_clean(&data)) {
      data._fieldinit_sequence_index = ++_fieldinit_count;
    }
    ++num_statics;
  }
  if (num_statics == 0) {
    delete fda;
    fda = no_static_fields();
  }

  // After the array is set up, store it; arbitrate among racers.
  if (!Atomic::replace_if_null(&_static_fields, fda)) {
    if (fda != no_static_fields()) {
      delete fda;
    }
  }
}

// Combined linear search pass to find the name, and also
// note missed field updates.  It could be a fancy binary search,
// except we want to do a linear walk anyway to look for updates.
// It is possible we missed an initial `putstatic`, or maybe it never happened.
// Work around the leaky detection by periodic checks for evidence of inits.
KlassTrainingData::FieldData*
KlassTrainingData::check_field_states_and_find_field(Symbol* name) {
  int len;
  if (_static_fields == nullptr || (len = _static_fields->length()) == 0)
    return nullptr;
  FieldData* result = nullptr;
  for (int i = 0; i < len; i++) {
    FieldData* fdata = _static_fields->adr_at(i);
    if (fdata->_name == name)  result = fdata;
    if (fdata->_fieldinit_sequence_index == 0 &&
        !field_state_is_clean(fdata)) {
      // Oops, a missed update.  Track it after the fact.
      assert(!all_field_states_done(), "");
      record_static_field_init(fdata, "unknown");
    }
  }
  return result;
}

bool KlassTrainingData::record_static_field_init(FieldData* fdata,
                                            const char* reason) {
  int& seq = fdata->_fieldinit_sequence_index;
  int PENDING = -1;
  int found = Atomic::cmpxchg(&seq, 0, PENDING, memory_order_conservative);
  if (found != 0)  return false;  // racer beat us to it
  Atomic::store(&seq, next_fieldinit_count());
  {
    ttyLocker ttyl;
    xtty->begin_elem("initialize_static_field");
    xtty->klass(holder());
    print_iclock_attr(holder(), xtty, seq);
    xtty->name(fdata->_name);
    xtty->print(" reason='%s'", reason);
    xtty->thread();
    xtty->stamp();
    xtty->end_elem();
  }
  return true;
}

void KlassTrainingData::print_klass_attrs(xmlStream* xtty,
                                     Klass* klass, const char* prefix) {
  if (!klass)  return;
  xtty->klass(klass, prefix);
  if (!klass->is_instance_klass())  return;

  // print a little more information in case it is useful
  InstanceKlass* ik = InstanceKlass::cast(klass);
  int ikf = ik->access_flags().as_int() & (u2)-1;
  ikf &= ~JVM_ACC_SUPER;  // this is strictly noise
  char ikf2[20];
  char* ikf2p = &ikf2[0];
  if (ik->is_sealed()) { *ikf2p++ = 's'; }
  *ikf2p = 0;
  // no need for is_hidden since the name makes it obvious
  xtty->print(" %skflags='%d%s'", prefix, ikf, &ikf2[0]);
  print_iclock_attr(ik, xtty, -1, prefix);
}

void KlassTrainingData::print_iclock_attr(InstanceKlass* klass,
                                          xmlStream* xtty,
                                          int fieldinit_index,
                                          const char* prefix) {
  KlassTrainingData* tdata = klass->training_data_or_null();
  const int all_fields_done = 9999;
  int clinit_index = 0;
  if (tdata != nullptr) {
    if (fieldinit_index < 0) {
      if (tdata->_clinit_is_done)
        fieldinit_index = all_fields_done;
      else {
        fieldinit_index = tdata->_fieldinit_count;
        if (fieldinit_index > 900) {
          // ... 42.899, 42.900, 42.900901, 42.900902, ... 42.930000
          fieldinit_index += 900000;
        }
      }
    }
    clinit_index = tdata->clinit_sequence_index_or_zero();
  }
  const char* istate = "";
  if (klass->is_initialized()) {
    if (tdata != nullptr)
      tdata->_clinit_is_done = true;  // notice this, just in case
    fieldinit_index = all_fields_done;
  } else if (klass->is_not_initialized()) {
    if (tdata == nullptr || clinit_index != 0)
      istate = "U";
  } else if (klass->is_being_initialized()) {
    // check for intermediate states:  R = recursive, O = other thread
    istate = klass->is_init_thread(JavaThread::current()) ? "R" : "O";
  } else {
    istate = "E";  // initialization error, which is very rare
  }
  if (fieldinit_index < 0)
    fieldinit_index = 0;
  if (fieldinit_index < 100000)
    xtty->print(" %siclock='%d.%03d%s'", prefix,
                clinit_index, fieldinit_index, istate);
  else
    // avoid clock wrap for ridiculous field counts
    xtty->print(" %siclock='%d.%06d%s'", prefix,
                clinit_index, fieldinit_index, istate);
}


// Decide if the field state looks clean.
// Without further effort we cannot tell if someone has just stored
// the default value, so this query can return false positives,
// claims that a field is "clean" even if it has been subject to updates.
bool KlassTrainingData::field_state_is_clean(FieldData* fdata) {
  oop mirror = holder()->java_mirror();
  int fo = fdata->_offset;
  switch (fdata->_type) {
  case T_OBJECT:
  case T_ARRAY:
    return (mirror->obj_field(fo) == nullptr);
  case T_BYTE:
    return (mirror->byte_field(fo) == 0);
  case T_BOOLEAN:
    return (mirror->bool_field(fo) == 0);
  case T_CHAR:
    return (mirror->char_field(fo) == 0);
  case T_SHORT:
    return (mirror->short_field(fo) == 0);
  case T_INT:
  case T_FLOAT:
    // use int field format to test for zero because of -0.0f
    return (mirror->int_field(fo) == 0);
  case T_LONG:
  case T_DOUBLE:
    // use long field format to test for zero because of -0.0d
    return (mirror->long_field(fo) == 0);
  default:
    break;
  }
  return true;
}

// called externally
bool KlassTrainingData::record_static_field_init(fieldDescriptor* fd,
                                            const char* reason) {
  if (!_static_fields)  return false;  // should not happen unless OOM
  if (fd->field_holder() != holder())  return false;  // should not happen...
  FieldData* fdp = check_field_states_and_find_field(fd->name());
  if (fdp == nullptr)  return false;
  return record_static_field_init(fdp, reason);
}

void KlassTrainingData::record_touch_common(xmlStream* xtty,
                                            const char* reason,
                                            CompileTask* jit_task,
                                            Klass* init_klass,
                                            Klass* requesting_klass,
                                            Symbol* name,
                                            Symbol* sig,
                                            const char* context) {
  if (xtty == nullptr)  return;  // no detailed logging
  xtty->begin_elem("initialization_touch reason='%s'", reason);
  if (context)  xtty->print(" context='%s'", context);
  print_klass_attrs(xtty, holder());
  if (name)  xtty->name(name);
  if (sig)   xtty->signature(sig);
  // report up to two requesting parties
  for (int pass = 0; pass <= 1; pass++) {
    Klass* k = !pass ? init_klass : requesting_klass;
    if (!k)  continue;
    if (pass && k == init_klass)  break;
    const char* prefix = !pass ? "init_" : "requesting_";
    if (k == holder()) {
      xtty->print(" %sklass='//self'", prefix); continue;
    }
    print_klass_attrs(xtty, k, prefix);
  }
  if (!init_klass && !requesting_klass) {
    xtty->print_raw(" requesting_klass=''");
  }
  if (jit_task != nullptr) {
    xtty->print(" compile_id='%d'", jit_task->compile_id());
  }
  xtty->thread();
  xtty->stamp();
  xtty->end_elem();
}

void KlassTrainingData::record_initialization_touch(const char* reason,
                                                    Symbol* name,
                                                    Symbol* sig,
                                                    Klass* requesting_klass,
                                                    const char* context,
                                                    TRAPS) {
  Klass* init_klass = THREAD->class_being_initialized();
  if (!strcmp(reason, "super")) {
    // Extra-special touch during class initialization per JVMS Step 7.
    // We track this touch as if from RK.<clinit>, even if RK doesn't have one.
    init_klass = requesting_klass;
    requesting_klass = nullptr;  // ignore any real <clinit> on stack
  }
  add_initialization_touch(init_klass ? init_klass : requesting_klass);
  ttyLocker ttyl;
  record_touch_common(xtty, reason, /*jit_env*/ nullptr,
                      init_klass, requesting_klass,
                      name, sig, context);
}

void KlassTrainingData::log_initialization(bool is_start) {
  if (xtty == nullptr)  return;
  ttyLocker ttyl;
  // Note:  These XML records might not nest properly.
  // So we use <init/> and <init_done/>, not <init> and </init>.
  if (is_start) {
    xtty->begin_elem("initialization");
    print_klass_attrs(xtty, holder());
    xtty->thread();
    xtty->stamp();
    xtty->end_elem();
  } else {
    xtty->begin_elem("initialization_done");
    print_klass_attrs(xtty, holder());
    xtty->thread();
    xtty->stamp();
    xtty->end_elem();
  }
}
