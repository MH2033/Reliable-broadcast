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

struct AckMessage {
  int seq_num;
  int sender_id;

  AckMessage(int seq, int sender) : seq_num(seq), sender_id(sender) {}
};

struct ViewChangeMessage {
  int process_id;
  std::vector<std::pair<std::string, int>> members;

  ViewChangeMessage(int id, const std::vector<std::pair<std::string, int>> mems)
      : process_id(id), members(mems) {}
};

struct JoinMessage {
  int process_id;
  std::string ip_address;
  JoinMessage(int id, std::string ip) : process_id(id), ip_address(ip) {}
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
  void HeartbeaThread();
  void handleMessage(const Message& message);
  void handleAck(const AckMessage& message);
  void handleViewChange();
  void handleJoin(std::string ip_address, int process_id);
  void sendToAll(const Message& message);
  void sendAckToAll(const AckMessage& message);
  void sendMsgToPeer(const Message& message, const std::string& peer);
  void sendViewChangeToPeer(const ViewChangeMessage& message,
                            const std::string& peer);
  void sendFlushToPeer(const std::string& peer);

  void sendJoinMessage();
  std::string getLocalIP();

  int process_id;
  std::vector<std::pair<std::string, int>> curr_view;
  std::vector<std::pair<std::string, int>> new_view;
  std::set<int> flush_complete;

  int port;
  int seq_num;
  std::mutex send_mtx;
  std::atomic<bool> view_change_in_progress{false};
  std::map<int, std::set<int>> acked;
  std::vector<Message> pending;
  bool running;
  int sockfd;
  struct sockaddr_in addr;

  std::condition_variable cv;
};

#endif  // RELIABLE_BROADCAST_H
