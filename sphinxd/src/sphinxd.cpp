/*
Copyright 2018 The Sphinxd Authors.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
*/

#include <sphinx/buffer.h>
#include <sphinx/hardware.h>
#include <sphinx/logmem.h>
#include <sphinx/memory.h>
#include <sphinx/protocol.h>
#include <sphinx/reactor.h>
#include <sphinx/string.h>

#include <cassert> // FIXME
#include <iostream>
#include <sstream>
#include <thread>
#include <vector>

#include <getopt.h>
#include <libgen.h>
#include <netinet/in.h>
#include <sys/mman.h>

#include "version.h"

static std::string program;

static constexpr int DEFAULT_TCP_PORT = 11211;
static constexpr int DEFAULT_UDP_PORT = 0; /* disabled */
static constexpr const char* DEFAULT_LISTEN_ADDR = "0.0.0.0";
static constexpr int DEFAULT_MEMORY_LIMIT = 64;
static constexpr int DEFAULT_SEGMENT_SIZE = 2;
static constexpr int DEFAULT_LISTEN_BACKLOG = 1024;
static constexpr int DEFAULT_NR_THREADS = 4;

struct Args
{
  std::string listen_addr = DEFAULT_LISTEN_ADDR;
  int tcp_port = DEFAULT_TCP_PORT;
  int memory_limit = DEFAULT_MEMORY_LIMIT; /* in MB */
  int segment_size = DEFAULT_SEGMENT_SIZE; /* in MB */
  int listen_backlog = DEFAULT_LISTEN_BACKLOG;
  int nr_threads = DEFAULT_NR_THREADS;
  std::string backend = sphinx::reactor::Reactor::default_backend();
  std::set<int> isolate_cpus;
  bool sched_fifo = false;
};

enum class Opcode : uint8_t
{
  Set,
  SetOk,
  SetErrorOutOfMemory,
  Get,
  GetOk,
};

struct Command
{
  std::shared_ptr<sphinx::reactor::Socket> sock;
  sphinx::buffer::Buffer buffer;
  Opcode op;
  uint8_t thread_id;
  uint8_t key_size;

  std::string_view key() const
  {
    return buffer.string_view().substr(0, key_size);
  }
  std::string_view blob() const
  {
    return buffer.string_view().substr(key_size);
  }
};

struct Connection
{
  sphinx::buffer::Buffer _rx_buffer;
};

class Server
{
  std::unique_ptr<sphinx::reactor::Reactor> _reactor;
  sphinx::logmem::Log _log;

public:
  Server(const sphinx::logmem::LogConfig& log_cfg,
         const std::string& backend,
         size_t thread_id,
         size_t nr_threads);
  void serve(const Args& args);

private:
  void on_message(void* data);
  void accept(int sockfd);
  void recv(Connection& conn,
            std::shared_ptr<sphinx::reactor::TcpSocket> sock,
            std::string_view msg);
  size_t process_one(std::shared_ptr<sphinx::reactor::TcpSocket> sock, std::string_view msg);
  size_t find_target(const sphinx::logmem::Hash& hash) const;
};

Server::Server(const sphinx::logmem::LogConfig& log_cfg,
               const std::string& backend,
               size_t thread_id,
               size_t nr_threads)
  : _reactor{sphinx::reactor::make_reactor(backend,
                                           thread_id,
                                           nr_threads,
                                           [this](void* data) { this->on_message(data); })}
  , _log{log_cfg}
{
}

void
Server::serve(const Args& args)
{
  auto accept_fn = [this](int sockfd) { this->accept(sockfd); };
  auto listener = sphinx::reactor::make_tcp_listener(
    args.listen_addr, args.tcp_port, args.listen_backlog, std::move(accept_fn));
  _reactor->accept(std::move(listener));
  _reactor->run();
}

