//===- AddressSanitizer.cpp - memory error detector -----------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file is a part of AddressSanitizer, an address sanity checker.
// Details of the algorithm:
//  https://github.com/google/sanitizers/wiki/AddressSanitizerAlgorithm
//
// FIXME: This sanitizer does not yet handle scalable vectors
//
//===----------------------------------------------------------------------===//

#include "llvm/Transforms/Instrumentation/AddressSanitizer.h"
#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DepthFirstIterator.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/Statistic.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/ADT/Triple.h"
#include "llvm/ADT/Twine.h"
#include "llvm/Analysis/MemoryBuiltins.h"
#include "llvm/Analysis/TargetLibraryInfo.h"
#include "llvm/Analysis/ValueTracking.h"
#include "llvm/BinaryFormat/MachO.h"
#include "llvm/IR/Argument.h"
#include "llvm/IR/Attributes.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Comdat.h"
#include "llvm/IR/Constant.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DIBuilder.h"
#include "llvm/IR/DataLayout.h"
#include "llvm/IR/DebugInfoMetadata.h"
#include "llvm/IR/DebugLoc.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Dominators.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/GlobalAlias.h"
#include "llvm/IR/GlobalValue.h"
#include "llvm/IR/GlobalVariable.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/InlineAsm.h"
#include "llvm/IR/InstVisitor.h"
#include "llvm/IR/InstrTypes.h"
#include "llvm/IR/Instruction.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/Intrinsics.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/MDBuilder.h"
#include "llvm/IR/Metadata.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Type.h"
#include "llvm/IR/Use.h"
#include "llvm/IR/Value.h"
#include "llvm/InitializePasses.h"
#include "llvm/MC/MCSectionMachO.h"
#include "llvm/Pass.h"
#include "llvm/Support/Casting.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/MathExtras.h"
#include "llvm/Support/ScopedPrinter.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Transforms/Instrumentation.h"
#include "llvm/Transforms/Instrumentation/AddressSanitizerCommon.h"
#include "llvm/Transforms/Utils/ASanStackFrameLayout.h"
#include "llvm/Transforms/Utils/BasicBlockUtils.h"
#include "llvm/Transforms/Utils/Local.h"
#include "llvm/Transforms/Utils/ModuleUtils.h"
#include "llvm/Transforms/Utils/PromoteMemToReg.h"
#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <iomanip>
#include <limits>
#include <memory>
#include <sstream>
#include <string>
#include <tuple>

// ASAN-- Helper Headers
#include "SlimasanProject.h"
#include "llvm/Analysis/PostDominators.h"
#include "llvm/Transforms/Scalar.h"
#include "llvm/Analysis/AliasAnalysis.h"
#include "llvm/Analysis/Loads.h"
#include "llvm/Analysis/LoopInfo.h"
#include "llvm/Analysis/OptimizationRemarkEmitter.h"
#include "llvm/Analysis/RegionInfo.h"
#include "llvm/Analysis/ScalarEvolution.h"
#include "llvm/Analysis/ScalarEvolutionExpressions.h"
#include "llvm/IR/PassManager.h"
#include "llvm/Analysis/CFG.h"

// ASAN-- Scalable Value
#define RZ_SIZE 16
#define CHECK_RANGE 64
#define CHECK_RANGE_LOOP 32
#define MAX_STEP_SIZE 8

using namespace llvm;

#define DEBUG_TYPE "asan"

static const uint64_t kDefaultShadowScale = 3;
static const uint64_t kDefaultShadowOffset32 = 1ULL << 29;
static const uint64_t kDefaultShadowOffset64 = 1ULL << 44;
static const uint64_t kDynamicShadowSentinel =
    std::numeric_limits<uint64_t>::max();
static const uint64_t kSmallX86_64ShadowOffsetBase = 0x7FFFFFFF;  // < 2G.
static const uint64_t kSmallX86_64ShadowOffsetAlignMask = ~0xFFFULL;
static const uint64_t kLinuxKasan_ShadowOffset64 = 0xdffffc0000000000;
static const uint64_t kPPC64_ShadowOffset64 = 1ULL << 44;
static const uint64_t kSystemZ_ShadowOffset64 = 1ULL << 52;
static const uint64_t kMIPS32_ShadowOffset32 = 0x0aaa0000;
static const uint64_t kMIPS64_ShadowOffset64 = 1ULL << 37;
static const uint64_t kAArch64_ShadowOffset64 = 1ULL << 36;
static const uint64_t kRISCV64_ShadowOffset64 = 0x20000000;
static const uint64_t kFreeBSD_ShadowOffset32 = 1ULL << 30;
static const uint64_t kFreeBSD_ShadowOffset64 = 1ULL << 46;
static const uint64_t kNetBSD_ShadowOffset32 = 1ULL << 30;
static const uint64_t kNetBSD_ShadowOffset64 = 1ULL << 46;
static const uint64_t kNetBSDKasan_ShadowOffset64 = 0xdfff900000000000;
static const uint64_t kPS4CPU_ShadowOffset64 = 1ULL << 40;
static const uint64_t kWindowsShadowOffset32 = 3ULL << 28;
static const uint64_t kEmscriptenShadowOffset = 0;

static const uint64_t kMyriadShadowScale = 5;
static const uint64_t kMyriadMemoryOffset32 = 0x80000000ULL;
static const uint64_t kMyriadMemorySize32 = 0x20000000ULL;
static const uint64_t kMyriadTagShift = 29;
static const uint64_t kMyriadDDRTag = 4;
static const uint64_t kMyriadCacheBitMask32 = 0x40000000ULL;

// The shadow memory space is dynamically allocated.
static const uint64_t kWindowsShadowOffset64 = kDynamicShadowSentinel;

static const size_t kMinStackMallocSize = 1 << 6;   // 64B
static const size_t kMaxStackMallocSize = 1 << 16;  // 64K
static const uintptr_t kCurrentStackFrameMagic = 0x41B58AB3;
static const uintptr_t kRetiredStackFrameMagic = 0x45E0360E;

const char kAsanModuleCtorName[] = "asan.module_ctor";
const char kAsanModuleDtorName[] = "asan.module_dtor";
static const uint64_t kAsanCtorAndDtorPriority = 1;
// On Emscripten, the system needs more than one priorities for constructors.
static const uint64_t kAsanEmscriptenCtorAndDtorPriority = 50;
const char kAsanReportErrorTemplate[] = "__asan_report_";
const char kAsanRegisterGlobalsName[] = "__asan_register_globals";
const char kAsanUnregisterGlobalsName[] = "__asan_unregister_globals";
const char kAsanRegisterImageGlobalsName[] = "__asan_register_image_globals";
const char kAsanUnregisterImageGlobalsName[] =
    "__asan_unregister_image_globals";
const char kAsanRegisterElfGlobalsName[] = "__asan_register_elf_globals";
const char kAsanUnregisterElfGlobalsName[] = "__asan_unregister_elf_globals";
const char kAsanPoisonGlobalsName[] = "__asan_before_dynamic_init";
const char kAsanUnpoisonGlobalsName[] = "__asan_after_dynamic_init";
const char kAsanInitName[] = "__asan_init";
const char kAsanVersionCheckNamePrefix[] = "__asan_version_mismatch_check_v";
const char kAsanPtrCmp[] = "__sanitizer_ptr_cmp";
const char kAsanPtrSub[] = "__sanitizer_ptr_sub";
const char kAsanHandleNoReturnName[] = "__asan_handle_no_return";
static const int kMaxAsanStackMallocSizeClass = 10;
const char kAsanStackMallocNameTemplate[] = "__asan_stack_malloc_";
const char kAsanStackFreeNameTemplate[] = "__asan_stack_free_";
const char kAsanGenPrefix[] = "___asan_gen_";
const char kODRGenPrefix[] = "__odr_asan_gen_";
const char kSanCovGenPrefix[] = "__sancov_gen_";
const char kAsanSetShadowPrefix[] = "__asan_set_shadow_";
const char kAsanPoisonStackMemoryName[] = "__asan_poison_stack_memory";
const char kAsanUnpoisonStackMemoryName[] = "__asan_unpoison_stack_memory";

// ASan version script has __asan_* wildcard. Triple underscore prevents a
// linker (gold) warning about attempting to export a local symbol.
const char kAsanGlobalsRegisteredFlagName[] = "___asan_globals_registered";

const char kAsanOptionDetectUseAfterReturn[] =
    "__asan_option_detect_stack_use_after_return";

const char kAsanShadowMemoryDynamicAddress[] =
    "__asan_shadow_memory_dynamic_address";

const char kAsanAllocaPoison[] = "__asan_alloca_poison";
const char kAsanAllocasUnpoison[] = "__asan_allocas_unpoison";

// Accesses sizes are powers of two: 1, 2, 4, 8, 16.
static const size_t kNumberOfAccessSizes = 5;

static const unsigned kAllocaRzSize = 32;

// Command-line flags.

static cl::opt<bool> ClEnableKasan(
    "asan-kernel", cl::desc("Enable KernelAddressSanitizer instrumentation"),
    cl::Hidden, cl::init(false));

static cl::opt<bool> ClRecover(
    "asan-recover",
    cl::desc("Enable recovery mode (continue-after-error)."),
    cl::Hidden, cl::init(false));

static cl::opt<bool> ClInsertVersionCheck(
    "asan-guard-against-version-mismatch",
    cl::desc("Guard against compiler/runtime version mismatch."),
    cl::Hidden, cl::init(true));

// This flag may need to be replaced with -f[no-]asan-reads.
static cl::opt<bool> ClInstrumentReads("asan-instrument-reads",
                                       cl::desc("instrument read instructions"),
                                       cl::Hidden, cl::init(true));

static cl::opt<bool> ClInstrumentWrites(
    "asan-instrument-writes", cl::desc("instrument write instructions"),
    cl::Hidden, cl::init(true));

static cl::opt<bool> ClInstrumentAtomics(
    "asan-instrument-atomics",
    cl::desc("instrument atomic instructions (rmw, cmpxchg)"), cl::Hidden,
    cl::init(true));

static cl::opt<bool>
    ClInstrumentByval("asan-instrument-byval",
                      cl::desc("instrument byval call arguments"), cl::Hidden,
                      cl::init(true));

static cl::opt<bool> ClAlwaysSlowPath(
    "asan-always-slow-path",
    cl::desc("use instrumentation with slow path for all accesses"), cl::Hidden,
    cl::init(false));

static cl::opt<bool> ClForceDynamicShadow(
    "asan-force-dynamic-shadow",
    cl::desc("Load shadow address into a local variable for each function"),
    cl::Hidden, cl::init(false));

static cl::opt<bool>
    ClWithIfunc("asan-with-ifunc",
                cl::desc("Access dynamic shadow through an ifunc global on "
                         "platforms that support this"),
                cl::Hidden, cl::init(true));

static cl::opt<bool> ClWithIfuncSuppressRemat(
    "asan-with-ifunc-suppress-remat",
    cl::desc("Suppress rematerialization of dynamic shadow address by passing "
             "it through inline asm in prologue."),
    cl::Hidden, cl::init(true));

// This flag limits the number of instructions to be instrumented
// in any given BB. Normally, this should be set to unlimited (INT_MAX),
// but due to http://llvm.org/bugs/show_bug.cgi?id=12652 we temporary
// set it to 10000.
static cl::opt<int> ClMaxInsnsToInstrumentPerBB(
    "asan-max-ins-per-bb", cl::init(10000),
    cl::desc("maximal number of instructions to instrument in any given BB"),
    cl::Hidden);

// This flag may need to be replaced with -f[no]asan-stack.
static cl::opt<bool> ClStack("asan-stack", cl::desc("Handle stack memory"),
                             cl::Hidden, cl::init(true));
static cl::opt<uint32_t> ClMaxInlinePoisoningSize(
    "asan-max-inline-poisoning-size",
    cl::desc(
        "Inline shadow poisoning for blocks up to the given size in bytes."),
    cl::Hidden, cl::init(64));

static cl::opt<bool> ClUseAfterReturn("asan-use-after-return",
                                      cl::desc("Check stack-use-after-return"),
                                      cl::Hidden, cl::init(true));

static cl::opt<bool> ClRedzoneByvalArgs("asan-redzone-byval-args",
                                        cl::desc("Create redzones for byval "
                                                 "arguments (extra copy "
                                                 "required)"), cl::Hidden,
                                        cl::init(true));

static cl::opt<bool> ClUseAfterScope("asan-use-after-scope",
                                     cl::desc("Check stack-use-after-scope"),
                                     cl::Hidden, cl::init(false));

// This flag may need to be replaced with -f[no]asan-globals.
static cl::opt<bool> ClGlobals("asan-globals",
                               cl::desc("Handle global objects"), cl::Hidden,
                               cl::init(true));

static cl::opt<bool> ClInitializers("asan-initialization-order",
                                    cl::desc("Handle C++ initializer order"),
                                    cl::Hidden, cl::init(false)); //ASAN--: Removing Unsatisfiable Checks

static cl::opt<bool> ClInvalidPointerPairs(
    "asan-detect-invalid-pointer-pair",
    cl::desc("Instrument <, <=, >, >=, - with pointer operands"), cl::Hidden,
    cl::init(false));

static cl::opt<bool> ClInvalidPointerCmp(
    "asan-detect-invalid-pointer-cmp",
    cl::desc("Instrument <, <=, >, >= with pointer operands"), cl::Hidden,
    cl::init(false));

static cl::opt<bool> ClInvalidPointerSub(
    "asan-detect-invalid-pointer-sub",
    cl::desc("Instrument - operations with pointer operands"), cl::Hidden,
    cl::init(false));

static cl::opt<unsigned> ClRealignStack(
    "asan-realign-stack",
    cl::desc("Realign stack to the value of this flag (power of two)"),
    cl::Hidden, cl::init(32));

static cl::opt<int> ClInstrumentationWithCallsThreshold(
    "asan-instrumentation-with-call-threshold",
    cl::desc(
        "If the function being instrumented contains more than "
        "this number of memory accesses, use callbacks instead of "
        "inline checks (-1 means never use callbacks)."),
    cl::Hidden, cl::init(7000));

static cl::opt<std::string> ClMemoryAccessCallbackPrefix(
    "asan-memory-access-callback-prefix",
    cl::desc("Prefix for memory access callbacks"), cl::Hidden,
    cl::init("__asan_"));

static cl::opt<bool>
    ClInstrumentDynamicAllocas("asan-instrument-dynamic-allocas",
                               cl::desc("instrument dynamic allocas"),
                               cl::Hidden, cl::init(true));

static cl::opt<bool> ClSkipPromotableAllocas(
    "asan-skip-promotable-allocas",
    cl::desc("Do not instrument promotable allocas"), cl::Hidden,
    cl::init(true));

// These flags allow to change the shadow mapping.
// The shadow mapping looks like
//    Shadow = (Mem >> scale) + offset

static cl::opt<int> ClMappingScale("asan-mapping-scale",
                                   cl::desc("scale of asan shadow mapping"),
                                   cl::Hidden, cl::init(0));

static cl::opt<uint64_t>
    ClMappingOffset("asan-mapping-offset",
                    cl::desc("offset of asan shadow mapping [EXPERIMENTAL]"),
                    cl::Hidden, cl::init(0));

// Optimization flags. Not user visible, used mostly for testing
// and benchmarking the tool.

static cl::opt<bool> ClOpt("asan-opt", cl::desc("Optimize instrumentation"),
                           cl::Hidden, cl::init(true));

static cl::opt<bool> ClOptSameTemp(
    "asan-opt-same-temp", cl::desc("Instrument the same temp just once"),
    cl::Hidden, cl::init(true));

static cl::opt<bool> ClOptGlobals("asan-opt-globals",
                                  cl::desc("Don't instrument scalar globals"),
                                  cl::Hidden, cl::init(true));

static cl::opt<bool> ClOptStack(
    "asan-opt-stack", cl::desc("Don't instrument scalar stack variables"),
    cl::Hidden, cl::init(true)); //ASAN--: Removing Unsatisfiable Checks

static cl::opt<bool> ClDynamicAllocaStack(
    "asan-stack-dynamic-alloca",
    cl::desc("Use dynamic alloca to represent stack variables"), cl::Hidden,
    cl::init(true));

static cl::opt<uint32_t> ClForceExperiment(
    "asan-force-experiment",
    cl::desc("Force optimization experiment (for testing)"), cl::Hidden,
    cl::init(0));

static cl::opt<bool>
    ClUsePrivateAlias("asan-use-private-alias",
                      cl::desc("Use private aliases for global variables"),
                      cl::Hidden, cl::init(false));

static cl::opt<bool>
    ClUseOdrIndicator("asan-use-odr-indicator",
                      cl::desc("Use odr indicators to improve ODR reporting"),
                      cl::Hidden, cl::init(false));

static cl::opt<bool>
    ClUseGlobalsGC("asan-globals-live-support",
                   cl::desc("Use linker features to support dead "
                            "code stripping of globals"),
                   cl::Hidden, cl::init(true));

// This is on by default even though there is a bug in gold:
// https://sourceware.org/bugzilla/show_bug.cgi?id=19002
static cl::opt<bool>
    ClWithComdat("asan-with-comdat",
                 cl::desc("Place ASan constructors in comdat sections"),
                 cl::Hidden, cl::init(true));

// Debug flags.

static cl::opt<int> ClDebug("asan-debug", cl::desc("debug"), cl::Hidden,
                            cl::init(0));

static cl::opt<int> ClDebugStack("asan-debug-stack", cl::desc("debug stack"),
                                 cl::Hidden, cl::init(0));

static cl::opt<std::string> ClDebugFunc("asan-debug-func", cl::Hidden,
                                        cl::desc("Debug func"));

static cl::opt<int> ClDebugMin("asan-debug-min", cl::desc("Debug min inst"),
                               cl::Hidden, cl::init(-1));

static cl::opt<int> ClDebugMax("asan-debug-max", cl::desc("Debug max inst"),
                               cl::Hidden, cl::init(-1));

STATISTIC(NumInstrumentedReads, "Number of instrumented reads");
STATISTIC(NumInstrumentedWrites, "Number of instrumented writes");
STATISTIC(NumOptimizedAccessesToGlobalVar,
          "Number of optimized accesses to global vars");
STATISTIC(NumOptimizedAccessesToStackVar,
          "Number of optimized accesses to stack vars");

namespace {

/// This struct defines the shadow mapping using the rule:
///   shadow = (mem >> Scale) ADD-or-OR Offset.
/// If InGlobal is true, then
///   extern char __asan_shadow[];
///   shadow = (mem >> Scale) + &__asan_shadow
struct ShadowMapping {
  int Scale;
  uint64_t Offset;
  bool OrShadowOffset;
  bool InGlobal;
};

} // end anonymous namespace

static ShadowMapping getShadowMapping(Triple &TargetTriple, int LongSize,
                                      bool IsKasan) {
  bool IsAndroid = TargetTriple.isAndroid();
  bool IsIOS = TargetTriple.isiOS() || TargetTriple.isWatchOS();
  bool IsMacOS = TargetTriple.isMacOSX();
  bool IsFreeBSD = TargetTriple.isOSFreeBSD();
  bool IsNetBSD = TargetTriple.isOSNetBSD();
  bool IsPS4CPU = TargetTriple.isPS4CPU();
  bool IsLinux = TargetTriple.isOSLinux();
  bool IsPPC64 = TargetTriple.getArch() == Triple::ppc64 ||
                 TargetTriple.getArch() == Triple::ppc64le;
  bool IsSystemZ = TargetTriple.getArch() == Triple::systemz;
  bool IsX86_64 = TargetTriple.getArch() == Triple::x86_64;
  bool IsMIPS32 = TargetTriple.isMIPS32();
  bool IsMIPS64 = TargetTriple.isMIPS64();
  bool IsArmOrThumb = TargetTriple.isARM() || TargetTriple.isThumb();
  bool IsAArch64 = TargetTriple.getArch() == Triple::aarch64;
  bool IsRISCV64 = TargetTriple.getArch() == Triple::riscv64;
  bool IsWindows = TargetTriple.isOSWindows();
  bool IsFuchsia = TargetTriple.isOSFuchsia();
  bool IsMyriad = TargetTriple.getVendor() == llvm::Triple::Myriad;
  bool IsEmscripten = TargetTriple.isOSEmscripten();

  ShadowMapping Mapping;

  Mapping.Scale = IsMyriad ? kMyriadShadowScale : kDefaultShadowScale;
  if (ClMappingScale.getNumOccurrences() > 0) {
    Mapping.Scale = ClMappingScale;
  }

  if (LongSize == 32) {
    if (IsAndroid)
      Mapping.Offset = kDynamicShadowSentinel;
    else if (IsMIPS32)
      Mapping.Offset = kMIPS32_ShadowOffset32;
    else if (IsFreeBSD)
      Mapping.Offset = kFreeBSD_ShadowOffset32;
    else if (IsNetBSD)
      Mapping.Offset = kNetBSD_ShadowOffset32;
    else if (IsIOS)
      Mapping.Offset = kDynamicShadowSentinel;
    else if (IsWindows)
      Mapping.Offset = kWindowsShadowOffset32;
    else if (IsEmscripten)
      Mapping.Offset = kEmscriptenShadowOffset;
    else if (IsMyriad) {
      uint64_t ShadowOffset = (kMyriadMemoryOffset32 + kMyriadMemorySize32 -
                               (kMyriadMemorySize32 >> Mapping.Scale));
      Mapping.Offset = ShadowOffset - (kMyriadMemoryOffset32 >> Mapping.Scale);
    }
    else
      Mapping.Offset = kDefaultShadowOffset32;
  } else {  // LongSize == 64
    // Fuchsia is always PIE, which means that the beginning of the address
    // space is always available.
    if (IsFuchsia)
      Mapping.Offset = 0;
    else if (IsPPC64)
      Mapping.Offset = kPPC64_ShadowOffset64;
    else if (IsSystemZ)
      Mapping.Offset = kSystemZ_ShadowOffset64;
    else if (IsFreeBSD && !IsMIPS64)
      Mapping.Offset = kFreeBSD_ShadowOffset64;
    else if (IsNetBSD) {
      if (IsKasan)
        Mapping.Offset = kNetBSDKasan_ShadowOffset64;
      else
        Mapping.Offset = kNetBSD_ShadowOffset64;
    } else if (IsPS4CPU)
      Mapping.Offset = kPS4CPU_ShadowOffset64;
    else if (IsLinux && IsX86_64) {
      if (IsKasan)
        Mapping.Offset = kLinuxKasan_ShadowOffset64;
      else
        Mapping.Offset = (kSmallX86_64ShadowOffsetBase &
                          (kSmallX86_64ShadowOffsetAlignMask << Mapping.Scale));
    } else if (IsWindows && IsX86_64) {
      Mapping.Offset = kWindowsShadowOffset64;
    } else if (IsMIPS64)
      Mapping.Offset = kMIPS64_ShadowOffset64;
    else if (IsIOS)
      Mapping.Offset = kDynamicShadowSentinel;
    else if (IsMacOS && IsAArch64)
      Mapping.Offset = kDynamicShadowSentinel;
    else if (IsAArch64)
      Mapping.Offset = kAArch64_ShadowOffset64;
    else if (IsRISCV64)
      Mapping.Offset = kRISCV64_ShadowOffset64;
    else
      Mapping.Offset = kDefaultShadowOffset64;
  }

  if (ClForceDynamicShadow) {
    Mapping.Offset = kDynamicShadowSentinel;
  }

  if (ClMappingOffset.getNumOccurrences() > 0) {
    Mapping.Offset = ClMappingOffset;
  }

  // OR-ing shadow offset if more efficient (at least on x86) if the offset
  // is a power of two, but on ppc64 we have to use add since the shadow
  // offset is not necessary 1/8-th of the address space.  On SystemZ,
  // we could OR the constant in a single instruction, but it's more
  // efficient to load it once and use indexed addressing.
  Mapping.OrShadowOffset = !IsAArch64 && !IsPPC64 && !IsSystemZ && !IsPS4CPU &&
                           !IsRISCV64 &&
                           !(Mapping.Offset & (Mapping.Offset - 1)) &&
                           Mapping.Offset != kDynamicShadowSentinel;
  bool IsAndroidWithIfuncSupport =
      IsAndroid && !TargetTriple.isAndroidVersionLT(21);
  Mapping.InGlobal = ClWithIfunc && IsAndroidWithIfuncSupport && IsArmOrThumb;

  return Mapping;
}

static uint64_t getRedzoneSizeForScale(int MappingScale) {
  // Redzone used for stack and globals is at least 32 bytes.
  // For scales 6 and 7, the redzone has to be 64 and 128 bytes respectively.
  return std::max(32U, 1U << MappingScale);
}

static uint64_t GetCtorAndDtorPriority(Triple &TargetTriple) {
  if (TargetTriple.isOSEmscripten()) {
    return kAsanEmscriptenCtorAndDtorPriority;
  } else {
    return kAsanCtorAndDtorPriority;
  }
}

namespace {

/// Module analysis for getting various metadata about the module.
class ASanGlobalsMetadataWrapperPass : public ModulePass {
public:
  static char ID;

  ASanGlobalsMetadataWrapperPass() : ModulePass(ID) {
    initializeASanGlobalsMetadataWrapperPassPass(
        *PassRegistry::getPassRegistry());
  }

  bool runOnModule(Module &M) override {
    GlobalsMD = GlobalsMetadata(M);
    return false;
  }

  StringRef getPassName() const override {
    return "ASanGlobalsMetadataWrapperPass";
  }

  void getAnalysisUsage(AnalysisUsage &AU) const override {
    AU.setPreservesAll();
  }

  GlobalsMetadata &getGlobalsMD() { return GlobalsMD; }

private:
  GlobalsMetadata GlobalsMD;
};

char ASanGlobalsMetadataWrapperPass::ID = 0;

/// AddressSanitizer: instrument the code in module to find memory bugs.
struct AddressSanitizer {
  AddressSanitizer(Module &M, const GlobalsMetadata *GlobalsMD,
                   bool CompileKernel = false, bool Recover = false,
                   bool UseAfterScope = false)
      : CompileKernel(ClEnableKasan.getNumOccurrences() > 0 ? ClEnableKasan
                                                            : CompileKernel),
        Recover(ClRecover.getNumOccurrences() > 0 ? ClRecover : Recover),
        UseAfterScope(UseAfterScope || ClUseAfterScope), GlobalsMD(*GlobalsMD) {
    C = &(M.getContext());
    LongSize = M.getDataLayout().getPointerSizeInBits();
    IntptrTy = Type::getIntNTy(*C, LongSize);
    TargetTriple = Triple(M.getTargetTriple());

    Mapping = getShadowMapping(TargetTriple, LongSize, this->CompileKernel);
  }

  uint64_t getAllocaSizeInBytes(const AllocaInst &AI) const {
    uint64_t ArraySize = 1;
    if (AI.isArrayAllocation()) {
      const ConstantInt *CI = dyn_cast<ConstantInt>(AI.getArraySize());
      assert(CI && "non-constant array size");
      ArraySize = CI->getZExtValue();
    }
    Type *Ty = AI.getAllocatedType();
    uint64_t SizeInBytes =
        AI.getModule()->getDataLayout().getTypeAllocSize(Ty);
    return SizeInBytes * ArraySize;
  }

