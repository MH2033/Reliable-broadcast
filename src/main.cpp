#include <iostream>
#include <thread>

#include "reliable_bcast.hpp"

int main(int argc, char* argv[]) {
  if (argc != 2) {
    std::cerr << "Usage: " << argv[0] << " <process_id>" << std::endl;
    return 1;
  }

  int process_id = std::stoi(argv[1]);
  ReliableBroadcast rb(process_id, 49588);
  rb.start();

  std::cout << "Enter command followed by message (Commands: 0 normal send, 1 "
               "Send to one peer and crash, 2 Crash after the first receive)"
            << std::endl;
  // Bulletin board simulation
  std::thread broadcaster([&rb]() {
    CommandType command;
    std::string message;
    while (true) {
      int mode;
      std::cin >> mode;
      std::getline(std::cin, message);
      command = static_cast<CommandType>(mode);
      rb.broadcast(command, message);
    }
  });

  broadcaster.join();
  rb.stop();

  return 0;
}
