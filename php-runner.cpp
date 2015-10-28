#define _FILE_OFFSET_BITS 64
#include <assert.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/time.h>
#include <ucontext.h>
#include <unistd.h>

#include "php-runner.h"

extern "C" {
#include "common/kprintf.h"
#include "common/server-functions.h"
#include "php-engine-vars.h"

query_stats_t query_stats;
long long query_stats_id = 1;
}

#include "runtime/allocator.h"
#include "runtime/exception.h"
#include "runtime/interface.h"

#include <exception>
using std::exception;

void PHPScriptBase::error (const char *error_message) {
  assert (is_running == true);
  current_script->state = rst_error;
  current_script->error_message = error_message;
  is_running = false;
  setcontext (&PHPScriptBase::exit_context);
}

void PHPScriptBase::check_tl() {
  if (tl_flag) {
    state = rst_error;
    error_message = "Timeout exit";
    pause();
  }
}

bool PHPScriptBase::is_protected (char *x) {
  return run_stack <= x && x < protected_end;
}

bool PHPScriptBase::check_stack_overflow (char *x) {
  assert (protected_end <= x && x < run_stack_end);
  long left = x - protected_end;
  return left < (1 << 12);
}


PHPScriptBase::PHPScriptBase (size_t mem_size, size_t stack_size)
  : state (rst_empty), query (NULL), mem_size (mem_size), stack_size (stack_size) {
  //fprintf (stderr, "PHPScriptBase: constructor\n");
  stack_size += 2 * getpagesize() - 1;
  stack_size /= getpagesize();
  stack_size *= getpagesize();
  run_stack = (char *)valloc (stack_size);
  assert (mprotect (run_stack, getpagesize(), PROT_NONE) == 0);
  protected_end = run_stack + getpagesize();
  run_stack_end = run_stack + stack_size;

  run_mem = (char *) malloc (mem_size);
  //fprintf (stderr, "[%p -> %p] [%p -> %p]\n", run_stack, run_stack_end, run_mem, run_mem + mem_size);
}

PHPScriptBase::~PHPScriptBase (void) {
  mprotect (run_stack, getpagesize(), PROT_READ | PROT_WRITE);
  free (run_stack);
  free (run_mem);
}

void PHPScriptBase::init (script_t *script, php_query_data *data_to_set) {
  assert (script != NULL);
  assert (state == rst_empty);

  query = NULL;
  state = rst_before_init;

  assert (state == rst_before_init);

  getcontext (&run_context);
  run_context.uc_stack.ss_sp = run_stack;
  run_context.uc_stack.ss_size = stack_size;
  run_context.uc_link = NULL;
  makecontext (&run_context, &cur_run, 0);

  run_main = script;
  data = data_to_set;

  state = rst_ready;

  error_message = "??? error";

  script_time = 0;
  net_time = 0;
  cur_timestamp = dl_time();
  queries_cnt = 0;

  query_stats_id++;
  memset (&query_stats, 0, sizeof (query_stats));

  PHPScriptBase::ml_flag = false;
}

void PHPScriptBase::pause() {
  //fprintf (stderr, "pause: \n");
  is_running = false;
  assert (swapcontext (&run_context, &exit_context) == 0);
  is_running = true;
  check_tl();
  //fprintf (stderr, "pause: ended\n");
}

void PHPScriptBase::resume() {
  assert (swapcontext (&exit_context, &run_context) == 0);
}

void dump_query_stats (void) {
  char tmp[100];
  char *s = tmp;
  if (query_stats.desc != NULL) {
    s += sprintf (s, "%s:", query_stats.desc);
  }
  if (query_stats.port != 0) {
    s += sprintf (s, "[port=%d]", query_stats.port);
  }
  if (query_stats.timeout > 0) {
    s += sprintf (s, "[timeout=%lf]", query_stats.timeout);
  }
  *s = 0;
  kprintf ("%s\n", tmp);

  if (query_stats.query != NULL) {
    const char *s = query_stats.query;
    const char *t = s;
    while (*t && *t != '\r' && *t != '\n') {
      t++;
    }
    kprintf ("QUERY:[%.*s]\n", (int)(t - s), s);
  }
}