void
Server::on_message(void* data)
{
  auto* cmd = reinterpret_cast<Command*>(data);
  switch (cmd->op) {
    case Opcode::Set: {
      if (this->_log.append(cmd->key(), cmd->blob())) {
        cmd->op = Opcode::SetOk;
      } else {
        cmd->op = Opcode::SetErrorOutOfMemory;
      }
      assert(_reactor->send_msg(cmd->thread_id, cmd));
      break;
    }
    case Opcode::SetOk: {
      std::string response{"STORED\r\n"};
      if (!cmd->sock->send(response.c_str(), response.size(), std::nullopt)) {
        _reactor->send(cmd->sock);
      }
      delete cmd;
      break;
    }
    case Opcode::SetErrorOutOfMemory: {
      std::string response{"SERVER_ERROR out of memory storing object\r\n"};
      if (!cmd->sock->send(response.c_str(), response.size(), std::nullopt)) {
        _reactor->send(cmd->sock);
      }
      delete cmd;
      break;
    }
    case Opcode::Get: {
      const auto& key = cmd->key();
      auto search = _log.find(key);
      std::string response;
      if (search) {
        const auto& value = *search;
        cmd->buffer.append(value);
      }
      cmd->op = Opcode::GetOk;
      assert(_reactor->send_msg(cmd->thread_id, cmd)); // FIXME
      break;
    }
    case Opcode::GetOk: {
      std::string response;
      if (!cmd->blob().empty()) {
        response += "VALUE ";
        response += cmd->key();
        response += " 0 ";
        response += sphinx::to_string(cmd->blob().size());
        response += "\r\n";
        response += cmd->blob();
        response += "\r\n";
      }
      response += "END\r\n";
      if (!cmd->sock->send(response.c_str(), response.size(), std::nullopt)) {
        _reactor->send(cmd->sock);
      }
      delete cmd;
      break;
    }
  }
}

void
Server::accept(int sockfd)
{
  Connection conn;
  auto recv_fn =
    [this, conn = std::move(conn)](const std::shared_ptr<sphinx::reactor::TcpSocket>& sock,
                                   std::string_view msg) mutable { this->recv(conn, sock, msg); };
  auto sock = std::make_shared<sphinx::reactor::TcpSocket>(sockfd, std::move(recv_fn));
  sock->set_tcp_nodelay(true);
  this->_reactor->recv(std::move(sock));
}

void
Server::recv(Connection& conn,
             std::shared_ptr<sphinx::reactor::TcpSocket> sock,
             std::string_view msg)
{
  if (msg.size() == 0) {
    _reactor->close(sock);
    return;
  }
  if (conn._rx_buffer.is_empty()) {
    for (;;) {
      if (msg.find('\n') == std::string_view::npos) {
        conn._rx_buffer.append(msg);
        break;
      }
      size_t nr_consumed = process_one(sock, msg);
      if (!nr_consumed) {
        conn._rx_buffer.append(msg);
        break;
      }
      msg.remove_prefix(nr_consumed);
    }
  } else {
    conn._rx_buffer.append(msg);
    for (;;) {
      msg = conn._rx_buffer.string_view();
      if (msg.find('\n') == std::string_view::npos) {
        break;
      }
      size_t nr_consumed = process_one(sock, msg);
      if (!nr_consumed) {
        break;
      }
      conn._rx_buffer.remove_prefix(nr_consumed);
    }
  }
}

size_t
Server::process_one(std::shared_ptr<sphinx::reactor::TcpSocket> sock, std::string_view msg)
{
  using namespace sphinx::memcache;
  Parser parser;
  size_t nr_consumed = parser.parse(msg);
  switch (parser._state) {
    case Parser::State::Error: {
      std::string response{"ERROR\r\n"};
      if (!sock->send(response.c_str(), response.size(), std::nullopt)) {
        _reactor->send(sock);
      }
      break;
    }
    case Parser::State::CmdSet: {
      size_t data_block_size = parser._blob_size + 2;
      if (msg.size() < (nr_consumed + data_block_size)) {
        nr_consumed = 0;
        break;
      }
      nr_consumed += data_block_size;
      const auto& key = parser.key();
      auto hash = sphinx::logmem::Object::hash_of(key);
      auto target_id = find_target(hash);
      std::string_view blob{parser._blob_start, parser._blob_size};
      if (target_id == _reactor->thread_id()) {
        if (this->_log.append(key, blob)) {
          std::string response{"STORED\r\n"};
          if (!sock->send(response.c_str(), response.size(), std::nullopt)) {
            _reactor->send(sock);
          }
        } else {
          std::string response{"SERVER_ERROR out of memory storing object\r\n"};
          if (!sock->send(response.c_str(), response.size(), std::nullopt)) {
            _reactor->send(sock);
          }
        }
      } else {
        Command* cmd = new Command();
        cmd->sock = sock;
        cmd->op = Opcode::Set;
        cmd->key_size = key.size();
        cmd->buffer.append(key);
        cmd->buffer.append(blob);
        cmd->thread_id = _reactor->thread_id();
        assert(_reactor->send_msg(target_id, cmd)); // FIXME
      }
      break;
    }
    case Parser::State::CmdGet: {
      const auto& key = parser.key();
      auto hash = sphinx::logmem::Object::hash_of(key);
      auto target_id = find_target(hash);
      if (target_id == _reactor->thread_id()) {
        std::string response;
        auto search = this->_log.find(key);
        if (search) {
          const auto& value = search.value();
          response += "VALUE ";
          response += key;
          response += " 0 ";
          response += sphinx::to_string(value.size());
          response += "\r\n";
          response += value;
          response += "\r\n";
        }
        response += "END\r\n";
        if (!sock->send(response.data(), response.size(), std::nullopt)) {
          _reactor->send(sock);
        }
      } else {
        Command* cmd = new Command();
        cmd->sock = sock;
        cmd->op = Opcode::Get;
        cmd->key_size = key.size();
        cmd->buffer.append(key);
        cmd->thread_id = _reactor->thread_id();
        assert(_reactor->send_msg(target_id, cmd)); // FIXME
      }
      break;
    }
  }
  return nr_consumed;
}

