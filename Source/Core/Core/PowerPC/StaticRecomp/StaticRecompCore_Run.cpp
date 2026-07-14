// RecompCore: StaticRecomp CPU core - Main execution loop.
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/PowerPC/StaticRecomp/StaticRecompCore.h"
#include "Core/System.h"
#include "Core/PowerPC/PowerPC.h"
#include "Core/PowerPC/Interpreter/Interpreter.h"
#include "Core/PowerPC/StaticRecomp/StaticRecompLockstep.h"
#include "Core/CoreTiming.h"
#include "Core/HW/CPU.h"
#include "Core/Config/MainSettings.h"
#include "Core/Config/ConfigManager.h"
#include "Core/HW/SystemTimers.h"

#include <algorithm>
#include <chrono>
#include <cstdio>

namespace
{
constexpr u32 SYNC_EXCEPTION_MASK = ~static_cast<u32>(
    EXCEPTION_EXTERNAL_INT | EXCEPTION_DECREMENTER | EXCEPTION_PERFORMANCE_MONITOR);
}

void StaticRecompCore::Run()
{
  auto& core_timing = m_system.GetCoreTiming();
  auto& power_pc = m_system.GetPowerPC();
  auto& ppc = power_pc.GetPPCState();
  auto& interpreter = m_system.GetInterpreter();
  auto& memory = m_system.GetMemory();
  const CPU::State* state_ptr = m_system.GetCPU().GetStatePtr();

  m_guest.ram = memory.GetRAM();
  m_guest.ram_size = memory.GetRamSizeReal();
  m_guest.exram = memory.GetEXRAM();
  m_guest.exram_size = memory.GetExRamSizeReal();
  InitLookupTable(m_guest.ram_size, m_guest.exram_size);

  const std::string initial_game_id = SConfig::GetInstance().GetGameID();
  m_module_active = m_module && (initial_game_id.empty() || initial_game_id == m_module->game_id);

  if (!m_module_active && m_fallback_jit)
  {
    m_fallback_jit->Run();
    return;
  }

  while (*state_ptr == CPU::State::Running)
  {
    core_timing.Advance();
    // CoreTiming events only set pending asynchronous exception bits. JIT dispatchers
    // explicitly deliver them after advancing the slice; native dispatch has no such
    // branch hook, so do it here before re-entering recompiled code.
    power_pc.CheckExternalExceptions();
    const std::string current_game_id = SConfig::GetInstance().GetGameID();
    m_module_active = m_module && (current_game_id.empty() || current_game_id == m_module->game_id);

    do
    {
      // MSR.FP needs no gate here: generated FPU instructions raise the
      // FP-unavailable exception themselves (ppc_fp_available).
      if (m_module_active && DispatchableAt(ppc.pc))
      {
        SyncIn();
        ++m_bursts;
        do
        {
          const bool do_ls = m_lockstep_verifier->ShouldCheck(m_guest.pc);
          if (do_ls)
          {
            m_lockstep_verifier->Prepare(m_guest);
          }

          const u32 dispatch_pc = m_guest.pc;
          const bool profile_dispatch =
              m_profile_dispatches && ((m_native_dispatches + 1) & 0x3FFu) == 0;
          const auto profile_start =
              profile_dispatch ? std::chrono::steady_clock::now() :
                                 std::chrono::steady_clock::time_point{};
          m_module->dispatch(&m_guest, dispatch_pc);
          ++m_native_dispatches;
          if (profile_dispatch)
          {
            const auto profile_end = std::chrono::steady_clock::now();
            const u64 elapsed_ns = static_cast<u64>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(profile_end - profile_start)
                    .count());
            const auto update_profile = [elapsed_ns](DispatchProfile& profile) {
              ++profile.samples;
              profile.total_ns += elapsed_ns;
              if (elapsed_ns > profile.max_ns)
                profile.max_ns = elapsed_ns;
            };
            update_profile(m_dispatch_profile[dispatch_pc]);
            update_profile(m_dispatch_profile_window[dispatch_pc]);

            const u64 now_ns = static_cast<u64>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(profile_end.time_since_epoch())
                    .count());
            if (m_profile_window_start_ns == 0)
            {
              m_profile_window_start_ns = now_ns;
              m_profile_window_native_start = m_native_dispatches;
              m_profile_window_fallback_start = m_fallback_steps;
              m_profile_window_hook_start = m_hook_fallback_instructions;
            }
            else if (now_ns - m_profile_window_start_ns >= 1000000000u)
            {
              const double seconds = static_cast<double>(now_ns - m_profile_window_start_ns) / 1e9;
              std::vector<std::pair<u32, DispatchProfile>> window(
                  m_dispatch_profile_window.begin(), m_dispatch_profile_window.end());
              std::ranges::sort(window, [](const auto& lhs, const auto& rhs) {
                return lhs.second.total_ns > rhs.second.total_ns;
              });
              std::fprintf(stderr,
                           "[staticrecomp-window] dispatch_rate=%.0f fallback_rate=%.0f "
                           "hook_rate=%.0f",
                           static_cast<double>(m_native_dispatches - m_profile_window_native_start) /
                               seconds,
                           static_cast<double>(m_fallback_steps - m_profile_window_fallback_start) /
                               seconds,
                           static_cast<double>(m_hook_fallback_instructions -
                                               m_profile_window_hook_start) /
                               seconds);
              const size_t count = std::min<size_t>(3, window.size());
              for (size_t i = 0; i < count; ++i)
              {
                const auto& [pc, sample] = window[i];
                std::fprintf(stderr, " slow%zu=0x%08X:%lluns/%llu", i, pc,
                             (unsigned long long)sample.total_ns,
                             (unsigned long long)sample.samples);
              }
              std::fputc('\n', stderr);
              m_dispatch_profile_window.clear();
              m_profile_window_start_ns = now_ns;
              m_profile_window_native_start = m_native_dispatches;
              m_profile_window_fallback_start = m_fallback_steps;
              m_profile_window_hook_start = m_hook_fallback_instructions;
            }
          }

          if (do_ls)
          {
            m_lockstep_verifier->Verify(m_guest);
          }

          // Flush the module's per-block cycle charges into Dolphin's
          // downcount. A dispatch that charged nothing (PC-switch default,
          // pure embedded data) still costs 1 so the burst always makes
          // downcount progress; this per-dispatch flush is also the
          // dispatcher back-edge timing check — CoreTiming regains control
          // with at least CachedInterpreter's per-block frequency, so
          // external-interrupt latency matches stock.
          const s64 charge = -m_guest.downcount;
          m_guest.downcount = 0;
          ppc.downcount -= static_cast<int>(charge > 0 ? charge : 1);
          m_charged_cycles += static_cast<u64>(charge > 0 ? charge : 1);
          m_guest.timebase += static_cast<u64>(charge > 0 ? charge : 1);

          // Idle loop skipping for configured target loops (e.g. SDK waits and OSIdleThread).
          for (const u32 idle_pc : m_idle_pcs)
          {
            if (idle_pc != 0 && m_guest.pc == idle_pc)
            {
              m_system.GetCoreTiming().Idle();
              break;
            }
          }

          // ctx->timebase is refreshed at burst start (SyncIn), and here we
          // incrementally advance it by the exact block cycle charges to
          // prevent guest busy-wait loops from spinning on a stale timebase.
          if (m_guest.exception)
          {
            // DolRecomp's runtime already redirected pc/msr/srr to the guest
            // exception vector; the flag only signals that it happened.
            m_guest.exception = 0;
            m_guest.program_exception = 0;
            ++m_native_exceptions;
          }
          if ((ppc.Exceptions & SYNC_EXCEPTION_MASK) != 0)
            break;  // Hook-raised synchronous exception: deliver via Dolphin below.
        } while (m_module_active && FastDispatchableAt(m_guest.pc) && ppc.downcount > 0 &&
                 *state_ptr == CPU::State::Running);
        SyncOut();
        if ((ppc.Exceptions & SYNC_EXCEPTION_MASK) != 0)
          power_pc.CheckExceptions();
      }
      else
      {
        // Run only the uncovered vector/stub, then hand control back as soon as
        // the PC re-enters module coverage. JitBase::Run() is an unbounded CPU
        // loop and cannot be used here: after the first exception vector it
        // would permanently take over execution from the static module.
        do
        {
          ppc.downcount -= interpreter.SingleStepInner();
          ++m_fallback_steps;
        } while (!(m_module_active && DispatchableAt(ppc.pc)) && ppc.downcount > 0 &&
                 *state_ptr == CPU::State::Running);
      }
    } while (ppc.downcount > 0 && *state_ptr == CPU::State::Running);
  }
}

void StaticRecompCore::SingleStep()
{
  // Debugger stepping runs through the interpreter; state outside Run() lives
  // in PowerPCState, so no sync is needed.
  auto& system = m_system;
  system.GetCoreTiming().Advance();
  system.GetPPCState().downcount -= system.GetInterpreter().SingleStepInner();
}