void PHPScriptBase::update_net_time (void) {
  double new_cur_timestamp = dl_time();

  double net_add = new_cur_timestamp - cur_timestamp;
  if (net_add > 0.1) {
    if (query_stats.q_id == query_stats_id) {
      dump_query_stats();
    }
    kprintf ("LONG query: %lf\n", net_add);
  }
  net_time += net_add;

  cur_timestamp = new_cur_timestamp;
}

void PHPScriptBase::update_script_time (void) {
  double new_cur_timestamp = dl_time();
  script_time += new_cur_timestamp - cur_timestamp;
  cur_timestamp = new_cur_timestamp;
}

run_state_t PHPScriptBase::iterate() {
  current_script = this;

  assert (state == rst_ready);
  state = rst_running;

  update_net_time();

  resume();

  update_script_time();

  queries_cnt += state == rst_query;
  memset (&query_stats, 0, sizeof (query_stats));
  query_stats_id++;

  return state;
}

void PHPScriptBase::finish() {
  assert (state == rst_finished || state == rst_error);
  run_state_t save_state = state;

  state = rst_uncleared;
  update_net_time();
  worker_acc_stats.tot_queries++;
  worker_acc_stats.worked_time += script_time + net_time;
  worker_acc_stats.net_time += net_time;
  worker_acc_stats.script_time += script_time;
  worker_acc_stats.tot_script_queries += queries_cnt;

  if (save_state == rst_error) {
    assert (error_message != NULL);
    kprintf ("Critical error during script execution: %s\n", error_message);
  }
  if (save_state == rst_error || (int)dl::memory_get_total_usage() >= 100000000) {
    if (data != NULL) {
      http_query_data *http_data = data->http_data;
      if (http_data != NULL) {
        assert (http_data->headers);

        kprintf ("HEADERS: len = %d\n%.*s\nEND HEADERS\n", http_data->headers_len, min (http_data->headers_len, 1 << 16), http_data->headers);
        kprintf ("POST: len = %d\n%.*s\nEND POST\n", http_data->post_len, min (http_data->post_len, 1 << 16), http_data->post == NULL ? "" : http_data->post);
      }
    }
  }

  static char buf[5000];
  buf[0] = 0;
  if (data != NULL) {
    http_query_data *http_data = data->http_data;
    if (http_data != NULL) {
      if (no_get_data_in_log) {
        sprintf (buf, "[uri = %.*s?<truncated>]", min (http_data->uri_len, 200), http_data->uri);
      } else {
        sprintf (buf, "[uri = %.*s?%.*s]", min (http_data->uri_len, 200), http_data->uri,
                                           min (http_data->get_len, 4000), http_data->get);
      }
    }
  }
  kprintf ("[worked = %.3lf, net = %.3lf, script = %.3lf, queries_cnt = %5d, static_memory = %9d, peak_memory = %9d, total_memory = %9d] %s\n",
                         script_time + net_time, net_time, script_time, queries_cnt,
                         (int)dl::static_memory_used, (int)dl::max_real_memory_used, (int)dl::memory_get_total_usage(), buf);
}

void PHPScriptBase::clear() {
  assert (state == rst_uncleared);
  run_main->clear();
  free_static();
  state = rst_empty;
}

void PHPScriptBase::ask_query (void *q) {
  assert (state == rst_running);
  query = q;
  state = rst_query;
  //fprintf (stderr, "ask_query: pause\n");
  pause();
  //fprintf (stderr, "ask_query: after pause\n");
}

void PHPScriptBase::set_script_result (script_result *res_to_set) {
  assert (state == rst_running);
  res = res_to_set;
  state = rst_finished;
  pause();
}

void PHPScriptBase::query_readed (void) {
  assert (is_running == false);
  assert (state == rst_query);
  state = rst_query_running;
}

void PHPScriptBase::query_answered (void) {
  assert (state == rst_query_running);
  state = rst_ready;
  //fprintf (stderr, "ok\n");
}