size_t
Server::find_target(const sphinx::logmem::Hash& hash) const
{
  size_t nr_threads = _reactor->nr_threads();
  if (nr_threads == 1) {
    return _reactor->thread_id();
  }
  return hash % nr_threads;
}

static void
print_version()
{
  std::cout << "Sphinx " << SPHINX_VERSION << std::endl;
}

static void
print_usage()
{
  std::cout << "Usage: " << program << " [OPTION]..." << std::endl;
  std::cout << "Start the Sphinx daemon." << std::endl;
  std::cout << std::endl;
  std::cout << "Options:" << std::endl;
  std::cout << "  -p, --port number           TCP port to listen to (default: " << DEFAULT_TCP_PORT
            << ")" << std::endl;
  std::cout << "  -U, --port number           UDP port to listen to (default: " << DEFAULT_UDP_PORT
            << ")" << std::endl;
  std::cout << "  -l, --listen address        interface to listen to (default: "
            << DEFAULT_LISTEN_ADDR << ")" << std::endl;
  std::cout << "  -m, --memory-limit number   Memory limit in MB (default: " << DEFAULT_MEMORY_LIMIT
            << ")" << std::endl;
  std::cout << "  -s, --segment-size number   Segment size in MB (default: " << DEFAULT_SEGMENT_SIZE
            << ")" << std::endl;
  std::cout << "  -b, --listen-backlog number Listen backlog size (default: "
            << DEFAULT_LISTEN_BACKLOG << ")" << std::endl;
  std::cout << "  -t, --threads number        number of threads to use (default: "
            << DEFAULT_NR_THREADS << ")" << std::endl;
  std::cout << "  -I, --io-backend name       I/O backend (default: "
            << sphinx::reactor::Reactor::default_backend() << ")" << std::endl;
  std::cout << "  -i, --isolate-cpus list     list of CPUs to isolate application threads"
            << std::endl;
  std::cout << "  -S, --sched-fifo            use SCHED_FIFO scheduling policy" << std::endl;
  std::cout << "      --help                  print this help text and exit" << std::endl;
  std::cout << "      --version               print Sphinx version and exit" << std::endl;
  std::cout << std::endl;
}

static void
print_opt_error(const std::string& option, const std::string& reason)
{
  std::cerr << program << ": " << reason << " '" << option << "' option" << std::endl;
  std::cerr << "Try '" << program << " --help' for more information" << std::endl;
}

static void
print_unrecognized_opt(const std::string& option)
{
  print_opt_error(option, "unregonized");
}

static std::set<int>
parse_cpu_list(const std::string& raw_cpu_list)
{
  std::set<int> cpu_list;
  std::istringstream iss(raw_cpu_list);
  std::string token;
  while (std::getline(iss, token, ',')) {
    cpu_list.emplace(std::stoi(token));
  }
  return cpu_list;
}

