#include "precompiled.hpp"
#include "classfile/classLoaderData.hpp"
#include "oops/method.hpp"
#include "oops/methodCounters.hpp"
#include "oops/trainingData.hpp"
#include "memory/resourceArea.hpp"
#include "runtime/os.hpp"

TrainingData::TrainingDataSet TrainingData::_training_data_set(1024);
TrainingData::TrainingDataSetLock* TrainingData::TrainingDataSetLocker::lock;

void TrainingData::initialize() {
  TrainingDataSetLocker::initialize();
}

void TrainingData::Key::dump() const {
  ResourceMark rm;
  const char *kn = (klass_name() != nullptr) ? klass_name()->as_C_string() : "null";
  const char *kln = (klass_loader_name() != nullptr) ? klass_loader_name()->as_C_string() : "null";
  const char *mn = (method_name() != nullptr) ? method_name()->as_C_string() : "null";
  const char *ms = (method_signature() != nullptr) ? method_signature()->as_C_string() : "null";
  tty->print_cr("MethodTrainingData::Key: _klass_name = %s, _klass_loader_name = %s, _method_name = %s, _method_signature = %s",
                kn, kln, mn, ms);
}

void MethodTrainingData::dump() const {
  ResourceMark rm;
  const char* n = name()->as_C_string();
  const char* s = signature()->as_C_string();
  klass()->dump();
  tty->print_cr("MethodTrainigData: _name = %s, _signature = %s, _level = %d, _only_inlined = %d",
                n, s, level(), only_inlined());
}

void KlassTrainingData::dump() const {
  ResourceMark rm;
  const char* n = name()->as_C_string();
  const char* ln = (loader_name() != nullptr) ? loader_name()->as_C_string() : "null";
  tty->print_cr("KlassTrainigData: _name = %s, _loader_name = %s", n, ln);
}

TrainingData::Key::Key(const methodHandle& method) {
  _klass_name = method->klass_name();
  _klass_loader_name = method->method_holder()->class_loader_data()->name();
  _method_name = method->name();
  _method_signature = method->signature();
}

TrainingData::Key::Key(const InstanceKlass* klass) {
  _klass_name = klass->name();
  _klass_loader_name = klass->class_loader_data()->name();
  _method_name = nullptr;
  _method_signature = nullptr;
}

void MethodTrainingData::notice_compilation(const methodHandle& method, int level, bool inlined) {
  if (!need_data()) {
    return;
  }
  MethodTrainingData* mtd = nullptr;

  // Try grabbing the cached value first.
  MethodCounters* mcs = method->method_counters();
  if (mcs != nullptr) {
    mtd = mcs->method_training_data();
  }

  // No cached value. Slow path.
  if (mtd == nullptr) {
    Key method_key(method);
    Key klass_key(method->method_holder());
    TrainingDataSetLocker l;
    TrainingData** v = training_data_set()->get(method_key);
    if (v != nullptr) {
      mtd = (*v)->as_MethodTrainingData();
    }
    if (mtd == nullptr) {
      KlassTrainingData* ktd = nullptr;
      v = training_data_set()->get(klass_key);
      if (v != nullptr) {
        ktd = (*v)->as_KlassTrainingData();
      }
      if (ktd == nullptr) {
        ktd = new KlassTrainingData(method->method_holder()->name(), method->method_holder()->class_loader_data()->name());
      }
      mtd = new MethodTrainingData(ktd, method->name(), method()->signature(), level, inlined);
      training_data_set()->put(klass_key, ktd);
      training_data_set()->put(method_key, mtd);
    }
    // Cache the value if we can.
    if (mcs == nullptr) {
      mcs = Method::build_method_counters(Thread::current(), method());
    }
    if (mcs != nullptr) {
      mcs->set_method_training_data(mtd);
    }
  }

  assert(mtd != nullptr, "Should have a MethodTrainingData");
  if (mtd->only_inlined() && !inlined) {
    mtd->set_only_inlined(false);
  }
  if (level == CompLevel_simple) {
    mtd->set_level(CompLevel_simple);
  } else if (level > mtd->level()) {
    mtd->set_level(level);
  }
}

MethodTrainingData* MethodTrainingData::get(const methodHandle& method) {
  if (!has_data()) {
    return nullptr;
  }
  MethodTrainingData* mtd = get_cached(method);
  if (mtd == nullptr) {
    TrainingData **v = nullptr;
    Key key(method);
    {
      TrainingData::TrainingDataSetLocker l;
      v = training_data_set()->get(key);
    }
    if (v == nullptr) {
      return nullptr;
    }
    mtd = (*v)->as_MethodTrainingData();
    if (mtd == nullptr) {
      return nullptr;
    }
    // Cache the pointer to the MethodTrainingData in MethodCounters for faster lookups.
    MethodCounters *mcs = method->method_counters();
    if (mcs == nullptr) {
      mcs = Method::build_method_counters(Thread::current(), method());
    }
    if (mcs != nullptr) {
      mcs->set_method_training_data(mtd);
    }
  }
  return mtd;
}

MethodTrainingData* MethodTrainingData::get_cached(const methodHandle& method) {
  if (!has_data()) {
    return nullptr;
  }
  MethodCounters* mcs = method->method_counters();
  if (mcs != nullptr) {
    return mcs->method_training_data();
  }
  return nullptr;
}

void TrainingData::dump_all() {
  TrainingData::TrainingDataSetLocker l;
  training_data_set()->iterate_all([](Key k, TrainingData* td) {
    tty->print_cr("*****");
    MethodTrainingData* mtd = td->as_MethodTrainingData();
    if (mtd != nullptr) {
      mtd->dump();
    }
    KlassTrainingData* ktd = td->as_KlassTrainingData();
    if (ktd != nullptr) {
      ktd->dump();
    }
  });
}