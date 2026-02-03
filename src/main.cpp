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
#include "./../include/parsed_tokens.h"

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

static vector<string> get_input();
static Parsed_Tokens parse_input(string& input);
static string handle_type(string command_string);
static string find_in_path(const string& command);
static bool is_executable(const fs::path& p);

enum Keywords {
  // Start with one because when we use .contains unordered_map member function any
  // key that isn't in the map will return 0, which will throw off the logic if we
  // start the enum with 0.
  ECHO = 1,
  TYPE,
  EXIT,
  PWD,
  CD,
  HISTORY
  
};

unordered_map<string, Keywords> builtin_map;

int main() {
  // Flush after every std::cout / std:cerr
  cout << std::unitbuf;
  cerr << std::unitbuf;

  builtin_map["echo"] = ECHO;
  builtin_map["type"] = TYPE;
  builtin_map["exit"] = EXIT;
  builtin_map["pwd"] = PWD;
  builtin_map["cd"] = CD;
  builtin_map["history"] = HISTORY;
  bool is_done = false;
  vector<string> command_history;
  // REPL Loop
  while (!is_done) {
    
    cout << "$ ";
    string input_string;
    std::getline(cin, input_string);
    Parsed_Tokens parsed_input = parse_input(input_string);
    string command = parsed_input.command;
    vector<string> args = parsed_input.args;
    string redirect_path = parsed_input.redirect;
    string redirect_symbol = parsed_input.redirect_symbol;
    int stdval;
    int original_buffer;

    
    if (!redirect_path.empty()) {
      if (redirect_symbol == "2>") {
        original_buffer = dup(STDERR_FILENO);
        stdval = STDERR_FILENO;
      }else {
        original_buffer = dup(STDOUT_FILENO);
        stdval = STDOUT_FILENO;
      }
      // Set up redirect for non-default commands (echo, cd, pwd, etc.)
      if (builtin_map[command]) {
        int fd = open(redirect_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
        dup2(fd, stdval);
        close(fd);
      }
    }
    string output = "";
    fs::path file_path;
    command_history.push_back(input_string);
    switch (builtin_map[command]) {
      case ECHO:
        for (auto& arg : args) {
          // If its not that last arg add a space between args, otherwise dont concat a space
          string string_to_be_added = (arg != args[args.size() - 1]) ? arg + " " : arg;
          output += string_to_be_added;
        }
        cout << output << endl;
        break;
      case TYPE:
        output = handle_type(args[0]);
        cout << output << endl;
        break;
      case CD:
        if (args[0] == "~") {
          // Get HOME env, convert to fs::path object, and set to current_path
          string home_path = getenv("HOME");
          file_path = fs::path(home_path);
          fs::current_path(file_path);
          break;
        }
        file_path = fs::path(args[0]);
        if (!fs::exists(file_path)) {
          cerr << "cd: " << file_path.c_str() << ": No such file or directory" << endl;
          break;
        }
        if (file_path.is_absolute()) {
          fs::current_path(file_path);
        } else { // else if (file_path.is_relative()){
          // Get the absolute path from the relative and set it to current_path
          file_path = fs::absolute(file_path);
          fs::current_path(file_path);
        }
        break;
      case PWD:
        cout << fs::current_path().string() << endl;
        break;
      case HISTORY:
        {
          int start = 0;
          if (args.size() > 0) {
            // ***try - except might be a good idea here
            int amount_to_view = stoi(args[0]);
            if (amount_to_view >= command_history.size()) {
              start = command_history.size();
            }
            else {
              start = command_history.size() - amount_to_view;
            }
          }
          for (int i = start; i < command_history.size(); i++) {
            cout << "\t" << i << "  " << command_history[i] << endl;
          }
          break;
        }
      case EXIT:
        is_done = true;
        break;
      default: // File path to a program (like cat or ls) or not a command
        string exe_path = find_in_path(command);
        if (exe_path != "") {
          vector<char*> argv;
          // Reserve space in the vector
          // The + 1 (plus one) is for the null pointer
          // the execv function requires a char* array that is null terminated
          // meaning the last entry has to be a nullptr
          argv.reserve(args.size() + 1);
          argv.push_back(command.data());
          for (auto& s : args) {
            argv.push_back(s.data()); // data() returns char* of string
          }
          argv.push_back(nullptr);
          char* const* pargs = argv.data();

          int status = 0;
          pid_t child_pid;
          cout.flush();
          child_pid = fork();

          if (child_pid == 0) {
            if (!redirect_path.empty()) {
              int fd = open(redirect_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
              if (dup2(fd, stdval) == -1) {cout << "fd: " << fd << " Path: " << redirect_path << " Redirect FAILED!" << endl;}
              close(fd);
              // cout << " Path: " << redirect_path << endl;
            }
            // The following tests wheather redirect works or not
            //write(stdval, "REDIRECT NOT OK\n", 16);
            execv(exe_path.c_str(), pargs);
            perror("execv failed");
            _exit(127);
          }
          else {
            waitpid(child_pid, &status, 0);
          }
        }
        else {
          cerr << command << ": command not found" << endl;
        }
        
        break;
    }if (!redirect_path.empty()) {
      dup2(original_buffer, stdval);
      close(original_buffer);
    }
    output = "";
    
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
    else if (input.substr(left+1, 3) == "1>>" || input.substr(left + 1, 3) == "2>>" || input[right] == '>')
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
 
static string handle_type(string command_string) {
  string output = "";
  if (builtin_map.contains(command_string)) {
      output = command_string + " is a shell builtin";
      return output;
  }

  output = find_in_path(command_string);
  if (output != "") {
      return (command_string + " is ") += output;
      // output is a file systems object
      // using += because file system paths override operator+= NOT operator+
  }

  output = command_string + ": not found";
  return output;
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

/*
git add .
git commit --allow-empty -m "Stage 22: Submission 1. "
git push origin master
 */