  /// Check if we want (and can) handle this alloca.
  bool isInterestingAlloca(const AllocaInst &AI);

  bool ignoreAccess(Value *Ptr);
  void getInterestingMemoryOperands(
      Instruction *I, SmallVectorImpl<InterestingMemoryOperand> &Interesting);

  void instrumentMop(ObjectSizeOffsetVisitor &ObjSizeVis,
                     InterestingMemoryOperand &O, bool UseCalls,
                     const DataLayout &DL);
  // ASAN-- Helper Functions
  void instrumentMopLoop(ObjectSizeOffsetVisitor &ObjSizeVis,
                    InterestingMemoryOperand &O, Instruction *PrevI, bool UseCalls,
                    const DataLayout &DL);
  void InvariantOptimizeHandler(Loop *L, std::set<Instruction *> &optimized, 
                      Function &F, InterestingMemoryOperand &Oper, ObjectSizeOffsetVisitor &ObjSizeVis, bool UseCalls);
  void MonotonicOptimizeHandler(Loop *L, std::set<Instruction *> &optimized, 
                      Function &F, InterestingMemoryOperand &Oper, ObjectSizeOffsetVisitor &ObjSizeVis, bool UseCalls, ScalarEvolution *SE);

  void instrumentPointerComparisonOrSubtraction(Instruction *I);
  void instrumentAddress(Instruction *OrigIns, Instruction *InsertBefore,
                         Value *Addr, uint32_t TypeSize, bool IsWrite,
                         Value *SizeArgument, bool UseCalls, uint32_t Exp);
  void instrumentUnusualSizeOrAlignment(Instruction *I,
                                        Instruction *InsertBefore, Value *Addr,
                                        uint32_t TypeSize, bool IsWrite,
                                        Value *SizeArgument, bool UseCalls,
                                        uint32_t Exp);
  Value *createSlowPathCmp(IRBuilder<> &IRB, Value *AddrLong,
                           Value *ShadowValue, uint32_t TypeSize);
  Instruction *generateCrashCode(Instruction *InsertBefore, Value *Addr,
                                 bool IsWrite, size_t AccessSizeIndex,
                                 Value *SizeArgument, uint32_t Exp);
  void instrumentMemIntrinsic(MemIntrinsic *MI);
  Value *memToShadow(Value *Shadow, IRBuilder<> &IRB);

  // ASAN-- Helper Functions
  void slimAsanOptimization(Function &F, SmallVector<InterestingMemoryOperand, 16> &OperandsToInstrument, AliasAnalysis *AA, LoopInfo *LI, ScalarEvolution *SE, ObjectSizeOffsetVisitor &ObjSizeVis, bool UseCalls);
  void sequentialExecuteOptimization(Function &F, SmallVector<InterestingMemoryOperand, 16> &OperandsToInstrument, AliasAnalysis *AA);
  void sequentialExecuteOptimizationPostDom(Function &F, SmallVector<InterestingMemoryOperand, 16> &OperandsToInstrument, AliasAnalysis *AA);
  bool ConservativeCallIntrinsicCheck(Instruction *InstStart, Instruction *InstEnd, std::set<Instruction *> &callIntrinsicSet, llvm::DominatorTree &DT, llvm::PostDominatorTree &PDT);
  void ConservativeCallIntrinsicCollect(Function &F, std::set<Instruction *> &callIntrinsicSet);
  void sequentialExecuteOptimizationBoost(Function &F, SmallVector<InterestingMemoryOperand, 16> &OperandsToInstrument);
  void baseAddrOffsetMapPreprocessing(SmallVector<InterestingMemoryOperand, 16> &OperandsToInstrument, std::map<std::pair<Value *, std::string>, std::set<std::pair<int64_t, Instruction *>>> &baseAddrOffsetMap_multi);
  void optimizeInstrumentation(Function &F, std::list<std::pair<int, std::pair<std::pair<int64_t, llvm::Instruction *>, std::vector<std::pair<int64_t, llvm::Instruction *>>>>> &rankPotentialRemoveInsts, std::set<Instruction *> &deleted, SmallVector<InterestingMemoryOperand, 16> &OperandsToInstrument);
  void mrgNeighborChks(Function &F, std::map<std::pair<Value *, std::string>, std::set<std::pair<int64_t, Instruction *>>> &baseAddrOffsetMap_multi, std::set<Instruction *> &deleted, SmallVector<InterestingMemoryOperand, 16> &OperandsToInstrument);
  void loopOptimization(Function &F, SmallVector<InterestingMemoryOperand, 16> &OperandsToInstrument, LoopInfo *LI, ScalarEvolution *SE, ObjectSizeOffsetVisitor &ObjSizeVis, bool UseCalls);
  enum addrType loopOptimizationCategorise(Function &F, Loop *L, InterestingMemoryOperand Oper, ScalarEvolution *SE);

  bool suppressInstrumentationSiteForDebug(int &Instrumented);
  bool instrumentFunction(Function &F, const TargetLibraryInfo *TLI, AliasAnalysis *AA, LoopInfo *LI, ScalarEvolution *SE);
  bool maybeInsertAsanInitAtFunctionEntry(Function &F);
  bool maybeInsertDynamicShadowAtFunctionEntry(Function &F);
  void markEscapedLocalAllocas(Function &F);

private:
  friend struct FunctionStackPoisoner;

  void initializeCallbacks(Module &M);

  bool LooksLikeCodeInBug11395(Instruction *I);
  bool GlobalIsLinkerInitialized(GlobalVariable *G);
  bool isSafeAccess(ObjectSizeOffsetVisitor &ObjSizeVis, Value *Addr,
                    uint64_t TypeSize) const;
  //ASAN--: Removing Unsatisfiable Checks
  bool isSafeAccessBoost(ObjectSizeOffsetVisitor &ObjSizeVis, Instruction *IndexInst, Value *Addr, Function *F) const;
  
  /// Helper to cleanup per-function state.
  struct FunctionStateRAII {
    AddressSanitizer *Pass;

    FunctionStateRAII(AddressSanitizer *Pass) : Pass(Pass) {
      assert(Pass->ProcessedAllocas.empty() &&
             "last pass forgot to clear cache");
      assert(!Pass->LocalDynamicShadow);
    }

    ~FunctionStateRAII() {
      Pass->LocalDynamicShadow = nullptr;
      Pass->ProcessedAllocas.clear();
    }
  };

  LLVMContext *C;
  Triple TargetTriple;
  int LongSize;
  bool CompileKernel;
  bool Recover;
  bool UseAfterScope;
  Type *IntptrTy;
  ShadowMapping Mapping;
  FunctionCallee AsanHandleNoReturnFunc;
  FunctionCallee AsanPtrCmpFunction, AsanPtrSubFunction;
  Constant *AsanShadowGlobal;

  // These arrays is indexed by AccessIsWrite, Experiment and log2(AccessSize).
  FunctionCallee AsanErrorCallback[2][2][kNumberOfAccessSizes];
  FunctionCallee AsanMemoryAccessCallback[2][2][kNumberOfAccessSizes];

  // These arrays is indexed by AccessIsWrite and Experiment.
  FunctionCallee AsanErrorCallbackSized[2][2];
  FunctionCallee AsanMemoryAccessCallbackSized[2][2];

  FunctionCallee AsanMemmove, AsanMemcpy, AsanMemset;
  Value *LocalDynamicShadow = nullptr;
  const GlobalsMetadata &GlobalsMD;
  DenseMap<const AllocaInst *, bool> ProcessedAllocas;
};

class AddressSanitizerLegacyPass : public FunctionPass {
public:
  static char ID;

  explicit AddressSanitizerLegacyPass(bool CompileKernel = false,
                                      bool Recover = false,
                                      bool UseAfterScope = false)
      : FunctionPass(ID), CompileKernel(CompileKernel), Recover(Recover),
        UseAfterScope(UseAfterScope) {
    initializeAddressSanitizerLegacyPassPass(*PassRegistry::getPassRegistry());
  }

  StringRef getPassName() const override {
    return "AddressSanitizerFunctionPass";
  }

  void getAnalysisUsage(AnalysisUsage &AU) const override {
    AU.addRequired<ASanGlobalsMetadataWrapperPass>();
    AU.addRequired<TargetLibraryInfoWrapperPass>();
    // ASAN-- Helper Wrappers
    AU.addRequired<DominatorTreeWrapperPass>();
    AU.addRequired<PostDominatorTreeWrapperPass>();
    AU.addRequired<AAResultsWrapperPass>();
    AU.addRequired<LoopInfoWrapperPass>();
    AU.addRequired<ScalarEvolutionWrapperPass>();
  }

  bool runOnFunction(Function &F) override {
    GlobalsMetadata &GlobalsMD =
        getAnalysis<ASanGlobalsMetadataWrapperPass>().getGlobalsMD();
    const TargetLibraryInfo *TLI =
        &getAnalysis<TargetLibraryInfoWrapperPass>().getTLI(F);
    AddressSanitizer ASan(*F.getParent(), &GlobalsMD, CompileKernel, Recover,
                          UseAfterScope);
    // ASAN-- Helper Wrappers
    AliasAnalysis *AA = &getAnalysis<AAResultsWrapperPass>().getAAResults();
    LoopInfo *LI = &getAnalysis<LoopInfoWrapperPass>().getLoopInfo();
    ScalarEvolution *SE = &getAnalysis<ScalarEvolutionWrapperPass>().getSE();
    return ASan.instrumentFunction(F, TLI, AA, LI, SE);
  }

private:
  bool CompileKernel;
  bool Recover;
  bool UseAfterScope;
};

class ModuleAddressSanitizer {
public:
  ModuleAddressSanitizer(Module &M, const GlobalsMetadata *GlobalsMD,
                         bool CompileKernel = false, bool Recover = false,
                         bool UseGlobalsGC = true, bool UseOdrIndicator = false)
      : GlobalsMD(*GlobalsMD),
        CompileKernel(ClEnableKasan.getNumOccurrences() > 0 ? ClEnableKasan
                                                            : CompileKernel),
        Recover(ClRecover.getNumOccurrences() > 0 ? ClRecover : Recover),
        UseGlobalsGC(UseGlobalsGC && ClUseGlobalsGC && !this->CompileKernel),
        // Enable aliases as they should have no downside with ODR indicators.
        UsePrivateAlias(UseOdrIndicator || ClUsePrivateAlias),
        UseOdrIndicator(UseOdrIndicator || ClUseOdrIndicator),
        // Not a typo: ClWithComdat is almost completely pointless without
        // ClUseGlobalsGC (because then it only works on modules without
        // globals, which are rare); it is a prerequisite for ClUseGlobalsGC;
        // and both suffer from gold PR19002 for which UseGlobalsGC constructor
        // argument is designed as workaround. Therefore, disable both
        // ClWithComdat and ClUseGlobalsGC unless the frontend says it's ok to
        // do globals-gc.
        UseCtorComdat(UseGlobalsGC && ClWithComdat && !this->CompileKernel) {
    C = &(M.getContext());
    int LongSize = M.getDataLayout().getPointerSizeInBits();
    IntptrTy = Type::getIntNTy(*C, LongSize);
    TargetTriple = Triple(M.getTargetTriple());
    Mapping = getShadowMapping(TargetTriple, LongSize, this->CompileKernel);
  }

  bool instrumentModule(Module &);

private:
  void initializeCallbacks(Module &M);

  bool InstrumentGlobals(IRBuilder<> &IRB, Module &M, bool *CtorComdat);
  void InstrumentGlobalsCOFF(IRBuilder<> &IRB, Module &M,
                             ArrayRef<GlobalVariable *> ExtendedGlobals,
                             ArrayRef<Constant *> MetadataInitializers);
  void InstrumentGlobalsELF(IRBuilder<> &IRB, Module &M,
                            ArrayRef<GlobalVariable *> ExtendedGlobals,
                            ArrayRef<Constant *> MetadataInitializers,
                            const std::string &UniqueModuleId);
  void InstrumentGlobalsMachO(IRBuilder<> &IRB, Module &M,
                              ArrayRef<GlobalVariable *> ExtendedGlobals,
                              ArrayRef<Constant *> MetadataInitializers);
  void
  InstrumentGlobalsWithMetadataArray(IRBuilder<> &IRB, Module &M,
                                     ArrayRef<GlobalVariable *> ExtendedGlobals,
                                     ArrayRef<Constant *> MetadataInitializers);

  GlobalVariable *CreateMetadataGlobal(Module &M, Constant *Initializer,
                                       StringRef OriginalName);
  void SetComdatForGlobalMetadata(GlobalVariable *G, GlobalVariable *Metadata,
                                  StringRef InternalSuffix);
  Instruction *CreateAsanModuleDtor(Module &M);

  const GlobalVariable *getExcludedAliasedGlobal(const GlobalAlias &GA) const;
  bool shouldInstrumentGlobal(GlobalVariable *G) const;
  bool ShouldUseMachOGlobalsSection() const;
  StringRef getGlobalMetadataSection() const;
  void poisonOneInitializer(Function &GlobalInit, GlobalValue *ModuleName);
  void createInitializerPoisonCalls(Module &M, GlobalValue *ModuleName);
  uint64_t getMinRedzoneSizeForGlobal() const {
    return getRedzoneSizeForScale(Mapping.Scale);
  }
  uint64_t getRedzoneSizeForGlobal(uint64_t SizeInBytes) const;
  int GetAsanVersion(const Module &M) const;

  const GlobalsMetadata &GlobalsMD;
  bool CompileKernel;
  bool Recover;
  bool UseGlobalsGC;
  bool UsePrivateAlias;
  bool UseOdrIndicator;
  bool UseCtorComdat;
  Type *IntptrTy;
  LLVMContext *C;
  Triple TargetTriple;
  ShadowMapping Mapping;
  FunctionCallee AsanPoisonGlobals;
  FunctionCallee AsanUnpoisonGlobals;
  FunctionCallee AsanRegisterGlobals;
  FunctionCallee AsanUnregisterGlobals;
  FunctionCallee AsanRegisterImageGlobals;
  FunctionCallee AsanUnregisterImageGlobals;
  FunctionCallee AsanRegisterElfGlobals;
  FunctionCallee AsanUnregisterElfGlobals;

  Function *AsanCtorFunction = nullptr;
  Function *AsanDtorFunction = nullptr;
};

class ModuleAddressSanitizerLegacyPass : public ModulePass {
public:
  static char ID;

  explicit ModuleAddressSanitizerLegacyPass(bool CompileKernel = false,
                                            bool Recover = false,
                                            bool UseGlobalGC = true,
                                            bool UseOdrIndicator = false)
      : ModulePass(ID), CompileKernel(CompileKernel), Recover(Recover),
        UseGlobalGC(UseGlobalGC), UseOdrIndicator(UseOdrIndicator) {
    initializeModuleAddressSanitizerLegacyPassPass(
        *PassRegistry::getPassRegistry());
  }

  StringRef getPassName() const override { return "ModuleAddressSanitizer"; }

  void getAnalysisUsage(AnalysisUsage &AU) const override {
    AU.addRequired<ASanGlobalsMetadataWrapperPass>();
  }

  bool runOnModule(Module &M) override {
    GlobalsMetadata &GlobalsMD =
        getAnalysis<ASanGlobalsMetadataWrapperPass>().getGlobalsMD();
    ModuleAddressSanitizer ASanModule(M, &GlobalsMD, CompileKernel, Recover,
                                      UseGlobalGC, UseOdrIndicator);
    return ASanModule.instrumentModule(M);
  }

private:
  bool CompileKernel;
  bool Recover;
  bool UseGlobalGC;
  bool UseOdrIndicator;
};

// Stack poisoning does not play well with exception handling.
// When an exception is thrown, we essentially bypass the code
// that unpoisones the stack. This is why the run-time library has
// to intercept __cxa_throw (as well as longjmp, etc) and unpoison the entire
// stack in the interceptor. This however does not work inside the
// actual function which catches the exception. Most likely because the
// compiler hoists the load of the shadow value somewhere too high.
// This causes asan to report a non-existing bug on 453.povray.
// It sounds like an LLVM bug.
struct FunctionStackPoisoner : public InstVisitor<FunctionStackPoisoner> {
  Function &F;
  AddressSanitizer &ASan;
  DIBuilder DIB;
  LLVMContext *C;
  Type *IntptrTy;
  Type *IntptrPtrTy;
  ShadowMapping Mapping;

  SmallVector<AllocaInst *, 16> AllocaVec;
  SmallVector<AllocaInst *, 16> StaticAllocasToMoveUp;
  SmallVector<Instruction *, 8> RetVec;
  unsigned StackAlignment;

  FunctionCallee AsanStackMallocFunc[kMaxAsanStackMallocSizeClass + 1],
      AsanStackFreeFunc[kMaxAsanStackMallocSizeClass + 1];
  FunctionCallee AsanSetShadowFunc[0x100] = {};
  FunctionCallee AsanPoisonStackMemoryFunc, AsanUnpoisonStackMemoryFunc;
  FunctionCallee AsanAllocaPoisonFunc, AsanAllocasUnpoisonFunc;

  // Stores a place and arguments of poisoning/unpoisoning call for alloca.
  struct AllocaPoisonCall {
    IntrinsicInst *InsBefore;
    AllocaInst *AI;
    uint64_t Size;
    bool DoPoison;
  };
  SmallVector<AllocaPoisonCall, 8> DynamicAllocaPoisonCallVec;
  SmallVector<AllocaPoisonCall, 8> StaticAllocaPoisonCallVec;
  bool HasUntracedLifetimeIntrinsic = false;

  SmallVector<AllocaInst *, 1> DynamicAllocaVec;
  SmallVector<IntrinsicInst *, 1> StackRestoreVec;
  AllocaInst *DynamicAllocaLayout = nullptr;
  IntrinsicInst *LocalEscapeCall = nullptr;

  bool HasInlineAsm = false;
  bool HasReturnsTwiceCall = false;

  FunctionStackPoisoner(Function &F, AddressSanitizer &ASan)
      : F(F), ASan(ASan), DIB(*F.getParent(), /*AllowUnresolved*/ false),
        C(ASan.C), IntptrTy(ASan.IntptrTy),
        IntptrPtrTy(PointerType::get(IntptrTy, 0)), Mapping(ASan.Mapping),
        StackAlignment(1 << Mapping.Scale) {}

  bool runOnFunction() {
    if (!ClStack) return false;

    if (ClRedzoneByvalArgs)
      copyArgsPassedByValToAllocas();

    // Collect alloca, ret, lifetime instructions etc.
    for (BasicBlock *BB : depth_first(&F.getEntryBlock())) visit(*BB);

    if (AllocaVec.empty() && DynamicAllocaVec.empty()) return false;

    initializeCallbacks(*F.getParent());

    if (HasUntracedLifetimeIntrinsic) {
      // If there are lifetime intrinsics which couldn't be traced back to an
      // alloca, we may not know exactly when a variable enters scope, and
      // therefore should "fail safe" by not poisoning them.
      StaticAllocaPoisonCallVec.clear();
      DynamicAllocaPoisonCallVec.clear();
    }

    processDynamicAllocas();
    processStaticAllocas();

    if (ClDebugStack) {
      LLVM_DEBUG(dbgs() << F);
    }
    return true;
  }

  // Arguments marked with the "byval" attribute are implicitly copied without
  // using an alloca instruction.  To produce redzones for those arguments, we
  // copy them a second time into memory allocated with an alloca instruction.
  void copyArgsPassedByValToAllocas();

  // Finds all Alloca instructions and puts
  // poisoned red zones around all of them.
  // Then unpoison everything back before the function returns.
  void processStaticAllocas();
  void processDynamicAllocas();

  void createDynamicAllocasInitStorage();

  // ----------------------- Visitors.
  /// Collect all Ret instructions, or the musttail call instruction if it
  /// precedes the return instruction.
  void visitReturnInst(ReturnInst &RI) {
    if (CallInst *CI = RI.getParent()->getTerminatingMustTailCall())
      RetVec.push_back(CI);
    else
      RetVec.push_back(&RI);
  }

  /// Collect all Resume instructions.
  void visitResumeInst(ResumeInst &RI) { RetVec.push_back(&RI); }

  /// Collect all CatchReturnInst instructions.
  void visitCleanupReturnInst(CleanupReturnInst &CRI) { RetVec.push_back(&CRI); }

  void unpoisonDynamicAllocasBeforeInst(Instruction *InstBefore,
                                        Value *SavedStack) {
    IRBuilder<> IRB(InstBefore);
    Value *DynamicAreaPtr = IRB.CreatePtrToInt(SavedStack, IntptrTy);
    // When we insert _asan_allocas_unpoison before @llvm.stackrestore, we
    // need to adjust extracted SP to compute the address of the most recent
    // alloca. We have a special @llvm.get.dynamic.area.offset intrinsic for
    // this purpose.
    if (!isa<ReturnInst>(InstBefore)) {
      Function *DynamicAreaOffsetFunc = Intrinsic::getDeclaration(
          InstBefore->getModule(), Intrinsic::get_dynamic_area_offset,
          {IntptrTy});

      Value *DynamicAreaOffset = IRB.CreateCall(DynamicAreaOffsetFunc, {});

      DynamicAreaPtr = IRB.CreateAdd(IRB.CreatePtrToInt(SavedStack, IntptrTy),
                                     DynamicAreaOffset);
    }

    IRB.CreateCall(
        AsanAllocasUnpoisonFunc,
        {IRB.CreateLoad(IntptrTy, DynamicAllocaLayout), DynamicAreaPtr});
  }

  // Unpoison dynamic allocas redzones.
  void unpoisonDynamicAllocas() {
    for (Instruction *Ret : RetVec)
      unpoisonDynamicAllocasBeforeInst(Ret, DynamicAllocaLayout);

    for (Instruction *StackRestoreInst : StackRestoreVec)
      unpoisonDynamicAllocasBeforeInst(StackRestoreInst,
                                       StackRestoreInst->getOperand(0));
  }

  // Deploy and poison redzones around dynamic alloca call. To do this, we
  // should replace this call with another one with changed parameters and
  // replace all its uses with new address, so
  //   addr = alloca type, old_size, align
  // is replaced by
  //   new_size = (old_size + additional_size) * sizeof(type)
  //   tmp = alloca i8, new_size, max(align, 32)
  //   addr = tmp + 32 (first 32 bytes are for the left redzone).
  // Additional_size is added to make new memory allocation contain not only
  // requested memory, but also left, partial and right redzones.
  void handleDynamicAllocaCall(AllocaInst *AI);

  /// Collect Alloca instructions we want (and can) handle.
  void visitAllocaInst(AllocaInst &AI) {
    if (!ASan.isInterestingAlloca(AI)) {
      if (AI.isStaticAlloca()) {
        // Skip over allocas that are present *before* the first instrumented
        // alloca, we don't want to move those around.
        if (AllocaVec.empty())
          return;

        StaticAllocasToMoveUp.push_back(&AI);
      }
      return;
    }

    StackAlignment = std::max(StackAlignment, AI.getAlignment());
    if (!AI.isStaticAlloca())
      DynamicAllocaVec.push_back(&AI);
    else
      AllocaVec.push_back(&AI);
  }

  /// Collect lifetime intrinsic calls to check for use-after-scope
  /// errors.
  void visitIntrinsicInst(IntrinsicInst &II) {
    Intrinsic::ID ID = II.getIntrinsicID();
    if (ID == Intrinsic::stackrestore) StackRestoreVec.push_back(&II);
    if (ID == Intrinsic::localescape) LocalEscapeCall = &II;
    if (!ASan.UseAfterScope)
      return;
    if (!II.isLifetimeStartOrEnd())
      return;
    // Found lifetime intrinsic, add ASan instrumentation if necessary.
    auto *Size = cast<ConstantInt>(II.getArgOperand(0));
    // If size argument is undefined, don't do anything.
    if (Size->isMinusOne()) return;
    // Check that size doesn't saturate uint64_t and can
    // be stored in IntptrTy.
    const uint64_t SizeValue = Size->getValue().getLimitedValue();
    if (SizeValue == ~0ULL ||
        !ConstantInt::isValueValidForType(IntptrTy, SizeValue))
      return;
    // Find alloca instruction that corresponds to llvm.lifetime argument.
    // Currently we can only handle lifetime markers pointing to the
    // beginning of the alloca.
    AllocaInst *AI = findAllocaForValue(II.getArgOperand(1), true);
    if (!AI) {
      HasUntracedLifetimeIntrinsic = true;
      return;
    }
    // We're interested only in allocas we can handle.
    if (!ASan.isInterestingAlloca(*AI))
      return;
    bool DoPoison = (ID == Intrinsic::lifetime_end);
    AllocaPoisonCall APC = {&II, AI, SizeValue, DoPoison};
    if (AI->isStaticAlloca())
      StaticAllocaPoisonCallVec.push_back(APC);
    else if (ClInstrumentDynamicAllocas)
      DynamicAllocaPoisonCallVec.push_back(APC);
  }

  void visitCallBase(CallBase &CB) {
    if (CallInst *CI = dyn_cast<CallInst>(&CB)) {
      HasInlineAsm |= CI->isInlineAsm() && &CB != ASan.LocalDynamicShadow;
      HasReturnsTwiceCall |= CI->canReturnTwice();
    }
  }

  // ---------------------- Helpers.
  void initializeCallbacks(Module &M);

  // Copies bytes from ShadowBytes into shadow memory for indexes where
  // ShadowMask is not zero. If ShadowMask[i] is zero, we assume that
  // ShadowBytes[i] is constantly zero and doesn't need to be overwritten.
  void copyToShadow(ArrayRef<uint8_t> ShadowMask, ArrayRef<uint8_t> ShadowBytes,
                    IRBuilder<> &IRB, Value *ShadowBase);
  void copyToShadow(ArrayRef<uint8_t> ShadowMask, ArrayRef<uint8_t> ShadowBytes,
                    size_t Begin, size_t End, IRBuilder<> &IRB,
                    Value *ShadowBase);
  void copyToShadowInline(ArrayRef<uint8_t> ShadowMask,
                          ArrayRef<uint8_t> ShadowBytes, size_t Begin,
                          size_t End, IRBuilder<> &IRB, Value *ShadowBase);

  void poisonAlloca(Value *V, uint64_t Size, IRBuilder<> &IRB, bool DoPoison);

  Value *createAllocaForLayout(IRBuilder<> &IRB, const ASanStackFrameLayout &L,
                               bool Dynamic);
  PHINode *createPHI(IRBuilder<> &IRB, Value *Cond, Value *ValueIfTrue,
                     Instruction *ThenTerm, Value *ValueIfFalse);
};

} // end anonymous namespace

void LocationMetadata::parse(MDNode *MDN) {
  assert(MDN->getNumOperands() == 3);
  MDString *DIFilename = cast<MDString>(MDN->getOperand(0));
  Filename = DIFilename->getString();
  LineNo = mdconst::extract<ConstantInt>(MDN->getOperand(1))->getLimitedValue();
  ColumnNo =
      mdconst::extract<ConstantInt>(MDN->getOperand(2))->getLimitedValue();
}

