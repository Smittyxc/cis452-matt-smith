#include <iostream>
#include <unistd.h>
#include <sys/ptrace.h>
#include <sys/wait.h>
#include <sys/errno.h>
#include <linux/ptrace.h>
#include <sys/user.h>
#include <vector>
#include <charconv>
#include <algorithm>
#include <string>

int main(int argc, char *argv[]) {
  std::string policies[4] = {"lru", "fifo", "clock", "lfu"};
  bool quiet = false;
  bool show_all = false;
  int max_pages = 10;
  std::string policy = "";
  // std::string executable;
  std::vector<char *> exec_args;
  int mref = 0;
  int pgfault = 0;

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
      show_all = true;
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

  // std::cout << quiet << "\n";
  // std::cout << show_all << "\n";
  // std::cout << policy << "\n";
  // std::cout << max_pages << "\n";

  // for (const auto &num : exec_args) {
  //   std::cout << num << " ";
  // }

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

    execvp(exec_args[0], exec_args.data());
  } else {
    // ptrace_syscall_info sc_info;
    user_regs_struct user_regs;

    // Wait for the child to stop before starting to trace its system calls
    wait(nullptr);
    perror("After first wait in parent");

    ptrace((__ptrace_request)PTRACE_SETOPTIONS, ch_pid, nullptr, PTRACE_O_TRACESYSGOOD);
    perror("After PTRACE_SETOPTIONS");

    // Start tracing at the (first) system call of the child process
    ptrace((__ptrace_request)PTRACE_SINGLESTEP, ch_pid, nullptr, nullptr);
    perror("After PTRACE_SYSCALL");

    unsigned long long prev_page = 0;
    while (true) {
      pid_t who = wait(nullptr);
      if (errno)
        perror("After parent wait in loop");

      if (who == -1 || errno)
        break;

      // Get information about the current system call
      ptrace((__ptrace_request)PTRACE_GETREGS, ch_pid, nullptr, &user_regs);

      if (errno)
        perror("After GET_SYSCALL_INFO");

      unsigned long long page_num = user_regs.rip >> PAGE_SHIFT;
      if (!quiet) {
        if (prev_page != page_num) {
          pgfault++;
        }

        if (show_all || (prev_page != page_num)) {
          std::cout << "[" << std::dec << ++mref << "/" << pgfault << "] "
                    << "At: " << std::hex << user_regs.rip
                    << " Page: " << page_num << std::dec << "\n";
        } else {
          std::cout << "[" << std::dec << ++mref << "/" << pgfault << "] "
                    << "At: " << std::hex << user_regs.rip << std::dec << "\n";
        }
      }

      prev_page = page_num;

      ptrace((__ptrace_request)PTRACE_SYSCALL, ch_pid, nullptr, nullptr);
    }
    std::cout << "From Parent" << std::endl;
  }
  return 0;
}
