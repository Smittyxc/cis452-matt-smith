#include <iostream>
#include <unistd.h>
#include <sys/ptrace.h>
#include <sys/wait.h>
#include <sys/errno.h>
#include <linux/ptrace.h>
#include <sys/user.h>
#include <vector>
#include <algorithm>
#include <string>
#include <list>
#include <unordered_map>
#include <climits>

class Fifo {
private:
  int _capacity;
  int count = 0;
  int head = 0;
  std::vector<unsigned long long> buffer;

public:
  Fifo(int capacity) {
    _capacity = capacity;
    buffer.resize(capacity);
  }
  // Circular buffer management
  void push(unsigned long long page) {
    buffer[head] = page;
    if (count < _capacity)
      count++;
    head = (head + 1) % _capacity;
  }

  bool handle_check(unsigned long long curr_page) {
    bool hit = false;
    for (int i = 0; i < count; i++) {
      if (buffer[i] == curr_page) {
        hit = true;
        break;
      }
    }
    if (!hit) {
      push(curr_page);
      return true;
    }
    return false;
  }
};

class Lru {
private:
  int _capacity;
  std::list<unsigned long long> buffer;

public:
  Lru(int capacity) {
    _capacity = capacity;
  }

  bool handle_check(unsigned long long curr_page) {
    // Check for hit
    auto it = std::find(buffer.begin(), buffer.end(), curr_page);

    if (it != buffer.end()) {
      // Bring existing page to front (most recently accessed)
      buffer.splice(buffer.begin(), buffer, it);
      return false;
    }

    // Page fault
    if (buffer.size() >= _capacity) {
      buffer.pop_back();
    }
    buffer.push_front(curr_page);
    return true;
  }
};

struct Lfu_Node {
  int frequency;
  int insert_time;
};

class Lfu {
private:
  int _capacity;
  int iteration = 0;
  std::unordered_map<unsigned long long, Lfu_Node> cache;

public:
  Lfu(int capacity) {
    _capacity = capacity;
  }

  bool handle_check(unsigned long long page) {
    auto it = cache.find(page);
    // Hit -> increase freq.
    if (it != cache.end()) {
      it->second.frequency++;
      return false;
    }
    Lfu_Node node = {1, ++iteration};

    // Miss -> eject by lfu, ties handled by FIFO
    if (cache.size() >= _capacity) {
      unsigned long long page_to_kick = 0;
      int oldest_time = INT_MAX;
      int least_frequency = INT_MAX;

      for (const auto &entry : cache) {
        if (entry.second.frequency < least_frequency) {
          least_frequency = entry.second.frequency;
          oldest_time = INT_MAX;
        }
        if (entry.second.frequency == least_frequency && entry.second.insert_time < oldest_time) {
          oldest_time = entry.second.insert_time;
          page_to_kick = entry.first;
        }
      }
      cache.erase(page_to_kick);
    }
    cache.insert({page, node});

    return true;
  }
};

struct Clock_Node {
  unsigned long long page;
  bool referenced;
};
class Clock {
private:
  int _capacity;
  int count = 0;
  int index = 0;
  std::vector<Clock_Node> buffer;

public:
  Clock(int capacity) {
    _capacity = capacity;
    buffer.resize(capacity);
  }

  bool handle_check(unsigned long long page) {
    for (int i = 0; i < count; i++) {
      if (buffer[i].page == page) {
        buffer[i].referenced = true;
        return false;
      }
    }

    if (count >= _capacity) {
      while (true) {
        if (buffer[index].referenced == false) {
          buffer[index] = {page, true};
          index = (index + 1) % _capacity;
          return true;
        }
        buffer[index].referenced = false;
        index = (index + 1) % _capacity;
      }
    }

    buffer[count++] = {page, true};
    return true;
  }
};

