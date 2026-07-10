#include <atomic>
#include <cstdint>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <syncstream>

// --- Caladan Headers ---
extern "C" {
#include <base/assert.h>
#include <base/compiler.h>
#include <base/time.h>
#include <net/ip.h>
#include <runtime/net.h>
#include <runtime/sync.h>
}

#include <net.h>
#include <runtime.h>
#include <thread.h>

// --- External Libraries ---
#include <concurqueue/concurrentqueue.h>

// --- Standard Linux Headers ---
#include <arpa/inet.h>  // For htons, inet_ntoa, INADDR_ANY, ntohs
#include <netinet/in.h>
#include <pthread.h>
#include <sys/eventfd.h>  // For eventfd
#include <sys/socket.h>
#include <unistd.h>

#include <cstring>

#include "ddb_helper/ctrl_proxy.hpp"
#include "nu/command_line.hpp"
#include "nu/ctrl_client.hpp"
#include "nu/runtime.hpp"

// --- Constants ---
constexpr int kBacklog = 4096;
constexpr int kCtrlServerIp = MAKE_IP_ADDR(18, 18, 1, 1);
constexpr bool kVerbose = false;

// --- Caladan/Nu Constants ---
constexpr auto kDefaultNumKThreads = 5;
constexpr auto kDefaultNumGuaranteedCores = 2;
constexpr auto kDefaultNumSpinningCores = 2;
// IP address of the proxy server
constexpr auto kProxyIP = "18.18.1.250";
// lpid of the proxy server, should pick on that doesn't collide with any
// potential lpids.
constexpr uint16_t kProxylpid = 20202;

// --- Proxy Server Constants ---
constexpr int kProxyServerPort = 20202;  // Port the proxy listens on

// --- Global Queue ---
moodycamel::ConcurrentQueue<RequestContext*> request_queue;

// --- KLT Client Handler ---
void* handle_client_connection(void* arg) {
  int client_fd = static_cast<int>(reinterpret_cast<intptr_t>(arg));
  std::osyncstream ss(std::cout);
  if (kVerbose) {
    ss << "[KLT Handler " << client_fd << "]: Thread started." << std::endl;
    ss.emit();
  }

  while (true) {
    auto req_ctx = std::make_unique<RequestContext>(client_fd);

    // Check if eventfd creation failed in the constructor
    if (req_ctx->completion_efd == -1) {
      ss << "[KLT Handler " << client_fd
         << "]: Failed to create eventfd during context creation. Closing "
            "connection."
         << std::endl;
      ss.emit();
      close(client_fd);
      return nullptr;
    }

    try {
      auto hdr = read_header(client_fd);
      ssize_t payload_size = hdr.len;
      auto payload = std::make_unique_for_overwrite<std::byte[]>(payload_size);
      receive_exact(client_fd, payload.get(), payload_size);
      req_ctx->req_hdr = std::move(hdr);
      req_ctx->req_payload = std::move(payload);

      // --- Enqueue for Processing by ULT ---
      if (!request_queue.enqueue(req_ctx.get())) {
        std::cerr
            << "[KLT Handler " << client_fd
            << "]: Failed to enqueue request! Queue might be full or blocked?"
            << std::endl;
        req_ctx->error_occurred = true;
        throw std::runtime_error("Failed to enqueue request");
      }

      if (kVerbose) {
        ss << "[KLT Handler " << client_fd
           << "]: Enqueued request for processing (efd="
           << req_ctx->completion_efd << ")." << std::endl;
        ss << "[KLT Handler " << client_fd
           << "]: Waiting for completion signal on eventfd "
           << req_ctx->completion_efd << "." << std::endl;
        ss.emit();
      }

      req_ctx->wait_for_completion();

      // --- Process Reply (Send back to Client) ---
      auto optional_err = req_ctx->check_error();
      if (optional_err.has_value()) {
        std::cerr << "[KLT Handler " << client_fd
                  << "]: Error occurred during processing: "
                  << optional_err.value() << std::endl;
        req_ctx->reply_error();
        continue;
      }

      if (req_ctx->reply_success()) {
        if (kVerbose) {
          ss << "[KLT Handler " << client_fd
             << "]: Successfully wrote back a reply." << std::endl;
        }
      } else {
        ss << "[KLT Handler " << client_fd
           << "]: Failed to write back a successful reply." << std::endl;
      }
    } catch (const std::exception& e) {
      std::cerr << "[KLT Handler " << client_fd << "]: Exception: " << e.what()
                << std::endl;
      if (client_fd != -1) {
        req_ctx->reply_error();
        break;
      }
    } catch (...) {
      std::cerr << "[KLT Handler " << client_fd
                << "]: Unknown exception occurred." << std::endl;
      if (client_fd != -1) {
        req_ctx->reply_error();
        break;
      }
    }
  }

  // --- Cleanup ---
  if (client_fd != -1) {
    close(client_fd);
    if (kVerbose) {
      ss << "[KLT Handler " << client_fd << "]: Closed client connection."
         << std::endl;
    }
  }

  if (kVerbose) {
    ss << "[KLT Handler " << client_fd << "]: Thread finished." << std::endl;
  }
  return nullptr;
}