// FIXME: It would be cleaner to instead attach relevant metadata to the globals
// we want to sanitize instead and reading this metadata on each pass over a
// function instead of reading module level metadata at first.
GlobalsMetadata::GlobalsMetadata(Module &M) {
  NamedMDNode *Globals = M.getNamedMetadata("llvm.asan.globals");
  if (!Globals)
    return;
  for (auto MDN : Globals->operands()) {
    // Metadata node contains the global and the fields of "Entry".
    assert(MDN->getNumOperands() == 5);
    auto *V = mdconst::extract_or_null<Constant>(MDN->getOperand(0));
    // The optimizer may optimize away a global entirely.
    if (!V)
      continue;
    auto *StrippedV = V->stripPointerCasts();
    auto *GV = dyn_cast<GlobalVariable>(StrippedV);
    if (!GV)
      continue;
    // We can already have an entry for GV if it was merged with another
    // global.
    Entry &E = Entries[GV];
    if (auto *Loc = cast_or_null<MDNode>(MDN->getOperand(1)))
      E.SourceLoc.parse(Loc);
    if (auto *Name = cast_or_null<MDString>(MDN->getOperand(2)))
      E.Name = Name->getString();
    ConstantInt *IsDynInit = mdconst::extract<ConstantInt>(MDN->getOperand(3));
    E.IsDynInit |= IsDynInit->isOne();
    ConstantInt *IsExcluded =
        mdconst::extract<ConstantInt>(MDN->getOperand(4));
    E.IsExcluded |= IsExcluded->isOne();
  }
}

AnalysisKey ASanGlobalsMetadataAnalysis::Key;

GlobalsMetadata ASanGlobalsMetadataAnalysis::run(Module &M,
                                                 ModuleAnalysisManager &AM) {
  return GlobalsMetadata(M);
}

AddressSanitizerPass::AddressSanitizerPass(bool CompileKernel, bool Recover,
                                           bool UseAfterScope)
    : CompileKernel(CompileKernel), Recover(Recover),
      UseAfterScope(UseAfterScope) {}

PreservedAnalyses AddressSanitizerPass::run(Function &F,
                                            AnalysisManager<Function> &AM) {
  auto &MAMProxy = AM.getResult<ModuleAnalysisManagerFunctionProxy>(F);
  Module &M = *F.getParent();
  if (auto *R = MAMProxy.getCachedResult<ASanGlobalsMetadataAnalysis>(M)) {
    const TargetLibraryInfo *TLI = &AM.getResult<TargetLibraryAnalysis>(F);
    AddressSanitizer Sanitizer(M, R, CompileKernel, Recover, UseAfterScope);
    AliasAnalysis *AA = nullptr;
    LoopInfo *LI = nullptr;
    ScalarEvolution *SE = nullptr;
    if (Sanitizer.instrumentFunction(F, TLI, AA, LI, SE))
      return PreservedAnalyses::none();
    return PreservedAnalyses::all();
  }

  report_fatal_error(
      "The ASanGlobalsMetadataAnalysis is required to run before "
      "AddressSanitizer can run");
  return PreservedAnalyses::all();
}

ModuleAddressSanitizerPass::ModuleAddressSanitizerPass(bool CompileKernel,
                                                       bool Recover,
                                                       bool UseGlobalGC,
                                                       bool UseOdrIndicator)
    : CompileKernel(CompileKernel), Recover(Recover), UseGlobalGC(UseGlobalGC),
      UseOdrIndicator(UseOdrIndicator) {}

PreservedAnalyses ModuleAddressSanitizerPass::run(Module &M,
                                                  AnalysisManager<Module> &AM) {
  GlobalsMetadata &GlobalsMD = AM.getResult<ASanGlobalsMetadataAnalysis>(M);
  ModuleAddressSanitizer Sanitizer(M, &GlobalsMD, CompileKernel, Recover,
                                   UseGlobalGC, UseOdrIndicator);
  if (Sanitizer.instrumentModule(M))
    return PreservedAnalyses::none();
  return PreservedAnalyses::all();
}

INITIALIZE_PASS(ASanGlobalsMetadataWrapperPass, "asan-globals-md",
                "Read metadata to mark which globals should be instrumented "
                "when running ASan.",
                false, true)

char AddressSanitizerLegacyPass::ID = 0;

INITIALIZE_PASS_BEGIN(
    AddressSanitizerLegacyPass, "asan",
    "AddressSanitizer: detects use-after-free and out-of-bounds bugs.", false,
    false)
INITIALIZE_PASS_DEPENDENCY(ASanGlobalsMetadataWrapperPass)
INITIALIZE_PASS_DEPENDENCY(TargetLibraryInfoWrapperPass)
INITIALIZE_PASS_END(
    AddressSanitizerLegacyPass, "asan",
    "AddressSanitizer: detects use-after-free and out-of-bounds bugs.", false,
    false)

FunctionPass *llvm::createAddressSanitizerFunctionPass(bool CompileKernel,
                                                       bool Recover,
                                                       bool UseAfterScope) {
  assert(!CompileKernel || Recover);
  return new AddressSanitizerLegacyPass(CompileKernel, Recover, UseAfterScope);
}

char ModuleAddressSanitizerLegacyPass::ID = 0;

INITIALIZE_PASS(
    ModuleAddressSanitizerLegacyPass, "asan-module",
    "AddressSanitizer: detects use-after-free and out-of-bounds bugs."
    "ModulePass",
    false, false)

ModulePass *llvm::createModuleAddressSanitizerLegacyPassPass(
    bool CompileKernel, bool Recover, bool UseGlobalsGC, bool UseOdrIndicator) {
  assert(!CompileKernel || Recover);
  return new ModuleAddressSanitizerLegacyPass(CompileKernel, Recover,
                                              UseGlobalsGC, UseOdrIndicator);
}

static size_t TypeSizeToSizeIndex(uint32_t TypeSize) {
  size_t Res = countTrailingZeros(TypeSize / 8);
  assert(Res < kNumberOfAccessSizes);
  return Res;
}

/// Create a global describing a source location.
static GlobalVariable *createPrivateGlobalForSourceLoc(Module &M,
                                                       LocationMetadata MD) {
  Constant *LocData[] = {
      createPrivateGlobalForString(M, MD.Filename, true, kAsanGenPrefix),
      ConstantInt::get(Type::getInt32Ty(M.getContext()), MD.LineNo),
      ConstantInt::get(Type::getInt32Ty(M.getContext()), MD.ColumnNo),
  };
  auto LocStruct = ConstantStruct::getAnon(LocData);
  auto GV = new GlobalVariable(M, LocStruct->getType(), true,
                               GlobalValue::PrivateLinkage, LocStruct,
                               kAsanGenPrefix);
  GV->setUnnamedAddr(GlobalValue::UnnamedAddr::Global);
  return GV;
}

/// Check if \p G has been created by a trusted compiler pass.
static bool GlobalWasGeneratedByCompiler(GlobalVariable *G) {
  // Do not instrument @llvm.global_ctors, @llvm.used, etc.
  if (G->getName().startswith("llvm."))
    return true;

  // Do not instrument asan globals.
  if (G->getName().startswith(kAsanGenPrefix) ||
      G->getName().startswith(kSanCovGenPrefix) ||
      G->getName().startswith(kODRGenPrefix))
    return true;

  // Do not instrument gcov counter arrays.
  if (G->getName() == "__llvm_gcov_ctr")
    return true;

  return false;
}

Value *AddressSanitizer::memToShadow(Value *Shadow, IRBuilder<> &IRB) {
  // Shadow >> scale
  Shadow = IRB.CreateLShr(Shadow, Mapping.Scale);
  if (Mapping.Offset == 0) return Shadow;
  // (Shadow >> scale) | offset
  Value *ShadowBase;
  if (LocalDynamicShadow)
    ShadowBase = LocalDynamicShadow;
  else
    ShadowBase = ConstantInt::get(IntptrTy, Mapping.Offset);
  if (Mapping.OrShadowOffset)
    return IRB.CreateOr(Shadow, ShadowBase);
  else
    return IRB.CreateAdd(Shadow, ShadowBase);
}

// Instrument memset/memmove/memcpy
void AddressSanitizer::instrumentMemIntrinsic(MemIntrinsic *MI) {
  IRBuilder<> IRB(MI);
  if (isa<MemTransferInst>(MI)) {
    IRB.CreateCall(
        isa<MemMoveInst>(MI) ? AsanMemmove : AsanMemcpy,
        {IRB.CreatePointerCast(MI->getOperand(0), IRB.getInt8PtrTy()),
         IRB.CreatePointerCast(MI->getOperand(1), IRB.getInt8PtrTy()),
         IRB.CreateIntCast(MI->getOperand(2), IntptrTy, false)});
  } else if (isa<MemSetInst>(MI)) {
    IRB.CreateCall(
        AsanMemset,
        {IRB.CreatePointerCast(MI->getOperand(0), IRB.getInt8PtrTy()),
         IRB.CreateIntCast(MI->getOperand(1), IRB.getInt32Ty(), false),
         IRB.CreateIntCast(MI->getOperand(2), IntptrTy, false)});
  }
  MI->eraseFromParent();
}

/// Check if we want (and can) handle this alloca.
bool AddressSanitizer::isInterestingAlloca(const AllocaInst &AI) {
  auto PreviouslySeenAllocaInfo = ProcessedAllocas.find(&AI);

  if (PreviouslySeenAllocaInfo != ProcessedAllocas.end())
    return PreviouslySeenAllocaInfo->getSecond();

  bool IsInteresting =
      (AI.getAllocatedType()->isSized() &&
       // alloca() may be called with 0 size, ignore it.
       ((!AI.isStaticAlloca()) || getAllocaSizeInBytes(AI) > 0) &&
       // We are only interested in allocas not promotable to registers.
       // Promotable allocas are common under -O0.
       (!ClSkipPromotableAllocas || !isAllocaPromotable(&AI)) &&
       // inalloca allocas are not treated as static, and we don't want
       // dynamic alloca instrumentation for them as well.
       !AI.isUsedWithInAlloca() &&
       // swifterror allocas are register promoted by ISel
       !AI.isSwiftError());

  ProcessedAllocas[&AI] = IsInteresting;
  return IsInteresting;
}

bool AddressSanitizer::ignoreAccess(Value *Ptr) {
  // Do not instrument acesses from different address spaces; we cannot deal
  // with them.
  Type *PtrTy = cast<PointerType>(Ptr->getType()->getScalarType());
  if (PtrTy->getPointerAddressSpace() != 0)
    return true;

  // Ignore swifterror addresses.
  // swifterror memory addresses are mem2reg promoted by instruction
  // selection. As such they cannot have regular uses like an instrumentation
  // function and it makes no sense to track them as memory.
  if (Ptr->isSwiftError())
    return true;

  // Treat memory accesses to promotable allocas as non-interesting since they
  // will not cause memory violations. This greatly speeds up the instrumented
  // executable at -O0.
  if (auto AI = dyn_cast_or_null<AllocaInst>(Ptr))
    if (ClSkipPromotableAllocas && !isInterestingAlloca(*AI))
      return true;

  return false;
}

void AddressSanitizer::getInterestingMemoryOperands(
    Instruction *I, SmallVectorImpl<InterestingMemoryOperand> &Interesting) {
  // Skip memory accesses inserted by another instrumentation.
  if (I->hasMetadata("nosanitize"))
    return;

  // Do not instrument the load fetching the dynamic shadow address.
  if (LocalDynamicShadow == I)
    return;

  if (LoadInst *LI = dyn_cast<LoadInst>(I)) {
    if (!ClInstrumentReads || ignoreAccess(LI->getPointerOperand()))
      return;
    Interesting.emplace_back(I, LI->getPointerOperandIndex(), false,
                             LI->getType(), LI->getAlign());
  } else if (StoreInst *SI = dyn_cast<StoreInst>(I)) {
    if (!ClInstrumentWrites || ignoreAccess(SI->getPointerOperand()))
      return;
    Interesting.emplace_back(I, SI->getPointerOperandIndex(), true,
                             SI->getValueOperand()->getType(), SI->getAlign());
  } else if (AtomicRMWInst *RMW = dyn_cast<AtomicRMWInst>(I)) {
    if (!ClInstrumentAtomics || ignoreAccess(RMW->getPointerOperand()))
      return;
    Interesting.emplace_back(I, RMW->getPointerOperandIndex(), true,
                             RMW->getValOperand()->getType(), None);
  } else if (AtomicCmpXchgInst *XCHG = dyn_cast<AtomicCmpXchgInst>(I)) {
    if (!ClInstrumentAtomics || ignoreAccess(XCHG->getPointerOperand()))
      return;
    Interesting.emplace_back(I, XCHG->getPointerOperandIndex(), true,
                             XCHG->getCompareOperand()->getType(), None);
  } else if (auto CI = dyn_cast<CallInst>(I)) {
    auto *F = CI->getCalledFunction();
    if (F && (F->getName().startswith("llvm.masked.load.") ||
              F->getName().startswith("llvm.masked.store."))) {
      bool IsWrite = F->getName().startswith("llvm.masked.store.");
      // Masked store has an initial operand for the value.
      unsigned OpOffset = IsWrite ? 1 : 0;
      if (IsWrite ? !ClInstrumentWrites : !ClInstrumentReads)
        return;

      auto BasePtr = CI->getOperand(OpOffset);
      if (ignoreAccess(BasePtr))
        return;
      auto Ty = cast<PointerType>(BasePtr->getType())->getElementType();
      MaybeAlign Alignment = Align(1);
      // Otherwise no alignment guarantees. We probably got Undef.
      if (auto *Op = dyn_cast<ConstantInt>(CI->getOperand(1 + OpOffset)))
        Alignment = Op->getMaybeAlignValue();
      Value *Mask = CI->getOperand(2 + OpOffset);
      Interesting.emplace_back(I, OpOffset, IsWrite, Ty, Alignment, Mask);
    } else {
      for (unsigned ArgNo = 0; ArgNo < CI->getNumArgOperands(); ArgNo++) {
        if (!ClInstrumentByval || !CI->isByValArgument(ArgNo) ||
            ignoreAccess(CI->getArgOperand(ArgNo)))
          continue;
        Type *Ty = CI->getParamByValType(ArgNo);
        Interesting.emplace_back(I, ArgNo, false, Ty, Align(1));
      }
    }
  }
}

static bool isPointerOperand(Value *V) {
  return V->getType()->isPointerTy() || isa<PtrToIntInst>(V);
}

// This is a rough heuristic; it may cause both false positives and
// false negatives. The proper implementation requires cooperation with
// the frontend.
static bool isInterestingPointerComparison(Instruction *I) {
  if (ICmpInst *Cmp = dyn_cast<ICmpInst>(I)) {
    if (!Cmp->isRelational())
      return false;
  } else {
    return false;
  }
  return isPointerOperand(I->getOperand(0)) &&
         isPointerOperand(I->getOperand(1));
}

// This is a rough heuristic; it may cause both false positives and
// false negatives. The proper implementation requires cooperation with
// the frontend.
static bool isInterestingPointerSubtraction(Instruction *I) {
  if (BinaryOperator *BO = dyn_cast<BinaryOperator>(I)) {
    if (BO->getOpcode() != Instruction::Sub)
      return false;
  } else {
    return false;
  }
  return isPointerOperand(I->getOperand(0)) &&
         isPointerOperand(I->getOperand(1));
}

bool AddressSanitizer::GlobalIsLinkerInitialized(GlobalVariable *G) {
  // If a global variable does not have dynamic initialization we don't
  // have to instrument it.  However, if a global does not have initializer
  // at all, we assume it has dynamic initializer (in other TU).
  //
  // FIXME: Metadata should be attched directly to the global directly instead
  // of being added to llvm.asan.globals.
  return G->hasInitializer() && !GlobalsMD.get(G).IsDynInit;
}

void AddressSanitizer::instrumentPointerComparisonOrSubtraction(
    Instruction *I) {
  IRBuilder<> IRB(I);
  FunctionCallee F = isa<ICmpInst>(I) ? AsanPtrCmpFunction : AsanPtrSubFunction;
  Value *Param[2] = {I->getOperand(0), I->getOperand(1)};
  for (Value *&i : Param) {
    if (i->getType()->isPointerTy())
      i = IRB.CreatePointerCast(i, IntptrTy);
  }
  IRB.CreateCall(F, Param);
}

static void doInstrumentAddress(AddressSanitizer *Pass, Instruction *I,
                                Instruction *InsertBefore, Value *Addr,
                                MaybeAlign Alignment, unsigned Granularity,
                                uint32_t TypeSize, bool IsWrite,
                                Value *SizeArgument, bool UseCalls,
                                uint32_t Exp) {
  // Instrument a 1-, 2-, 4-, 8-, or 16- byte access with one check
  // if the data is properly aligned.
  if ((TypeSize == 8 || TypeSize == 16 || TypeSize == 32 || TypeSize == 64 ||
       TypeSize == 128) &&
      (!Alignment || *Alignment >= Granularity || *Alignment >= TypeSize / 8))
    return Pass->instrumentAddress(I, InsertBefore, Addr, TypeSize, IsWrite,
                                   nullptr, UseCalls, Exp);
  Pass->instrumentUnusualSizeOrAlignment(I, InsertBefore, Addr, TypeSize,
                                         IsWrite, nullptr, UseCalls, Exp);
}

static void instrumentMaskedLoadOrStore(AddressSanitizer *Pass,
                                        const DataLayout &DL, Type *IntptrTy,
                                        Value *Mask, Instruction *I,
                                        Value *Addr, MaybeAlign Alignment,
                                        unsigned Granularity, uint32_t TypeSize,
                                        bool IsWrite, Value *SizeArgument,
                                        bool UseCalls, uint32_t Exp) {
  auto *VTy = cast<FixedVectorType>(
      cast<PointerType>(Addr->getType())->getElementType());
  uint64_t ElemTypeSize = DL.getTypeStoreSizeInBits(VTy->getScalarType());
  unsigned Num = VTy->getNumElements();
  auto Zero = ConstantInt::get(IntptrTy, 0);
  for (unsigned Idx = 0; Idx < Num; ++Idx) {
    Value *InstrumentedAddress = nullptr;
    Instruction *InsertBefore = I;
    if (auto *Vector = dyn_cast<ConstantVector>(Mask)) {
      // dyn_cast as we might get UndefValue
      if (auto *Masked = dyn_cast<ConstantInt>(Vector->getOperand(Idx))) {
        if (Masked->isZero())
          // Mask is constant false, so no instrumentation needed.
          continue;
        // If we have a true or undef value, fall through to doInstrumentAddress
        // with InsertBefore == I
      }
    } else {
      IRBuilder<> IRB(I);
      Value *MaskElem = IRB.CreateExtractElement(Mask, Idx);
      Instruction *ThenTerm = SplitBlockAndInsertIfThen(MaskElem, I, false);
      InsertBefore = ThenTerm;
    }

    IRBuilder<> IRB(InsertBefore);
    InstrumentedAddress =
        IRB.CreateGEP(VTy, Addr, {Zero, ConstantInt::get(IntptrTy, Idx)});
    doInstrumentAddress(Pass, I, InsertBefore, InstrumentedAddress, Alignment,
                        Granularity, ElemTypeSize, IsWrite, SizeArgument,
                        UseCalls, Exp);
  }
}

static void instrumentMaskedLoadOrStoreLoop(AddressSanitizer *Pass,
                                        const DataLayout &DL, Type *IntptrTy,
                                        Value *Mask, Instruction *I, Instruction *PrevI,
                                        Value *Addr, MaybeAlign Alignment,
                                        unsigned Granularity, uint32_t TypeSize,
                                        bool IsWrite, Value *SizeArgument,
                                        bool UseCalls, uint32_t Exp) {
  auto *VTy = cast<FixedVectorType>(
      cast<PointerType>(Addr->getType())->getElementType());
  uint64_t ElemTypeSize = DL.getTypeStoreSizeInBits(VTy->getScalarType());
  unsigned Num = VTy->getNumElements();
  auto Zero = ConstantInt::get(IntptrTy, 0);
  for (unsigned Idx = 0; Idx < Num; ++Idx) {
    Value *InstrumentedAddress = nullptr;
    Instruction *InsertBefore = PrevI;
    if (auto *Vector = dyn_cast<ConstantVector>(Mask)) {
      // dyn_cast as we might get UndefValue
      if (auto *Masked = dyn_cast<ConstantInt>(Vector->getOperand(Idx))) {
        if (Masked->isZero())
          // Mask is constant false, so no instrumentation needed.
          continue;
        // If we have a true or undef value, fall through to doInstrumentAddress
        // with InsertBefore == I
      }
    } else {
      IRBuilder<> IRB(I);
      Value *MaskElem = IRB.CreateExtractElement(Mask, Idx);
      Instruction *ThenTerm = SplitBlockAndInsertIfThen(MaskElem, I, false);
      InsertBefore = ThenTerm;
    }

    IRBuilder<> IRB(InsertBefore);
    InstrumentedAddress =
        IRB.CreateGEP(VTy, Addr, {Zero, ConstantInt::get(IntptrTy, Idx)});
    doInstrumentAddress(Pass, I, InsertBefore, InstrumentedAddress, Alignment,
                        Granularity, ElemTypeSize, IsWrite, SizeArgument,
                        UseCalls, Exp);
  }
}

void AddressSanitizer::instrumentMop(ObjectSizeOffsetVisitor &ObjSizeVis,
                                     InterestingMemoryOperand &O, bool UseCalls,
                                     const DataLayout &DL) {
  Value *Addr = O.getPtr();
  Instruction *Insn = O.getInsn();

  // Optimization experiments.
  // The experiments can be used to evaluate potential optimizations that remove
  // instrumentation (assess false negatives). Instead of completely removing
  // some instrumentation, you set Exp to a non-zero value (mask of optimization
  // experiments that want to remove instrumentation of this instruction).
  // If Exp is non-zero, this pass will emit special calls into runtime
  // (e.g. __asan_report_exp_load1 instead of __asan_report_load1). These calls
  // make runtime terminate the program in a special way (with a different
  // exit status). Then you run the new compiler on a buggy corpus, collect
  // the special terminations (ideally, you don't see them at all -- no false
  // negatives) and make the decision on the optimization.
  uint32_t Exp = ClForceExperiment;

  if (ClOpt && ClOptGlobals) {
    // If initialization order checking is disabled, a simple access to a
    // dynamically initialized global is always valid.
    GlobalVariable *G = dyn_cast<GlobalVariable>(getUnderlyingObject(Addr));

    if (G && (!ClInitializers || GlobalIsLinkerInitialized(G))) {

      if (isSafeAccess(ObjSizeVis, Addr, O.TypeSize)) {

        NumOptimizedAccessesToGlobalVar++;
        return;
      }
      //ASAN--: Removing Unsatisfiable Checks
      if (isSafeAccessBoost(ObjSizeVis, Insn, Addr, Insn->getFunction())) {
        NumOptimizedAccessesToGlobalVar++;
        return;
      }
    }
  }

  if (ClOpt && ClOptStack) {

    if (isa<AllocaInst>(getUnderlyingObject(Addr))) {

      if (isSafeAccess(ObjSizeVis, Addr, O.TypeSize)) {
        
        NumOptimizedAccessesToStackVar++;
        return;
      }
      //ASAN--: Removing Unsatisfiable Checks
      if (isSafeAccessBoost(ObjSizeVis, Insn, Addr, Insn->getFunction())) {
        NumOptimizedAccessesToStackVar++;
        return;
      } 
    }
  }

  if (O.IsWrite)
    NumInstrumentedWrites++;
  else
    NumInstrumentedReads++;

  unsigned Granularity = 1 << Mapping.Scale;
  if (O.MaybeMask) {
    instrumentMaskedLoadOrStore(this, DL, IntptrTy, O.MaybeMask, O.getInsn(),
                                Addr, O.Alignment, Granularity, O.TypeSize,
                                O.IsWrite, nullptr, UseCalls, Exp);
  } else {
    doInstrumentAddress(this, O.getInsn(), O.getInsn(), Addr, O.Alignment,
                        Granularity, O.TypeSize, O.IsWrite, nullptr, UseCalls,
                        Exp);
  }
}

void AddressSanitizer::instrumentMopLoop(ObjectSizeOffsetVisitor &ObjSizeVis,
                                     InterestingMemoryOperand &O, Instruction *PrevI, bool UseCalls,
                                     const DataLayout &DL) {
  Value *Addr = O.getPtr();

  // Optimization experiments.
  // The experiments can be used to evaluate potential optimizations that remove
  // instrumentation (assess false negatives). Instead of completely removing
  // some instrumentation, you set Exp to a non-zero value (mask of optimization
  // experiments that want to remove instrumentation of this instruction).
  // If Exp is non-zero, this pass will emit special calls into runtime
  // (e.g. __asan_report_exp_load1 instead of __asan_report_load1). These calls
  // make runtime terminate the program in a special way (with a different
  // exit status). Then you run the new compiler on a buggy corpus, collect
  // the special terminations (ideally, you don't see them at all -- no false
  // negatives) and make the decision on the optimization.
  uint32_t Exp = ClForceExperiment;

  if (ClOpt && ClOptGlobals) {
    // If initialization order checking is disabled, a simple access to a
    // dynamically initialized global is always valid.
    GlobalVariable *G = dyn_cast<GlobalVariable>(getUnderlyingObject(Addr));
    if (G && (!ClInitializers || GlobalIsLinkerInitialized(G)) &&
        isSafeAccess(ObjSizeVis, Addr, O.TypeSize)) {
      NumOptimizedAccessesToGlobalVar++;
      return;
    }
  }

  if (ClOpt && ClOptStack) {
    // A direct inbounds access to a stack variable is always valid.
    if (isa<AllocaInst>(getUnderlyingObject(Addr)) &&
        isSafeAccess(ObjSizeVis, Addr, O.TypeSize)) {
      NumOptimizedAccessesToStackVar++;
      return;
    }
  }

  if (O.IsWrite)
    NumInstrumentedWrites++;
  else
    NumInstrumentedReads++;

  unsigned Granularity = 1 << Mapping.Scale;
  if (O.MaybeMask) {
    instrumentMaskedLoadOrStoreLoop(this, DL, IntptrTy, O.MaybeMask, O.getInsn(), PrevI,
                                Addr, O.Alignment, Granularity, O.TypeSize,
                                O.IsWrite, nullptr, UseCalls, Exp);
  } else {
    doInstrumentAddress(this, O.getInsn(), PrevI, Addr, O.Alignment,
                        Granularity, O.TypeSize, O.IsWrite, nullptr, UseCalls,
                        Exp);
  }
}

Instruction *AddressSanitizer::generateCrashCode(Instruction *InsertBefore,
                                                 Value *Addr, bool IsWrite,
                                                 size_t AccessSizeIndex,
                                                 Value *SizeArgument,
                                                 uint32_t Exp) {
  IRBuilder<> IRB(InsertBefore);
  Value *ExpVal = Exp == 0 ? nullptr : ConstantInt::get(IRB.getInt32Ty(), Exp);
  CallInst *Call = nullptr;
  if (SizeArgument) {
    if (Exp == 0)
      Call = IRB.CreateCall(AsanErrorCallbackSized[IsWrite][0],
                            {Addr, SizeArgument});
    else
      Call = IRB.CreateCall(AsanErrorCallbackSized[IsWrite][1],
                            {Addr, SizeArgument, ExpVal});
  } else {
    if (Exp == 0)
      Call =
          IRB.CreateCall(AsanErrorCallback[IsWrite][0][AccessSizeIndex], Addr);
    else
      Call = IRB.CreateCall(AsanErrorCallback[IsWrite][1][AccessSizeIndex],
                            {Addr, ExpVal});
  }

  Call->setCannotMerge();
  return Call;
}