void PHPScriptBase::run (void) {
  is_running = true;
  check_tl();

  if (data != NULL) {
    http_query_data *http_data = data->http_data;
    if (http_data != NULL) {
      //fprintf (stderr, "arguments\n");
      //fprintf (stderr, "[uri = %.*s]\n", http_data->uri_len, http_data->uri);
      //fprintf (stderr, "[get = %.*s]\n", http_data->get_len, http_data->get);
      //fprintf (stderr, "[headers = %.*s]\n", http_data->headers_len, http_data->headers);
      //fprintf (stderr, "[post = %.*s]\n", http_data->post_len, http_data->post);
    }

    rpc_query_data *rpc_data = data->rpc_data;
    if (rpc_data != NULL) {
      /*
      fprintf (stderr, "N = %d\n", rpc_data->len);
      for (int i = 0; i < rpc_data->len; i++) {
        fprintf (stderr, "%d: %10d\n", i, rpc_data->data[i]);
      }
      */
    }
  }
  assert (run_main->run != NULL);

//  regex_ptr = NULL;
#ifdef FAST_EXCEPTIONS
  CurException = NULL;
  run_main->run (data, run_mem, mem_size);
  if (!CurException) {
    set_script_result (NULL);
  } else {
    const Exception &e = *CurException;
    const char *msg = dl_pstr ("%s%d%sError %d: %s.\nUnhandled Exception caught in file %s at line %d.\n"
                               "Backtrace:\n%s",
                               engine_tag, (int)time (NULL), engine_pid,
                               e.code, e.message.c_str(), e.file.c_str(), e.line,
                               f$exception_getTraceAsString (e).c_str());
    fprintf (stderr, "%s", msg);
    fprintf (stderr, "-------------------------------\n\n");
    error (msg);
  }
#else
  //in fact it may lead to undefined behaviour
  try {
    run_main->run (data, run_mem, mem_size);
    set_script_result (NULL);
  } catch (Exception &e) {
    const char *msg = dl_pstr ("%s%d%sError %d: %s.\nUnhandled Exception caught in file %s at line %d.\n"
                               "Backtrace:\n%s",
                               engine_tag, (int)time (NULL), engine_pid,
                               e.code, e.message.c_str(), e.file.c_str(), e.line,
                               f$exception_getTraceAsString (e).c_str());
    fprintf (stderr, "%s", msg);
    fprintf (stderr, "-------------------------------\n\n");
    error (msg);
  } catch (exception &e) {
    const char *msg = dl_pstr ("unhandled stl exception caught [%s]\n", e.what());
    fprintf (stderr, "%s", msg);
    error (msg);
  } catch (...) {
    error ("unhandled unknown exception caught\n");
  }
#endif
}

double PHPScriptBase::get_net_time (void) const {
  return net_time;
}

double PHPScriptBase::get_script_time (void) {
  assert (state == rst_running);
  update_script_time();
  return script_time;
}

int PHPScriptBase::get_net_queries_count (void) const {
  return queries_cnt;
}


PHPScriptBase * volatile PHPScriptBase::current_script;
ucontext_t PHPScriptBase::exit_context;
volatile bool PHPScriptBase::is_running = false;
volatile bool PHPScriptBase::tl_flag = false;
volatile bool PHPScriptBase::ml_flag = false;

int msq (int x) {
  //fprintf (stderr, "msq: (x = %d)\n", x);
  //TODO: this memory may leak
  query_int *q = new query_int();
  q->type = q_int;
  q->val = x;
  //fprintf (stderr, "msq: ask query\n");
  PHPScriptBase::current_script->ask_query ((void *)q);
  //fprintf (stderr, "msq: go query result\n");
  int res = q->res;
  delete q;
  return res;
}

//TODO: sometimes I need to call old handlers
//TODO: recheck!
inline void kwrite_str (int fd, const char *s) {
  const char *t = s;
  while (*t) {
    t++;
  }
  int len = (int)(t - s);
  kwrite (fd, s, len);
}

inline void write_str (int fd, const char *s) {
  const char *t = s;
  while (*t) {
    t++;
  }
  int len = (int)(t - s);
  if (len > 1000) {
    len = 1000;
  }
  write (fd, s, len);
}

void sigalrm_handler (int signum) {
  if (dl::in_critical_section) {
    kwrite_str (2, "pending sigalrm catched\n");
    dl::pending_signals |= 1 << signum;
    return;
  }
  dl::pending_signals = 0;
  
  if (false) {
    void *buffer[64];
    int nptrs = backtrace (buffer, 64);
    backtrace_symbols_fd (buffer, nptrs, 2);
  }

  kwrite_str (2, "in sigalrm_handler\n");
  PHPScriptBase::tl_flag = true;
  if (PHPScriptBase::is_running) {
    kwrite_str (2, "timeout exit\n");
    PHPScriptBase::error ("timeout exit");
    assert ("unreachable point" && 0);
  }
}