// --- KLT Listener Thread ---
void* kthread_handle(void* arg) {
  int server_fd = -1;
  struct sockaddr_in address;
  int opt = 1;
  socklen_t addrlen = sizeof(address);

  // --- Server Setup ---
  server_fd = socket(AF_INET, SOCK_STREAM, 0);
  if (server_fd < 0) {
    perror("FATAL: socket failed");
    // Consider signaling main thread to exit
    return nullptr;
  }

  // Allow reuse of address and port, crucial for quick restarts
  if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR | SO_REUSEPORT, &opt,
                 sizeof(opt))) {
    perror("FATAL: setsockopt failed");
    close(server_fd);
    return nullptr;
  }

  // Prepare the sockaddr_in structure
  memset(&address, 0, sizeof(address));
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = INADDR_ANY;  // Listen on all available interfaces
  address.sin_port = htons(kProxyServerPort);

  // Bind the socket to the address and port
  if (bind(server_fd, (struct sockaddr*)&address, sizeof(address)) < 0) {
    perror("FATAL: bind failed");
    close(server_fd);
    return nullptr;
  }

  // Start listening for incoming connections
  if (listen(server_fd, kBacklog) < 0) {
    perror("FATAL: listen failed");
    close(server_fd);
    return nullptr;
  }

  std::osyncstream(std::cout) << "[KLT Listener]: Server listening on port "
                              << kProxyServerPort << std::endl;

  // --- Accept Connections Loop ---
  while (true) {
    int new_socket = accept(server_fd, (struct sockaddr*)&address, &addrlen);
    if (new_socket < 0) {
      perror("accept failed");
      sleep(1);
      continue;
    }

    std::osyncstream ss(std::cout);
    ss << "[KLT Listener]: Accepted connection from "
       << inet_ntoa(address.sin_addr) << ":" << ntohs(address.sin_port)
       << " on fd " << new_socket << std::endl;
    ss.emit();

    // --- Spawn a Handler Thread for the New Client ---
    pthread_t client_tid;
    if (pthread_create(
            &client_tid, nullptr, handle_client_connection,
            reinterpret_cast<void*>(static_cast<intptr_t>(new_socket))) != 0) {
      perror("pthread_create for client handler failed");
      close(new_socket);
    } else {
      pthread_detach(client_tid);
      ss << "[KLT Listener]: Spawned handler thread " << client_tid
         << " for fd " << new_socket << "." << std::endl;
      ss.emit();
    }
  }

  std::osyncstream(std::cout)
      << "[KLT Listener]: Shutting down listener (unreachable)." << std::endl;
  close(server_fd);
  return nullptr;
}

namespace nu {
class CtrlProxy {
 public:
  CtrlProxy(nu::NodeIP ctrl_svr_ip, nu::lpid_t lpid) {
    // Nu's ControllerClient takes a Runtime::Mode where quicksand takes a
    // `bool main`. The proxy is a plain (non-main) server that only resolves
    // proclet locations, so kServer with isol=false.
    ctrl_client_ = std::make_unique<nu::ControllerClient>(
        ctrl_svr_ip, nu::Runtime::Mode::kServer, lpid, /* isol = */ false);
  }