Value *AddressSanitizer::createSlowPathCmp(IRBuilder<> &IRB, Value *AddrLong,
                                           Value *ShadowValue,
                                           uint32_t TypeSize) {
  size_t Granularity = static_cast<size_t>(1) << Mapping.Scale;
  // Addr & (Granularity - 1)
  Value *LastAccessedByte =
      IRB.CreateAnd(AddrLong, ConstantInt::get(IntptrTy, Granularity - 1));
  // (Addr & (Granularity - 1)) + size - 1
  if (TypeSize / 8 > 1)
    LastAccessedByte = IRB.CreateAdd(
        LastAccessedByte, ConstantInt::get(IntptrTy, TypeSize / 8 - 1));
  // (uint8_t) ((Addr & (Granularity-1)) + size - 1)
  LastAccessedByte =
      IRB.CreateIntCast(LastAccessedByte, ShadowValue->getType(), false);
  // ((uint8_t) ((Addr & (Granularity-1)) + size - 1)) >= ShadowValue
  return IRB.CreateICmpSGE(LastAccessedByte, ShadowValue);
}

void AddressSanitizer::instrumentAddress(Instruction *OrigIns,
                                         Instruction *InsertBefore, Value *Addr,
                                         uint32_t TypeSize, bool IsWrite,
                                         Value *SizeArgument, bool UseCalls,
                                         uint32_t Exp) {
  bool IsMyriad = TargetTriple.getVendor() == llvm::Triple::Myriad;

  IRBuilder<> IRB(InsertBefore);
  Value *AddrLong = IRB.CreatePointerCast(Addr, IntptrTy);
  size_t AccessSizeIndex = TypeSizeToSizeIndex(TypeSize);

  if (UseCalls) {
    if (Exp == 0)
      IRB.CreateCall(AsanMemoryAccessCallback[IsWrite][0][AccessSizeIndex],
                     AddrLong);
    else
      IRB.CreateCall(AsanMemoryAccessCallback[IsWrite][1][AccessSizeIndex],
                     {AddrLong, ConstantInt::get(IRB.getInt32Ty(), Exp)});
    return;
  }

  if (IsMyriad) {
    // Strip the cache bit and do range check.
    // AddrLong &= ~kMyriadCacheBitMask32
    AddrLong = IRB.CreateAnd(AddrLong, ~kMyriadCacheBitMask32);
    // Tag = AddrLong >> kMyriadTagShift
    Value *Tag = IRB.CreateLShr(AddrLong, kMyriadTagShift);
    // Tag == kMyriadDDRTag
    Value *TagCheck =
        IRB.CreateICmpEQ(Tag, ConstantInt::get(IntptrTy, kMyriadDDRTag));

    Instruction *TagCheckTerm =
        SplitBlockAndInsertIfThen(TagCheck, InsertBefore, false,
                                  MDBuilder(*C).createBranchWeights(1, 100000));
    assert(cast<BranchInst>(TagCheckTerm)->isUnconditional());
    IRB.SetInsertPoint(TagCheckTerm);
    InsertBefore = TagCheckTerm;
  }

  Type *ShadowTy =
      IntegerType::get(*C, std::max(8U, TypeSize >> Mapping.Scale));
  Type *ShadowPtrTy = PointerType::get(ShadowTy, 0);
  Value *ShadowPtr = memToShadow(AddrLong, IRB);
  Value *CmpVal = Constant::getNullValue(ShadowTy);
  Value *ShadowValue =
      IRB.CreateLoad(ShadowTy, IRB.CreateIntToPtr(ShadowPtr, ShadowPtrTy));

  Value *Cmp = IRB.CreateICmpNE(ShadowValue, CmpVal);
  size_t Granularity = 1ULL << Mapping.Scale;
  Instruction *CrashTerm = nullptr;

  if (ClAlwaysSlowPath || (TypeSize < 8 * Granularity)) {
    // We use branch weights for the slow path check, to indicate that the slow
    // path is rarely taken. This seems to be the case for SPEC benchmarks.
    Instruction *CheckTerm = SplitBlockAndInsertIfThen(
        Cmp, InsertBefore, false, MDBuilder(*C).createBranchWeights(1, 100000));
    assert(cast<BranchInst>(CheckTerm)->isUnconditional());
    BasicBlock *NextBB = CheckTerm->getSuccessor(0);
    IRB.SetInsertPoint(CheckTerm);
    Value *Cmp2 = createSlowPathCmp(IRB, AddrLong, ShadowValue, TypeSize);
    if (Recover) {
      CrashTerm = SplitBlockAndInsertIfThen(Cmp2, CheckTerm, false);
    } else {
      BasicBlock *CrashBlock =
        BasicBlock::Create(*C, "", NextBB->getParent(), NextBB);
      CrashTerm = new UnreachableInst(*C, CrashBlock);
      BranchInst *NewTerm = BranchInst::Create(CrashBlock, NextBB, Cmp2);
      ReplaceInstWithInst(CheckTerm, NewTerm);
    }
  } else {
    CrashTerm = SplitBlockAndInsertIfThen(Cmp, InsertBefore, !Recover);
  }

  Instruction *Crash = generateCrashCode(CrashTerm, AddrLong, IsWrite,
                                         AccessSizeIndex, SizeArgument, Exp);
  Crash->setDebugLoc(OrigIns->getDebugLoc());
}

// Instrument unusual size or unusual alignment.
// We can not do it with a single check, so we do 1-byte check for the first
// and the last bytes. We call __asan_report_*_n(addr, real_size) to be able
// to report the actual access size.
void AddressSanitizer::instrumentUnusualSizeOrAlignment(
    Instruction *I, Instruction *InsertBefore, Value *Addr, uint32_t TypeSize,
    bool IsWrite, Value *SizeArgument, bool UseCalls, uint32_t Exp) {
  IRBuilder<> IRB(InsertBefore);
  Value *Size = ConstantInt::get(IntptrTy, TypeSize / 8);
  Value *AddrLong = IRB.CreatePointerCast(Addr, IntptrTy);
  if (UseCalls) {
    if (Exp == 0)
      IRB.CreateCall(AsanMemoryAccessCallbackSized[IsWrite][0],
                     {AddrLong, Size});
    else
      IRB.CreateCall(AsanMemoryAccessCallbackSized[IsWrite][1],
                     {AddrLong, Size, ConstantInt::get(IRB.getInt32Ty(), Exp)});
  } else {
    Value *LastByte = IRB.CreateIntToPtr(
        IRB.CreateAdd(AddrLong, ConstantInt::get(IntptrTy, TypeSize / 8 - 1)),
        Addr->getType());
    instrumentAddress(I, InsertBefore, Addr, 8, IsWrite, Size, false, Exp);
    instrumentAddress(I, InsertBefore, LastByte, 8, IsWrite, Size, false, Exp);
  }
}

void ModuleAddressSanitizer::poisonOneInitializer(Function &GlobalInit,
                                                  GlobalValue *ModuleName) {
  // Set up the arguments to our poison/unpoison functions.
  IRBuilder<> IRB(&GlobalInit.front(),
                  GlobalInit.front().getFirstInsertionPt());

  // Add a call to poison all external globals before the given function starts.
  Value *ModuleNameAddr = ConstantExpr::getPointerCast(ModuleName, IntptrTy);
  IRB.CreateCall(AsanPoisonGlobals, ModuleNameAddr);

  // Add calls to unpoison all globals before each return instruction.
  for (auto &BB : GlobalInit.getBasicBlockList())
    if (ReturnInst *RI = dyn_cast<ReturnInst>(BB.getTerminator()))
      CallInst::Create(AsanUnpoisonGlobals, "", RI);
}

void ModuleAddressSanitizer::createInitializerPoisonCalls(
    Module &M, GlobalValue *ModuleName) {
  GlobalVariable *GV = M.getGlobalVariable("llvm.global_ctors");
  if (!GV)
    return;

  ConstantArray *CA = dyn_cast<ConstantArray>(GV->getInitializer());
  if (!CA)
    return;

  for (Use &OP : CA->operands()) {
    if (isa<ConstantAggregateZero>(OP)) continue;
    ConstantStruct *CS = cast<ConstantStruct>(OP);

    // Must have a function or null ptr.
    if (Function *F = dyn_cast<Function>(CS->getOperand(1))) {
      if (F->getName() == kAsanModuleCtorName) continue;
      auto *Priority = cast<ConstantInt>(CS->getOperand(0));
      // Don't instrument CTORs that will run before asan.module_ctor.
      if (Priority->getLimitedValue() <= GetCtorAndDtorPriority(TargetTriple))
        continue;
      poisonOneInitializer(*F, ModuleName);
    }
  }
}

const GlobalVariable *
ModuleAddressSanitizer::getExcludedAliasedGlobal(const GlobalAlias &GA) const {
  // In case this function should be expanded to include rules that do not just
  // apply when CompileKernel is true, either guard all existing rules with an
  // 'if (CompileKernel) { ... }' or be absolutely sure that all these rules
  // should also apply to user space.
  assert(CompileKernel && "Only expecting to be called when compiling kernel");

  const Constant *C = GA.getAliasee();

  // When compiling the kernel, globals that are aliased by symbols prefixed
  // by "__" are special and cannot be padded with a redzone.
  if (GA.getName().startswith("__"))
    return dyn_cast<GlobalVariable>(C->stripPointerCastsAndAliases());

  return nullptr;
}

bool ModuleAddressSanitizer::shouldInstrumentGlobal(GlobalVariable *G) const {
  Type *Ty = G->getValueType();
  LLVM_DEBUG(dbgs() << "GLOBAL: " << *G << "\n");

  // FIXME: Metadata should be attched directly to the global directly instead
  // of being added to llvm.asan.globals.
  if (GlobalsMD.get(G).IsExcluded) return false;
  if (!Ty->isSized()) return false;
  if (!G->hasInitializer()) return false;
  // Only instrument globals of default address spaces
  if (G->getAddressSpace()) return false;
  if (GlobalWasGeneratedByCompiler(G)) return false; // Our own globals.
  // Two problems with thread-locals:
  //   - The address of the main thread's copy can't be computed at link-time.
  //   - Need to poison all copies, not just the main thread's one.
  if (G->isThreadLocal()) return false;
  // For now, just ignore this Global if the alignment is large.
  if (G->getAlignment() > getMinRedzoneSizeForGlobal()) return false;

  // For non-COFF targets, only instrument globals known to be defined by this
  // TU.
  // FIXME: We can instrument comdat globals on ELF if we are using the
  // GC-friendly metadata scheme.
  if (!TargetTriple.isOSBinFormatCOFF()) {
    if (!G->hasExactDefinition() || G->hasComdat())
      return false;
  } else {
    // On COFF, don't instrument non-ODR linkages.
    if (G->isInterposable())
      return false;
  }

  // If a comdat is present, it must have a selection kind that implies ODR
  // semantics: no duplicates, any, or exact match.
  if (Comdat *C = G->getComdat()) {
    switch (C->getSelectionKind()) {
    case Comdat::Any:
    case Comdat::ExactMatch:
    case Comdat::NoDuplicates:
      break;
    case Comdat::Largest:
    case Comdat::SameSize:
      return false;
    }
  }

  if (G->hasSection()) {
    // The kernel uses explicit sections for mostly special global variables
    // that we should not instrument. E.g. the kernel may rely on their layout
    // without redzones, or remove them at link time ("discard.*"), etc.
    if (CompileKernel)
      return false;

    StringRef Section = G->getSection();

    // Globals from llvm.metadata aren't emitted, do not instrument them.
    if (Section == "llvm.metadata") return false;
    // Do not instrument globals from special LLVM sections.
    if (Section.find("__llvm") != StringRef::npos || Section.find("__LLVM") != StringRef::npos) return false;

    // Do not instrument function pointers to initialization and termination
    // routines: dynamic linker will not properly handle redzones.
    if (Section.startswith(".preinit_array") ||
        Section.startswith(".init_array") ||
        Section.startswith(".fini_array")) {
      return false;
    }

    // Do not instrument user-defined sections (with names resembling
    // valid C identifiers)
    if (TargetTriple.isOSBinFormatELF()) {
      if (llvm::all_of(Section,
                       [](char c) { return llvm::isAlnum(c) || c == '_'; }))
        return false;
    }

    // On COFF, if the section name contains '$', it is highly likely that the
    // user is using section sorting to create an array of globals similar to
    // the way initialization callbacks are registered in .init_array and
    // .CRT$XCU. The ATL also registers things in .ATL$__[azm]. Adding redzones
    // to such globals is counterproductive, because the intent is that they
    // will form an array, and out-of-bounds accesses are expected.
    // See https://github.com/google/sanitizers/issues/305
    // and http://msdn.microsoft.com/en-US/en-en/library/bb918180(v=vs.120).aspx
    if (TargetTriple.isOSBinFormatCOFF() && Section.contains('$')) {
      LLVM_DEBUG(dbgs() << "Ignoring global in sorted section (contains '$'): "
                        << *G << "\n");
      return false;
    }

    if (TargetTriple.isOSBinFormatMachO()) {
      StringRef ParsedSegment, ParsedSection;
      unsigned TAA = 0, StubSize = 0;
      bool TAAParsed;
      std::string ErrorCode = MCSectionMachO::ParseSectionSpecifier(
          Section, ParsedSegment, ParsedSection, TAA, TAAParsed, StubSize);
      assert(ErrorCode.empty() && "Invalid section specifier.");

      // Ignore the globals from the __OBJC section. The ObjC runtime assumes
      // those conform to /usr/lib/objc/runtime.h, so we can't add redzones to
      // them.
      if (ParsedSegment == "__OBJC" ||
          (ParsedSegment == "__DATA" && ParsedSection.startswith("__objc_"))) {
        LLVM_DEBUG(dbgs() << "Ignoring ObjC runtime global: " << *G << "\n");
        return false;
      }
      // See https://github.com/google/sanitizers/issues/32
      // Constant CFString instances are compiled in the following way:
      //  -- the string buffer is emitted into
      //     __TEXT,__cstring,cstring_literals
      //  -- the constant NSConstantString structure referencing that buffer
      //     is placed into __DATA,__cfstring
      // Therefore there's no point in placing redzones into __DATA,__cfstring.
      // Moreover, it causes the linker to crash on OS X 10.7
      if (ParsedSegment == "__DATA" && ParsedSection == "__cfstring") {
        LLVM_DEBUG(dbgs() << "Ignoring CFString: " << *G << "\n");
        return false;
      }
      // The linker merges the contents of cstring_literals and removes the
      // trailing zeroes.
      if (ParsedSegment == "__TEXT" && (TAA & MachO::S_CSTRING_LITERALS)) {
        LLVM_DEBUG(dbgs() << "Ignoring a cstring literal: " << *G << "\n");
        return false;
      }
    }
  }

  if (CompileKernel) {
    // Globals that prefixed by "__" are special and cannot be padded with a
    // redzone.
    if (G->getName().startswith("__"))
      return false;
  }

  return true;
}

// On Mach-O platforms, we emit global metadata in a separate section of the
// binary in order to allow the linker to properly dead strip. This is only
// supported on recent versions of ld64.
bool ModuleAddressSanitizer::ShouldUseMachOGlobalsSection() const {
  if (!TargetTriple.isOSBinFormatMachO())
    return false;

  if (TargetTriple.isMacOSX() && !TargetTriple.isMacOSXVersionLT(10, 11))
    return true;
  if (TargetTriple.isiOS() /* or tvOS */ && !TargetTriple.isOSVersionLT(9))
    return true;
  if (TargetTriple.isWatchOS() && !TargetTriple.isOSVersionLT(2))
    return true;

  return false;
}

StringRef ModuleAddressSanitizer::getGlobalMetadataSection() const {
  switch (TargetTriple.getObjectFormat()) {
  case Triple::COFF:  return ".ASAN$GL";
  case Triple::ELF:   return "asan_globals";
  case Triple::MachO: return "__DATA,__asan_globals,regular";
  case Triple::Wasm:
  case Triple::GOFF:
  case Triple::XCOFF:
    report_fatal_error(
        "ModuleAddressSanitizer not implemented for object file format");
  case Triple::UnknownObjectFormat:
    break;
  }
  llvm_unreachable("unsupported object format");
}

void ModuleAddressSanitizer::initializeCallbacks(Module &M) {
  IRBuilder<> IRB(*C);

  // Declare our poisoning and unpoisoning functions.
  AsanPoisonGlobals =
      M.getOrInsertFunction(kAsanPoisonGlobalsName, IRB.getVoidTy(), IntptrTy);
  AsanUnpoisonGlobals =
      M.getOrInsertFunction(kAsanUnpoisonGlobalsName, IRB.getVoidTy());

  // Declare functions that register/unregister globals.
  AsanRegisterGlobals = M.getOrInsertFunction(
      kAsanRegisterGlobalsName, IRB.getVoidTy(), IntptrTy, IntptrTy);
  AsanUnregisterGlobals = M.getOrInsertFunction(
      kAsanUnregisterGlobalsName, IRB.getVoidTy(), IntptrTy, IntptrTy);

  // Declare the functions that find globals in a shared object and then invoke
  // the (un)register function on them.
  AsanRegisterImageGlobals = M.getOrInsertFunction(
      kAsanRegisterImageGlobalsName, IRB.getVoidTy(), IntptrTy);
  AsanUnregisterImageGlobals = M.getOrInsertFunction(
      kAsanUnregisterImageGlobalsName, IRB.getVoidTy(), IntptrTy);

  AsanRegisterElfGlobals =
      M.getOrInsertFunction(kAsanRegisterElfGlobalsName, IRB.getVoidTy(),
                            IntptrTy, IntptrTy, IntptrTy);
  AsanUnregisterElfGlobals =
      M.getOrInsertFunction(kAsanUnregisterElfGlobalsName, IRB.getVoidTy(),
                            IntptrTy, IntptrTy, IntptrTy);
}

// Put the metadata and the instrumented global in the same group. This ensures
// that the metadata is discarded if the instrumented global is discarded.
void ModuleAddressSanitizer::SetComdatForGlobalMetadata(
    GlobalVariable *G, GlobalVariable *Metadata, StringRef InternalSuffix) {
  Module &M = *G->getParent();
  Comdat *C = G->getComdat();
  if (!C) {
    if (!G->hasName()) {
      // If G is unnamed, it must be internal. Give it an artificial name
      // so we can put it in a comdat.
      assert(G->hasLocalLinkage());
      G->setName(Twine(kAsanGenPrefix) + "_anon_global");
    }

    if (!InternalSuffix.empty() && G->hasLocalLinkage()) {
      std::string Name = std::string(G->getName());
      Name += InternalSuffix;
      C = M.getOrInsertComdat(Name);
    } else {
      C = M.getOrInsertComdat(G->getName());
    }

    // Make this IMAGE_COMDAT_SELECT_NODUPLICATES on COFF. Also upgrade private
    // linkage to internal linkage so that a symbol table entry is emitted. This
    // is necessary in order to create the comdat group.
    if (TargetTriple.isOSBinFormatCOFF()) {
      C->setSelectionKind(Comdat::NoDuplicates);
      if (G->hasPrivateLinkage())
        G->setLinkage(GlobalValue::InternalLinkage);
    }
    G->setComdat(C);
  }

  assert(G->hasComdat());
  Metadata->setComdat(G->getComdat());
}

// Create a separate metadata global and put it in the appropriate ASan
// global registration section.
GlobalVariable *
ModuleAddressSanitizer::CreateMetadataGlobal(Module &M, Constant *Initializer,
                                             StringRef OriginalName) {
  auto Linkage = TargetTriple.isOSBinFormatMachO()
                     ? GlobalVariable::InternalLinkage
                     : GlobalVariable::PrivateLinkage;
  GlobalVariable *Metadata = new GlobalVariable(
      M, Initializer->getType(), false, Linkage, Initializer,
      Twine("__asan_global_") + GlobalValue::dropLLVMManglingEscape(OriginalName));
  Metadata->setSection(getGlobalMetadataSection());
  return Metadata;
}

Instruction *ModuleAddressSanitizer::CreateAsanModuleDtor(Module &M) {
  AsanDtorFunction =
      Function::Create(FunctionType::get(Type::getVoidTy(*C), false),
                       GlobalValue::InternalLinkage, kAsanModuleDtorName, &M);
  BasicBlock *AsanDtorBB = BasicBlock::Create(*C, "", AsanDtorFunction);

  return ReturnInst::Create(*C, AsanDtorBB);
}

void ModuleAddressSanitizer::InstrumentGlobalsCOFF(
    IRBuilder<> &IRB, Module &M, ArrayRef<GlobalVariable *> ExtendedGlobals,
    ArrayRef<Constant *> MetadataInitializers) {
  assert(ExtendedGlobals.size() == MetadataInitializers.size());
  auto &DL = M.getDataLayout();

  SmallVector<GlobalValue *, 16> MetadataGlobals(ExtendedGlobals.size());
  for (size_t i = 0; i < ExtendedGlobals.size(); i++) {
    Constant *Initializer = MetadataInitializers[i];
    GlobalVariable *G = ExtendedGlobals[i];
    GlobalVariable *Metadata =
        CreateMetadataGlobal(M, Initializer, G->getName());
    MDNode *MD = MDNode::get(M.getContext(), ValueAsMetadata::get(G));
    Metadata->setMetadata(LLVMContext::MD_associated, MD);
    MetadataGlobals[i] = Metadata;

    // The MSVC linker always inserts padding when linking incrementally. We
    // cope with that by aligning each struct to its size, which must be a power
    // of two.
    unsigned SizeOfGlobalStruct = DL.getTypeAllocSize(Initializer->getType());
    assert(isPowerOf2_32(SizeOfGlobalStruct) &&
           "global metadata will not be padded appropriately");
    Metadata->setAlignment(assumeAligned(SizeOfGlobalStruct));

    SetComdatForGlobalMetadata(G, Metadata, "");
  }

  // Update llvm.compiler.used, adding the new metadata globals. This is
  // needed so that during LTO these variables stay alive.
  if (!MetadataGlobals.empty())
    appendToCompilerUsed(M, MetadataGlobals);
}

void ModuleAddressSanitizer::InstrumentGlobalsELF(
    IRBuilder<> &IRB, Module &M, ArrayRef<GlobalVariable *> ExtendedGlobals,
    ArrayRef<Constant *> MetadataInitializers,
    const std::string &UniqueModuleId) {
  assert(ExtendedGlobals.size() == MetadataInitializers.size());

  SmallVector<GlobalValue *, 16> MetadataGlobals(ExtendedGlobals.size());
  for (size_t i = 0; i < ExtendedGlobals.size(); i++) {
    GlobalVariable *G = ExtendedGlobals[i];
    GlobalVariable *Metadata =
        CreateMetadataGlobal(M, MetadataInitializers[i], G->getName());
    MDNode *MD = MDNode::get(M.getContext(), ValueAsMetadata::get(G));
    Metadata->setMetadata(LLVMContext::MD_associated, MD);
    MetadataGlobals[i] = Metadata;

    SetComdatForGlobalMetadata(G, Metadata, UniqueModuleId);
  }

  // Update llvm.compiler.used, adding the new metadata globals. This is
  // needed so that during LTO these variables stay alive.
  if (!MetadataGlobals.empty())
    appendToCompilerUsed(M, MetadataGlobals);

  // RegisteredFlag serves two purposes. First, we can pass it to dladdr()
  // to look up the loaded image that contains it. Second, we can store in it
  // whether registration has already occurred, to prevent duplicate
  // registration.
  //
  // Common linkage ensures that there is only one global per shared library.
  GlobalVariable *RegisteredFlag = new GlobalVariable(
      M, IntptrTy, false, GlobalVariable::CommonLinkage,
      ConstantInt::get(IntptrTy, 0), kAsanGlobalsRegisteredFlagName);
  RegisteredFlag->setVisibility(GlobalVariable::HiddenVisibility);

  // Create start and stop symbols.
  GlobalVariable *StartELFMetadata = new GlobalVariable(
      M, IntptrTy, false, GlobalVariable::ExternalWeakLinkage, nullptr,
      "__start_" + getGlobalMetadataSection());
  StartELFMetadata->setVisibility(GlobalVariable::HiddenVisibility);
  GlobalVariable *StopELFMetadata = new GlobalVariable(
      M, IntptrTy, false, GlobalVariable::ExternalWeakLinkage, nullptr,
      "__stop_" + getGlobalMetadataSection());
  StopELFMetadata->setVisibility(GlobalVariable::HiddenVisibility);

  // Create a call to register the globals with the runtime.
  IRB.CreateCall(AsanRegisterElfGlobals,
                 {IRB.CreatePointerCast(RegisteredFlag, IntptrTy),
                  IRB.CreatePointerCast(StartELFMetadata, IntptrTy),
                  IRB.CreatePointerCast(StopELFMetadata, IntptrTy)});

  // We also need to unregister globals at the end, e.g., when a shared library
  // gets closed.
  IRBuilder<> IRB_Dtor(CreateAsanModuleDtor(M));
  IRB_Dtor.CreateCall(AsanUnregisterElfGlobals,
                      {IRB.CreatePointerCast(RegisteredFlag, IntptrTy),
                       IRB.CreatePointerCast(StartELFMetadata, IntptrTy),
                       IRB.CreatePointerCast(StopELFMetadata, IntptrTy)});
}

void ModuleAddressSanitizer::InstrumentGlobalsMachO(
    IRBuilder<> &IRB, Module &M, ArrayRef<GlobalVariable *> ExtendedGlobals,
    ArrayRef<Constant *> MetadataInitializers) {
  assert(ExtendedGlobals.size() == MetadataInitializers.size());

  // On recent Mach-O platforms, use a structure which binds the liveness of
  // the global variable to the metadata struct. Keep the list of "Liveness" GV
  // created to be added to llvm.compiler.used
  StructType *LivenessTy = StructType::get(IntptrTy, IntptrTy);
  SmallVector<GlobalValue *, 16> LivenessGlobals(ExtendedGlobals.size());

  for (size_t i = 0; i < ExtendedGlobals.size(); i++) {
    Constant *Initializer = MetadataInitializers[i];
    GlobalVariable *G = ExtendedGlobals[i];
    GlobalVariable *Metadata =
        CreateMetadataGlobal(M, Initializer, G->getName());

    // On recent Mach-O platforms, we emit the global metadata in a way that
    // allows the linker to properly strip dead globals.
    auto LivenessBinder =
        ConstantStruct::get(LivenessTy, Initializer->getAggregateElement(0u),
                            ConstantExpr::getPointerCast(Metadata, IntptrTy));
    GlobalVariable *Liveness = new GlobalVariable(
        M, LivenessTy, false, GlobalVariable::InternalLinkage, LivenessBinder,
        Twine("__asan_binder_") + G->getName());
    Liveness->setSection("__DATA,__asan_liveness,regular,live_support");
    LivenessGlobals[i] = Liveness;
  }

  // Update llvm.compiler.used, adding the new liveness globals. This is
  // needed so that during LTO these variables stay alive. The alternative
  // would be to have the linker handling the LTO symbols, but libLTO
  // current API does not expose access to the section for each symbol.
  if (!LivenessGlobals.empty())
    appendToCompilerUsed(M, LivenessGlobals);

  // RegisteredFlag serves two purposes. First, we can pass it to dladdr()
  // to look up the loaded image that contains it. Second, we can store in it
  // whether registration has already occurred, to prevent duplicate
  // registration.
  //
  // common linkage ensures that there is only one global per shared library.
  GlobalVariable *RegisteredFlag = new GlobalVariable(
      M, IntptrTy, false, GlobalVariable::CommonLinkage,
      ConstantInt::get(IntptrTy, 0), kAsanGlobalsRegisteredFlagName);
  RegisteredFlag->setVisibility(GlobalVariable::HiddenVisibility);

  IRB.CreateCall(AsanRegisterImageGlobals,
                 {IRB.CreatePointerCast(RegisteredFlag, IntptrTy)});

  // We also need to unregister globals at the end, e.g., when a shared library
  // gets closed.
  IRBuilder<> IRB_Dtor(CreateAsanModuleDtor(M));
  IRB_Dtor.CreateCall(AsanUnregisterImageGlobals,
                      {IRB.CreatePointerCast(RegisteredFlag, IntptrTy)});
}

