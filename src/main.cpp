// C, POSIX, GNU Headers/Libraries
#include <stdlib.h> // chdir(), 
#include <errno.h>
#include <sys/wait.h> // waitpid(),
#include <fcntl.h>
#include <stdio.h>
#include <unistd.h> // STDERR, STDOUT
// Readline automates a lot of the work it has:
// builtin button support
// automatically tracks history
// auto complete for files (need to register a new function or commands)
#include <readline/readline.h>
#include <readline/history.h>

// C++ Headers/Libraries
#include <iostream>
#include <string>
#include <sstream>
#include <fstream>
#include <vector>
#include <unordered_map>
#include <filesystem>

// Type aliasing
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

// Namespacing
namespace fs = std::filesystem;

// Class/struct prototypes
class RedirectManager;
struct ShellContext;
struct Command;
struct TokenizerState;
struct Token;

// FUNCTION PROTOTYPES

// Handler for function mapping
using HandlerFunc = void(*)(const vector<string>&, ShellContext&);

// CLASS/STRUCT OBJECTS
class RedirectManager {
private:
  string path;
  int original_fd = -1;
  int target_fd = -1;
  bool active = false;
  bool is_appending = false;
  int open_type = O_TRUNC; // Defult to truncation when opening file

public:
  RedirectManager(const string& r_path, const string& symbol) : path(r_path) {
    if (path.empty()) return;

    if (symbol.rfind("2") != string::npos) {
      target_fd = STDERR_FILENO;
    }
    else {
      target_fd = STDOUT_FILENO;
    }

    if (symbol.rfind(">>") != string::npos) {
      open_type = O_APPEND;
    }
    
    original_fd = dup(target_fd);
    int fd = open(path.c_str(), O_WRONLY | O_CREAT | open_type, 0644);
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
  int get_open_type() const {return open_type;}
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

const vector<string>& commands = {
  "echo",
  "type",
  "pwd",
  "cd",
  "hist",
  "exit"
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

// helpers
static void register_builtins(ShellContext& shell_ctx);
static string find_in_path(const string& command);
static bool is_executable(const fs::path& p);

char* command_generator(const char* text, int state) {
  static size_t index;
  static size_t len;

  if (state == 0) {
    index = 0;
    len = strlen(text);
  }

  while (index < commands.size()) {
    const std::string& cmd = commands[index++];
    if (cmd.compare(0, len, text) == 0) {
      return strdup(cmd.c_str());
    }
  }
  return nullptr;
}

char** command_completion(const char* text, int start, int end) {
  if (start == 0) {
    
    return rl_completion_matches(text, command_generator);
  }

  return nullptr;
}

// ENTRY POINT
int main() {
  // Flush after every std::cout / std:cerr
  cout << std::unitbuf;
  cerr << std::unitbuf;

  rl_attempted_completion_function = command_completion;

  // Create a single shell context instance for the entire run of the program
  // Create the built-in commands list
  ShellContext shell_ctx;
  register_builtins(shell_ctx);
  using_history();
  // REPL Loop
  while (!shell_ctx.is_done) {
    string input_string;
    char* line;
    line = readline("$ ");
    if (!line) {
      continue;
    }
    add_history(line);
    input_string = string(line); // Convert the const char * to string to be compatible with existing code
    Command cmd = tokenize_and_parse(input_string);
    
    shell_ctx.command_history.push_back(input_string); // Obsolete; readline automatically saves history
    RedirectManager redirect(cmd.redirect_path, cmd.redirect_symbol);

    auto it = shell_ctx.handlers.find(cmd.name);
    if (it != shell_ctx.handlers.end()) {
      it->second(cmd.args, shell_ctx);
    } else {
      handle_external_command(cmd.name, cmd.args, redirect);
    }
    free(line); // Readline uses malloc so manual mempry freeing is required
  }
}


// COMMAND HANDLERS
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
  // TODO: Change to use readline history functionality instead of manually
  // saving to a vector
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
      int fd = open(redirect.get_path().c_str(), O_WRONLY | O_CREAT | redirect.get_open_type(), 0644);
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

// PARSERS AND TOKENIZERS
static Command tokenize_and_parse(const string& input) {
  vector<Token> tokens = tokenize(input);
  return parse_tokens(tokens);
};

// Separates out the user string input into a list of words, or tokens by
// finding delimiters; quotes, spaces, etc
// it calls process_char for the handling of individual characters
static vector<Token> tokenize(const string& input) {
  vector<Token> tokens;
  string current;
  TokenizerState state;
  for (int i = 0; i < input.size(); i++) {
    char c = input[i];
    char next = (i + 1 < input.size()) ? input[i + 1] : '\0';
    char next_next = (i + 2 < input.size()) ? input[i + 2] : '\0';

    if (!state.in_single_quote && !state.in_double_quote && !state.escape_next) {
      
      if ((c == '1' || c == '2') && next == '>') {
        if (!current.empty()) {
          tokens.push_back({current, false});
          current.clear();
        }
        string op;
        op += c;
        op += '>';
        if (next_next == '>') { // Overwrite Symbol
          op += '>'; // Append Symbol
          i++;
        }
        tokens.push_back({op, true});
        i++;
        continue;
      }

      if (c == '>') {
        if (!current.empty()) {
          tokens.push_back({current, false});
          current.clear();
        }
        if (next == '>') {
          tokens.push_back({">>", true}); // Append Symbol
          i++;
        } else {
          tokens.push_back({">", true}); // Overwrite Symbol
        }
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

// This function is called by tokenize and processes every individual char
// Returns true or false depending if it was added to the current token
// True: added to current token
// False: NOT added to current token (delimiter)
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
    // exit function as false
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

// After receiving user input in the form of a list (tokens), this function parses that list
// into; command, arguments, redirect symbol, and redirect path
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

// HELPER FUNCTIONS
static void register_builtins(ShellContext& shell_ctx) {
  shell_ctx.handlers["echo"] = handle_echo;
  shell_ctx.handlers["type"] = handle_type;
  shell_ctx.handlers["exit"] = handle_exit;
  shell_ctx.handlers["pwd"] = handle_pwd;
  shell_ctx.handlers["cd"] = handle_cd;
  shell_ctx.handlers["history"] = handle_history;
}

// Primarily is designed to locate executables
// Gets our PATH variable and seraches the list of directories in PATH
// for an executable matching the string command
// commands like ls and cat utilize this function in order to be found
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
