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

struct ShellContext;

using HandlerFunc = void(*)(const vector<string>&, ShellContext&);

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

struct ShellContext {
  unordered_map<string, HandlerFunc> handlers;
  vector<string> command_history;
  bool is_done = false;
};

struct Command {
  string name;
  vector<string> args;
  string redirect_path;
  string redirect_symbol;

  bool has_redirect() const {return !redirect_path.empty();}
};

struct TokenizerState {
  bool in_single_quote = false;
  bool in_double_quote = false;
  bool escape_next = false;
};

struct Token {
  string value;
  bool is_redirect_op = false;
};

// Tokenizers and parsers
static Command tokenize_and_parse(const string& input);
static bool process_char(char c, char next_char, string& current_token, TokenizerState& state);
static vector<Token> tokenize(const string& input);
static Command parse_tokens(const vector<Token>& tokens);

// handlers
static void handle_echo(const vector<string>& args, ShellContext& shell_ctx); // wont use context
static void handle_type(const vector<string>& args, ShellContext& shell_ctx); // from string to vector<string> and return void
static void handle_cd(const vector<string>& args, ShellContext& shell_ctx); // wont use context
static void handle_pwd(const vector<string>& args, ShellContext& shell_ctx); // wont use either
static void handle_history(const vector<string>& args, ShellContext& shell_ctx);
static void handle_exit(const vector<string>& args, ShellContext& shell_ctx); // wont use args
static void handle_external_command(const string& command, const vector<string>& args, const RedirectManager& redirect);

static void register_builtins(ShellContext& shell_ctx);
static string find_in_path(const string& command);
static bool is_executable(const fs::path& p);

int main() {
  // Flush after every std::cout / std:cerr
  cout << std::unitbuf;
  cerr << std::unitbuf;

  ShellContext shell_ctx;
  register_builtins(shell_ctx);

  // REPL Loop
  while (!shell_ctx.is_done) {
    
    cout << "$ ";
    string input_string;
    std::getline(cin, input_string);
    Command cmd = tokenize_and_parse(input_string);
    
    shell_ctx.command_history.push_back(input_string);
    RedirectManager redirect(cmd.redirect_path, cmd.redirect_symbol);

    auto it = shell_ctx.handlers.find(cmd.name);
    if (it != shell_ctx.handlers.end()) {
      it->second(cmd.args, shell_ctx);
    } else {
      handle_external_command(cmd.name, cmd.args, redirect);
    }
  }
}

static void register_builtins(ShellContext& shell_ctx) {
  shell_ctx.handlers["echo"] = handle_echo;
  shell_ctx.handlers["type"] = handle_type;
  shell_ctx.handlers["exit"] = handle_exit;
  shell_ctx.handlers["pwd"] = handle_pwd;
  shell_ctx.handlers["cd"] = handle_cd;
  shell_ctx.handlers["history"] = handle_history;
}

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
  if (shell_ctx.handlers.contains(args[0])) {
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

  //cout << "Command: " << command << " Exe path: " << exe_path << endl;
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
    // write(redirect.get_target_fd(), "REDIRECT NOT OK\n", 16);
    execv(exe_path.c_str(), pargs);
    perror("execv failed");
    _exit(127);
  } else {
    waitpid(child_pid, &status, 0);
  }
}

static Command tokenize_and_parse(const string& input) {
  vector<Token> tokens = tokenize(input);
  return parse_tokens(tokens);
};

static bool process_char(char c, char next_char, string& current_token, TokenizerState& state) {
  // Here we are escaping the current char
  if (state.escape_next) {
    current_token += c;
    state.escape_next = false;
    return true;
  }

  if (c == ' ') {
    if (state.in_single_quote || state.in_double_quote) {
      current_token += c;
      return true;
    }
    // If not in any quotes or being escaped then it acts as a delimiter
    return false;
  }

  // Handle single quotes
  if (c == '\'') {
    if (state.in_double_quote) {
      current_token += c;
    } else {
      state.in_single_quote = !state.in_single_quote;
    }
    return true;
  }

  // Handle double quotes
  if (c == '"') {
    if (state.in_single_quote) {
      current_token += c;
    } else {
      state.in_double_quote = !state.in_double_quote;
    }

    return true;
  }

  // Blacklash
  if (c == '\\') {
    if (state.in_single_quote) {
      current_token += c;
    } else if (state.in_double_quote) {
      if (next_char == '"' || next_char == '\\') {
        state.escape_next = true;
      } else {
        current_token += c;
      }
    } else {
      state.escape_next = true;
    }
    return true;
  }

  // Non-special char
  current_token += c;
  return true;
}

static vector<Token> tokenize(const string& input) {
  vector<Token> tokens;
  string current;
  TokenizerState state;
  for (int i = 0; i < input.size(); i++) {
    char c = input[i];
    char next = (i + 1 < input.size()) ? input[i + 1] : '\0';

    if (!state.in_single_quote && !state.in_double_quote && !state.escape_next) {

      if ((c == '1' || c == '2') && next == '>') {
        if (!current.empty()) {
          tokens.push_back({current, false});
          current.clear();
        }
        string op;
        op += c;
        op += '>';
        tokens.push_back({op, true});
        i++;
        continue;
      }

      if (c == '>') {
        if (!current.empty()) {
          tokens.push_back({current, false});
          current.clear();
        }
        tokens.push_back({">", true});
        continue;
      }
    }

    bool was_appened = process_char(c, next, current, state);

    if (!was_appened && !current.empty()) {
      tokens.push_back({current, false});
      current.clear();
    }
  }

  if (!current.empty()) {
    tokens.push_back({current, false});
  }

  return tokens;
}

static Command parse_tokens(const vector<Token>& tokens) {
  Command cmd;
  bool expect_redirect_path = false;
  string pending_redirect_symbol;

  for (const auto& token : tokens) {
    if (token.is_redirect_op) {
      expect_redirect_path = true;
      pending_redirect_symbol = token.value;
      continue;
    }
    if (expect_redirect_path) {
      cmd.redirect_path = token.value;
      cmd.redirect_symbol = pending_redirect_symbol;
      expect_redirect_path = false;
      continue;
    }

    if (cmd.name.empty()) {
      cmd.name = token.value;
    } else {
      cmd.args.push_back(token.value);
    }
  }

  return cmd;
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
