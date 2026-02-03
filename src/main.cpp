// C and POSIX Headers/Libraries
#include <unistd.h> // getcwd(),
#include <stdlib.h> // chdir(), 
#include <errno.h>
#include <sys/wait.h> // waitpid(),
#include <fcntl.h>
#include <stdio.h>

// C++ Headers/Libraries
#include <iostream>
#include <string>
#include <sstream>
#include <fstream>
#include <vector>
#include <unordered_map>
#include <filesystem>
#include <span>
#include "./../include/Parsed_Tokens.h"

using std::cout,
std::cerr,
std::cin,
std::string,
std::endl,
std::istringstream,
std::vector,
std::unordered_map,
std::ofstream,
std::ifstream,
std::streambuf;

namespace fs = std::filesystem;

class RedirectManager {
private:
  string path;
  int original_fd = -1;
  int target_fd = -1;
  bool active = false;

public:
  RedirectManager(const string& r_path, const string& symbol) : path(r_path) {
    if (path.empty()) return;

    if (symbol == "2>") {
      target_fd = STDERR_FILENO;
    }
    else {
      target_fd = STDOUT_FILENO;
    }

    original_fd = dup(target_fd);
    int fd = open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd != -1) {
      dup2(fd, target_fd);
      close(fd);
      active = true;
    }
  }

  ~RedirectManager() {
    if (active && original_fd != -1) {
      dup2(original_fd, target_fd);
      close(original_fd);
    }
  }

  RedirectManager(const RedirectManager&) = delete;
  RedirectManager& operator=(const RedirectManager&) = delete;

  int get_target_fd() const {return target_fd;}
  bool is_active() const {return active;}
  const string& get_path() const {return path;}
};

enum Commands {
  // Start with one because when we use .contains unordered_map member function any
  // key that isn't in the map will return 0, which will throw off the logic if we
  // start the enum with 0.
  ECHO = 1,
  TYPE,
  EXIT,
  PWD,
  CD,
  HIST
  
};

struct ShellContext {
  unordered_map<string, Commands> builtin_map;
  vector<string> command_history;
  bool is_done = false;
};

static vector<string> get_input();
static Parsed_Tokens parse_input(string& input);
static string find_in_path(const string& command);
static bool is_executable(const fs::path& p);

// handlers
static void handle_echo(const vector<string>& args, ShellContext& shell_ctx); // wont use context
static void handle_type(const vector<string>& args, ShellContext& shell_ctx); // from string to vector<string> and return void
static void handle_cd(const vector<string>& args, ShellContext& shell_ctx); // wont use context
static void handle_pwd(const vector<string>& args, ShellContext& shell_ctx); // wont use either
static void handle_history(const vector<string>& args, ShellContext& shell_ctx);
static void handle_exit(const vector<string>& args, ShellContext& shell_ctx); // wont use args
static void handle_external_command(const string& command, const vector<string>& args, const RedirectManager& redirect);

int main() {
  // Flush after every std::cout / std:cerr
  cout << std::unitbuf;
  cerr << std::unitbuf;

  ShellContext shell_ctx;
  
  shell_ctx.builtin_map["echo"] = ECHO;
  shell_ctx.builtin_map["type"] = TYPE;
  shell_ctx.builtin_map["exit"] = EXIT;
  shell_ctx.builtin_map["pwd"] = PWD;
  shell_ctx.builtin_map["cd"] = CD;
  shell_ctx.builtin_map["history"] = HIST;
  
  // REPL Loop
  while (!shell_ctx.is_done) {
    
    cout << "$ ";
    string input_string;
    std::getline(cin, input_string);
    Parsed_Tokens parsed_input = parse_input(input_string);
    string command = parsed_input.command;
    vector<string> args = parsed_input.args;
    string redirect_path = parsed_input.redirect;
    string redirect_symbol = parsed_input.redirect_symbol;
    
    shell_ctx.command_history.push_back(input_string);
    RedirectManager redirect(redirect_path, redirect_symbol);
    switch (shell_ctx.builtin_map[command]) {
      case ECHO: handle_echo(args, shell_ctx); break;
      case TYPE: handle_type(args, shell_ctx); break;
      case CD: handle_cd(args, shell_ctx); break;
      case PWD: handle_pwd(args, shell_ctx); break;
      case HIST: handle_history(args, shell_ctx); break;
      case EXIT: handle_exit(args, shell_ctx); break;
      // File path to a program (like cat or ls) or not a command
      default: handle_external_command(command, args, redirect); break;
    }
  }
}