void sigusr2_handler (int signum) {
  kwrite_str (2, "in sigusr2_handler\n");
  if (dl::in_critical_section) {
    dl::pending_signals |= 1 << signum;
    kwrite_str (2, "  in critical section: signal delayed\n");
    return;
  }
  dl::pending_signals = 0;

  PHPScriptBase::ml_flag = true;
  if (PHPScriptBase::is_running) {
    kwrite_str (2, "memory limit exit\n");
    PHPScriptBase::error ("memory limit exit");
    assert ("unreachable point" && 0);
  }
}

void sigsegv_handler (int signum __attribute__((unused)), siginfo_t *info, void *data __attribute__((unused))) {
  write_str (2, engine_tag);
  char buf[13], *s = buf + 13;
  int t = (int)time (NULL);
  *--s = 0;
  do {
    *--s = (char)(t % 10 + '0');
    t /= 10;
  } while (t > 0);
  write_str (2, s);
  write_str (2, engine_pid);

  void *addr = info->si_addr;
  if (PHPScriptBase::is_running && PHPScriptBase::current_script->is_protected ((char *)addr)){
    write_str (2, "Error -1: Callstack overflow");
/*    if (regex_ptr != NULL) {
      write_str (2, " regex = [");
      write_str (2, regex_ptr->c_str());
      write_str (2, "]\n");
    }*/
    dl_print_backtrace();
    if (dl::in_critical_section) {
      kwrite_str (2, "In critical section: calling _exit (124)\n");
      _exit (124);
    } else {
      PHPScriptBase::error ("sigsegv(stack overflow)");
    }
  } else {
    write_str (2, "Error -2: Segmentation fault");
    //dl_runtime_handler (signum);
    if (PHPScriptBase::current_script->data) {
      http_query_data *data = PHPScriptBase::current_script->data->http_data;
      if (data) {
        write_str (2, "\nuri\n");
        write(2, data->uri, data->uri_len);
        write_str (2, "\nget\n");
        write(2, data->get, data->get_len);
        write_str (2, "\nheaders\n");
        write(2, data->headers, data->headers_len);
        write_str (2, "\npost\n");
        write(2, data->post, data->post_len);
      }
    }
    dl_print_backtrace();
    _exit (123);
  }
}

static __inline__ void *get_sp (void) {
  void *sp;
#if defined(__i386__)
  __asm__ volatile ("movl %%esp, %[r]" : [r] "=r" (sp));
#elif defined(__x86_64__)
  __asm__ volatile ("movq %%rsp, %[r]" : [r] "=r" (sp));
#endif
  return sp;
}

void check_stack_overflow (void) __attribute__ ((noinline));
void check_stack_overflow (void) {
  if (PHPScriptBase::is_running) {
    void *sp = get_sp();
    bool f = PHPScriptBase::current_script->check_stack_overflow ((char *)sp);
    if (f) {
      raise (SIGUSR2);
      fprintf (stderr, "_exiting in check_stack_overflow\n");
      _exit (1);
    }
  }
}

//C interface
void init_handlers (void) {
  stack_t segv_stack;
  segv_stack.ss_sp = valloc (SEGV_STACK_SIZE);
  segv_stack.ss_flags = 0;
  segv_stack.ss_size = SEGV_STACK_SIZE;
  sigaltstack (&segv_stack, NULL);

  ksignal (SIGALRM, sigalrm_handler);
  ksignal (SIGUSR2, sigusr2_handler);

  dl_sigaction (SIGSEGV, NULL, dl_get_empty_sigset(), SA_SIGINFO | SA_ONSTACK | SA_RESTART, sigsegv_handler);
  dl_sigaction (SIGBUS, NULL, dl_get_empty_sigset(), SA_SIGINFO | SA_ONSTACK | SA_RESTART, sigsegv_handler);
}

void php_script_finish (void *ptr) {
  ((PHPScriptBase *)ptr)->finish();
}

void php_script_clear (void *ptr) {
  ((PHPScriptBase *)ptr)->clear();
}

void php_script_free (void *ptr) {
  delete (PHPScriptBase *)ptr;
}

void php_script_init (void *ptr, script_t *f_run, php_query_data *data_to_set) {
  ((PHPScriptBase *)ptr)->init (f_run, data_to_set);
}

