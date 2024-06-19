#include <iostream>
#include <thread>

#include "reliable_bcast.hpp"

int main(int argc, char* argv[]) {
  if (argc != 2) {
    std::cerr << "Usage: " << argv[0] << " <process_id>" << std::endl;
    return 1;
  }

  int process_id = std::stoi(argv[1]);
  ReliableBroadcast rb(process_id, 12345);
  rb.start();

  // Bulletin board simulation
  std::thread broadcaster([&rb]() {
    std::string message;
    while (true) {
      std::getline(std::cin, message);
      rb.broadcast(message);
    }
  });

  broadcaster.join();
  rb.stop();

  return 0;
}