static Parsed_Tokens parse_input(string& input) {
  vector<string> word_list; // Holds a list of all arguments passed into the program
  string word_in_window;
  bool is_inside_single_quote = false;
  bool is_inside_double_quote = false;
  bool escape_next_char = false;
  bool is_redirecting = false;
  bool is_escaping_next_char = false;
  string redirect_symbol;
  int char_index = 0;
  int left = 0;
  int right = 0;
  while (left <= right && right < input.size()) {
    // Rethink this, potentially use flags for the quotes
    if (input[right] == ' ') {
      if (!is_inside_single_quote && !is_inside_double_quote && !is_escaping_next_char) {
        if (!word_in_window.empty()) {
          word_list.push_back(word_in_window);
          word_in_window = "";
          left = right;
        }
      }// hello 'world' word = hello; word_list = {hello} word = ""; input[left] = ' ' input[right] = ' (after incrementing at the end)
      else {
        word_in_window += input[right];
        is_escaping_next_char = false;
      }
    } // hello 'world'
    else if (input[right] == '\'') { /* ' */
      if (is_inside_double_quote || is_escaping_next_char) {
        word_in_window += input[right];
        is_escaping_next_char = false;
      }
      else if(!is_inside_single_quote && !is_inside_double_quote) {
        is_inside_single_quote = !is_inside_single_quote;
        left = right;
      }
      else if (is_inside_single_quote) {
        is_inside_single_quote = !is_inside_single_quote;
        left = right;
      }
    }
    else if (input[right] == '"') { /* " */
      if (is_inside_single_quote || is_escaping_next_char) {
        word_in_window += input[right];
        is_escaping_next_char = false;
      }
      else if (!is_inside_double_quote && !is_inside_single_quote) {
        is_inside_double_quote = !is_inside_double_quote;
        left = right;
      }
      else if (is_inside_double_quote) {
        is_inside_double_quote = !is_inside_double_quote;
        left = right;
      }
    }
    else if (input[right] == '\\') { /* \ */
      if (!is_inside_single_quote && !is_inside_double_quote && !is_escaping_next_char) {
        is_escaping_next_char = true;
      }
      else if (is_escaping_next_char) {
        word_in_window += input[right];
        is_escaping_next_char = false;
      }
      else if (is_inside_double_quote) {
          // Escape only certain chars
          if (right < input.size() - 1) { // size = 3 [0],[1],[2]; right = 1; right < 3 - 1; right < 2
            if (input[right+1] == '"' || input[right+1] == '\\') {
              is_escaping_next_char = true;
            }
            else {
              word_in_window += input[right];
            }
          }
      }
      else if (is_inside_single_quote) { /*else*/
        word_in_window += input[right];
      }
    }
    else if (input.substr(left+1, 2) == "1>" || input.substr(left + 1, 2) == "2>" || input[right] == '>') { // 1> or >
      if (is_redirecting)
      {
        left = right;
      }
      else if (input.substr(left + 1, 2) == "2>") {
        is_redirecting = true;
        redirect_symbol = "2>";
      }
      else { // "1>" or ">"
        is_redirecting = true;
        redirect_symbol = ">";
      }
    }
    else {
      word_in_window += input[right];
      is_escaping_next_char = false;
    }
    right++;
  }

  string redirect_path = "";
  if (word_in_window.size() > 0) {
    if (is_redirecting) {
      redirect_path = word_in_window;
    }
    else {
      word_list.push_back(word_in_window);
    }
  }

  string command = word_list.front();
  word_list.erase(word_list.begin());
  Parsed_Tokens parsed{command, word_list, redirect_path, redirect_symbol};
  return parsed;
};

static void handle_echo(const vector<string>& args, ShellContext& shell_ctx) {
  string output = "";
  for (size_t i = 0; i < args.size(); i++) {
    // If its not that last arg add a space between args, otherwise dont concat a space
    output += args[i];
    if (i != args.size() - 1) {
      output += " ";
    }
  }
  cout << output << endl;
}
 
