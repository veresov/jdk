#include "compiler/methodTrainingData.hpp"
#include "oops/method.hpp"
#include "oops/methodCounters.hpp"
#include "memory/resourceArea.hpp"
#include "runtime/os.hpp"

MethodTrainingData::MethodTrainingDataSet MethodTrainingData::_method_training_data_set(1024);
MethodTrainingData::MethodTrainingDataSetLock* MethodTrainingData::MethodTrainingDataSetLocker::lock;

void MethodTrainingData::initialize() {
    MethodTrainingDataSetLocker::initialize();
}

void MethodTrainingData::load_profiles() {
  if (LoadProfiles == nullptr) {
    return;
  }
  MethodTrainingDataSetLocker l;
  int fd = os::open(LoadProfiles, O_RDONLY, 0666);
  if (fd != -1) {
    FILE* profile_file = os::fdopen(fd, "r");
    if (profile_file != nullptr) {
      fileStream profile_stream(profile_file, /*need_close=*/true);
      char line[4096];
      char* buffer;
      char method_name[4096];
      int level;
      int only_inlined;
      do {
        buffer = profile_stream.readln(line, sizeof(line));
        if (buffer != nullptr) {
          sscanf(buffer, "%s %d %d", method_name, &level, &only_inlined);
          if (!method_training_data_set()->contains(method_name)) {
            MethodTrainingData* mtd = new MethodTrainingData(method_name, level, only_inlined);
            method_training_data_set()->put(mtd->method_name(), mtd);
          }
        }
      } while (buffer != nullptr);
    }
  } else {
    tty->print_cr("# Can't open file to load profiles.");
  }
}


void MethodTrainingData::store_profiles() {
  if (StoreProfiles == nullptr) {
    return;
  }
  MethodTrainingDataSetLocker l;
  int fd = os::open(StoreProfiles, O_WRONLY | O_CREAT | O_TRUNC, 0666);
  if (fd != -1) {
    FILE* profile_file = os::fdopen(fd, "w");
    if (profile_file != nullptr) {
      fileStream profile_stream(profile_file, /*need_close=*/true);
      auto save_record = [&](const char*, MethodTrainingData* mtd) {
        profile_stream.print_cr("%s %d %d", mtd->method_name(), mtd->level(), mtd->only_inlined());
      };
      method_training_data_set()->iterate_all(save_record);
    }
  } else {
    tty->print_cr("# Can't open file to store profiles.");
  }
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
    ResourceMark rm;
    const char* method_name = method->name_and_sig_as_C_string();
    MethodTrainingDataSetLocker l;
    MethodTrainingData** v = method_training_data_set()->get(method_name);
    if (v == nullptr) {
      mtd = new MethodTrainingData(method_name, level, inlined);
      method_training_data_set()->put(mtd->method_name(), mtd);
    } else {
      mtd = *v;
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
    ResourceMark rm;
    const char* method_name = method->name_and_sig_as_C_string();
    MethodTrainingData **v = nullptr;
    {
      MethodTrainingData::MethodTrainingDataSetLocker l;
      v = method_training_data_set()->get(method_name);
    }
    if (v == nullptr) {
      return nullptr;
    }

    mtd = *v;

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

void MethodTrainingData::dump() {
  MethodTrainingData::MethodTrainingDataSetLocker l;
  method_training_data_set()->iterate_all([](const char*, MethodTrainingData* mtd) {
    tty->print_cr("%s", mtd->method_name());
  });
}