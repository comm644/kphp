// Compiler for PHP (aka KPHP)
// Copyright (c) 2023 LLC «V Kontakte»
// Distributed under the GPL v3 License, see LICENSE.notice.txt

#include "runtime/oom_handler.h"

#include "common/kprintf.h"
#include "runtime/allocator.h"
#include "runtime/critical_section.h"
#include "runtime/php_assert.h"
#include "server/php-runner.h"
#include "runtime/resumable.h"

bool f$register_kphp_on_oom_callback(const on_oom_callback_t &callback) {
  OomHandler &oom_handler_ctx = vk::singleton<OomHandler>::get();
  if (PhpScript::current_script->oom_handling_memory_ratio == 0) {
    php_warning("Trying to register OOM handler callback with no memory to run it on. You need to specify '--oom-handling-memory-ratio' option for it.");
    return false;
  }
  oom_handler_ctx.set_callback(callback);
  return true;
}

void OomHandler::invoke() noexcept {
  if (!callback_ || !PhpScript::in_script_context) {
    return;
  }
  if (callback_running_) {
    php_critical_error("Out of memory error happened inside OOM handler");
  } else {
    kprintf("Invoking OOM handler\n");
    dl::CriticalSectionGuard guard;
    forcibly_stop_all_running_resumables();
    dl::get_default_script_allocator().unfreeze_oom_handling_memory();
    callback_running_ = true;
    callback_();
    callback_running_ = false;
  }
}

void OomHandler::set_callback(const on_oom_callback_t &callback) noexcept {
  callback_ = callback;
}

void OomHandler::reset() noexcept {
  callback_ = {};
  callback_running_ = false;
}