static void handle_type(const vector<string>& args, ShellContext& shell_ctx) {
  string output = "";
  if (shell_ctx.builtin_map.contains(args[0])) {
      cout << args[0] << " is a shell builtin" << endl;
      return;
  }

  output = find_in_path(args[0]);
  if (output != "") {
      cout << args[0] << " is " << output << endl;
      return;
  }

  cout << args[0] << ": not found" << endl;
}

static void handle_cd(const vector<string>& args, ShellContext& shell_ctx) {
  fs::path file_path;
  // Get HOME env, convert to fs::path object, and set to current_path
  if (args[0] == "~") {
    string home_path = getenv("HOME");
    file_path = fs::path(home_path);
    fs::current_path(file_path);
    return;
  }
  
  file_path = fs::path(args[0]);
  if (!fs::exists(file_path)) {
    cerr << "cd: " << file_path.c_str() << ": No such file or directory" << endl;
    return;
  }
  
  if (file_path.is_absolute()) {
    fs::current_path(file_path);
  } else {// else if (file_path.is_relative()){
          // Get the absolute path from the relative and set it to current_path
    file_path = fs::absolute(file_path);
    fs::current_path(file_path);
  }
}

static void handle_pwd(const vector<string>& args, ShellContext& shell_ctx) {
  cout << fs::current_path().string() << endl;
}

static void handle_history(const vector<string>& args, ShellContext& shell_ctx) {
  // ***try - except might be a good idea here
  int start = 0;
  if (args.size() > 0) {
    int amount_to_view = stoi(args[0]);
    if (amount_to_view >= static_cast<int>(shell_ctx.command_history.size())) {
      start = 0;
    } else {
      start = shell_ctx.command_history.size() - amount_to_view;
    }
  }
  for (size_t i = start; i < shell_ctx.command_history.size(); i++) {
    cout << "\t" << i << "  " << shell_ctx.command_history[i] << endl;
  }
}

static void handle_exit(const vector<string>& args, ShellContext& shell_ctx) {
  shell_ctx.is_done = true;
}

static void handle_external_command(const string& command, const vector<string>& args, const RedirectManager& redirect) {
  string exe_path = find_in_path(command);
  if (exe_path.empty()) {
    cerr << command << ": command not found" << endl;
    return;
  }

  vector<char*> argv;
  // Reserve space in the vector
  // The + 1 (plus one) is for the null pointer
  // the execv function requires a char* array that is null terminated
  // meaning the last entry has to be a nullptr
  argv.reserve(args.size() + 2);
  argv.push_back(const_cast<char*>(command.data())); // data() returns char* of string
  for (const auto& s : args) {
    argv.push_back(const_cast<char*>(s.data()));
  }
  argv.push_back(nullptr);
  char* const* pargs = argv.data();

  int status = 0;
  pid_t child_pid;
  cout.flush();
  child_pid = fork();

  if (child_pid == 0) {
    if (redirect.is_active()) {
      int fd = open(redirect.get_path().c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
      dup2(fd, redirect.get_target_fd());
      close(fd);
    }
    // The following tests wheather redirect works or not
    //write(stdval, "REDIRECT NOT OK\n", 16);
    execv(exe_path.c_str(), pargs);
    perror("execv failed");
    _exit(127);
  } else {
    waitpid(child_pid, &status, 0);
  }
}

static string find_in_path(const string& command) {
  const string path = getenv("PATH");
  istringstream path_stream(path);
  string directory;
  fs::path full_path;

  while (getline(path_stream, directory, ':')) {
      full_path = fs::path(directory) / command;
      if (!fs::exists(full_path)) {
          continue;
      }
      if (is_executable(full_path)) {
        return full_path;
      }
  }
  return "";
}

static bool is_executable(const fs::path& full_path) {
  auto perms = fs::status(full_path).permissions();
  // Checks for permissions for executable manually with each exec bit
  return (perms & fs::perms::owner_exec) != fs::perms::none ||
          (perms & fs::perms::group_exec) != fs::perms::none ||
          (perms & fs::perms::others_exec) != fs::perms::none;
}