void ModuleAddressSanitizer::InstrumentGlobalsWithMetadataArray(
    IRBuilder<> &IRB, Module &M, ArrayRef<GlobalVariable *> ExtendedGlobals,
    ArrayRef<Constant *> MetadataInitializers) {
  assert(ExtendedGlobals.size() == MetadataInitializers.size());
  unsigned N = ExtendedGlobals.size();
  assert(N > 0);

  // On platforms that don't have a custom metadata section, we emit an array
  // of global metadata structures.
  ArrayType *ArrayOfGlobalStructTy =
      ArrayType::get(MetadataInitializers[0]->getType(), N);
  auto AllGlobals = new GlobalVariable(
      M, ArrayOfGlobalStructTy, false, GlobalVariable::InternalLinkage,
      ConstantArray::get(ArrayOfGlobalStructTy, MetadataInitializers), "");
  if (Mapping.Scale > 3)
    AllGlobals->setAlignment(Align(1ULL << Mapping.Scale));

  IRB.CreateCall(AsanRegisterGlobals,
                 {IRB.CreatePointerCast(AllGlobals, IntptrTy),
                  ConstantInt::get(IntptrTy, N)});

  // We also need to unregister globals at the end, e.g., when a shared library
  // gets closed.
  IRBuilder<> IRB_Dtor(CreateAsanModuleDtor(M));
  IRB_Dtor.CreateCall(AsanUnregisterGlobals,
                      {IRB.CreatePointerCast(AllGlobals, IntptrTy),
                       ConstantInt::get(IntptrTy, N)});
}

// This function replaces all global variables with new variables that have
// trailing redzones. It also creates a function that poisons
// redzones and inserts this function into llvm.global_ctors.
// Sets *CtorComdat to true if the global registration code emitted into the
// asan constructor is comdat-compatible.
bool ModuleAddressSanitizer::InstrumentGlobals(IRBuilder<> &IRB, Module &M,
                                               bool *CtorComdat) {
  *CtorComdat = false;

  // Build set of globals that are aliased by some GA, where
  // getExcludedAliasedGlobal(GA) returns the relevant GlobalVariable.
  SmallPtrSet<const GlobalVariable *, 16> AliasedGlobalExclusions;
  if (CompileKernel) {
    for (auto &GA : M.aliases()) {
      if (const GlobalVariable *GV = getExcludedAliasedGlobal(GA))
        AliasedGlobalExclusions.insert(GV);
    }
  }

  SmallVector<GlobalVariable *, 16> GlobalsToChange;
  for (auto &G : M.globals()) {
    if (!AliasedGlobalExclusions.count(&G) && shouldInstrumentGlobal(&G))
      GlobalsToChange.push_back(&G);
  }

  size_t n = GlobalsToChange.size();
  if (n == 0) {
    *CtorComdat = true;
    return false;
  }

  auto &DL = M.getDataLayout();

  // A global is described by a structure
  //   size_t beg;
  //   size_t size;
  //   size_t size_with_redzone;
  //   const char *name;
  //   const char *module_name;
  //   size_t has_dynamic_init;
  //   void *source_location;
  //   size_t odr_indicator;
  // We initialize an array of such structures and pass it to a run-time call.
  StructType *GlobalStructTy =
      StructType::get(IntptrTy, IntptrTy, IntptrTy, IntptrTy, IntptrTy,
                      IntptrTy, IntptrTy, IntptrTy);
  SmallVector<GlobalVariable *, 16> NewGlobals(n);
  SmallVector<Constant *, 16> Initializers(n);

  bool HasDynamicallyInitializedGlobals = false;

  // We shouldn't merge same module names, as this string serves as unique
  // module ID in runtime.
  GlobalVariable *ModuleName = createPrivateGlobalForString(
      M, M.getModuleIdentifier(), /*AllowMerging*/ false, kAsanGenPrefix);

  for (size_t i = 0; i < n; i++) {
    GlobalVariable *G = GlobalsToChange[i];

    // FIXME: Metadata should be attched directly to the global directly instead
    // of being added to llvm.asan.globals.
    auto MD = GlobalsMD.get(G);
    StringRef NameForGlobal = G->getName();
    // Create string holding the global name (use global name from metadata
    // if it's available, otherwise just write the name of global variable).
    GlobalVariable *Name = createPrivateGlobalForString(
        M, MD.Name.empty() ? NameForGlobal : MD.Name,
        /*AllowMerging*/ true, kAsanGenPrefix);

    Type *Ty = G->getValueType();
    const uint64_t SizeInBytes = DL.getTypeAllocSize(Ty);
    const uint64_t RightRedzoneSize = getRedzoneSizeForGlobal(SizeInBytes);
    Type *RightRedZoneTy = ArrayType::get(IRB.getInt8Ty(), RightRedzoneSize);

    StructType *NewTy = StructType::get(Ty, RightRedZoneTy);
    Constant *NewInitializer = ConstantStruct::get(
        NewTy, G->getInitializer(), Constant::getNullValue(RightRedZoneTy));

    // Create a new global variable with enough space for a redzone.
    GlobalValue::LinkageTypes Linkage = G->getLinkage();
    if (G->isConstant() && Linkage == GlobalValue::PrivateLinkage)
      Linkage = GlobalValue::InternalLinkage;
    GlobalVariable *NewGlobal =
        new GlobalVariable(M, NewTy, G->isConstant(), Linkage, NewInitializer,
                           "", G, G->getThreadLocalMode());
    NewGlobal->copyAttributesFrom(G);
    NewGlobal->setComdat(G->getComdat());
    NewGlobal->setAlignment(MaybeAlign(getMinRedzoneSizeForGlobal()));
    // Don't fold globals with redzones. ODR violation detector and redzone
    // poisoning implicitly creates a dependence on the global's address, so it
    // is no longer valid for it to be marked unnamed_addr.
    NewGlobal->setUnnamedAddr(GlobalValue::UnnamedAddr::None);

    // Move null-terminated C strings to "__asan_cstring" section on Darwin.
    if (TargetTriple.isOSBinFormatMachO() && !G->hasSection() &&
        G->isConstant()) {
      auto Seq = dyn_cast<ConstantDataSequential>(G->getInitializer());
      if (Seq && Seq->isCString())
        NewGlobal->setSection("__TEXT,__asan_cstring,regular");
    }

    // Transfer the debug info and type metadata.  The payload starts at offset
    // zero so we can copy the metadata over as is.
    NewGlobal->copyMetadata(G, 0);

    Value *Indices2[2];
    Indices2[0] = IRB.getInt32(0);
    Indices2[1] = IRB.getInt32(0);

    G->replaceAllUsesWith(
        ConstantExpr::getGetElementPtr(NewTy, NewGlobal, Indices2, true));
    NewGlobal->takeName(G);
    G->eraseFromParent();
    NewGlobals[i] = NewGlobal;

    Constant *SourceLoc;
    if (!MD.SourceLoc.empty()) {
      auto SourceLocGlobal = createPrivateGlobalForSourceLoc(M, MD.SourceLoc);
      SourceLoc = ConstantExpr::getPointerCast(SourceLocGlobal, IntptrTy);
    } else {
      SourceLoc = ConstantInt::get(IntptrTy, 0);
    }

    Constant *ODRIndicator = ConstantExpr::getNullValue(IRB.getInt8PtrTy());
    GlobalValue *InstrumentedGlobal = NewGlobal;

    bool CanUsePrivateAliases =
        TargetTriple.isOSBinFormatELF() || TargetTriple.isOSBinFormatMachO() ||
        TargetTriple.isOSBinFormatWasm();
    if (CanUsePrivateAliases && UsePrivateAlias) {
      // Create local alias for NewGlobal to avoid crash on ODR between
      // instrumented and non-instrumented libraries.
      InstrumentedGlobal =
          GlobalAlias::create(GlobalValue::PrivateLinkage, "", NewGlobal);
    }

    // ODR should not happen for local linkage.
    if (NewGlobal->hasLocalLinkage()) {
      ODRIndicator = ConstantExpr::getIntToPtr(ConstantInt::get(IntptrTy, -1),
                                               IRB.getInt8PtrTy());
    } else if (UseOdrIndicator) {
      // With local aliases, we need to provide another externally visible
      // symbol __odr_asan_XXX to detect ODR violation.
      auto *ODRIndicatorSym =
          new GlobalVariable(M, IRB.getInt8Ty(), false, Linkage,
                             Constant::getNullValue(IRB.getInt8Ty()),
                             kODRGenPrefix + NameForGlobal, nullptr,
                             NewGlobal->getThreadLocalMode());

      // Set meaningful attributes for indicator symbol.
      ODRIndicatorSym->setVisibility(NewGlobal->getVisibility());
      ODRIndicatorSym->setDLLStorageClass(NewGlobal->getDLLStorageClass());
      ODRIndicatorSym->setAlignment(Align(1));
      ODRIndicator = ODRIndicatorSym;
    }

    Constant *Initializer = ConstantStruct::get(
        GlobalStructTy,
        ConstantExpr::getPointerCast(InstrumentedGlobal, IntptrTy),
        ConstantInt::get(IntptrTy, SizeInBytes),
        ConstantInt::get(IntptrTy, SizeInBytes + RightRedzoneSize),
        ConstantExpr::getPointerCast(Name, IntptrTy),
        ConstantExpr::getPointerCast(ModuleName, IntptrTy),
        ConstantInt::get(IntptrTy, MD.IsDynInit), SourceLoc,
        ConstantExpr::getPointerCast(ODRIndicator, IntptrTy));

    if (ClInitializers && MD.IsDynInit) HasDynamicallyInitializedGlobals = true;

    LLVM_DEBUG(dbgs() << "NEW GLOBAL: " << *NewGlobal << "\n");

    Initializers[i] = Initializer;
  }

  // Add instrumented globals to llvm.compiler.used list to avoid LTO from
  // ConstantMerge'ing them.
  SmallVector<GlobalValue *, 16> GlobalsToAddToUsedList;
  for (size_t i = 0; i < n; i++) {
    GlobalVariable *G = NewGlobals[i];
    if (G->getName().empty()) continue;
    GlobalsToAddToUsedList.push_back(G);
  }
  appendToCompilerUsed(M, ArrayRef<GlobalValue *>(GlobalsToAddToUsedList));

  std::string ELFUniqueModuleId =
      (UseGlobalsGC && TargetTriple.isOSBinFormatELF()) ? getUniqueModuleId(&M)
                                                        : "";

  if (!ELFUniqueModuleId.empty()) {
    InstrumentGlobalsELF(IRB, M, NewGlobals, Initializers, ELFUniqueModuleId);
    *CtorComdat = true;
  } else if (UseGlobalsGC && TargetTriple.isOSBinFormatCOFF()) {
    InstrumentGlobalsCOFF(IRB, M, NewGlobals, Initializers);
  } else if (UseGlobalsGC && ShouldUseMachOGlobalsSection()) {
    InstrumentGlobalsMachO(IRB, M, NewGlobals, Initializers);
  } else {
    InstrumentGlobalsWithMetadataArray(IRB, M, NewGlobals, Initializers);
  }

  // Create calls for poisoning before initializers run and unpoisoning after.
  if (HasDynamicallyInitializedGlobals)
    createInitializerPoisonCalls(M, ModuleName);

  LLVM_DEBUG(dbgs() << M);
  return true;
}

uint64_t
ModuleAddressSanitizer::getRedzoneSizeForGlobal(uint64_t SizeInBytes) const {
  constexpr uint64_t kMaxRZ = 1 << 18;
  const uint64_t MinRZ = getMinRedzoneSizeForGlobal();

  // Calculate RZ, where MinRZ <= RZ <= MaxRZ, and RZ ~ 1/4 * SizeInBytes.
  uint64_t RZ =
      std::max(MinRZ, std::min(kMaxRZ, (SizeInBytes / MinRZ / 4) * MinRZ));

  // Round up to multiple of MinRZ.
  if (SizeInBytes % MinRZ)
    RZ += MinRZ - (SizeInBytes % MinRZ);
  assert((RZ + SizeInBytes) % MinRZ == 0);

  return RZ;
}

int ModuleAddressSanitizer::GetAsanVersion(const Module &M) const {
  int LongSize = M.getDataLayout().getPointerSizeInBits();
  bool isAndroid = Triple(M.getTargetTriple()).isAndroid();
  int Version = 8;
  // 32-bit Android is one version ahead because of the switch to dynamic
  // shadow.
  Version += (LongSize == 32 && isAndroid);
  return Version;
}

bool ModuleAddressSanitizer::instrumentModule(Module &M) {
  initializeCallbacks(M);

  // Create a module constructor. A destructor is created lazily because not all
  // platforms, and not all modules need it.
  if (CompileKernel) {
    // The kernel always builds with its own runtime, and therefore does not
    // need the init and version check calls.
    AsanCtorFunction = createSanitizerCtor(M, kAsanModuleCtorName);
  } else {
    std::string AsanVersion = std::to_string(GetAsanVersion(M));
    std::string VersionCheckName =
        ClInsertVersionCheck ? (kAsanVersionCheckNamePrefix + AsanVersion) : "";
    std::tie(AsanCtorFunction, std::ignore) =
        createSanitizerCtorAndInitFunctions(M, kAsanModuleCtorName,
                                            kAsanInitName, /*InitArgTypes=*/{},
                                            /*InitArgs=*/{}, VersionCheckName);
  }

  bool CtorComdat = true;
  if (ClGlobals) {
    IRBuilder<> IRB(AsanCtorFunction->getEntryBlock().getTerminator());
    InstrumentGlobals(IRB, M, &CtorComdat);
  }

  const uint64_t Priority = GetCtorAndDtorPriority(TargetTriple);

  // Put the constructor and destructor in comdat if both
  // (1) global instrumentation is not TU-specific
  // (2) target is ELF.
  if (UseCtorComdat && TargetTriple.isOSBinFormatELF() && CtorComdat) {
    AsanCtorFunction->setComdat(M.getOrInsertComdat(kAsanModuleCtorName));
    appendToGlobalCtors(M, AsanCtorFunction, Priority, AsanCtorFunction);
    if (AsanDtorFunction) {
      AsanDtorFunction->setComdat(M.getOrInsertComdat(kAsanModuleDtorName));
      appendToGlobalDtors(M, AsanDtorFunction, Priority, AsanDtorFunction);
    }
  } else {
    appendToGlobalCtors(M, AsanCtorFunction, Priority);
    if (AsanDtorFunction)
      appendToGlobalDtors(M, AsanDtorFunction, Priority);
  }

  return true;
}

void AddressSanitizer::initializeCallbacks(Module &M) {
  IRBuilder<> IRB(*C);
  // Create __asan_report* callbacks.
  // IsWrite, TypeSize and Exp are encoded in the function name.
  for (int Exp = 0; Exp < 2; Exp++) {
    for (size_t AccessIsWrite = 0; AccessIsWrite <= 1; AccessIsWrite++) {
      const std::string TypeStr = AccessIsWrite ? "store" : "load";
      const std::string ExpStr = Exp ? "exp_" : "";
      const std::string EndingStr = Recover ? "_noabort" : "";

      SmallVector<Type *, 3> Args2 = {IntptrTy, IntptrTy};
      SmallVector<Type *, 2> Args1{1, IntptrTy};
      if (Exp) {
        Type *ExpType = Type::getInt32Ty(*C);
        Args2.push_back(ExpType);
        Args1.push_back(ExpType);
      }
      AsanErrorCallbackSized[AccessIsWrite][Exp] = M.getOrInsertFunction(
          kAsanReportErrorTemplate + ExpStr + TypeStr + "_n" + EndingStr,
          FunctionType::get(IRB.getVoidTy(), Args2, false));

      AsanMemoryAccessCallbackSized[AccessIsWrite][Exp] = M.getOrInsertFunction(
          ClMemoryAccessCallbackPrefix + ExpStr + TypeStr + "N" + EndingStr,
          FunctionType::get(IRB.getVoidTy(), Args2, false));

      for (size_t AccessSizeIndex = 0; AccessSizeIndex < kNumberOfAccessSizes;
           AccessSizeIndex++) {
        const std::string Suffix = TypeStr + itostr(1ULL << AccessSizeIndex);
        AsanErrorCallback[AccessIsWrite][Exp][AccessSizeIndex] =
            M.getOrInsertFunction(
                kAsanReportErrorTemplate + ExpStr + Suffix + EndingStr,
                FunctionType::get(IRB.getVoidTy(), Args1, false));

        AsanMemoryAccessCallback[AccessIsWrite][Exp][AccessSizeIndex] =
            M.getOrInsertFunction(
                ClMemoryAccessCallbackPrefix + ExpStr + Suffix + EndingStr,
                FunctionType::get(IRB.getVoidTy(), Args1, false));
      }
    }
  }

  const std::string MemIntrinCallbackPrefix =
      CompileKernel ? std::string("") : ClMemoryAccessCallbackPrefix;
  AsanMemmove = M.getOrInsertFunction(MemIntrinCallbackPrefix + "memmove",
                                      IRB.getInt8PtrTy(), IRB.getInt8PtrTy(),
                                      IRB.getInt8PtrTy(), IntptrTy);
  AsanMemcpy = M.getOrInsertFunction(MemIntrinCallbackPrefix + "memcpy",
                                     IRB.getInt8PtrTy(), IRB.getInt8PtrTy(),
                                     IRB.getInt8PtrTy(), IntptrTy);
  AsanMemset = M.getOrInsertFunction(MemIntrinCallbackPrefix + "memset",
                                     IRB.getInt8PtrTy(), IRB.getInt8PtrTy(),
                                     IRB.getInt32Ty(), IntptrTy);

  AsanHandleNoReturnFunc =
      M.getOrInsertFunction(kAsanHandleNoReturnName, IRB.getVoidTy());

  AsanPtrCmpFunction =
      M.getOrInsertFunction(kAsanPtrCmp, IRB.getVoidTy(), IntptrTy, IntptrTy);
  AsanPtrSubFunction =
      M.getOrInsertFunction(kAsanPtrSub, IRB.getVoidTy(), IntptrTy, IntptrTy);
  if (Mapping.InGlobal)
    AsanShadowGlobal = M.getOrInsertGlobal("__asan_shadow",
                                           ArrayType::get(IRB.getInt8Ty(), 0));
}

bool AddressSanitizer::maybeInsertAsanInitAtFunctionEntry(Function &F) {
  // For each NSObject descendant having a +load method, this method is invoked
  // by the ObjC runtime before any of the static constructors is called.
  // Therefore we need to instrument such methods with a call to __asan_init
  // at the beginning in order to initialize our runtime before any access to
  // the shadow memory.
  // We cannot just ignore these methods, because they may call other
  // instrumented functions.
  if (F.getName().find(" load]") != std::string::npos) {
    FunctionCallee AsanInitFunction =
        declareSanitizerInitFunction(*F.getParent(), kAsanInitName, {});
    IRBuilder<> IRB(&F.front(), F.front().begin());
    IRB.CreateCall(AsanInitFunction, {});
    return true;
  }
  return false;
}

bool AddressSanitizer::maybeInsertDynamicShadowAtFunctionEntry(Function &F) {
  // Generate code only when dynamic addressing is needed.
  if (Mapping.Offset != kDynamicShadowSentinel)
    return false;

  IRBuilder<> IRB(&F.front().front());
  if (Mapping.InGlobal) {
    if (ClWithIfuncSuppressRemat) {
      // An empty inline asm with input reg == output reg.
      // An opaque pointer-to-int cast, basically.
      InlineAsm *Asm = InlineAsm::get(
          FunctionType::get(IntptrTy, {AsanShadowGlobal->getType()}, false),
          StringRef(""), StringRef("=r,0"),
          /*hasSideEffects=*/false);
      LocalDynamicShadow =
          IRB.CreateCall(Asm, {AsanShadowGlobal}, ".asan.shadow");
    } else {
      LocalDynamicShadow =
          IRB.CreatePointerCast(AsanShadowGlobal, IntptrTy, ".asan.shadow");
    }
  } else {
    Value *GlobalDynamicAddress = F.getParent()->getOrInsertGlobal(
        kAsanShadowMemoryDynamicAddress, IntptrTy);
    LocalDynamicShadow = IRB.CreateLoad(IntptrTy, GlobalDynamicAddress);
  }
  return true;
}

void AddressSanitizer::markEscapedLocalAllocas(Function &F) {
  // Find the one possible call to llvm.localescape and pre-mark allocas passed
  // to it as uninteresting. This assumes we haven't started processing allocas
  // yet. This check is done up front because iterating the use list in
  // isInterestingAlloca would be algorithmically slower.
  assert(ProcessedAllocas.empty() && "must process localescape before allocas");

  // Try to get the declaration of llvm.localescape. If it's not in the module,
  // we can exit early.
  if (!F.getParent()->getFunction("llvm.localescape")) return;

  // Look for a call to llvm.localescape call in the entry block. It can't be in
  // any other block.
  for (Instruction &I : F.getEntryBlock()) {
    IntrinsicInst *II = dyn_cast<IntrinsicInst>(&I);
    if (II && II->getIntrinsicID() == Intrinsic::localescape) {
      // We found a call. Mark all the allocas passed in as uninteresting.
      for (Value *Arg : II->arg_operands()) {
        AllocaInst *AI = dyn_cast<AllocaInst>(Arg->stripPointerCasts());
        assert(AI && AI->isStaticAlloca() &&
               "non-static alloca arg to localescape");
        ProcessedAllocas[AI] = false;
      }
      break;
    }
  }
}

bool AddressSanitizer::suppressInstrumentationSiteForDebug(int &Instrumented) {
  bool ShouldInstrument =
      ClDebugMin < 0 || ClDebugMax < 0 ||
      (Instrumented >= ClDebugMin && Instrumented <= ClDebugMax);
  Instrumented++;
  return !ShouldInstrument;
}

void AddressSanitizer::sequentialExecuteOptimizationPostDom(Function &F, SmallVector<InterestingMemoryOperand, 16> &OperandsToInstrument, AliasAnalysis *AA) {

  auto PDT = PostDominatorTree();
  PDT.recalculate(F);
  std::map<Value *, std::set<InterestingMemoryOperand *>> AddrToInstructions;

  // pre-processing
  // group instructions that access the same address (alias considered)
  for (InterestingMemoryOperand &Operand : OperandsToInstrument) {
    if (Value *Addr = Operand.getPtr()) {

      if (AddrToInstructions.find(Addr) == AddrToInstructions.end()) {

        bool aliasFound = false;
        //handle the possibility of alias
        for (auto item : AddrToInstructions) {
          if (AA->isMustAlias(item.first, Addr)) {
            aliasFound = true;
            AddrToInstructions[item.first].insert(&Operand);
            break;
          }
        }
        //found an alias, done
        if (aliasFound) continue;

        //never appeared in the map, so add a slot
        AddrToInstructions.insert(std::pair<Value *, std::set<InterestingMemoryOperand *>>(Addr, std::set<InterestingMemoryOperand *>()));
      }
      //add the inst to the target slot (either the newly created one or an existing one)
      AddrToInstructions[Addr].insert(&Operand);
    }
  }

  std::set<Instruction *> deleted;

  for (auto item : AddrToInstructions) {
    for (auto inst1 : item.second) {
      //well, the instruction has been deleted, so who cares
      if (deleted.find(inst1->getInsn()) != deleted.end())
        continue;

      for (auto inst2 : item.second) {
        //avoid checking itself
        if (inst1->getInsn() == inst2->getInsn() || deleted.find(inst2->getInsn()) != deleted.end())
          continue;	

        if (PDT.dominates(inst1->getInsn()->getParent(), inst2->getInsn()->getParent())){
          deleted.insert(inst2->getInsn());
        }
      }
    }
  }
  //Let's only keep the non-deleted ones
  SmallVector<InterestingMemoryOperand, 16> SEOTempToInstrument(OperandsToInstrument);
  OperandsToInstrument.clear();

  for (auto item: SEOTempToInstrument) {
    if (deleted.find(item.getInsn()) == deleted.end())
      OperandsToInstrument.push_back(item);
  }
}

void AddressSanitizer::ConservativeCallIntrinsicCollect(Function &F, std::set<Instruction *> &callIntrinsicSet) {

  for (auto &BB : F) {
    for (auto &Inst : BB) {
      // Here we check if current instruction is call instruction
      if (CallInst *CI = dyn_cast<CallInst>(&Inst)) {
        callIntrinsicSet.insert(&Inst);
        continue;
      }
      IntrinsicInst *II = dyn_cast<IntrinsicInst>(&Inst);
      // Here we check if Intrinsic ID is lifetime_end
      if (II && II->getIntrinsicID() == Intrinsic::lifetime_end) {
        callIntrinsicSet.insert(&Inst);
        continue;
      }
    }
  }
}

bool isPostDominatWrapper(Instruction *InstStart, Instruction *TargetInst, llvm::PostDominatorTree &PDT) {
  
  BasicBlock *StartBB = InstStart->getParent();
  BasicBlock *TargetBB = TargetInst->getParent();
  if (StartBB == TargetBB) {
    for (auto &itrInst : *StartBB) {
      if (&itrInst == InstStart) {
        return false;
      }
      if (&itrInst == TargetInst) {
        return true;
      }
    }
  }
  return PDT.dominates(StartBB, TargetBB);
}

bool AddressSanitizer::ConservativeCallIntrinsicCheck(Instruction *InstStart, Instruction *InstEnd, std::set<Instruction *> &callIntrinsicSet, llvm::DominatorTree &DT, llvm::PostDominatorTree &PDT) {

  for (auto TargetInst : callIntrinsicSet) {
    // InstStart -> TargetInst -> InstEnd && InstStart !PostDominat TargetInst
    if (isPotentiallyReachable(InstStart, TargetInst) && isPotentiallyReachable(TargetInst, InstEnd) && !isPostDominatWrapper(InstStart, TargetInst, PDT)) {
      return false;
    }
  }
  return true;
}

