#include "../src/log_tail.hpp"
#include <cassert>
#include <cstdio>
#include <fstream>
#include <string>
#include <unistd.h>

using namespace ark_log_tail;

namespace {
std::string line(const std::string& message) {
  return "[2026.09.24-03.00.00:074][  7]" + message + "\r\n";
}
void append(const std::string& path, const std::string& text) {
  std::ofstream file(path, std::ios::app | std::ios::binary);
  file << text;
  assert(file.good());
}
}

int main() {
  assert(safe_message(line("Saving world...").substr(0, line("Saving world...").size() - 2)) == "Saving world...");
  assert(safe_message("[2026.09.24-03.00.00:074][  7]World Save Complete. Took 0.569239") ==
         "World Save Complete. Took 0.569239");
  assert(safe_message("[2026.09.24-03.00.00:074][  7]ARK Version: 361.7") == "ARK Version: 361.7");
  assert(safe_message("[2026.09.24-03.00.00:074][  7]Primal Game Data Took 34.89 seconds") ==
         "Primal Game Data Took 34.89 seconds");
  assert(safe_message("[2026.09.24-03.00.00:074][  7]Commandline: password=secret").empty());
  assert(safe_message("[2026.09.24-03.00.00:074][  7]Saved from 10.0.0.1").empty());

  char folder[] = "/tmp/ark-log-tail-XXXXXX";
  assert(mkdtemp(folder));
  const std::string path = std::string(folder) + "/ShooterGame.log";
  append(path, line("Saving world..."));
  Tail tail(path);
  assert(tail.prime());
  assert(tail.poll().messages.empty()); // no historical replay
  append(path, line("Commandline: RCONPassword=secret"));
  append(path, line("Saving world..."));
  append(path, "[2026.09.24-03.00.00:074][  7]World Save Complete. Took ");
  auto result = tail.poll();
  assert(result.messages.size() == 1 && result.messages[0] == "Saving world...");
  append(path, "0.7\n");
  result = tail.poll();
  assert(result.messages.size() == 1 && result.messages[0] == "World Save Complete. Took 0.7");

  append(path, line(std::string(600, 'X')));
  append(path, line("Saving world..."));
  result = tail.poll();
  assert(result.dropped && result.messages.size() == 1 && result.messages[0] == "Saving world...");

  const std::string rotated = path + ".1";
  assert(rename(path.c_str(), rotated.c_str()) == 0);
  append(path, line("New Save Format enabled"));
  result = tail.poll();
  assert(result.messages.size() == 1 && result.messages[0] == "New Save Format enabled");
  {
    std::ofstream trunc(path, std::ios::trunc | std::ios::binary);
    trunc << line("ARK Version: 361.7");
  }
  result = tail.poll();
  assert(result.messages.size() == 1 && result.messages[0] == "ARK Version: 361.7");
  for (size_t i = 0; i < max_events + 1; ++i) append(path, line("Saving world..."));
  result = tail.poll();
  assert(result.messages.size() == max_events && result.dropped);

  const std::string link = std::string(folder) + "/link.log";
  assert(symlink(path.c_str(), link.c_str()) == 0);
  Tail symlink_tail(link);
  assert(!symlink_tail.prime());
  assert(symlink_tail.poll().messages.empty());
  unlink(link.c_str());
  unlink(path.c_str());
  unlink(rotated.c_str());
  rmdir(folder);
}
