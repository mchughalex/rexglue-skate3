/**
 * @file        system/function_dispatcher.cpp
 * @brief       Guest function dispatch coordinator for recompiled code
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 *
 * @remarks     Derived from Xenia's runtime::Processor (Ben Vanik, 2020).
 *              Stripped of emulation-era dead code and renamed to reflect its
 *              role as a function dispatch table rather than a CPU emulator.
 */

#include <array>
#include <span>

#include <rex/assert.h>
#include <rex/dbg.h>
#include <rex/logging.h>
#include <rex/perf/counter.h>
#include <rex/memory.h>
#include <rex/ppc/context.h>
#include <rex/runtime.h>
#include <rex/system/function_dispatcher.h>
#include <rex/system/thread_state.h>

namespace rex::runtime {

namespace {

FunctionDispatcher* GetBoundFunctionDispatcher() {
  Runtime* runtime = Runtime::instance();
  return runtime ? runtime->function_dispatcher() : nullptr;
}

bool ReadGuestWords(uint8_t* base, uint32_t address, std::span<uint32_t> words) {
  constexpr uint64_t kGuestAddressSpace = UINT64_C(1) << 32;
  if (!base || uint64_t(address) + words.size_bytes() > kGuestAddressSpace ||
      !debug::ReadMemory(base + address, words.data(), words.size_bytes())) {
    return false;
  }

  for (auto& word : words)
    word = rex::byte_swap(word);
  return true;
}

void LogGuestWords(const char* label, uint8_t* base, uint32_t address, uint32_t rows) {
  for (uint32_t row = 0; row < rows; ++row) {
    std::array<uint32_t, 4> words;
    const uint64_t current = uint64_t(address) + row * sizeof(words);
    if (current > UINT32_MAX || !ReadGuestWords(base, uint32_t(current), words)) {
      REXLOG_CRITICAL("guest memory {} +{:02X}: unreadable", label, row * sizeof(words));
      return;
    }
    REXLOG_CRITICAL("guest memory {} @{:08X}: {:08X} {:08X} {:08X} {:08X}", label,
                    uint32_t(current), words[0], words[1], words[2], words[3]);
  }
}

void LogGuestStack(uint8_t* base, uint32_t sp) {
  constexpr uint32_t kMaxGuestFrames = 16;
  constexpr uint32_t kSavedLinkOffset = 8;
  for (uint32_t frame = 0; frame < kMaxGuestFrames; ++frame) {
    uint32_t caller = 0, link = 0;
    if (!ReadGuestWords(base, sp, {&caller, 1}) || caller <= sp || (caller & 15) != 0 ||
        !ReadGuestWords(base, caller - kSavedLinkOffset, {&link, 1})) {
      REXLOG_CRITICAL("guest stack stopped at {:08X}: unreadable or invalid backchain", sp);
      return;
    }
    REXLOG_CRITICAL("guest frame {} sp={:08X} caller_sp={:08X} return={:08X}", frame, sp, caller,
                    link);
    sp = caller;
  }
}

void LogGuestCall(PPCContext& ctx, uint8_t* base) {
  // Only the invalid-call trap gathers state. Valid dispatch stays unchanged.
  REXLOG_CRITICAL("guest invalid-call target={:08X}", ctx.last_indirect_target);
  if (const auto* thread = ThreadState::Get()) {
    REXLOG_CRITICAL("guest thread id={:08X}", thread->thread_id());
  }
#if !defined(REX_CONFIG_SKIP_LR)
  REXLOG_CRITICAL("guest lr={:016X}", ctx.lr);
#endif
#if !defined(REX_CONFIG_CTR_AS_LOCAL)
  REXLOG_CRITICAL("guest ctr={:016X}", ctx.ctr.u64);
#endif
#if !defined(REX_CONFIG_XER_AS_LOCAL)
  REXLOG_CRITICAL("guest xer so={} ov={} ca={}", ctx.xer.so, ctx.xer.ov, ctx.xer.ca);
#endif
#if !defined(REX_CONFIG_CR_AS_LOCAL)
  REXLOG_CRITICAL("guest cr={:X}{:X}{:X}{:X}{:X}{:X}{:X}{:X}", ctx.cr0.raw(), ctx.cr1.raw(),
                  ctx.cr2.raw(), ctx.cr3.raw(), ctx.cr4.raw(), ctx.cr5.raw(), ctx.cr6.raw(),
                  ctx.cr7.raw());
#endif
  const std::array<const PPCRegister*, 32> registers = {
#if !defined(REX_CONFIG_NON_ARGUMENT_AS_LOCAL)
      &ctx.r0,  &ctx.r1,  &ctx.r2,  &ctx.r3,
#else
      nullptr,  &ctx.r1, nullptr, &ctx.r3,
#endif
      &ctx.r4,  &ctx.r5,  &ctx.r6,  &ctx.r7,  &ctx.r8,  &ctx.r9,  &ctx.r10,
#if !defined(REX_CONFIG_NON_ARGUMENT_AS_LOCAL)
      &ctx.r11, &ctx.r12,
#else
      nullptr,  nullptr,
#endif
      &ctx.r13,
#if !defined(REX_CONFIG_NON_VOLATILE_AS_LOCAL)
      &ctx.r14, &ctx.r15, &ctx.r16, &ctx.r17, &ctx.r18, &ctx.r19, &ctx.r20, &ctx.r21, &ctx.r22,
      &ctx.r23, &ctx.r24, &ctx.r25, &ctx.r26, &ctx.r27, &ctx.r28, &ctx.r29, &ctx.r30, &ctx.r31,
#endif
  };
  for (size_t reg = 0; reg < registers.size(); ++reg) {
    if (registers[reg])
      REXLOG_CRITICAL("guest r{}={:016X}", reg, registers[reg]->u64);
  }

  constexpr uint32_t kArgumentRows = 4;
  constexpr uint32_t kNodeRows = 8;
  LogGuestWords("r1 stack", base, ctx.r1.u32, kArgumentRows);
  LogGuestWords("r3", base, ctx.r3.u32, kArgumentRows);
  LogGuestWords("r4", base, ctx.r4.u32, kArgumentRows);
#if !defined(REX_CONFIG_NON_VOLATILE_AS_LOCAL)
  std::array<uint32_t, 4> node;
  if (ReadGuestWords(base, ctx.r31.u32, node) && node[2] == ctx.last_indirect_target) {
    REXLOG_CRITICAL(
        "guest callback candidate r31={:08X} next={:08X} prev={:08X} "
        "target={:08X} data={:08X}",
        ctx.r31.u32, node[0], node[1], node[2], node[3]);
  }
  LogGuestWords("r29", base, ctx.r29.u32, kArgumentRows);
  LogGuestWords("r30", base, ctx.r30.u32, kArgumentRows);
  LogGuestWords("r31 node candidate", base, ctx.r31.u32, kNodeRows);
#endif
  LogGuestStack(base, ctx.r1.u32);
}

}  // namespace

static void InvalidFunctionTrap(PPCContext& ctx, uint8_t* base) {
  LogGuestCall(ctx, base);
  REX_FATAL("Call to invalid or unregistered function at guest address 0x{:08X}",
            ctx.last_indirect_target);
}

PPCFunc* ResolveIndirectFunction(uint32_t guest_address) {
  FunctionDispatcher* dispatcher = GetBoundFunctionDispatcher();
  if (!dispatcher) {
    return &InvalidFunctionTrap;
  }

  if (PPCFunc* func = dispatcher->GetFunction(guest_address)) {
    return func;
  }

  return &InvalidFunctionTrap;
}

FunctionDispatcher::FunctionDispatcher(rex::memory::Memory* memory, ExportResolver* export_resolver)
    : memory_(memory), export_resolver_(export_resolver) {}

FunctionDispatcher::~FunctionDispatcher() = default;

bool FunctionDispatcher::Execute(ThreadState* thread_state, uint32_t address) {
  SCOPE_profile_cpu_f("cpu");
  PROFILE_FUNCTION_DISPATCHED();

  PPCFunc* fn = GetFunction(address);
  if (!fn) {
    REXCPU_ERROR("Execute({:08X}): function not in function table", address);
    return false;
  }

  auto* ctx = thread_state->context();
  auto* previous_thread_state = ThreadState::Get();

  // Rebind the active guest thread for cross-module callbacks.
  ThreadState::Bind(thread_state);

  // Pad out stack a bit, as some games seem to overwrite the caller by about 16 to 32b.
  ctx->r1.u64 -= 64 + 112;

  uint64_t previous_lr = ctx->lr;
  ctx->lr = 0xBCBCBCBC;

  fn(*ctx, memory_->virtual_membase());

  ctx->lr = previous_lr;
  ctx->r1.u64 += 64 + 112;
  ThreadState::Bind(previous_thread_state);

  return true;
}

uint64_t FunctionDispatcher::Execute(ThreadState* thread_state, uint32_t address, uint64_t args[],
                                     size_t arg_count) {
  SCOPE_profile_cpu_f("cpu");

  auto* ctx = thread_state->context();

  if (arg_count > 0)
    ctx->r3.u64 = args[0];
  if (arg_count > 1)
    ctx->r4.u64 = args[1];
  if (arg_count > 2)
    ctx->r5.u64 = args[2];
  if (arg_count > 3)
    ctx->r6.u64 = args[3];
  if (arg_count > 4)
    ctx->r7.u64 = args[4];
  if (arg_count > 5)
    ctx->r8.u64 = args[5];
  if (arg_count > 6)
    ctx->r9.u64 = args[6];
  if (arg_count > 7)
    ctx->r10.u64 = args[7];

  // FIXME: stack-arg path assumes 32-bit values; 64-bit and float args are wrong.
  if (arg_count > 8) {
    auto stack_arg_base =
        memory_->TranslateVirtual(static_cast<uint32_t>(ctx->r1.u64) + 0x54 - (64 + 112));
    for (size_t i = 8; i < arg_count; i++) {
      memory::store_and_swap<uint32_t>(stack_arg_base + ((i - 8) * 8),
                                       static_cast<uint32_t>(args[i]));
    }
  }

  if (!Execute(thread_state, address)) {
    return 0xDEADBABE;
  }
  return ctx->r3.u64;
}

uint64_t FunctionDispatcher::ExecuteInterrupt(ThreadState* thread_state, uint32_t address,
                                              uint64_t args[], size_t arg_count) {
  SCOPE_profile_cpu_f("cpu");
  PROFILE_INTERRUPT_DISPATCHED();

  // Hold the global lock during interrupt dispatch.
  auto global_lock = global_critical_region_.Acquire();

  auto* ctx = thread_state->context();
  assert_true(arg_count <= 5);

  if (arg_count > 0)
    ctx->r3.u64 = args[0];
  if (arg_count > 1)
    ctx->r4.u64 = args[1];
  if (arg_count > 2)
    ctx->r5.u64 = args[2];
  if (arg_count > 3)
    ctx->r6.u64 = args[3];
  if (arg_count > 4)
    ctx->r7.u64 = args[4];

  // TLS ptr must be zero during interrupts. Some games check this and early-exit
  // routines when under interrupts.
  auto pcr_address = memory_->TranslateVirtual(static_cast<uint32_t>(ctx->r13.u64));
  uint32_t old_tls_ptr = memory::load_and_swap<uint32_t>(pcr_address);
  memory::store_and_swap<uint32_t>(pcr_address, 0);

  if (!Execute(thread_state, address)) {
    return 0xDEADBABE;
  }

  // Restore TLS ptr.
  memory::store_and_swap<uint32_t>(pcr_address, old_tls_ptr);

  return ctx->r3.u64;
}

// rexglue function table management

bool FunctionDispatcher::InitializeFunctionTable(uint32_t code_base, uint32_t code_size,
                                                 uint32_t image_base, uint32_t image_size,
                                                 bool is_entrypoint) {
  std::lock_guard<std::recursive_mutex> lock(dispatch_mutex_);

  if (is_entrypoint && entrypoint_code_base_ != 0) {
    REXLOG_ERROR("InitializeFunctionTable: entrypoint already registered at {:08X}",
                 entrypoint_code_base_);
    return false;
  }

  uint32_t new_table_end = image_base + image_size + (code_size + kThunkReserveSize) * 2;
  uint32_t new_code_end = code_base + code_size + kThunkReserveSize;
  for (const auto& existing : module_tables_) {
    uint32_t existing_table_end =
        existing.image_base + existing.image_size + (existing.code_size + kThunkReserveSize) * 2;
    uint32_t existing_code_end = existing.code_base + existing.code_size + kThunkReserveSize;
    if (image_base < existing_table_end && new_table_end > existing.image_base) {
      REXLOG_ERROR("Module image range [{:08X}, {:08X}) overlaps existing [{:08X}, {:08X})",
                   image_base, new_table_end, existing.image_base, existing_table_end);
      return false;
    }
    if (code_base < existing_code_end && new_code_end > existing.code_base) {
      REXLOG_ERROR("Module code range [{:08X}, {:08X}) overlaps existing [{:08X}, {:08X})",
                   code_base, new_code_end, existing.code_base, existing_code_end);
      return false;
    }
  }

  if (!memory_->InitializeFunctionTable(code_base, code_size, image_base, image_size)) {
    REXLOG_ERROR("Failed to initialize guest memory function table");
    return false;
  }

  module_tables_.push_back({
      .code_base = code_base,
      .code_size = code_size,
      .image_base = image_base,
      .image_size = image_size,
      .next_thunk_address = code_base + code_size,
      .thunk_limit = code_base + code_size + kThunkReserveSize,
  });

  if (is_entrypoint) {
    entrypoint_code_base_ = code_base;
  }

  REXLOG_INFO("Function table initialized for module: code={:08X}-{:08X}, image={:08X}-{:08X}",
              code_base, code_base + code_size, image_base, image_base + image_size);
  return true;
}

FunctionDispatcher::ModuleTableInfo* FunctionDispatcher::FindModuleByAddress(
    uint32_t guest_address) {
  for (auto& mod : module_tables_) {
    if (guest_address >= mod.code_base && guest_address < mod.thunk_limit) {
      return &mod;
    }
  }
  return nullptr;
}

uint32_t FunctionDispatcher::FindCallerModuleBase(uint32_t guest_address) {
  std::lock_guard<std::recursive_mutex> lock(dispatch_mutex_);
  if (auto* mod = FindModuleByAddress(guest_address)) {
    return mod->code_base;
  }
  return 0;
}

bool FunctionDispatcher::SetFunction(uint32_t guest_address, ::PPCFunc* func) {
  std::lock_guard<std::recursive_mutex> lock(dispatch_mutex_);
  assert_true(!module_tables_.empty());

  if (!FindModuleByAddress(guest_address)) {
    REXLOG_ERROR("SetFunction: address {:08X} outside all registered module ranges", guest_address);
    return false;
  }

  function_table_[guest_address] = func;

  if (!memory_->SetFunction(guest_address, func)) {
    REXLOG_ERROR("SetFunction: dispatcher / Memory module-table state out of sync at {:08X}",
                 guest_address);
    function_table_.erase(guest_address);
    return false;
  }

  if (recording_) {
    recording_addresses_.push_back(guest_address);
  }
  return true;
}

::PPCFunc* FunctionDispatcher::GetFunction(uint32_t guest_address) {
  std::lock_guard<std::recursive_mutex> lock(dispatch_mutex_);
  auto it = function_table_.find(guest_address);
  if (it != function_table_.end()) {
    return it->second;
  }
  return nullptr;
}

uint32_t FunctionDispatcher::AllocateThunk(::PPCFunc* func, uint32_t caller_address) {
  std::lock_guard<std::recursive_mutex> lock(dispatch_mutex_);
  auto* mod = FindModuleByAddress(caller_address);
  if (!mod) {
    if (caller_address != 0) {
      REXLOG_ERROR("AllocateThunk: caller_address {:08X} not in any registered module",
                   caller_address);
      return 0;
    }
    if (entrypoint_code_base_ == 0) {
      REXLOG_ERROR("AllocateThunk: caller_address=0 but no entrypoint registered");
      return 0;
    }
    mod = FindModuleByAddress(entrypoint_code_base_);
    if (!mod) {
      REXLOG_ERROR("AllocateThunk: entrypoint code_base {:08X} not in module_tables_",
                   entrypoint_code_base_);
      return 0;
    }
  }

  if (mod->next_thunk_address >= mod->thunk_limit) {
    REXLOG_ERROR("Thunk address space exhausted for module at {:08X}", mod->code_base);
    return 0;
  }
  uint32_t addr = mod->next_thunk_address;
  mod->next_thunk_address += 4;
  if (!SetFunction(addr, func)) {
    mod->next_thunk_address -= 4;
    return 0;
  }
  return addr;
}

void FunctionDispatcher::RegisterModule(const std::string& module_id, uint32_t code_base,
                                        RegisterFn register_func) {
  std::lock_guard<std::recursive_mutex> lock(dispatch_mutex_);
  if (recording_) {
    REX_FATAL("RegisterModule called while already recording (re-entrancy)");
    return;
  }

  if (module_addresses_.find(module_id) != module_addresses_.end()) {
    REXLOG_WARN("RegisterModule: '{}' is already registered; cleaning up prior batch", module_id);
    UnregisterModule(module_id);
  }

  REXLOG_INFO("Registering module: {} (code_base={:08X})", module_id, code_base);

  recording_addresses_.clear();
  recording_ = true;

  struct RecordingGuard {
    FunctionDispatcher* self;
    ~RecordingGuard() {
      self->recording_ = false;
      self->recording_addresses_.clear();
    }
  } guard{this};

  register_func(this);

  ModuleRegistration reg;
  reg.code_base = code_base;
  reg.addresses = std::move(recording_addresses_);

  size_t count = reg.addresses.size();
  module_addresses_[module_id] = std::move(reg);

  REXLOG_INFO("Module '{}' registered {} functions", module_id, count);
}

std::optional<std::pair<uint32_t, uint32_t>> FunctionDispatcher::UnregisterModule(
    const std::string& module_id) {
  std::lock_guard<std::recursive_mutex> lock(dispatch_mutex_);
  auto it = module_addresses_.find(module_id);
  if (it == module_addresses_.end()) {
    REXLOG_WARN("UnregisterModule: module '{}' not found", module_id);
    return std::nullopt;
  }

  REXLOG_INFO("Unregistering module: {} ({} functions)", module_id, it->second.addresses.size());

  auto table_it = std::find_if(module_tables_.begin(), module_tables_.end(),
                               [code_base = it->second.code_base](const ModuleTableInfo& mti) {
                                 return mti.code_base == code_base;
                               });

  for (uint32_t addr : it->second.addresses) {
    function_table_.erase(addr);
    memory_->SetFunction(addr, nullptr);
  }

  std::optional<std::pair<uint32_t, uint32_t>> cleared_range;
  if (table_it != module_tables_.end()) {
    uint32_t pool_start = table_it->code_base + table_it->code_size;
    uint32_t pool_end = table_it->next_thunk_address;
    for (uint32_t addr = pool_start; addr < pool_end; addr += 4) {
      function_table_.erase(addr);
      memory_->SetFunction(addr, nullptr);
    }
    cleared_range = std::make_pair(pool_start, pool_end);

    if (table_it->code_base == entrypoint_code_base_) {
      entrypoint_code_base_ = 0;
    }
    memory_->DestroyFunctionTable(table_it->code_base);
    module_tables_.erase(table_it);
  }

  module_addresses_.erase(it);

  return cleared_range;
}

}  // namespace rex::runtime