  ~CtrlProxy() {}

  // proxy the `resolve_proclet`.
  nu::NodeIP resolve_proclet(nu::ProcletID id) {
    return this->ctrl_client_->resolve_proclet(id);
  }

 private:
  std::unique_ptr<nu::ControllerClient> ctrl_client_;
};

void handle_proclet_query(std::shared_ptr<CtrlProxy> proxy,
                          RequestContext* req_ctx) {
  // query the proclet
  std::osyncstream ss(std::cout);
  if (req_ctx->req_hdr.len != sizeof(uint64_t)) {
    std::ostringstream oss;
    oss << "[Caladan ULT for " << req_ctx->client_fd
        << "]: Error: Invalid payload size for command 1. Expected "
        << sizeof(uint64_t) << ", got " << req_ctx->req_hdr.len << "."
        << std::endl;
    std::cerr << oss.str() << std::endl;
    req_ctx->mark_error(oss.str());
  } else {
    uint64_t proclet_id;
    if (!req_ctx->req_payload) {
      std::cerr << "[Caladan ULT for " << req_ctx->client_fd
                << "]: Error: Request payload is null for command 1."
                << std::endl;
      req_ctx->error_occurred = true;
    } else {
      memcpy(&proclet_id, req_ctx->req_payload.get(), sizeof(uint64_t));
      proclet_id = ntoh64(proclet_id);

      ss << "[Caladan ULT for " << req_ctx->client_fd
         << "]: Received ProcletID query for ID " << proclet_id << "."
         << std::endl;
      ss.emit();

      auto node_ip = proxy->resolve_proclet(proclet_id);
      if (kVerbose) {
        char buf[IP_ADDR_STR_LEN];
        auto ip_addr = std::string(ip_addr_to_str(node_ip, buf));
        ss << "[Caladan ULT for " << req_ctx->client_fd
           << "]: Resolved ProcletID " << proclet_id
           << " to NodeIP: " << ip_addr << " (" << node_ip << ")" << std::endl;
        ss.emit();
      }

      auto resp = ProcletQueryResp{proclet_id, node_ip};
      auto reply_payload =
          std::make_unique_for_overwrite<std::byte[]>(ProcletQueryResp::SIZE);
      resp.serialize(
          std::span<std::byte>{reply_payload.get(), ProcletQueryResp::SIZE});
      req_ctx->reply_hdr = ProcletCtrlHdr::from_req_hdr(req_ctx->req_hdr,
                                                        ProcletQueryResp::SIZE);
      req_ctx->reply_payload = std::move(reply_payload);
    }
  }
}

void handle_request_in_caladan(std::shared_ptr<CtrlProxy> proxy,
                               RequestContext* req_ctx) {
  std::osyncstream ss(std::cout);
  ss << "[Caladan ULT for " << req_ctx->client_fd << "]: Started processing."
     << std::endl;
  ss.emit();
  auto cmd = req_ctx->req_hdr.cmd;

  if (cmd == 1) {
    handle_proclet_query(proxy, req_ctx);
  } else {
    std::ostringstream oss;
    oss << "[Caladan ULT for " << req_ctx->client_fd
        << "]: Error: Unknown command " << cmd << "." << std::endl;
    std::cerr << oss.str() << std::endl;
    req_ctx->mark_error(oss.str());
  }

  req_ctx->mark_complete();

  // --- Signal Completion via eventfd ---
  if (req_ctx->completion_efd != -1) {
    uint64_t signal_value = 1;
    ssize_t n =
        write(req_ctx->completion_efd, &signal_value, sizeof(signal_value));
    if (n != sizeof(signal_value)) {
      auto err = std::string("FATAL: write to eventfd failed in ULT");
      std::cerr << err << std::endl;
      req_ctx->mark_error(err);
    } else {
      if (kVerbose) {
        ss << "[Caladan ULT for " << req_ctx->client_fd
           << "]: Signaled completion on eventfd " << req_ctx->completion_efd
           << "." << std::endl;
        ss.emit();
      }
    }
  } else {
    std::ostringstream oss;
    oss << "[Caladan ULT for " << req_ctx->client_fd
        << "]: FATAL: Invalid completion_efd, cannot signal KLT!";
    std::cerr << oss.str() << std::endl;
    req_ctx->mark_error(oss.str());
  }

  ss << "[Caladan ULT for " << req_ctx->client_fd << "]: Finished."
     << std::endl;
  ss.emit();
}

int ctrl_proxy_main(int argc, char** argv) {
  nu::CaladanOptionsDesc desc(kDefaultNumGuaranteedCores,
                              kDefaultNumSpinningCores, kProxyIP);
  desc.parse(argc, argv);
  desc.kthreads = kDefaultNumKThreads;

  auto conf_path = desc.conf_path;
  if (conf_path.empty()) {
    conf_path = ".conf_" + std::to_string(getpid());
    write_options_to_file(conf_path, desc);
  }

  int ret = rt::RuntimeInit(conf_path, [&] {
    std::osyncstream ss(std::cout);

    if (conf_path.starts_with(".conf_")) {
      BUG_ON(remove(conf_path.c_str()));
    }

    ss << "[Caladan]: initialize Quicksand runtime." << std::endl;

    new (nu::get_runtime()) nu::Runtime();
    get_runtime()->init_base();

    auto proxy = std::make_shared<CtrlProxy>(kCtrlServerIp, kProxylpid);
    ss << "[Caladan]: Runtime initialized. Starting request processing loop."
       << std::endl;
    ss.emit();

    while (true) {
      // The kernel thread owns the request context.
      // Therefore, it takes care of the cleanup.
      // No need to free the pointer here.
      RequestContext* req_ctx = nullptr;

      if (request_queue.try_dequeue(req_ctx)) {
        ss << "[Caladan]: dequeued the request..." << std::endl;
        ss.emit();
        if (req_ctx->error_occurred) {
          std::cerr << "[Caladan]: Dequeued context for fd "
                    << req_ctx->client_fd
                    << " which already has an error. Signaling KLT immediately."
                    << std::endl;
          // Signal immediately without processing to unblock KLT
          if (req_ctx->completion_efd != -1) {
            uint64_t signal_value = 1;
            // Best effort signal, ignore write error here as KLT needs
            // unblocking anyway
            write(req_ctx->completion_efd, &signal_value, sizeof(signal_value));
          }
          continue;
        }

        if (kVerbose) {
          ss << "[Caladan]: Dequeued request for client fd "
             << req_ctx->client_fd << " (efd=" << req_ctx->completion_efd
             << "). Spawning ULT..." << std::endl;
          ss.emit();
        }

        // --- Spawn a Caladan ULT to handle this request ---
        rt::Spawn([=] { handle_request_in_caladan(proxy, req_ctx); });
      } else {
        rt::Yield();
      }
    }

    ss << "[Caladan]: Exiting processing loop (unreachable)." << std::endl;
  });

  if (ret) {
    std::cerr << "[Main]: Failed to start Caladan runtime, return code: " << ret
              << std::endl;
    // TODO: more graceful shutdown
    return ret;
  }
  return 0;
}
}  // namespace nu

int main(int argc, char** argv) {
  pthread_t listener_tid;
  std::osyncstream ss(std::cout);
  ss << "[Main]: Creating Linux listener thread..." << std::endl;

  if (pthread_create(&listener_tid, nullptr, kthread_handle, nullptr) != 0) {
    std::cerr << "FATAL: Failed to create Linux listener thread" << std::endl;
    return 1;
  }

  ss << "[Main]: Linux listener thread created (TID=" << listener_tid << ")."
     << std::endl;

  ss << "[Main]: Initializing Caladan runtime." << std::endl;

  // join the listener thread
  // pthread_join(listener_tid, nullptr);

  return nu::ctrl_proxy_main(argc, argv);
}