run_state_t php_script_iterate (void *ptr) {
  return ((PHPScriptBase *)ptr)->iterate();
}

query_base *php_script_get_query (void *ptr) {
  return (query_base *)((PHPScriptBase *)ptr)->query;
}

script_result *php_script_get_res (void *ptr) {
  return ((PHPScriptBase *)ptr)->res;
}

void php_script_query_readed (void *ptr) {
  ((PHPScriptBase *)ptr)->query_readed();
}

void php_script_query_answered (void *ptr) {
  ((PHPScriptBase *)ptr)->query_answered();
}

run_state_t php_script_get_state (void *ptr) {
  return ((PHPScriptBase *)ptr)->state;
}

void *php_script_create (size_t mem_size, size_t stack_size) {
  return (void *)(new PHPScriptBase (mem_size, stack_size));
}

void php_script_terminate (void *ptr, const char *error_message) {
  ((PHPScriptBase *)ptr)->state = rst_error;
  ((PHPScriptBase *)ptr)->error_message = error_message;
}

const char *php_script_get_error (void *ptr) {
  return ((PHPScriptBase *)ptr)->error_message;
}

void php_script_set_timeout (double t) {
  assert (t >= 0.1);

  static struct itimerval timer;
  timer.it_interval.tv_sec = 0;
  timer.it_interval.tv_usec = 0;

  timer.it_value.tv_sec = 0;
  timer.it_value.tv_usec = 0;

  //stop old timer
  setitimer (ITIMER_REAL, &timer, NULL);

  if (t > MAX_SCRIPT_TIMEOUT) {
    return;
  }

  int sec = (int)t, usec = (int)((t - sec) * 1000000);
  timer.it_value.tv_sec = sec;
  timer.it_value.tv_usec = usec;

  PHPScriptBase::tl_flag = false;
  setitimer (ITIMER_REAL, &timer, NULL);
}

static php_immediate_stats_t imm_stats[2];
static sig_atomic_t imm_stats_i = 0;

static php_immediate_stats_t *get_new_imm_stats (void) {
  return &imm_stats[imm_stats_i ^ 1];
}
static void upd_imm_stats (void) {
  int x = imm_stats_i;
  imm_stats_i = 1 ^ x;
  memcpy (get_new_imm_stats(), get_imm_stats(), sizeof (php_immediate_stats_t));
}

typedef enum {ss_idle, ss_wait_net, ss_running, ss_custom} server_status_t;
static void upd_server_status (server_status_t server_status, const char *desc, int desc_len) {
  php_immediate_stats_t *imm = get_new_imm_stats();
  switch (server_status) {
    case ss_idle:
      imm->is_running = 0;
      imm->is_wait_net = 0;
      break;
    case ss_wait_net:
      imm->is_running = 1;
      imm->is_wait_net = 1;
      break;
    case ss_running:
      imm->is_running = 1;
      imm->is_wait_net = 0;
      break;
    case ss_custom:
      break;
  }
  if (desc_len >= IMM_STATS_DESC_LEN) {
    desc_len = IMM_STATS_DESC_LEN - 1;
  }

  if (server_status != ss_custom) {
    imm->timestamp = dl_time();
    memcpy (imm->desc, desc, desc_len);
    imm->desc[desc_len] = 0;
  } else {
    imm->custom_timestamp = dl_time();
    memcpy (imm->custom_desc, desc, desc_len);
    imm->custom_desc[desc_len] = 0;
  }

  upd_imm_stats();
}

php_immediate_stats_t *get_imm_stats (void) {
  return &imm_stats[imm_stats_i];
}
void custom_server_status (const char *status, int status_len) {
  upd_server_status (ss_custom, status, status_len);
}
void server_status_rpc (int port, long long actor_id, double start_time) {
  php_immediate_stats_t *imm = get_new_imm_stats();
  imm->port = port;
  imm->actor_id = actor_id;
  imm->rpc_timestamp = start_time;
  upd_imm_stats();
}
void idle_server_status (void) {
  upd_server_status (ss_idle, "Idle", 4);
}
void wait_net_server_status (void) {
  upd_server_status (ss_wait_net, "WaitNet", 7);
}
void running_server_status (void) {
  upd_server_status (ss_running, "Running", 7);
}

void PHPScriptBase::cur_run (void) {
  current_script->run();
}

