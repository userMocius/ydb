//===-- Signposts.cpp - Interval debug annotations ------------------------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#include "llvm/Support/Signposts.h"
#include "llvm/Support/Timer.h"

#include "llvm/Config/config.h"
#if LLVM_SUPPORT_XCODE_SIGNPOSTS
#include "llvm/ADT/DenseMap.h"
#include "llvm/Support/Mutex.h"
#error #include <os/signpost.h>
#endif // if LLVM_SUPPORT_XCODE_SIGNPOSTS

using namespace llvm;

#if LLVM_SUPPORT_XCODE_SIGNPOSTS
namespace {
os_log_t *LogCreator() {
  os_log_t *X = new os_log_t;
  *X = os_log_create("org.llvm.signposts", OS_LOG_CATEGORY_POINTS_OF_INTEREST);
  return X;
}
void LogDeleter(os_log_t *X) {
  os_release(*X);
  delete X;
}
} // end anonymous namespace

namespace llvm {
class SignpostEmitterImpl {
  using LogPtrTy = std::unique_ptr<os_log_t, std::function<void(os_log_t *)>>;
  using LogTy = LogPtrTy::element_type;

  LogPtrTy SignpostLog;
  DenseMap<const void *, os_signpost_id_t> Signposts;
  sys::SmartMutex<true> Mutex;

  LogTy &getLogger() const { return *SignpostLog; }
  os_signpost_id_t getSignpostForObject(const void *O) {
    sys::SmartScopedLock<true> Lock(Mutex);
    const auto &I = Signposts.find(O);
    if (I != Signposts.end())
      return I->second;

    const auto &Inserted = Signposts.insert(
        std::make_pair(O, os_signpost_id_make_with_pointer(getLogger(), O)));
    return Inserted.first->second;
  }

public:
  SignpostEmitterImpl() : SignpostLog(LogCreator(), LogDeleter), Signposts() {}

  bool isEnabled() const { return os_signpost_enabled(*SignpostLog); }

  void startInterval(const void *O, llvm::StringRef Name) {
    if (isEnabled()) {
      // Both strings used here are required to be constant literal strings.
      os_signpost_interval_begin(getLogger(), getSignpostForObject(O),
                                 "LLVM Timers", "Begin %s", Name.data());
    }
  }

  void endInterval(const void *O, llvm::StringRef Name) {
    if (isEnabled()) {
      // Both strings used here are required to be constant literal strings.
      os_signpost_interval_end(getLogger(), getSignpostForObject(O),
                               "LLVM Timers", "End %s", Name.data());
    }
  }
};
} // end namespace llvm
#endif // if LLVM_SUPPORT_XCODE_SIGNPOSTS

#if LLVM_SUPPORT_XCODE_SIGNPOSTS
#define HAVE_ANY_SIGNPOST_IMPL 1
#else
#define HAVE_ANY_SIGNPOST_IMPL 0
#endif

SignpostEmitter::SignpostEmitter() {
#if HAVE_ANY_SIGNPOST_IMPL
  Impl = new SignpostEmitterImpl();
#else  // if HAVE_ANY_SIGNPOST_IMPL
  Impl = nullptr;
#endif // if !HAVE_ANY_SIGNPOST_IMPL
}

SignpostEmitter::~SignpostEmitter() {
#if HAVE_ANY_SIGNPOST_IMPL
  delete Impl;
#endif // if HAVE_ANY_SIGNPOST_IMPL
}

bool SignpostEmitter::isEnabled() const {
#if HAVE_ANY_SIGNPOST_IMPL
  return Impl->isEnabled();
#else
  return false;
#endif // if !HAVE_ANY_SIGNPOST_IMPL
}

void SignpostEmitter::startInterval(const void *O, StringRef Name) {
#if HAVE_ANY_SIGNPOST_IMPL
  if (Impl == nullptr)
    return;
  return Impl->startInterval(O, Name);
#endif // if !HAVE_ANY_SIGNPOST_IMPL
}

void SignpostEmitter::endInterval(const void *O, StringRef Name) {
#if HAVE_ANY_SIGNPOST_IMPL
  if (Impl == nullptr)
    return;
  Impl->endInterval(O, Name);
#endif // if !HAVE_ANY_SIGNPOST_IMPL
}