static Args
parse_cmd_line(int argc, char* argv[])
{
  static struct option long_options[] = {{"port", required_argument, 0, 'p'},
                                         {"listen", required_argument, 0, 'l'},
                                         {"memory-limit", required_argument, 0, 'm'},
                                         {"segment-size", required_argument, 0, 's'},
                                         {"listen-backlog", required_argument, 0, 'b'},
                                         {"threads", required_argument, 0, 't'},
                                         {"io-backend", required_argument, 0, 'I'},
                                         {"isolate-cpus", required_argument, 0, 'i'},
                                         {"sched-fifo", no_argument, 0, 'S'},
                                         {"help", no_argument, 0, 'h'},
                                         {"version", no_argument, 0, 'v'},
                                         {0, 0, 0, 0}};
  Args args;
  int opt, long_index;
  while ((opt = ::getopt_long(argc, argv, "p:l:m:s:b:t:I:i:S", long_options, &long_index)) != -1) {
    switch (opt) {
      case 'p':
        args.tcp_port = std::stoi(optarg);
        break;
      case 'l':
        args.listen_addr = optarg;
        break;
      case 'm':
        args.memory_limit = std::stoi(optarg);
        break;
      case 's':
        args.segment_size = std::stoi(optarg);
        break;
      case 'b':
        args.listen_backlog = std::stoi(optarg);
        break;
      case 't':
        args.nr_threads = std::stoi(optarg);
        break;
      case 'I':
        args.backend = optarg;
        break;
      case 'i':
        args.isolate_cpus = parse_cpu_list(optarg);
        break;
      case 'S':
        args.sched_fifo = true;
        break;
      case 'h':
        print_usage();
        std::exit(EXIT_SUCCESS);
      case 'v':
        print_version();
        std::exit(EXIT_SUCCESS);
      case '?':
        print_unrecognized_opt(argv[optind - 1]);
        std::exit(EXIT_FAILURE);
      default:
        print_usage();
        std::exit(EXIT_FAILURE);
    }
  }
  if (args.memory_limit % args.nr_threads != 0) {
    throw std::invalid_argument("memory limit (" + std::to_string(args.memory_limit) +
                                ") is not divisible by number of threads (" +
                                std::to_string(args.nr_threads) +
                                "), which is required for partitioning");
  }
  return args;
}

void
server_thread(size_t thread_id, std::optional<int> cpu_id, const Args& args)
{
  try {
    if (cpu_id) {
      cpu_set_t cpuset;
      CPU_ZERO(&cpuset);
      CPU_SET(*cpu_id, &cpuset);
      auto err = ::pthread_setaffinity_np(::pthread_self(), sizeof(cpu_set_t), &cpuset);
      if (err != 0) {
        throw std::system_error(err, std::system_category(), "pthread_setaffinity_np");
      }
    }
    if (args.sched_fifo) {
      ::sched_param param = {};
      param.sched_priority = 1;
      auto err = ::pthread_setschedparam(::pthread_self(), SCHED_FIFO, &param);
      if (err != 0) {
        throw std::system_error(errno, std::system_category(), "pthread_setschedparam");
      }
    }
    size_t mem_size = size_t(args.memory_limit) * 1024 * 1024;
    sphinx::memory::Memory memory = sphinx::memory::Memory::mmap(mem_size / args.nr_threads);
    sphinx::logmem::LogConfig log_cfg;
    log_cfg.segment_size = args.segment_size * 1024 * 1024;
    log_cfg.memory_ptr = reinterpret_cast<char*>(memory.addr());
    log_cfg.memory_size = memory.size();
    Server server{log_cfg, args.backend, thread_id, size_t(args.nr_threads)};
    server.serve(args);
  } catch (const std::exception& e) {
    std::cerr << "error: " << e.what() << std::endl;
    std::exit(EXIT_FAILURE);
  }
}

struct CpuAffinity
{
  std::set<int> isolate_cpus;
  std::optional<int> next_id;
  CpuAffinity(const std::set<int> isolate_cpus)
    : isolate_cpus{isolate_cpus}
  {
  }
  int next_cpu_id()
  {
    int id = next_id.value_or(0);
    for (;;) {
      if (isolate_cpus.count(id) == 0) {
        break;
      }
      id++;
    }
    next_id = id + 1;
    return id;
  }
};

int
main(int argc, char* argv[])
{
  static_assert(sizeof(Command) <= sphinx::hardware::cache_line_size);
  try {
    program = ::basename(argv[0]);
    auto args = parse_cmd_line(argc, argv);
    CpuAffinity cpu_affinity{args.isolate_cpus};
    std::vector<std::thread> threads;
    for (int i = 0; i < args.nr_threads; i++) {
      auto thread = std::thread{server_thread, i, cpu_affinity.next_cpu_id(), args};
      threads.push_back(std::move(thread));
    }
    for (auto& t : threads) {
      t.join();
    }
  } catch (const std::exception& e) {
    std::cerr << "error: " << e.what() << std::endl;
    std::exit(EXIT_FAILURE);
  }
  std::exit(EXIT_SUCCESS);
}
