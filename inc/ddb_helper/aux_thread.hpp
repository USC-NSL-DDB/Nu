#pragma once

#include <condition_variable>  // For std::condition_variable
#include <iostream>      // For std::cout, std::cerr, std::endl, std::flush
#include <mutex>         // For std::mutex, std::unique_lock, std::once_flag
#include <string>        // For std::string
#include <system_error>  // For std::system_error (exception handling)
#include <thread>        // For std::thread

// Platform-specific includes for thread naming
#ifdef __linux__
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <pthread.h>
#elif defined(__APPLE__) || defined(__MACH__)
#include <pthread.h>
#endif

namespace DDB {
const std::string DDB_AUX_THREAD_NAME = "DDB_AUX_THREAD";

static std::once_flag g_start_flag;

inline std::mutex& get_helper_mutex() {
  static std::mutex mtx;
  return mtx;
}

inline std::condition_variable& get_helper_cv() {
  static std::condition_variable cv;
  return cv;
}

inline void ddb_aux_thread_main_func() {
  // --- Set the thread name (platform-specific, best effort) ---
  try {
#if defined(__linux__) || defined(__APPLE__) || defined(__MACH__)
    // Get the native handle (pthread_t) and set the name
    // Note: Name length limit is often 15/16 chars (+ null terminator)
    pthread_t native_handle = pthread_self();
    std::string short_name = DDB_AUX_THREAD_NAME.substr(0, 15);
    int ret = pthread_setname_np(native_handle, short_name.c_str());
    if (ret != 0) {
      std::cerr << "[DDB Aux] Warning: Failed to set thread name: "
                << std::system_category().message(ret) << std::endl;
    }
#endif
  } catch (...) {
    std::cerr << "[DDB Aux] Warning: Exception during thread naming."
              << std::endl;
  }

  // --- Wait indefinitely using Mutex and Condition Variable ---
  try {
    std::unique_lock<std::mutex> lock(get_helper_mutex());
    get_helper_cv().wait(lock, [] { return false; });

    // This point should ideally never be reached if wait is working correctly
    std::cerr << "[DDB Aux] Warning: Wait finished unexpectedly." << std::endl
              << std::flush;

  } catch (const std::system_error& e) {
    std::cerr << "[DDB Aux] System error during wait: " << e.what() << " ("
              << e.code() << ")" << std::endl;
  } catch (...) {
    std::cerr << "[DDB Aux] Unknown exception during wait." << std::endl;
  }
  std::cerr << "[DDB Aux] Exiting unexpectedly." << std::endl;
}

// --- Public API ---

/**
 * @brief Starts the DDB auxiliary thread if it hasn't been started already.
 *
 * This function is safe to call multiple times and from multiple threads;
 * it guarantees that the helper thread is created and detached only once.
 * Call this early in your application's initialization (e.g., near the
 * beginning of main()).
 *
 * The aux thread will be named DDB_AUX_THREAD_NAME (if supported by the OS)
 * and will wait indefinitely, consuming minimal resources until needed by
 * DDB/GDB.
 *
 * @param verbose If true, print status messages to std::cout/std::cerr. Default
 * is true.
 * @return true if the helper thread was successfully started now or previously.
 * @return false if an error occurred during the first attempt to start the
 * thread.
 */
inline bool start_ddb_aux_thread(bool verbose = true) {
  bool success = true;
  bool started_now = false;

  try {
    std::call_once(g_start_flag, [&]() {
      // This lambda is executed only by the first thread reaching call_once
      started_now = true;  // Mark that we are the ones starting it
      if (verbose) {
        std::cout << "[DDB Aux] First call: Launching DDB aux thread ("
                  << DDB_AUX_THREAD_NAME << ")..." << std::endl
                  << std::flush;
      }
      try {
        std::thread aux_thread(ddb_aux_thread_main_func);
        aux_thread.detach();
        if (verbose) {
          std::cout << "[DDB Aux] DDB Aux thread detached and running."
                    << std::endl
                    << std::flush;
        }
      } catch (const std::system_error& e) {
        std::cerr << "[DDB Aux] FATAL: Failed to create the DDB aux thread: "
                  << e.what() << " (" << e.code() << ")" << std::endl
                  << std::flush;
        success = false;
        throw;
      } catch (...) {
        std::cerr << "[DDB Aux] FATAL: Unknown exception during DDB aux "
                     "thread creation."
                  << std::endl
                  << std::flush;
        success = false;
        throw;
      }
    });

    // If this call didn't start the thread, but a previous call did (and
    // succeeded), we still return true. If the previous call failed, call_once
    // won't retry, but the standard doesn't guarantee how state is handled if
    // the callable throws. Assuming call_once won't execute again if the first
    // attempt threw *out* of it. Re-checking success here isn't strictly
    // necessary if we assume call_once behavior. If this call *wasn't* the one
    // that started it, we know a previous call initiated it. We rely on the
    // 'success' variable captured by the lambda to report failure *if* this
    // call was the one responsible for starting.
    if (!started_now && verbose) {
      std::cout << "[DDB Aux] DDB aux thread was already started." << std::endl
                << std::flush;
    }
  } catch (const std::system_error& e) {
    if (started_now) {
      std::cerr
          << "[DDB Aux] Exception caught after attempting to start thread: "
          << e.what() << std::endl
          << std::flush;
    }
    success = false;
  } catch (...) {
    if (started_now) {
      std::cerr << "[DDB Aux] Unknown exception caught after attempting to "
                   "start thread."
                << std::endl
                << std::flush;
    }
    success = false;
  }
  return success;
}
}  // namespace DDB