int main(int argc, char *argv[]) {
  std::string policies[4] = {"lru", "fifo", "clock", "lfu"};
  bool quiet = false;
  bool verbose = false;
  int max_pages = 10;
  std::string policy = "";
  std::vector<char *> exec_args;
  int mref = 0;
  int pgfault = 0;
  unsigned long long prev_page = 0;

  if (argc < 2) {
    std::cerr << "Insufficient arguments provided" << "\n";
    return 1;
  };

  for (int i = 1; i < argc; i++) {
    if (std::string(argv[i]) == "-q") {
      quiet = true;
    } else if (std::string(argv[i]) == "-n") {
      if (i + 1 < argc) {
        max_pages = std::stoi(argv[++i]);
      } else {
        std::cerr << "Insufficient arguments provided" << "\n";
        return 1;
      };
    } else if (std::string(argv[i]) == "-v") {
      verbose = true;
    } else if (std::string(argv[i]) == "-p") {
      if (i + 1 < argc) {
        policy = argv[++i];
        // Verify user provided policy
        bool found = std::find(std::begin(policies), std::end(policies), policy) != std::end(policies);
        if (!found) {
          std::cerr << "Invalid policy provided" << "\n";
          return 1;
        }
      } else {
        std::cerr << "Insufficient args" << "\n";
        return 1;
      };
    } else {
      exec_args.push_back(argv[i]);
    }
  }

  if (policy == "") {
    std::cerr << "No policy provided";
    return 1;
  }

  pid_t ch_pid = fork();

  if (ch_pid == 0) {
    // Tell parent to trace this process
    ptrace(static_cast<__ptrace_request>(PTRACE_TRACEME), getpid(), nullptr, nullptr);
    raise(SIGSTOP); // stop myself so the parent can start tracing

    if (exec_args.empty()) {
      std::cerr << "No executable provided" << "\n";
      return 1;
    }
    exec_args.push_back(nullptr);

    execvp(exec_args[0], exec_args.data());
  } else {
    user_regs_struct user_regs;
    Fifo fifo = Fifo(max_pages);
    Lru lru = Lru(max_pages);
    Lfu lfu = Lfu(max_pages);
    Clock clock = Clock(max_pages);

    // Wait for the child to stop before starting to trace its system calls
    wait(nullptr);

    // Start tracing at every memory access
    ptrace((__ptrace_request)PTRACE_SINGLESTEP, ch_pid, nullptr, nullptr);

    while (true) {
      pid_t who = wait(nullptr);
      if (who == -1 || errno)
        break;

      // Get information about the current system call
      ptrace((__ptrace_request)PTRACE_GETREGS, ch_pid, nullptr, &user_regs);

      unsigned long long page_num = user_regs.rip >> PAGE_SHIFT;
      bool is_page_fault = false;

      if (policy == "lru") {
        is_page_fault = lru.handle_check(page_num);
      } else if (policy == "fifo") {
        is_page_fault = fifo.handle_check(page_num);
      } else if (policy == "lfu") {
        is_page_fault = lfu.handle_check(page_num);
      } else if (policy == "clock") {
        is_page_fault = clock.handle_check(page_num);
      } else {
        std::cerr << "No valid policy" << "\n";
        return 1;
      }

      mref++;
      if (is_page_fault) {
        pgfault++;
      }

      // Print formatting
      bool page_shifted = (page_num != prev_page);
      if (quiet) {
        if (is_page_fault) {
          std::cout << "Page Fault #" << pgfault << " at "
                    << std::hex << user_regs.rip << std::dec << "\n";
        }
      } else {
        if (page_shifted) {
          std::cout << "[" << std::dec << mref << "/" << pgfault << "] "
                    << "At: " << std::hex << user_regs.rip
                    << " Page " << page_num << std::dec << "\n";
        } else if (verbose) {
          std::cout << "[" << std::dec << mref << "/" << pgfault << "] "
                    << "At: " << std::hex << user_regs.rip << std::dec << "\n";
        }
      }
      prev_page = page_num;

      ptrace((__ptrace_request)PTRACE_SINGLESTEP, ch_pid, nullptr, nullptr);
    }

    // Sumary formatting
    double fault_rate = (mref > 0) ? (static_cast<double>(pgfault) / mref) * 100.0 : 0.0;
    std::cout << "Page Allocation: " << max_pages << ", RefCount: " << mref
              << " Page Faults: " << pgfault << " (" << fault_rate << "%)\n";
  }
  return 0;
}