void AddressSanitizer::sequentialExecuteOptimization(Function &F, SmallVector<InterestingMemoryOperand, 16> &OperandsToInstrument, AliasAnalysis *AA) {

  auto DT = DominatorTree(F);
  std::map<Value *, std::set<InterestingMemoryOperand *>> AddrToInstructions;
  auto PDT = PostDominatorTree();
  PDT.recalculate(F);

  // pre-processing
  // group instructions that access the same address (alias considered)
  for (InterestingMemoryOperand &Operand : OperandsToInstrument) {
    if (Value *Addr = Operand.getPtr()) {

      if (AddrToInstructions.find(Addr) == AddrToInstructions.end()) {

        bool aliasFound = false;
        //handle the possibility of alias
        for (auto item : AddrToInstructions) {
          if (AA->isMustAlias(item.first, Addr)) {
            aliasFound = true;
            AddrToInstructions[item.first].insert(&Operand);
            break;
          }
        }
        //found an alias, done
        if (aliasFound) continue;

        //never appeared in the map, so add a slot
        AddrToInstructions.insert(std::pair<Value *, std::set<InterestingMemoryOperand *>>(Addr, std::set<InterestingMemoryOperand *>()));
      }
      //add the inst to the target slot (either the newly created one or an existing one)
      AddrToInstructions[Addr].insert(&Operand);
    }
  }

  std::set<Instruction *> deleted;

  std::set<Instruction *> callIntrinsicSet; 

  ConservativeCallIntrinsicCollect(F, callIntrinsicSet);

  for (auto item : AddrToInstructions) {
    for (auto inst1 : item.second) {
      //well, the instruction has been deleted, so who cares
      if (deleted.find(inst1->getInsn()) != deleted.end())
        continue;

      for (auto inst2 : item.second) {
        //avoid checking itself
        if (inst1->getInsn() == inst2->getInsn() || deleted.find(inst2->getInsn()) != deleted.end())
          continue;	

        if (DT.dominates(inst1->getInsn(), inst2->getInsn()) && ConservativeCallIntrinsicCheck(inst1->getInsn(), inst2->getInsn(), callIntrinsicSet, DT, PDT)){
          deleted.insert(inst2->getInsn());
        }
      }
    }
  }
  //Let's only keep the non-deleted ones
  SmallVector<InterestingMemoryOperand, 16> SEOTempToInstrument(OperandsToInstrument);
  OperandsToInstrument.clear();

  for (auto item: SEOTempToInstrument) {
    if (deleted.find(item.getInsn()) == deleted.end())
      OperandsToInstrument.push_back(item);
  }
}

void preprocessPotentialRemoveInsts(Function &F, std::pair<const std::pair<llvm::Value *, std::string>, std::set<std::pair<int64_t, llvm::Instruction *>>> &baseAddrOffsetSet, std::map<Instruction *, std::set<std::pair<Instruction *, Instruction *>>> &potentialRemoveInsts) {
  
  auto DT = DominatorTree(F);

  auto PDT = PostDominatorTree();

  PDT.recalculate(F);
  
  // offsetInstA is node A
  for (auto offsetInstA : baseAddrOffsetSet.second) {
    // offsetInstB is node B
    for (auto offsetInstB : baseAddrOffsetSet.second) {
      if (offsetInstA == offsetInstB)
        continue;
      // offsetInstC is node C
      for (auto offsetInstC : baseAddrOffsetSet.second) {
        if (offsetInstA == offsetInstC || offsetInstB == offsetInstC)
          continue;

        // Here we ensure (A dominate B OR A post-dominate B) AND (OFFSET(C) > OFFSET(B) AND OFFSET(B) > OFFSET(A) AND OFFSET(C) - OFFSET(A) < 16)
        if ( (DT.dominates(offsetInstA.second, offsetInstB.second) || PDT.dominates((offsetInstA.second)->getParent(), (offsetInstB.second)->getParent())) 
            && (offsetInstC.first > offsetInstB.first && offsetInstB.first > offsetInstA.first && offsetInstC.first - offsetInstA.first < RZ_SIZE) ) {
          // If above conditions are satisfied, then ASan check on B can be removed.
          if (potentialRemoveInsts.find(offsetInstB.second) == potentialRemoveInsts.end()) {
            potentialRemoveInsts.insert(std::pair<Instruction *, std::set<std::pair<Instruction *, Instruction *>>>(offsetInstB.second, std::set<std::pair<Instruction *, Instruction *>>()));
          }
          // Store the ASan check removable instruction B, and the pair of instructions A and C that ensure the ASan Check to map
          std::pair<Instruction *, Instruction *> InstsPair;
          InstsPair.first = offsetInstA.second;
          InstsPair.second = offsetInstC.second;
          potentialRemoveInsts[offsetInstB.second].insert(InstsPair);
        } 
      }
    }
  }
}

void rankRemovableInsts(std::map<Instruction *, std::set<std::pair<Instruction *, Instruction *>>> &potentialRemoveInsts, std::list<std::pair<int, Instruction *>> &rankPotentialRemoveInsts) {
  for(auto instVectorMap = potentialRemoveInsts.begin(); instVectorMap != potentialRemoveInsts.end(); ++instVectorMap) {
    int countInst = 0;
    for(auto instVector = potentialRemoveInsts.begin(); instVector != potentialRemoveInsts.end(); ++instVector) {
      if (instVector == instVectorMap)
        continue;
      for (auto instPair = (*instVector).second.begin(); instPair != (*instVector).second.end(); ++instPair) {
        if ((*instVector).first == instPair->first || (*instVector).first == instPair->second) {
          countInst++;
        }
      } 
    }
    rankPotentialRemoveInsts.push_back(std::pair<int, Instruction *>(countInst, (*instVectorMap).first));
  }
}

void removeInstructionFunc(std::map<Instruction *, std::set<std::pair<Instruction *, Instruction *>>> &potentialRemoveInsts, std::set<Instruction *> &deleted) {
  
  std::list<std::pair<int, Instruction *>> rankPotentialRemoveInsts;

  rankRemovableInsts(potentialRemoveInsts, rankPotentialRemoveInsts);

  rankPotentialRemoveInsts.sort();

  for(auto countInst : rankPotentialRemoveInsts) {
    bool removeInst = true;
    for (auto elem : deleted) {
      removeInst = false;
      for (auto instPair : potentialRemoveInsts[countInst.second]) {
        if (instPair.first != elem && instPair.second != elem) {
          removeInst = true;
          break;
        }
      }
      if (!removeInst)
        break;
    }
    if (!removeInst)
      continue;
    deleted.insert(countInst.second);
    // remove all pairs that contain current key instruction and update the map
    for(auto instVectorMap = potentialRemoveInsts.begin(); instVectorMap != potentialRemoveInsts.end(); ++instVectorMap) {
      for (auto instPair = (*instVectorMap).second.begin(); instPair != (*instVectorMap).second.end();) {
        if (countInst.second == instPair->first || countInst.second == instPair->second) {
          instPair = (*instVectorMap).second.erase(instPair);
        }
        else {
          ++instPair;
        }
      } 
    }
  }
}

void rmNeighborChks(Function &F,std::map<std::pair<Value *, std::string>, std::set<std::pair<int64_t, Instruction *>>> &baseAddrOffsetMap_multi, std::set<Instruction *> &deleted) {

  for (auto baseAddrOffsetSet : baseAddrOffsetMap_multi) {
    // Create a map to store the ASan check removable instruction, and the pair of instruction to ensure the ASan check
    std::map<Instruction *, std::set<std::pair<Instruction *, Instruction *>>> potentialRemoveInsts;
    // Cases for size of set >= 3
    if ((baseAddrOffsetSet.second).size() >=3 ) {
      preprocessPotentialRemoveInsts(F, baseAddrOffsetSet, potentialRemoveInsts);
      removeInstructionFunc(potentialRemoveInsts, deleted); 
    }
  }
}

void singleIndexCaseHandler(std::map<std::pair<Value *, std::string>, std::set<std::pair<int64_t, Instruction *>>> &baseAddrOffsetMap_multi, GetElementPtrInst *Gep_Inst, Instruction *Inst) {
  
  Value *baseAddr = Gep_Inst->getPointerOperand();
  // In order to make form unified, we create a string place holder
  std::string offsets_single;
  std::pair<Value *, std::string> key;
  std::pair<int64_t, Instruction *> value;

  if (auto *offsetAddr = dyn_cast<ConstantInt>(Gep_Inst->idx_begin())) {
    key.first = baseAddr;
    key.second = offsets_single;
    if (baseAddrOffsetMap_multi.find(key) == baseAddrOffsetMap_multi.end()) {
      //never appeared in the map, so add a slot
      baseAddrOffsetMap_multi.insert(std::pair<std::pair<Value *, std::string>, std::set<std::pair<int64_t, Instruction *> >>(key, std::set<std::pair<int64_t, Instruction *>>()));
    }
    // Convert last offset into int
    int64_t intLastOffset = offsetAddr->getSExtValue();
    value.first = intLastOffset;
    value.second = Inst;
    baseAddrOffsetMap_multi[key].insert(value);
  }
  return;
}

void multiIndexCaseHandler(std::map<std::pair<Value *, std::string>, std::set<std::pair<int64_t, Instruction *>>> &baseAddrOffsetMap_multi, GetElementPtrInst *Gep_Inst, Instruction *Inst) {
      
  Value *baseAddr = Gep_Inst->getPointerOperand();
  std::pair<Value *, std::string> key;
  std::pair<int64_t, Instruction *> value;
  
  // String to collect offsets from beg to end - 1
  std::string offsets;
  bool offsetConstantInt = true;
  for (auto& index : make_range(Gep_Inst->idx_begin(), Gep_Inst->idx_end() - 1)) {
    if (auto *offsetAddr_multi = dyn_cast<ConstantInt>(index)) {
      int64_t intOffset = offsetAddr_multi->getSExtValue();
      offsets.push_back(intOffset);
    } else {
      offsetConstantInt = false;
      break;
    }
  }

  if (!offsetConstantInt) {
    return;
  }
  
  // Here we check the value of last offset
  if (auto *offsetAddr_last = dyn_cast<ConstantInt>(Gep_Inst->idx_end() - 1)) {
    key.first = baseAddr;
    key.second = offsets;
    if (baseAddrOffsetMap_multi.find(key) == baseAddrOffsetMap_multi.end()) {
      //never appeared in the map, so add a slot
        baseAddrOffsetMap_multi.insert(std::pair<std::pair<Value *, std::string>, std::set<std::pair<int64_t, Instruction *>>>(key, std::set<std::pair<int64_t, Instruction *>>()));
    }
    // Convert last offset into int
    int64_t intLastOffset = offsetAddr_last->getSExtValue();
    value.first = intLastOffset;
    value.second = Inst;
    baseAddrOffsetMap_multi[key].insert(value);
  }
  return;
}

void AddressSanitizer::baseAddrOffsetMapPreprocessing(SmallVector<InterestingMemoryOperand, 16> &OperandsToInstrument, std::map<std::pair<Value *, std::string>, std::set<std::pair<int64_t, Instruction *>>> &baseAddrOffsetMap_multi) {

  for (auto oper : OperandsToInstrument) {

    Value *addr = oper.getPtr();
    if (!addr)
      continue;

    while (CastInst *Cast_Inst = dyn_cast<CastInst>(addr))
      addr = Cast_Inst->getOperand(0);
    
    // Check if current address is from a gep instruction
    if (GetElementPtrInst *Gep_Inst = dyn_cast<GetElementPtrInst>(addr)) {

      if (Gep_Inst->getNumIndices() == 1) {
        singleIndexCaseHandler(baseAddrOffsetMap_multi, Gep_Inst, oper.getInsn());
        continue;
      }
      multiIndexCaseHandler(baseAddrOffsetMap_multi, Gep_Inst, oper.getInsn());
      continue;
    }
  }
}

void updateBaseAddrOffsetMap(std::map<std::pair<Value *, std::string>, std::set<std::pair<int64_t, Instruction *>>> &baseAddrOffsetMap_multi, std::set<Instruction *> &deleted) {
  for (auto baseAddrOffsetSet = baseAddrOffsetMap_multi.begin(); baseAddrOffsetSet != baseAddrOffsetMap_multi.end(); ++baseAddrOffsetSet) {
    for (auto offsetInst = (*baseAddrOffsetSet).second.begin(); offsetInst != (*baseAddrOffsetSet).second.end();) {
      if (deleted.find((*offsetInst).second) != deleted.end()) {
        offsetInst = (*baseAddrOffsetSet).second.erase(offsetInst);
      }
      else {
        ++offsetInst;
      }
    }
  }
}

// Fucntion to handle DT and PDT for pairwised nodes
static bool checkConditionPairwisedNodes(llvm::DominatorTree &DT, llvm::PostDominatorTree &PDT, Instruction *A, Instruction *B) {
  return DT.dominates(A, B) && (A->getParent() == B->getParent() || PDT.dominates(B->getParent(), A->getParent()));
}

void preprocessInstructionsMap(Function &F, std::pair<const std::pair<llvm::Value *, std::string>, std::set<std::pair<int64_t, llvm::Instruction *>>> &baseAddrOffsetSet, std::map<std::pair<int64_t, llvm::Instruction *>, std::vector<std::pair<int64_t, llvm::Instruction *>>> &instructionsMap) {
  
  llvm::DominatorTree DT = DominatorTree(F);

  llvm::PostDominatorTree PDT = PostDominatorTree();

  PDT.recalculate(F);
  // offsetInstA is node A
  for (auto offsetInstA : baseAddrOffsetSet.second) {
    // offsetInstB is node B
    for (auto offsetInstB : baseAddrOffsetSet.second) {
      if (offsetInstA == offsetInstB)
        continue;

      // Here we ensure (A dominate B OR B post-dominte A) AND distance between A and B is less than 64
      if ( offsetInstA.first - offsetInstB.first < CHECK_RANGE && offsetInstB.first - offsetInstA.first < CHECK_RANGE 
          && (checkConditionPairwisedNodes(DT, PDT, offsetInstA.second, offsetInstB.second) || (checkConditionPairwisedNodes(DT, PDT, offsetInstB.second, offsetInstA.second)))) {
        // If all above conditions are satisfied, then we can remove the ASan check on B
        if (instructionsMap.find(offsetInstA) == instructionsMap.end()) {
          instructionsMap.insert(std::pair<std::pair<int64_t, llvm::Instruction *>, std::vector<std::pair<int64_t, llvm::Instruction *>>>(offsetInstA, std::vector<std::pair<int64_t, llvm::Instruction *>>()));
        }
        // Store the ASan check removable instruction B to map
        instructionsMap[offsetInstA].push_back(offsetInstB);
      }
      else {
        break;
      } 
    }
  }
}

void prioritiseRemovableInst(std::map<std::pair<int64_t, llvm::Instruction *>, std::vector<std::pair<int64_t, llvm::Instruction *>>> &instructionsMap, std::list<std::pair<int, std::pair<std::pair<int64_t, llvm::Instruction *>, std::vector<std::pair<int64_t, llvm::Instruction *>>>>> &rankPotentialRemoveInsts) {

  for (auto instVector : instructionsMap) {
    rankPotentialRemoveInsts.push_back(std::pair<int, std::pair<std::pair<int64_t, llvm::Instruction *>, std::vector<std::pair<int64_t, llvm::Instruction *>>>>(instVector.second.size(), instVector));
  }

  rankPotentialRemoveInsts.sort();

  rankPotentialRemoveInsts.reverse();

}

std::pair<int64_t, llvm::Instruction *> getLastInst(Function &F, std::pair<int, std::pair<std::pair<int64_t, llvm::Instruction *>, std::vector<std::pair<int64_t, llvm::Instruction *>>>> optInst) {

  llvm::DominatorTree DT = DominatorTree(F);

  std::pair<int64_t, llvm::Instruction *> lastInst = optInst.second.first;

  for (auto eachInst : optInst.second.second) {
    if (DT.dominates(eachInst.second, lastInst.second)) {
      continue;
    }
    else {
      lastInst = eachInst;
    }
  }

  return lastInst;
}

std::pair<int64_t, llvm::Instruction *> getMinDistance(std::pair<int, std::pair<std::pair<int64_t, llvm::Instruction *>, std::vector<std::pair<int64_t, llvm::Instruction *>>>> optInst) {

  std::pair<int64_t, llvm::Instruction *> minOffset = optInst.second.second.front();

  for (auto eachInst : optInst.second.second) {
    if (eachInst.first < minOffset.first) {
      minOffset = eachInst;
    }
    else {
      continue;
    }
  }

  return minOffset;
}

int getMaxDistance(std::pair<int, std::pair<std::pair<int64_t, llvm::Instruction *>, std::vector<std::pair<int64_t, llvm::Instruction *>>>> optInst) {

  int maxOffset = 0;

  for (auto eachInst : optInst.second.second) {
    if (eachInst.first > maxOffset) {
      maxOffset = eachInst.first;
    }
    else {
      continue;
    }
  }

  return maxOffset;
}

void AddressSanitizer::optimizeInstrumentation(Function &F, std::list<std::pair<int, std::pair<std::pair<int64_t, llvm::Instruction *>, std::vector<std::pair<int64_t, llvm::Instruction *>>>>> &rankPotentialRemoveInsts, std::set<Instruction *> &deleted, SmallVector<InterestingMemoryOperand, 16> &OperandsToInstrument) {
  
  bool IsWrite;
  MaybeAlign Alignment;
  uint64_t TypeSize;
  Value *addr;
  Instruction *cur_Inst;
  
  for (auto optInst = rankPotentialRemoveInsts.begin(); optInst != rankPotentialRemoveInsts.end(); ++optInst) {

    std::pair<int64_t, llvm::Instruction *> lastInst = getLastInst(F, (*optInst));

    std::pair<int64_t, llvm::Instruction *> minInst = getMinDistance(*optInst);

    int maxDistance = getMaxDistance(*optInst) - minInst.first;

    for (auto item: OperandsToInstrument) {
      if (item.getInsn() == minInst.second) {
        addr = item.getPtr();
        IsWrite = item.IsWrite;
        Alignment = item.Alignment;
        TypeSize = item.TypeSize;
        cur_Inst = item.getInsn();
      }
    }

    if (!addr) {
      deleted.insert(cur_Inst);
      continue;
    }

    // Map current address to shadow memory, and check 64 bits range
    IRBuilder<> IRB(lastInst.second);
    Value *AddrLong = IRB.CreatePointerCast(addr, IntptrTy);
    Type *ShadowTy = IntegerType::get(*C, 8U);
    if (8 <= maxDistance && maxDistance < 16) {
      ShadowTy = IntegerType::get(*C, 16U);
    }
    else if (16 <= maxDistance && maxDistance < 32) {
      ShadowTy = IntegerType::get(*C, 32U);
    }
    else if (32 <= maxDistance && maxDistance < 64) {
      ShadowTy = IntegerType::get(*C, 64U);
    }
    Type *ShadowPtrTy = PointerType::get(ShadowTy, 0);
    Value *ShadowPtr = memToShadow(AddrLong, IRB);
    Value *CmpVal = Constant::getNullValue(ShadowTy);
    Value *ShadowValue = IRB.CreateLoad(IRB.CreateIntToPtr(ShadowPtr, ShadowPtrTy));
    Value *Cmp = IRB.CreateICmpNE(ShadowValue, CmpVal);

    Instruction *CheckTerm = SplitBlockAndInsertIfThen(Cmp, lastInst.second, false);

    IRBuilder<> IRBasanCheck(CheckTerm);
    // If shadow memory check != 0, then we do regular ASan check
    unsigned Granularity = 1 << Mapping.Scale;
    if ((TypeSize == 8 || TypeSize == 16 || TypeSize == 32 || TypeSize == 64 || TypeSize == 128) && (!Alignment || *Alignment >= Granularity || *Alignment >= TypeSize / 8)) {
      instrumentAddress(CheckTerm, CheckTerm, addr, TypeSize, IsWrite, nullptr, false, 0);
    } else {
      instrumentUnusualSizeOrAlignment(CheckTerm, CheckTerm, addr, TypeSize, IsWrite, nullptr, false, 0);
    }
    deleted.insert((*optInst).second.first.second);

    for (auto eachInst : (*optInst).second.second) {
      // For each instruction, we will check the BoostInit value to decide add ASan check or not
      bool IsWrite;
      MaybeAlign Alignment;
      uint64_t TypeSize;
      Value *eachAddr;

      for (auto item: OperandsToInstrument) {
        if (item.getInsn() == minInst.second) {
          eachAddr = item.getPtr();
          IsWrite = item.IsWrite;
          Alignment = item.Alignment;
          TypeSize = item.TypeSize;
        }
      }

      if (!eachAddr) {
        deleted.insert(eachInst.second);
        continue;
      }

      unsigned Granularity = 1 << Mapping.Scale;
      if ((TypeSize == 8 || TypeSize == 16 || TypeSize == 32 || TypeSize == 64 || TypeSize == 128) && (!Alignment || *Alignment >= Granularity || *Alignment >= TypeSize / 8)) {
        instrumentAddress(CheckTerm, CheckTerm, eachAddr, TypeSize, IsWrite, nullptr, false, 0);
      } else {
        instrumentUnusualSizeOrAlignment(CheckTerm, CheckTerm, eachAddr, TypeSize, IsWrite, nullptr, false, 0);
      }
      deleted.insert(eachInst.second);
    }

    // eliminate the removable instructions, and update the list
    for (auto optInstChild = rankPotentialRemoveInsts.begin(); optInstChild != rankPotentialRemoveInsts.end();) {

      if(std::find((*optInst).second.second.begin(), (*optInst).second.second.end(), (*optInstChild).second.first) != (*optInst).second.second.end()) {
        optInstChild = rankPotentialRemoveInsts.erase(optInstChild);
      }
      else {
        ++optInstChild;
      }
    }
  }
}

void AddressSanitizer::mrgNeighborChks(Function &F, std::map<std::pair<Value *, std::string>, std::set<std::pair<int64_t, Instruction *>>> &baseAddrOffsetMap_multi, std::set<Instruction *> &deleted, SmallVector<InterestingMemoryOperand, 16> &OperandsToInstrument) {

  auto DT = DominatorTree(F);

  auto PDT = PostDominatorTree();

  PDT.recalculate(F);

  for (auto baseAddrOffsetSet : baseAddrOffsetMap_multi) {
    // Create a map to store the instruction, and a vector of instructions it can remove
    std::map<std::pair<int64_t, llvm::Instruction *>, std::vector<std::pair<int64_t, llvm::Instruction *>>> instructionsMap;
    std::list<std::pair<int, std::pair<std::pair<int64_t, llvm::Instruction *>, std::vector<std::pair<int64_t, llvm::Instruction *>>>>> rankPotentialRemoveInsts;
    // Cases for size of set >= 2
    if ((baseAddrOffsetSet.second).size() >= 2) {

      preprocessInstructionsMap(F, baseAddrOffsetSet, instructionsMap);

      prioritiseRemovableInst(instructionsMap, rankPotentialRemoveInsts);

      optimizeInstrumentation(F, rankPotentialRemoveInsts, deleted, OperandsToInstrument);
    } 
  }
}

void AddressSanitizer::sequentialExecuteOptimizationBoost(Function &F, SmallVector<InterestingMemoryOperand, 16> &OperandsToInstrument) {

  std::map<std::pair<Value *, std::string>, std::set<std::pair<int64_t, Instruction *>>> baseAddrOffsetMap_multi;

  baseAddrOffsetMapPreprocessing(OperandsToInstrument, baseAddrOffsetMap_multi);

  std::set<Instruction *> deleted;

  // ASAN--: Removing Neighbor Checks
  rmNeighborChks(F, baseAddrOffsetMap_multi, deleted);

  updateBaseAddrOffsetMap(baseAddrOffsetMap_multi, deleted);

  // ASAN--: Merging Neighbor Checks
  mrgNeighborChks(F, baseAddrOffsetMap_multi, deleted, OperandsToInstrument);

  SmallVector<InterestingMemoryOperand, 16> SEOTempToInstrument(OperandsToInstrument);
  OperandsToInstrument.clear();

  for (auto item: SEOTempToInstrument) {
    if (deleted.find(item.getInsn()) == deleted.end())
      OperandsToInstrument.push_back(item);
  }
}

void AddressSanitizer::InvariantOptimizeHandler(Loop *L, std::set<Instruction *> &optimized, 
                        Function &F, InterestingMemoryOperand &Oper, ObjectSizeOffsetVisitor &ObjSizeVis, bool UseCalls) {
  auto DT = DominatorTree(F);
  auto ExitBB = L->getExitBlock();

  bool IsWrite = Oper.IsWrite;
  MaybeAlign Alignment = Oper.Alignment;
	uint64_t TypeSize = Oper.TypeSize;

  Value *addr = Oper.getPtr();
  if (!addr) {
    return;
  }
    
  if (!ExitBB) {
    return;
  }

  auto exitInst = (*ExitBB).getFirstNonPHI();

  if (DT.dominates(Oper.getInsn(), ExitBB)) {
    instrumentMopLoop(ObjSizeVis, Oper, exitInst, UseCalls, F.getParent()->getDataLayout());
    optimized.insert(Oper.getInsn());
    return;
  } 
  else {    
    // Create local variable Tracer, and assign 0 as initial value
    IRBuilder<> IRBinit(F.getEntryBlock().getFirstNonPHI());
    Value *Tracer = IRBinit.CreateAlloca(IntptrTy, nullptr, "Tracer");
    IRBinit.CreateStore(ConstantInt::get(IntptrTy, 0), Tracer);

    // Assign memory access address to the Tracer
    IRBuilder<> IRBassign(Oper.getInsn());
    Value *AddrCast = IRBassign.CreatePointerCast(addr, IntptrTy);
    IRBassign.CreateStore(AddrCast, Tracer);

    // Check the Tracer value to decide add ASan check or not.
    IRBuilder<> IRBcheck(exitInst);
    Value *LITracer = IRBcheck.CreateLoad(Tracer);
    Value *Cmp = IRBcheck.CreateICmpNE(LITracer, ConstantInt::get(IntptrTy, 0));
    Instruction *CheckTerm = SplitBlockAndInsertIfThen(Cmp, exitInst, false);

    IRBuilder<> IRBasanCheck(CheckTerm);
    if (ClOpt && ClOptGlobals) {
      GlobalVariable *G = dyn_cast<GlobalVariable>(getUnderlyingObject(LITracer));
      if (G && (!ClInitializers || GlobalIsLinkerInitialized(G)) &&
          isSafeAccess(ObjSizeVis, LITracer, TypeSize)) {
        optimized.insert(Oper.getInsn());
        return;
      }
    }

    if (ClOpt && ClOptStack) {
      // A direct inbounds access to a stack variable is always valid.
      if (isa<AllocaInst>(getUnderlyingObject(LITracer)) &&
          isSafeAccess(ObjSizeVis, LITracer, TypeSize)) {
        optimized.insert(Oper.getInsn());
        return;
      }
    }

    unsigned Granularity = 1 << Mapping.Scale;
    if ((TypeSize == 8 || TypeSize == 16 || TypeSize == 32 || TypeSize == 64 || TypeSize == 128) && (!Alignment || *Alignment >= Granularity || *Alignment >= TypeSize / 8)) {
      instrumentAddress(CheckTerm, CheckTerm, LITracer, TypeSize, IsWrite, nullptr, UseCalls, 0);
    } else {
      instrumentUnusualSizeOrAlignment(CheckTerm, CheckTerm, LITracer, TypeSize, IsWrite, nullptr, UseCalls, 0);
    }


    // Condition to handel current loop is outer most loop
    if (L->getParentLoop() == nullptr) {
      optimized.insert(Oper.getInsn());
      return;
    }
    
    // If current loop is inner loop, we will re-init the tracer to 0
    IRBuilder<> IRBreInit(exitInst);
    IRBreInit.CreateStore(ConstantInt::get(IntptrTy, 0), Tracer);
    optimized.insert(Oper.getInsn());
    return;
  }
  return;
  
}

