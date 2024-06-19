#ifndef RELIABLE_BROADCAST_H
#define RELIABLE_BROADCAST_H

#include <netinet/in.h>

#include <condition_variable>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

struct Message {
  int seq_num;
  int sender_id;
  std::string content;

  Message(int seq, int sender, const std::string& msg)
      : seq_num(seq), sender_id(sender), content(msg) {}
};

struct DiscoveryMessage {
  int process_id;
  std::string ip_address;

  DiscoveryMessage(int id, const std::string& ip)
      : process_id(id), ip_address(ip) {}
};

class ReliableBroadcast {
 public:
  ReliableBroadcast(int process_id, int port);
  void broadcast(const std::string& message);
  void start();
  void stop();
  void deliver(const Message& message);

 private:
  void receiverThread();
  void discoveryThread();
  void handleMessage(const Message& message);
  void handleDiscoveryMessage(const DiscoveryMessage& discovery_msg);
  void sendToAll(const Message& message);
  void sendToPeer(const Message& message, const std::string& peer);
  void sendDiscoveryMessage();
  std::string getLocalIP();

  int process_id;
  std::vector<std::pair<std::string, int>> peers;
  int port;
  int seq_num;
  std::mutex mtx;
  std::map<int, std::set<int>> acked;
  std::vector<Message> pending;
  bool running;
  int sockfd;
  struct sockaddr_in addr;

  std::condition_variable cv;
};

#endif  // RELIABLE_BROADCAST_H