enum SCEVType SCEVTypeCalculation(std::vector<SCEVType> SCEVTypeCombination) {

  if (std::find(SCEVTypeCombination.begin(), SCEVTypeCombination.end(), SEUnknown) != SCEVTypeCombination.end()) {
    return SEUnknown;
  }

  for (auto type : SCEVTypeCombination) {
    if (type == SEIncrease) {
      return SEIncrease;
    } 
    if (type == SEDecrease) {
      return SEDecrease;
    } 
  }
  return SEUnknown;
}

enum SCEVType getInitValueFromSCEV(const SCEV *Expr, Value **initValue, ScalarEvolution *SE, Loop *L, int64_t &initIndex, int64_t &stepSize) {

  if (const SCEVConstant *SC = dyn_cast<SCEVConstant>(Expr)) {
    *initValue = SC->getValue();
    return SEConstant;
  }

  if (const SCEVAddRecExpr *AddRec = dyn_cast<SCEVAddRecExpr>(Expr)) {
    const SCEV *start = AddRec->getStart();
    if (const SCEVConstant *init = dyn_cast<SCEVConstant>(start)) {
      auto *index = init->getValue();
      int64_t indexint = index->getSExtValue();
      initIndex = indexint;
    }
    SCEVType initType = getInitValueFromSCEV(start, initValue, SE, L, initIndex, stepSize);
    if (initType == SEUnknown) {
      return SEUnknown;
    }

    const SCEV *Step = AddRec->getStepRecurrence(*SE);
    if (const SCEVConstant *SC = dyn_cast<SCEVConstant>(Step)) {
      auto *StepRecurrence = SC->getValue();
      int64_t StepRecurrenceInt = StepRecurrence->getSExtValue();
      stepSize = StepRecurrenceInt;
      if (StepRecurrenceInt < 0) {
        return SEDecrease;
      }
      if (StepRecurrenceInt > 0) {
        return SEIncrease;  
      }
    }
    return SEUnknown;
  }

  if (const SCEVUnknown *SU = dyn_cast<SCEVUnknown>(Expr)) {
    *initValue = SU->getValue();
    std::vector<Value *> backs;
    std::vector<Value *> processedAddr;

    if (checkAddrType(*initValue, backs, processedAddr, SE, L) == IBIO) {
      return SELoopInvariant;
    }
    return SEUnknown;
  }

  if (const SCEVAddExpr *AddExpr = dyn_cast<SCEVAddExpr>(Expr)) {
    std::vector<SCEVType> SCEVTypeCombination;
    SCEVTypeCombination.push_back(getInitValueFromSCEV(AddExpr->getOperand(0), initValue, SE, L, initIndex, stepSize));
    for (unsigned i = 1, e = AddExpr->getNumOperands(); i != e; ++i) {
      SCEVTypeCombination.push_back(getInitValueFromSCEV(AddExpr->getOperand(i), initValue, SE, L, initIndex, stepSize));
    }
    return SCEVTypeCalculation(SCEVTypeCombination);
  }

  if (const SCEVMulExpr *MulExpr = dyn_cast<SCEVMulExpr>(Expr)) {
    std::vector<SCEVType> SCEVTypeCombination;
    SCEVTypeCombination.push_back(getInitValueFromSCEV(MulExpr->getOperand(0), initValue, SE, L, initIndex, stepSize));
    for (unsigned i = 1, e = MulExpr->getNumOperands(); i != e; ++i) {
      SCEVTypeCombination.push_back(getInitValueFromSCEV(MulExpr->getOperand(i), initValue, SE, L, initIndex, stepSize));
    }
    return SCEVTypeCalculation(SCEVTypeCombination);
  }

  if (const SCEVUDivExpr *UDivExpr = dyn_cast<SCEVUDivExpr>(Expr)) {
    std::vector<SCEVType> SCEVTypeCombination;
    SCEVTypeCombination.push_back(getInitValueFromSCEV(UDivExpr->getLHS(), initValue, SE, L, initIndex, stepSize));
    SCEVTypeCombination.push_back(getInitValueFromSCEV(UDivExpr->getRHS(), initValue, SE, L, initIndex, stepSize));
    return SCEVTypeCalculation(SCEVTypeCombination);
  }

  if (const SCEVSMaxExpr *SMaxExpr = dyn_cast<SCEVSMaxExpr>(Expr)) {
    std::vector<SCEVType> SCEVTypeCombination;
    SCEVTypeCombination.push_back(getInitValueFromSCEV(SMaxExpr->getOperand(0), initValue, SE, L, initIndex, stepSize));
    SCEVTypeCombination.push_back(getInitValueFromSCEV(SMaxExpr->getOperand(1), initValue, SE, L, initIndex, stepSize));
    return SCEVTypeCalculation(SCEVTypeCombination);
  }

  if (const SCEVUMaxExpr *UMaxExpr = dyn_cast<SCEVUMaxExpr>(Expr)) {
    std::vector<SCEVType> SCEVTypeCombination;
    SCEVTypeCombination.push_back(getInitValueFromSCEV(UMaxExpr->getOperand(0), initValue, SE, L, initIndex, stepSize));
    SCEVTypeCombination.push_back(getInitValueFromSCEV(UMaxExpr->getOperand(1), initValue, SE, L, initIndex, stepSize));
    return SCEVTypeCalculation(SCEVTypeCombination);
  }

  if (const SCEVTruncateExpr *Truncate = dyn_cast<SCEVTruncateExpr>(Expr)) {
    const SCEV *op = Truncate->getOperand();
    SCEVType TruncateSCEVType = getInitValueFromSCEV(op, initValue, SE, L, initIndex, stepSize);
    // The bit size of the value must be larger than the bit size of the destination type, ty2.
    return TruncateSCEVType;
  }

  if (const SCEVZeroExtendExpr *ZeroExtend = dyn_cast<SCEVZeroExtendExpr>(Expr)) {
    const SCEV *op = ZeroExtend->getOperand();
    SCEVType ZeroExtendSCEVType = getInitValueFromSCEV(op, initValue, SE, L, initIndex, stepSize);
    // The bit size of the value must be smaller than the bit size of the destination type, ty2.
    return ZeroExtendSCEVType;
  }

  if (const SCEVSignExtendExpr *SignExtend = dyn_cast<SCEVSignExtendExpr>(Expr)) {
    const SCEV *op = SignExtend->getOperand();
    SCEVType SignExtendSCEVType = getInitValueFromSCEV(op, initValue, SE, L, initIndex, stepSize);
    // The bit size of the value must be smaller than the bit size of the destination type, ty2.
    return SignExtendSCEVType;
  }

  if (const SCEVCouldNotCompute *CNC = dyn_cast<SCEVCouldNotCompute>(Expr)) {
    return SEUnknown;
  }

  return SEUnknown;
}

void AddressSanitizer::MonotonicOptimizeHandler(Loop *L, std::set<Instruction *> &optimized, 
                        Function &F, InterestingMemoryOperand &Oper, ObjectSizeOffsetVisitor &ObjSizeVis, bool UseCalls, ScalarEvolution *SE) {
  
  bool IsWrite = Oper.IsWrite;
  MaybeAlign Alignment = Oper.Alignment;
	uint64_t TypeSize = Oper.TypeSize;
  unsigned Granularity = 1 << Mapping.Scale;
  Instruction *Inst = Oper.getInsn();
  auto DT = DominatorTree(F);
  auto ExitBB = L->getExitBlock();

  if (!ExitBB) {
    return;
  }

  auto exitInst = (*ExitBB).getFirstNonPHI();

  Value *addr = Oper.getPtr();
  if (!addr) 
    return;
  
  const SCEV *PtrSCEVA = SE->getSCEV(addr);
  
  Value *initValue;

  int64_t initIndex;

  int64_t stepSize;

  SCEVType initType = getInitValueFromSCEV(PtrSCEVA, &initValue, SE, L, initIndex, stepSize);

  if (initType == SEUnknown) {
    return;
  }

  if (stepSize > MAX_STEP_SIZE) {
    return;
  }

  IRBuilder<> IRBinsertCheck(Inst);

  Value *AddrLong = IRBinsertCheck.CreatePointerCast(addr, IntptrTy);

  Value *RHS = ConstantInt::get(IntptrTy, CHECK_RANGE_LOOP);

  Value *ModInst = IRBinsertCheck.CreateURem(AddrLong, RHS);

  Value *StepSizeLong = ConstantInt::get(IntptrTy, std::abs(stepSize));

  Value *Cmp = IRBinsertCheck.CreateICmpULT(ModInst, StepSizeLong);

  Instruction *CheckTerm = SplitBlockAndInsertIfThen(Cmp, Inst, false);

  IRBuilder<> IRBasanCheck(CheckTerm);

  if ((TypeSize == 8 || TypeSize == 16 || TypeSize == 32 || TypeSize == 64 || TypeSize == 128) && (!Alignment || *Alignment >= Granularity || *Alignment >= TypeSize / 8)) {
    instrumentAddress(CheckTerm, CheckTerm, addr, TypeSize, IsWrite, nullptr, UseCalls, 0);
  } else {
    instrumentUnusualSizeOrAlignment(CheckTerm, CheckTerm, addr, TypeSize, IsWrite, nullptr, UseCalls, 0);
  }

  if (DT.dominates(Inst, ExitBB)) {

    IRBuilder<> IRBreChk(exitInst);
    Value *InitValue = IRBreChk.CreatePointerCast(initValue, IntptrTy);
    Value *ExitCmp = IRBreChk.CreateICmpNE(InitValue, AddrLong);
    Instruction *ExitCheckTerm = SplitBlockAndInsertIfThen(ExitCmp, exitInst, false);
    if ((TypeSize == 8 || TypeSize == 16 || TypeSize == 32 || TypeSize == 64 || TypeSize == 128) && (!Alignment || *Alignment >= Granularity || *Alignment >= TypeSize / 8)) {
        instrumentAddress(ExitCheckTerm, ExitCheckTerm, addr, TypeSize, IsWrite, nullptr, UseCalls, 0);
    } else {
        instrumentUnusualSizeOrAlignment(ExitCheckTerm, ExitCheckTerm, addr, TypeSize, IsWrite, nullptr, UseCalls, 0);
    }
  }

  optimized.insert(Inst);
  return;
  
}

enum addrType AddressSanitizer::loopOptimizationCategorise(Function &F, Loop *L, InterestingMemoryOperand Oper, ScalarEvolution *SE) {

  std::vector<Value *> backs;
  std::vector<Value *> processedAddr;

  if (Value* addr = Oper.getPtr()) {
	  btraceInLoop(addr, backs, L);
	  return checkAddrType(addr, backs, processedAddr, SE, L);
  }
  return UNKNOWN; 
}

void AddressSanitizer::loopOptimization(Function &F, SmallVector<InterestingMemoryOperand, 16> &OperandsToInstrument, LoopInfo *LI, ScalarEvolution *SE, ObjectSizeOffsetVisitor &ObjSizeVis, bool UseCalls) {

  std::set<Instruction *> optimized; 
  for (auto Oper : OperandsToInstrument) {
    // Check if current instruction is inside loop
    if (Loop *L = LI->getLoopFor(Oper.getInsn()->getParent())) {
      // We will categorise the type of optimization
      if (loopOptimizationCategorise(F, L, Oper, SE) == IBIO) {
        // optimized.insert(Oper.getInsn());
        InvariantOptimizeHandler(L, optimized, F, Oper, ObjSizeVis, UseCalls);
      } else {
        // optimized.insert(Oper.getInsn());
        MonotonicOptimizeHandler(L, optimized, F, Oper, ObjSizeVis, UseCalls, SE);
      }
    } 
  }

  // errs() << "Original Size: " << OperandsToInstrument.size() << "\n";
	SmallVector<InterestingMemoryOperand, 16> LOTempToInstrument(OperandsToInstrument);
	OperandsToInstrument.clear();

  // errs() << "Remove Size: " << optimized.size() << "\n";

	for (auto item: LOTempToInstrument) {
		if (optimized.find(item.getInsn()) == optimized.end())
			OperandsToInstrument.push_back(item);
	}
  // errs() << "After Size: " << OperandsToInstrument.size() << "\n";

}

void AddressSanitizer::slimAsanOptimization(Function &F, SmallVector<InterestingMemoryOperand, 16> &OperandsToInstrument, AliasAnalysis *AA, LoopInfo *LI, ScalarEvolution *SE, ObjectSizeOffsetVisitor &ObjSizeVis, bool UseCalls) {

  // ASAN--: Removing Recurring Checks
  sequentialExecuteOptimizationPostDom(F, OperandsToInstrument, AA);

  sequentialExecuteOptimization(F, OperandsToInstrument, AA);
  // ASAN--: Optimizing Neighbor Checks
  sequentialExecuteOptimizationBoost(F, OperandsToInstrument);
  // ASAN--: Optimizing Checks in Loops
  loopOptimization(F, OperandsToInstrument, LI, SE, ObjSizeVis, UseCalls);

}

bool AddressSanitizer::instrumentFunction(Function &F,
                                          const TargetLibraryInfo *TLI, AliasAnalysis *AA, LoopInfo *LI, ScalarEvolution *SE) {
  if (F.getLinkage() == GlobalValue::AvailableExternallyLinkage) return false;
  if (!ClDebugFunc.empty() && ClDebugFunc == F.getName()) return false;
  if (F.getName().startswith("__asan_")) return false;

  bool FunctionModified = false;

  // If needed, insert __asan_init before checking for SanitizeAddress attr.
  // This function needs to be called even if the function body is not
  // instrumented.
  if (maybeInsertAsanInitAtFunctionEntry(F))
    FunctionModified = true;

  // Leave if the function doesn't need instrumentation.
  if (!F.hasFnAttribute(Attribute::SanitizeAddress)) return FunctionModified;

  LLVM_DEBUG(dbgs() << "ASAN instrumenting:\n" << F << "\n");

  initializeCallbacks(*F.getParent());

  FunctionStateRAII CleanupObj(this);

  FunctionModified |= maybeInsertDynamicShadowAtFunctionEntry(F);

  // We can't instrument allocas used with llvm.localescape. Only static allocas
  // can be passed to that intrinsic.
  markEscapedLocalAllocas(F);

  // We want to instrument every address only once per basic block (unless there
  // are calls between uses).
  SmallPtrSet<Value *, 16> TempsToInstrument;
  SmallVector<InterestingMemoryOperand, 16> OperandsToInstrument;
  SmallVector<MemIntrinsic *, 16> IntrinToInstrument;
  SmallVector<Instruction *, 8> NoReturnCalls;
  SmallVector<BasicBlock *, 16> AllBlocks;
  SmallVector<Instruction *, 16> PointerComparisonsOrSubtracts;
  int NumAllocas = 0;

  // Fill the set of memory operations to instrument.
  for (auto &BB : F) {
    AllBlocks.push_back(&BB);
    TempsToInstrument.clear();
    int NumInsnsPerBB = 0;
    for (auto &Inst : BB) {
      if (LooksLikeCodeInBug11395(&Inst)) return false;
      SmallVector<InterestingMemoryOperand, 1> InterestingOperands;
      getInterestingMemoryOperands(&Inst, InterestingOperands);

      if (!InterestingOperands.empty()) {
        for (auto &Operand : InterestingOperands) {
          if (ClOpt && ClOptSameTemp) {
            Value *Ptr = Operand.getPtr();
            // If we have a mask, skip instrumentation if we've already
            // instrumented the full object. But don't add to TempsToInstrument
            // because we might get another load/store with a different mask.
            if (Operand.MaybeMask) {
              if (TempsToInstrument.count(Ptr))
                continue; // We've seen this (whole) temp in the current BB.
            } else {
              if (!TempsToInstrument.insert(Ptr).second)
                continue; // We've seen this temp in the current BB.
            }
          }
          OperandsToInstrument.push_back(Operand);
          NumInsnsPerBB++;
        }
      } else if (((ClInvalidPointerPairs || ClInvalidPointerCmp) &&
                  isInterestingPointerComparison(&Inst)) ||
                 ((ClInvalidPointerPairs || ClInvalidPointerSub) &&
                  isInterestingPointerSubtraction(&Inst))) {
        PointerComparisonsOrSubtracts.push_back(&Inst);
      } else if (MemIntrinsic *MI = dyn_cast<MemIntrinsic>(&Inst)) {
        // ok, take it.
        IntrinToInstrument.push_back(MI);
        NumInsnsPerBB++;
      } else {
        if (isa<AllocaInst>(Inst)) NumAllocas++;
        if (auto *CB = dyn_cast<CallBase>(&Inst)) {
          // A call inside BB.
          TempsToInstrument.clear();
          if (CB->doesNotReturn() && !CB->hasMetadata("nosanitize"))
            NoReturnCalls.push_back(CB);
        }
        if (CallInst *CI = dyn_cast<CallInst>(&Inst))
          maybeMarkSanitizerLibraryCallNoBuiltin(CI, TLI);
      }
      if (NumInsnsPerBB >= ClMaxInsnsToInstrumentPerBB) break;
    }
  }

  bool UseCalls = (ClInstrumentationWithCallsThreshold >= 0 &&
                   OperandsToInstrument.size() + IntrinToInstrument.size() >
                       (unsigned)ClInstrumentationWithCallsThreshold);
  const DataLayout &DL = F.getParent()->getDataLayout();
  ObjectSizeOpts ObjSizeOpts;
  ObjSizeOpts.RoundToAlign = true;
  ObjectSizeOffsetVisitor ObjSizeVis(DL, TLI, F.getContext(), ObjSizeOpts);

  // ASAN-- Optimizations
  slimAsanOptimization(F, OperandsToInstrument, AA, LI, SE, ObjSizeVis, UseCalls);

  // Instrument.
  int NumInstrumented = 0;
  for (auto &Operand : OperandsToInstrument) {
    if (!suppressInstrumentationSiteForDebug(NumInstrumented))
      instrumentMop(ObjSizeVis, Operand, UseCalls,
                    F.getParent()->getDataLayout());
    FunctionModified = true;
  }
  for (auto Inst : IntrinToInstrument) {
    if (!suppressInstrumentationSiteForDebug(NumInstrumented))
      instrumentMemIntrinsic(Inst);
    FunctionModified = true;
  }

  FunctionStackPoisoner FSP(F, *this);
  bool ChangedStack = FSP.runOnFunction();

  // We must unpoison the stack before NoReturn calls (throw, _exit, etc).
  // See e.g. https://github.com/google/sanitizers/issues/37
  for (auto CI : NoReturnCalls) {
    IRBuilder<> IRB(CI);
    IRB.CreateCall(AsanHandleNoReturnFunc, {});
  }

  for (auto Inst : PointerComparisonsOrSubtracts) {
    instrumentPointerComparisonOrSubtraction(Inst);
    FunctionModified = true;
  }

  if (ChangedStack || !NoReturnCalls.empty())
    FunctionModified = true;

  LLVM_DEBUG(dbgs() << "ASAN done instrumenting: " << FunctionModified << " "
                    << F << "\n");

  return FunctionModified;
}

// Workaround for bug 11395: we don't want to instrument stack in functions
// with large assembly blobs (32-bit only), otherwise reg alloc may crash.
// FIXME: remove once the bug 11395 is fixed.
bool AddressSanitizer::LooksLikeCodeInBug11395(Instruction *I) {
  if (LongSize != 32) return false;
  CallInst *CI = dyn_cast<CallInst>(I);
  if (!CI || !CI->isInlineAsm()) return false;
  if (CI->getNumArgOperands() <= 5) return false;
  // We have inline assembly with quite a few arguments.
  return true;
}

void FunctionStackPoisoner::initializeCallbacks(Module &M) {
  IRBuilder<> IRB(*C);
  for (int i = 0; i <= kMaxAsanStackMallocSizeClass; i++) {
    std::string Suffix = itostr(i);
    AsanStackMallocFunc[i] = M.getOrInsertFunction(
        kAsanStackMallocNameTemplate + Suffix, IntptrTy, IntptrTy);
    AsanStackFreeFunc[i] =
        M.getOrInsertFunction(kAsanStackFreeNameTemplate + Suffix,
                              IRB.getVoidTy(), IntptrTy, IntptrTy);
  }
  if (ASan.UseAfterScope) {
    AsanPoisonStackMemoryFunc = M.getOrInsertFunction(
        kAsanPoisonStackMemoryName, IRB.getVoidTy(), IntptrTy, IntptrTy);
    AsanUnpoisonStackMemoryFunc = M.getOrInsertFunction(
        kAsanUnpoisonStackMemoryName, IRB.getVoidTy(), IntptrTy, IntptrTy);
  }

  for (size_t Val : {0x00, 0xf1, 0xf2, 0xf3, 0xf5, 0xf8}) {
    std::ostringstream Name;
    Name << kAsanSetShadowPrefix;
    Name << std::setw(2) << std::setfill('0') << std::hex << Val;
    AsanSetShadowFunc[Val] =
        M.getOrInsertFunction(Name.str(), IRB.getVoidTy(), IntptrTy, IntptrTy);
  }

  AsanAllocaPoisonFunc = M.getOrInsertFunction(
      kAsanAllocaPoison, IRB.getVoidTy(), IntptrTy, IntptrTy);
  AsanAllocasUnpoisonFunc = M.getOrInsertFunction(
      kAsanAllocasUnpoison, IRB.getVoidTy(), IntptrTy, IntptrTy);
}

void FunctionStackPoisoner::copyToShadowInline(ArrayRef<uint8_t> ShadowMask,
                                               ArrayRef<uint8_t> ShadowBytes,
                                               size_t Begin, size_t End,
                                               IRBuilder<> &IRB,
                                               Value *ShadowBase) {
  if (Begin >= End)
    return;

  const size_t LargestStoreSizeInBytes =
      std::min<size_t>(sizeof(uint64_t), ASan.LongSize / 8);

  const bool IsLittleEndian = F.getParent()->getDataLayout().isLittleEndian();

  // Poison given range in shadow using larges store size with out leading and
  // trailing zeros in ShadowMask. Zeros never change, so they need neither
  // poisoning nor up-poisoning. Still we don't mind if some of them get into a
  // middle of a store.
  for (size_t i = Begin; i < End;) {
    if (!ShadowMask[i]) {
      assert(!ShadowBytes[i]);
      ++i;
      continue;
    }

    size_t StoreSizeInBytes = LargestStoreSizeInBytes;
    // Fit store size into the range.
    while (StoreSizeInBytes > End - i)
      StoreSizeInBytes /= 2;

    // Minimize store size by trimming trailing zeros.
    for (size_t j = StoreSizeInBytes - 1; j && !ShadowMask[i + j]; --j) {
      while (j <= StoreSizeInBytes / 2)
        StoreSizeInBytes /= 2;
    }

    uint64_t Val = 0;
    for (size_t j = 0; j < StoreSizeInBytes; j++) {
      if (IsLittleEndian)
        Val |= (uint64_t)ShadowBytes[i + j] << (8 * j);
      else
        Val = (Val << 8) | ShadowBytes[i + j];
    }

    Value *Ptr = IRB.CreateAdd(ShadowBase, ConstantInt::get(IntptrTy, i));
    Value *Poison = IRB.getIntN(StoreSizeInBytes * 8, Val);
    IRB.CreateAlignedStore(
        Poison, IRB.CreateIntToPtr(Ptr, Poison->getType()->getPointerTo()),
        Align(1));

    i += StoreSizeInBytes;
  }
}

void FunctionStackPoisoner::copyToShadow(ArrayRef<uint8_t> ShadowMask,
                                         ArrayRef<uint8_t> ShadowBytes,
                                         IRBuilder<> &IRB, Value *ShadowBase) {
  copyToShadow(ShadowMask, ShadowBytes, 0, ShadowMask.size(), IRB, ShadowBase);
}

void FunctionStackPoisoner::copyToShadow(ArrayRef<uint8_t> ShadowMask,
                                         ArrayRef<uint8_t> ShadowBytes,
                                         size_t Begin, size_t End,
                                         IRBuilder<> &IRB, Value *ShadowBase) {
  assert(ShadowMask.size() == ShadowBytes.size());
  size_t Done = Begin;
  for (size_t i = Begin, j = Begin + 1; i < End; i = j++) {
    if (!ShadowMask[i]) {
      assert(!ShadowBytes[i]);
      continue;
    }
    uint8_t Val = ShadowBytes[i];
    if (!AsanSetShadowFunc[Val])
      continue;

    // Skip same values.
    for (; j < End && ShadowMask[j] && Val == ShadowBytes[j]; ++j) {
    }

    if (j - i >= ClMaxInlinePoisoningSize) {
      copyToShadowInline(ShadowMask, ShadowBytes, Done, i, IRB, ShadowBase);
      IRB.CreateCall(AsanSetShadowFunc[Val],
                     {IRB.CreateAdd(ShadowBase, ConstantInt::get(IntptrTy, i)),
                      ConstantInt::get(IntptrTy, j - i)});
      Done = j;
    }
  }

  copyToShadowInline(ShadowMask, ShadowBytes, Done, End, IRB, ShadowBase);
}

// Fake stack allocator (asan_fake_stack.h) has 11 size classes
// for every power of 2 from kMinStackMallocSize to kMaxAsanStackMallocSizeClass
static int StackMallocSizeClass(uint64_t LocalStackSize) {
  assert(LocalStackSize <= kMaxStackMallocSize);
  uint64_t MaxSize = kMinStackMallocSize;
  for (int i = 0;; i++, MaxSize *= 2)
    if (LocalStackSize <= MaxSize) return i;
  llvm_unreachable("impossible LocalStackSize");
}

void FunctionStackPoisoner::copyArgsPassedByValToAllocas() {
  Instruction *CopyInsertPoint = &F.front().front();
  if (CopyInsertPoint == ASan.LocalDynamicShadow) {
    // Insert after the dynamic shadow location is determined
    CopyInsertPoint = CopyInsertPoint->getNextNode();
    assert(CopyInsertPoint);
  }
  IRBuilder<> IRB(CopyInsertPoint);
  const DataLayout &DL = F.getParent()->getDataLayout();
  for (Argument &Arg : F.args()) {
    if (Arg.hasByValAttr()) {
      Type *Ty = Arg.getParamByValType();
      const Align Alignment =
          DL.getValueOrABITypeAlignment(Arg.getParamAlign(), Ty);

      AllocaInst *AI = IRB.CreateAlloca(
          Ty, nullptr,
          (Arg.hasName() ? Arg.getName() : "Arg" + Twine(Arg.getArgNo())) +
              ".byval");
      AI->setAlignment(Alignment);
      Arg.replaceAllUsesWith(AI);

      uint64_t AllocSize = DL.getTypeAllocSize(Ty);
      IRB.CreateMemCpy(AI, Alignment, &Arg, Alignment, AllocSize);
    }
  }
}

PHINode *FunctionStackPoisoner::createPHI(IRBuilder<> &IRB, Value *Cond,
                                          Value *ValueIfTrue,
                                          Instruction *ThenTerm,
                                          Value *ValueIfFalse) {
  PHINode *PHI = IRB.CreatePHI(IntptrTy, 2);
  BasicBlock *CondBlock = cast<Instruction>(Cond)->getParent();
  PHI->addIncoming(ValueIfFalse, CondBlock);
  BasicBlock *ThenBlock = ThenTerm->getParent();
  PHI->addIncoming(ValueIfTrue, ThenBlock);
  return PHI;
}

Value *FunctionStackPoisoner::createAllocaForLayout(
    IRBuilder<> &IRB, const ASanStackFrameLayout &L, bool Dynamic) {
  AllocaInst *Alloca;
  if (Dynamic) {
    Alloca = IRB.CreateAlloca(IRB.getInt8Ty(),
                              ConstantInt::get(IRB.getInt64Ty(), L.FrameSize),
                              "MyAlloca");
  } else {
    Alloca = IRB.CreateAlloca(ArrayType::get(IRB.getInt8Ty(), L.FrameSize),
                              nullptr, "MyAlloca");
    assert(Alloca->isStaticAlloca());
  }
  assert((ClRealignStack & (ClRealignStack - 1)) == 0);
  size_t FrameAlignment = std::max(L.FrameAlignment, (size_t)ClRealignStack);
  Alloca->setAlignment(Align(FrameAlignment));
  return IRB.CreatePointerCast(Alloca, IntptrTy);
}

void FunctionStackPoisoner::createDynamicAllocasInitStorage() {
  BasicBlock &FirstBB = *F.begin();
  IRBuilder<> IRB(dyn_cast<Instruction>(FirstBB.begin()));
  DynamicAllocaLayout = IRB.CreateAlloca(IntptrTy, nullptr);
  IRB.CreateStore(Constant::getNullValue(IntptrTy), DynamicAllocaLayout);
  DynamicAllocaLayout->setAlignment(Align(32));
}

void FunctionStackPoisoner::processDynamicAllocas() {
  if (!ClInstrumentDynamicAllocas || DynamicAllocaVec.empty()) {
    assert(DynamicAllocaPoisonCallVec.empty());
    return;
  }

  // Insert poison calls for lifetime intrinsics for dynamic allocas.
  for (const auto &APC : DynamicAllocaPoisonCallVec) {
    assert(APC.InsBefore);
    assert(APC.AI);
    assert(ASan.isInterestingAlloca(*APC.AI));
    assert(!APC.AI->isStaticAlloca());

    IRBuilder<> IRB(APC.InsBefore);
    poisonAlloca(APC.AI, APC.Size, IRB, APC.DoPoison);
    // Dynamic allocas will be unpoisoned unconditionally below in
    // unpoisonDynamicAllocas.
    // Flag that we need unpoison static allocas.
  }

  // Handle dynamic allocas.
  createDynamicAllocasInitStorage();
  for (auto &AI : DynamicAllocaVec)
    handleDynamicAllocaCall(AI);
  unpoisonDynamicAllocas();
}

/// Collect instructions in the entry block after \p InsBefore which initialize
/// permanent storage for a function argument. These instructions must remain in
/// the entry block so that uninitialized values do not appear in backtraces. An
/// added benefit is that this conserves spill slots. This does not move stores
/// before instrumented / "interesting" allocas.
static void findStoresToUninstrumentedArgAllocas(
    AddressSanitizer &ASan, Instruction &InsBefore,
    SmallVectorImpl<Instruction *> &InitInsts) {
  Instruction *Start = InsBefore.getNextNonDebugInstruction();
  for (Instruction *It = Start; It; It = It->getNextNonDebugInstruction()) {
    // Argument initialization looks like:
    // 1) store <Argument>, <Alloca> OR
    // 2) <CastArgument> = cast <Argument> to ...
    //    store <CastArgument> to <Alloca>
    // Do not consider any other kind of instruction.
    //
    // Note: This covers all known cases, but may not be exhaustive. An
    // alternative to pattern-matching stores is to DFS over all Argument uses:
    // this might be more general, but is probably much more complicated.
    if (isa<AllocaInst>(It) || isa<CastInst>(It))
      continue;
    if (auto *Store = dyn_cast<StoreInst>(It)) {
      // The store destination must be an alloca that isn't interesting for
      // ASan to instrument. These are moved up before InsBefore, and they're
      // not interesting because allocas for arguments can be mem2reg'd.
      auto *Alloca = dyn_cast<AllocaInst>(Store->getPointerOperand());
      if (!Alloca || ASan.isInterestingAlloca(*Alloca))
        continue;

      Value *Val = Store->getValueOperand();
      bool IsDirectArgInit = isa<Argument>(Val);
      bool IsArgInitViaCast =
          isa<CastInst>(Val) &&
          isa<Argument>(cast<CastInst>(Val)->getOperand(0)) &&
          // Check that the cast appears directly before the store. Otherwise
          // moving the cast before InsBefore may break the IR.
          Val == It->getPrevNonDebugInstruction();
      bool IsArgInit = IsDirectArgInit || IsArgInitViaCast;
      if (!IsArgInit)
        continue;

      if (IsArgInitViaCast)
        InitInsts.push_back(cast<Instruction>(Val));
      InitInsts.push_back(Store);
      continue;
    }

    // Do not reorder past unknown instructions: argument initialization should
    // only involve casts and stores.
    return;
  }
}

void FunctionStackPoisoner::processStaticAllocas() {
  if (AllocaVec.empty()) {
    assert(StaticAllocaPoisonCallVec.empty());
    return;
  }

  int StackMallocIdx = -1;
  DebugLoc EntryDebugLocation;
  if (auto SP = F.getSubprogram())
    EntryDebugLocation =
        DILocation::get(SP->getContext(), SP->getScopeLine(), 0, SP);

  Instruction *InsBefore = AllocaVec[0];
  IRBuilder<> IRB(InsBefore);

  // Make sure non-instrumented allocas stay in the entry block. Otherwise,
  // debug info is broken, because only entry-block allocas are treated as
  // regular stack slots.
  auto InsBeforeB = InsBefore->getParent();
  assert(InsBeforeB == &F.getEntryBlock());
  for (auto *AI : StaticAllocasToMoveUp)
    if (AI->getParent() == InsBeforeB)
      AI->moveBefore(InsBefore);

  // Move stores of arguments into entry-block allocas as well. This prevents
  // extra stack slots from being generated (to house the argument values until
  // they can be stored into the allocas). This also prevents uninitialized
  // values from being shown in backtraces.
  SmallVector<Instruction *, 8> ArgInitInsts;
  findStoresToUninstrumentedArgAllocas(ASan, *InsBefore, ArgInitInsts);
  for (Instruction *ArgInitInst : ArgInitInsts)
    ArgInitInst->moveBefore(InsBefore);

  // If we have a call to llvm.localescape, keep it in the entry block.
  if (LocalEscapeCall) LocalEscapeCall->moveBefore(InsBefore);

  SmallVector<ASanStackVariableDescription, 16> SVD;
  SVD.reserve(AllocaVec.size());
  for (AllocaInst *AI : AllocaVec) {
    ASanStackVariableDescription D = {AI->getName().data(),
                                      ASan.getAllocaSizeInBytes(*AI),
                                      0,
                                      AI->getAlignment(),
                                      AI,
                                      0,
                                      0};
    SVD.push_back(D);
  }

  // Minimal header size (left redzone) is 4 pointers,
  // i.e. 32 bytes on 64-bit platforms and 16 bytes in 32-bit platforms.
  size_t Granularity = 1ULL << Mapping.Scale;
  size_t MinHeaderSize = std::max((size_t)ASan.LongSize / 2, Granularity);
  const ASanStackFrameLayout &L =
      ComputeASanStackFrameLayout(SVD, Granularity, MinHeaderSize);

  // Build AllocaToSVDMap for ASanStackVariableDescription lookup.
  DenseMap<const AllocaInst *, ASanStackVariableDescription *> AllocaToSVDMap;
  for (auto &Desc : SVD)
    AllocaToSVDMap[Desc.AI] = &Desc;

  // Update SVD with information from lifetime intrinsics.
  for (const auto &APC : StaticAllocaPoisonCallVec) {
    assert(APC.InsBefore);
    assert(APC.AI);
    assert(ASan.isInterestingAlloca(*APC.AI));
    assert(APC.AI->isStaticAlloca());

    ASanStackVariableDescription &Desc = *AllocaToSVDMap[APC.AI];
    Desc.LifetimeSize = Desc.Size;
    if (const DILocation *FnLoc = EntryDebugLocation.get()) {
      if (const DILocation *LifetimeLoc = APC.InsBefore->getDebugLoc().get()) {
        if (LifetimeLoc->getFile() == FnLoc->getFile())
          if (unsigned Line = LifetimeLoc->getLine())
            Desc.Line = std::min(Desc.Line ? Desc.Line : Line, Line);
      }
    }
  }

  auto DescriptionString = ComputeASanStackFrameDescription(SVD);
  LLVM_DEBUG(dbgs() << DescriptionString << " --- " << L.FrameSize << "\n");
  uint64_t LocalStackSize = L.FrameSize;
  bool DoStackMalloc = ClUseAfterReturn && !ASan.CompileKernel &&
                       LocalStackSize <= kMaxStackMallocSize;
  bool DoDynamicAlloca = ClDynamicAllocaStack;
  // Don't do dynamic alloca or stack malloc if:
  // 1) There is inline asm: too often it makes assumptions on which registers
  //    are available.
  // 2) There is a returns_twice call (typically setjmp), which is
  //    optimization-hostile, and doesn't play well with introduced indirect
  //    register-relative calculation of local variable addresses.
  DoDynamicAlloca &= !HasInlineAsm && !HasReturnsTwiceCall;
  DoStackMalloc &= !HasInlineAsm && !HasReturnsTwiceCall;

  Value *StaticAlloca =
      DoDynamicAlloca ? nullptr : createAllocaForLayout(IRB, L, false);

  Value *FakeStack;
  Value *LocalStackBase;
  Value *LocalStackBaseAlloca;
  uint8_t DIExprFlags = DIExpression::ApplyOffset;

  if (DoStackMalloc) {
    LocalStackBaseAlloca =
        IRB.CreateAlloca(IntptrTy, nullptr, "asan_local_stack_base");
    // void *FakeStack = __asan_option_detect_stack_use_after_return
    //     ? __asan_stack_malloc_N(LocalStackSize)
    //     : nullptr;
    // void *LocalStackBase = (FakeStack) ? FakeStack : alloca(LocalStackSize);
    Constant *OptionDetectUseAfterReturn = F.getParent()->getOrInsertGlobal(
        kAsanOptionDetectUseAfterReturn, IRB.getInt32Ty());
    Value *UseAfterReturnIsEnabled = IRB.CreateICmpNE(
        IRB.CreateLoad(IRB.getInt32Ty(), OptionDetectUseAfterReturn),
        Constant::getNullValue(IRB.getInt32Ty()));
    Instruction *Term =
        SplitBlockAndInsertIfThen(UseAfterReturnIsEnabled, InsBefore, false);
    IRBuilder<> IRBIf(Term);
    StackMallocIdx = StackMallocSizeClass(LocalStackSize);
    assert(StackMallocIdx <= kMaxAsanStackMallocSizeClass);
    Value *FakeStackValue =
        IRBIf.CreateCall(AsanStackMallocFunc[StackMallocIdx],
                         ConstantInt::get(IntptrTy, LocalStackSize));
    IRB.SetInsertPoint(InsBefore);
    FakeStack = createPHI(IRB, UseAfterReturnIsEnabled, FakeStackValue, Term,
                          ConstantInt::get(IntptrTy, 0));

    Value *NoFakeStack =
        IRB.CreateICmpEQ(FakeStack, Constant::getNullValue(IntptrTy));
    Term = SplitBlockAndInsertIfThen(NoFakeStack, InsBefore, false);
    IRBIf.SetInsertPoint(Term);
    Value *AllocaValue =
        DoDynamicAlloca ? createAllocaForLayout(IRBIf, L, true) : StaticAlloca;

    IRB.SetInsertPoint(InsBefore);
    LocalStackBase = createPHI(IRB, NoFakeStack, AllocaValue, Term, FakeStack);
    IRB.CreateStore(LocalStackBase, LocalStackBaseAlloca);
    DIExprFlags |= DIExpression::DerefBefore;
  } else {
    // void *FakeStack = nullptr;
    // void *LocalStackBase = alloca(LocalStackSize);
    FakeStack = ConstantInt::get(IntptrTy, 0);
    LocalStackBase =
        DoDynamicAlloca ? createAllocaForLayout(IRB, L, true) : StaticAlloca;
    LocalStackBaseAlloca = LocalStackBase;
  }

  // It shouldn't matter whether we pass an `alloca` or a `ptrtoint` as the
  // dbg.declare address opereand, but passing a `ptrtoint` seems to confuse
  // later passes and can result in dropped variable coverage in debug info.
  Value *LocalStackBaseAllocaPtr =
      isa<PtrToIntInst>(LocalStackBaseAlloca)
          ? cast<PtrToIntInst>(LocalStackBaseAlloca)->getPointerOperand()
          : LocalStackBaseAlloca;
  assert(isa<AllocaInst>(LocalStackBaseAllocaPtr) &&
         "Variable descriptions relative to ASan stack base will be dropped");

  // Replace Alloca instructions with base+offset.
  for (const auto &Desc : SVD) {
    AllocaInst *AI = Desc.AI;
    replaceDbgDeclare(AI, LocalStackBaseAllocaPtr, DIB, DIExprFlags,
                      Desc.Offset);
    Value *NewAllocaPtr = IRB.CreateIntToPtr(
        IRB.CreateAdd(LocalStackBase, ConstantInt::get(IntptrTy, Desc.Offset)),
        AI->getType());
    AI->replaceAllUsesWith(NewAllocaPtr);
  }

  // The left-most redzone has enough space for at least 4 pointers.
  // Write the Magic value to redzone[0].
  Value *BasePlus0 = IRB.CreateIntToPtr(LocalStackBase, IntptrPtrTy);
  IRB.CreateStore(ConstantInt::get(IntptrTy, kCurrentStackFrameMagic),
                  BasePlus0);
  // Write the frame description constant to redzone[1].
  Value *BasePlus1 = IRB.CreateIntToPtr(
      IRB.CreateAdd(LocalStackBase,
                    ConstantInt::get(IntptrTy, ASan.LongSize / 8)),
      IntptrPtrTy);
  GlobalVariable *StackDescriptionGlobal =
      createPrivateGlobalForString(*F.getParent(), DescriptionString,
                                   /*AllowMerging*/ true, kAsanGenPrefix);
  Value *Description = IRB.CreatePointerCast(StackDescriptionGlobal, IntptrTy);
  IRB.CreateStore(Description, BasePlus1);
  // Write the PC to redzone[2].
  Value *BasePlus2 = IRB.CreateIntToPtr(
      IRB.CreateAdd(LocalStackBase,
                    ConstantInt::get(IntptrTy, 2 * ASan.LongSize / 8)),
      IntptrPtrTy);
  IRB.CreateStore(IRB.CreatePointerCast(&F, IntptrTy), BasePlus2);

  const auto &ShadowAfterScope = GetShadowBytesAfterScope(SVD, L);

  // Poison the stack red zones at the entry.
  Value *ShadowBase = ASan.memToShadow(LocalStackBase, IRB);
  // As mask we must use most poisoned case: red zones and after scope.
  // As bytes we can use either the same or just red zones only.
  copyToShadow(ShadowAfterScope, ShadowAfterScope, IRB, ShadowBase);

  if (!StaticAllocaPoisonCallVec.empty()) {
    const auto &ShadowInScope = GetShadowBytes(SVD, L);

    // Poison static allocas near lifetime intrinsics.
    for (const auto &APC : StaticAllocaPoisonCallVec) {
      const ASanStackVariableDescription &Desc = *AllocaToSVDMap[APC.AI];
      assert(Desc.Offset % L.Granularity == 0);
      size_t Begin = Desc.Offset / L.Granularity;
      size_t End = Begin + (APC.Size + L.Granularity - 1) / L.Granularity;

      IRBuilder<> IRB(APC.InsBefore);
      copyToShadow(ShadowAfterScope,
                   APC.DoPoison ? ShadowAfterScope : ShadowInScope, Begin, End,
                   IRB, ShadowBase);
    }
  }

  SmallVector<uint8_t, 64> ShadowClean(ShadowAfterScope.size(), 0);
  SmallVector<uint8_t, 64> ShadowAfterReturn;

  // (Un)poison the stack before all ret instructions.
  for (Instruction *Ret : RetVec) {
    IRBuilder<> IRBRet(Ret);
    // Mark the current frame as retired.
    IRBRet.CreateStore(ConstantInt::get(IntptrTy, kRetiredStackFrameMagic),
                       BasePlus0);
    if (DoStackMalloc) {
      assert(StackMallocIdx >= 0);
      // if FakeStack != 0  // LocalStackBase == FakeStack
      //     // In use-after-return mode, poison the whole stack frame.
      //     if StackMallocIdx <= 4
      //         // For small sizes inline the whole thing:
      //         memset(ShadowBase, kAsanStackAfterReturnMagic, ShadowSize);
      //         **SavedFlagPtr(FakeStack) = 0
      //     else
      //         __asan_stack_free_N(FakeStack, LocalStackSize)
      // else
      //     <This is not a fake stack; unpoison the redzones>
      Value *Cmp =
          IRBRet.CreateICmpNE(FakeStack, Constant::getNullValue(IntptrTy));
      Instruction *ThenTerm, *ElseTerm;
      SplitBlockAndInsertIfThenElse(Cmp, Ret, &ThenTerm, &ElseTerm);

      IRBuilder<> IRBPoison(ThenTerm);
      if (StackMallocIdx <= 4) {
        int ClassSize = kMinStackMallocSize << StackMallocIdx;
        ShadowAfterReturn.resize(ClassSize / L.Granularity,
                                 kAsanStackUseAfterReturnMagic);
        copyToShadow(ShadowAfterReturn, ShadowAfterReturn, IRBPoison,
                     ShadowBase);
        Value *SavedFlagPtrPtr = IRBPoison.CreateAdd(
            FakeStack,
            ConstantInt::get(IntptrTy, ClassSize - ASan.LongSize / 8));
        Value *SavedFlagPtr = IRBPoison.CreateLoad(
            IntptrTy, IRBPoison.CreateIntToPtr(SavedFlagPtrPtr, IntptrPtrTy));
        IRBPoison.CreateStore(
            Constant::getNullValue(IRBPoison.getInt8Ty()),
            IRBPoison.CreateIntToPtr(SavedFlagPtr, IRBPoison.getInt8PtrTy()));
      } else {
        // For larger frames call __asan_stack_free_*.
        IRBPoison.CreateCall(
            AsanStackFreeFunc[StackMallocIdx],
            {FakeStack, ConstantInt::get(IntptrTy, LocalStackSize)});
      }

      IRBuilder<> IRBElse(ElseTerm);
      copyToShadow(ShadowAfterScope, ShadowClean, IRBElse, ShadowBase);
    } else {
      copyToShadow(ShadowAfterScope, ShadowClean, IRBRet, ShadowBase);
    }
  }

  // We are done. Remove the old unused alloca instructions.
  for (auto AI : AllocaVec) AI->eraseFromParent();
}

void FunctionStackPoisoner::poisonAlloca(Value *V, uint64_t Size,
                                         IRBuilder<> &IRB, bool DoPoison) {
  // For now just insert the call to ASan runtime.
  Value *AddrArg = IRB.CreatePointerCast(V, IntptrTy);
  Value *SizeArg = ConstantInt::get(IntptrTy, Size);
  IRB.CreateCall(
      DoPoison ? AsanPoisonStackMemoryFunc : AsanUnpoisonStackMemoryFunc,
      {AddrArg, SizeArg});
}

// Handling llvm.lifetime intrinsics for a given %alloca:
// (1) collect all llvm.lifetime.xxx(%size, %value) describing the alloca.
// (2) if %size is constant, poison memory for llvm.lifetime.end (to detect
//     invalid accesses) and unpoison it for llvm.lifetime.start (the memory
//     could be poisoned by previous llvm.lifetime.end instruction, as the
//     variable may go in and out of scope several times, e.g. in loops).
// (3) if we poisoned at least one %alloca in a function,
//     unpoison the whole stack frame at function exit.
void FunctionStackPoisoner::handleDynamicAllocaCall(AllocaInst *AI) {
  IRBuilder<> IRB(AI);

  const unsigned Alignment = std::max(kAllocaRzSize, AI->getAlignment());
  const uint64_t AllocaRedzoneMask = kAllocaRzSize - 1;

  Value *Zero = Constant::getNullValue(IntptrTy);
  Value *AllocaRzSize = ConstantInt::get(IntptrTy, kAllocaRzSize);
  Value *AllocaRzMask = ConstantInt::get(IntptrTy, AllocaRedzoneMask);

  // Since we need to extend alloca with additional memory to locate
  // redzones, and OldSize is number of allocated blocks with
  // ElementSize size, get allocated memory size in bytes by
  // OldSize * ElementSize.
  const unsigned ElementSize =
      F.getParent()->getDataLayout().getTypeAllocSize(AI->getAllocatedType());
  Value *OldSize =
      IRB.CreateMul(IRB.CreateIntCast(AI->getArraySize(), IntptrTy, false),
                    ConstantInt::get(IntptrTy, ElementSize));

  // PartialSize = OldSize % 32
  Value *PartialSize = IRB.CreateAnd(OldSize, AllocaRzMask);

  // Misalign = kAllocaRzSize - PartialSize;
  Value *Misalign = IRB.CreateSub(AllocaRzSize, PartialSize);

  // PartialPadding = Misalign != kAllocaRzSize ? Misalign : 0;
  Value *Cond = IRB.CreateICmpNE(Misalign, AllocaRzSize);
  Value *PartialPadding = IRB.CreateSelect(Cond, Misalign, Zero);

  // AdditionalChunkSize = Alignment + PartialPadding + kAllocaRzSize
  // Alignment is added to locate left redzone, PartialPadding for possible
  // partial redzone and kAllocaRzSize for right redzone respectively.
  Value *AdditionalChunkSize = IRB.CreateAdd(
      ConstantInt::get(IntptrTy, Alignment + kAllocaRzSize), PartialPadding);

  Value *NewSize = IRB.CreateAdd(OldSize, AdditionalChunkSize);

  // Insert new alloca with new NewSize and Alignment params.
  AllocaInst *NewAlloca = IRB.CreateAlloca(IRB.getInt8Ty(), NewSize);
  NewAlloca->setAlignment(Align(Alignment));

  // NewAddress = Address + Alignment
  Value *NewAddress = IRB.CreateAdd(IRB.CreatePtrToInt(NewAlloca, IntptrTy),
                                    ConstantInt::get(IntptrTy, Alignment));

  // Insert __asan_alloca_poison call for new created alloca.
  IRB.CreateCall(AsanAllocaPoisonFunc, {NewAddress, OldSize});

  // Store the last alloca's address to DynamicAllocaLayout. We'll need this
  // for unpoisoning stuff.
  IRB.CreateStore(IRB.CreatePtrToInt(NewAlloca, IntptrTy), DynamicAllocaLayout);

  Value *NewAddressPtr = IRB.CreateIntToPtr(NewAddress, AI->getType());

  // Replace all uses of AddessReturnedByAlloca with NewAddressPtr.
  AI->replaceAllUsesWith(NewAddressPtr);

  // We are done. Erase old alloca from parent.
  AI->eraseFromParent();
}

// isSafeAccess returns true if Addr is always inbounds with respect to its
// base object. For example, it is a field access or an array access with
// constant inbounds index.
bool AddressSanitizer::isSafeAccess(ObjectSizeOffsetVisitor &ObjSizeVis,
                                    Value *Addr, uint64_t TypeSize) const {
  SizeOffsetType SizeOffset = ObjSizeVis.compute(Addr);
  if (!ObjSizeVis.bothKnown(SizeOffset)) return false;
  uint64_t Size = SizeOffset.first.getZExtValue();
  int64_t Offset = SizeOffset.second.getSExtValue();
  // Three checks are required to ensure safety:
  // . Offset >= 0  (since the offset is given from the base ptr)
  // . Size >= Offset  (unsigned)
  // . Size - Offset >= NeededSize  (unsigned)
  return Offset >= 0 && Size >= uint64_t(Offset) &&
         Size - uint64_t(Offset) >= TypeSize / 8;
}

//ASAN--: Removing Unsatisfiable Checks
bool AddressSanitizer::isSafeAccessBoost(ObjectSizeOffsetVisitor &ObjSizeVis, Instruction *IndexInst, Value *Addr, Function *F) const {

  auto DT = DominatorTree(*F);

  if (GetElementPtrInst *Gep_Inst = dyn_cast<GetElementPtrInst>(Addr)) {

    for (auto& Index : make_range(Gep_Inst->idx_begin(), Gep_Inst->idx_end())) {

      for (User *U : Index->users()) {
        
        if (CmpInst *i_cmp = dyn_cast<CmpInst>(U)) {

          if (DT.dominates(i_cmp, IndexInst)) {
            if (Index == i_cmp->getOperand(0) && isa<ConstantData>(i_cmp->getOperand(1))) {

              auto IndexSize = i_cmp->getOperand(1);

              auto ConstantSize = dyn_cast<ConstantInt>(IndexSize);

              int64_t MaxOffset = ConstantSize->getSExtValue();

              auto type = Gep_Inst->getPointerOperandType();

              if (auto pttp = cast<PointerType>(type)) {
                auto pttpee = pttp->getElementType();

                if (isa<ArrayType>(pttpee)) {
                  auto ObjSize = pttpee->getArrayNumElements();
                  
                  return ObjSize >= MaxOffset;
                }
              }
              if (isa<ArrayType>(type)) {
                auto ObjSize = type->getArrayNumElements();
                
                return ObjSize >= MaxOffset;
              }
            }

            if (Index == i_cmp->getOperand(1) && isa<ConstantData>(i_cmp->getOperand(0))) {

              auto IndexSize = i_cmp->getOperand(0);

              auto ConstantSize = dyn_cast<ConstantInt>(IndexSize);

              int64_t MaxOffset = ConstantSize->getSExtValue();

              auto type = Gep_Inst->getPointerOperandType();

              if (auto pttp = cast<PointerType>(type)) {
                auto pttpee = pttp->getElementType();

                if (isa<ArrayType>(pttpee)) {
                  auto ObjSize = pttpee->getArrayNumElements();
                  
                  return ObjSize >= MaxOffset;
                }
              }
              if (isa<ArrayType>(type)) {
                auto ObjSize = type->getArrayNumElements();
                return ObjSize >= MaxOffset;
              }
            }
          }
        }
      } 
    }
  }
  return false;
}
