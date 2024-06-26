#include "reliable_bcast.hpp"

#include <arpa/inet.h>
#include <ifaddrs.h>
#include <netdb.h>
#include <unistd.h>

#include <chrono>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <sstream>

using namespace std;
constexpr int TTL = 3;
inline std::string curr_timestamp() {
  // Get current system offset from utc
  std::time_t now = std::time(nullptr);
  // Print current time
  std::stringstream curr_time;
  curr_time << "[" << std::put_time(std::localtime(&now), "%T") << "] ";
  return curr_time.str();
}
ReliableBroadcast::ReliableBroadcast(int process_id, int port)
    : process_id(process_id), port(port), seq_num(0), running(true) {
  sockfd = socket(AF_INET, SOCK_DGRAM, 0);
  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_port = htons(port);
  addr.sin_addr.s_addr = htonl(INADDR_ANY);
  bind(sockfd, (struct sockaddr*)&addr, sizeof(addr));
  if (process_id == 0)
    curr_view.push_back(std::make_pair(getLocalIP(), process_id));
  else
    sendJoinMessage();
}

void ReliableBroadcast::broadcast(const CommandType command,
                                  const std::string& message) {
  std::lock_guard<std::mutex> lock(send_mtx);
  Message msg(seq_num++, process_id, message);
  if (command == CommandType::CRASH_ON_RECEIVE) {
    crash_on_receive = true;
  }
  for (auto peer : curr_view) {
    sendMsgToPeer(msg, peer.first);
    if (command == CommandType::SEND_AND_CRASH) exit(1);
  }
}

void ReliableBroadcast::start() {
  std::thread(&ReliableBroadcast::receiverThread, this).detach();
  std::thread(&ReliableBroadcast::HeartbeaThread, this).detach();
}

void ReliableBroadcast::stop() {
  running = false;
  close(sockfd);
}

void ReliableBroadcast::deliver(const Message& message) {
  std::time_t now = std::time(nullptr);
  std::cout << curr_timestamp() << "Delivered message from "
            << message.sender_id << ": " << message.content << std::endl;
}

void ReliableBroadcast::receiverThread() {
  char buffer[1024];
  while (running) {
    struct sockaddr_in sender_addr;
    socklen_t addr_len = sizeof(sender_addr);
    int bytes_received = recvfrom(sockfd, buffer, sizeof(buffer), 0,
                                  (struct sockaddr*)&sender_addr, &addr_len);
    if (bytes_received > 0) {
      std::lock_guard<std::mutex> lock(send_mtx);
      std::string received_message(buffer, bytes_received);
      std::cerr << "[DEBUG] " << curr_timestamp()
                << "Received message : " << received_message << std::endl;
      std::istringstream iss(received_message);
      std::string type;
      iss >> type;
      if (type == "MSG") {
        int seq_num, sender_id;
        std::string content;
        iss >> seq_num >> sender_id;
        if (sender_id != process_id && crash_on_receive) {
          running = false;
          break;
        };
        std::getline(iss, content);
        handleMessage(Message(seq_num, sender_id, content));
      } else if (type == "VIEW_CHANGE" && process_id != 0) {
        int sender_id;
        iss >> sender_id;
        new_view.clear();
        view_change_in_progress = true;
        while (!iss.eof()) {
          std::string ip_address;
          int process_id;
          iss >> ip_address >> process_id;
          new_view.push_back(std::make_pair(ip_address, process_id));
        }
        if (curr_view.size() == 0) {
          curr_view = new_view;
          view_change_in_progress = false;
        } else {
          handleViewChange();
        }
      } else if (type == "JOIN" && process_id == 0) {
        int sender_id;
        std::string ip_address;
        iss >> sender_id >> ip_address;
        view_change_in_progress = true;
        handleJoin(ip_address, sender_id);
      } else if (type == "ACK") {
        int seq_num, sender_id;
        iss >> seq_num >> sender_id;
        handleAck(AckMessage(seq_num, sender_id));
      } else if (type == "FLUSH" && process_id == 0) {
        int sender_id;
        iss >> sender_id;
        flush_complete.insert(sender_id);
        if (flush_complete.size() == curr_view.size()) {
          flush_complete.clear();
          acked.clear();
          pending.clear();
          view_change_in_progress = false;
          curr_view = new_view;
          cerr << "[DEBUG] " << curr_timestamp() << "View change: ";
          for (auto peer : curr_view) {
            cerr << peer.first << " " << peer.second << " ";
          }
          cerr << endl;
          sendInstallView();
        }
      } else if (type == "INSTALL_VIEW" && process_id != 0) {
        int sender_id;
        iss >> sender_id;
        acked.clear();
        pending.clear();
        view_change_in_progress = false;
        curr_view = new_view;
        cerr << "[DEBUG] " << curr_timestamp() << "View change: ";
        for (auto peer : curr_view) {
          cerr << peer.first << " " << peer.second << " ";
        }
        cerr << endl;

      } else if (type == "HEART_BEAT" && process_id == 0) {
        int sender_id;
        std::string ip_address;
        iss >> sender_id >> ip_address;
        ttl[sender_id] = TTL;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
  }
}

void ReliableBroadcast::handleJoin(std::string ip_address, int id) {
  new_view = curr_view;
  new_view.push_back(std::make_pair(ip_address, id));
  ttl[id] = TTL;
  ViewChangeMessage view_change(process_id, new_view);
  for (auto peer : curr_view) {
    sendViewChangeToPeer(view_change, peer.first);
    for (auto msg : pending) {
      sendMsgToPeer(msg, peer.first);
    }
    sendFlushToPeer(peer.first);
  }
  // Send view change to the new peer
  sendViewChangeToPeer(view_change, ip_address);
}

void ReliableBroadcast::sendFlushToPeer(const std::string& peer) {
  std::string flush_message = "FLUSH " + std::to_string(process_id);
  struct sockaddr_in peer_addr;
  peer_addr.sin_family = AF_INET;
  peer_addr.sin_port = htons(port);
  inet_pton(AF_INET, peer.c_str(), &peer_addr.sin_addr);
  sendto(sockfd, flush_message.c_str(), flush_message.size(), 0,
         (struct sockaddr*)&peer_addr, sizeof(peer_addr));
}
void ReliableBroadcast::handleViewChange() {
  for (auto peer : curr_view) {
    for (auto msg : pending) {
      sendMsgToPeer(msg, peer.first);
    }
    sendFlushToPeer(peer.first);
  }
}

void ReliableBroadcast::handleMessage(const Message& message) {
  // if (message.sender_id == process_id) return;  // Ignore self
  acked[message.seq_num].insert(message.sender_id);
  Message forward_msg(message.seq_num, process_id, message.content);
  pending.push_back(message);
  sendAckToAll(AckMessage(message.seq_num, process_id));
}

void ReliableBroadcast::handleAck(const AckMessage& message) {
  // if (message.sender_id == process_id) return;  // Ignore self
  acked[message.seq_num].insert(message.sender_id);

  for (auto it = pending.begin(); it != pending.end();) {
    if (acked[it->seq_num].size() == curr_view.size()) {
      deliver(*it);
      acked.erase(it->seq_num);
      it = pending.erase(it);
    } else {
      ++it;
    }
  }
}

void ReliableBroadcast::sendAckToAll(const AckMessage& message) {
  std::string serialized_message = "ACK " + std::to_string(message.seq_num) +
                                   " " + std::to_string(message.sender_id);
  for (const auto& peer : curr_view) {
    struct sockaddr_in peer_addr;
    peer_addr.sin_family = AF_INET;
    peer_addr.sin_port = htons(port);
    inet_pton(AF_INET, peer.first.c_str(), &peer_addr.sin_addr);
    sendto(sockfd, serialized_message.c_str(), serialized_message.size(), 0,
           (struct sockaddr*)&peer_addr, sizeof(peer_addr));
  }
}
void ReliableBroadcast::sendToAll(const Message& message) {
  std::string serialized_message = "MSG " + std::to_string(message.seq_num) +
                                   " " + std::to_string(message.sender_id) +
                                   " " + message.content;
  for (const auto& peer : curr_view) {
    struct sockaddr_in peer_addr;
    peer_addr.sin_family = AF_INET;
    peer_addr.sin_port = htons(port);
    inet_pton(AF_INET, peer.first.c_str(), &peer_addr.sin_addr);
    sendto(sockfd, serialized_message.c_str(), serialized_message.size(), 0,
           (struct sockaddr*)&peer_addr, sizeof(peer_addr));
  }
}

void ReliableBroadcast::sendInstallView() {
  std::string serialized_message = "INSTALL_VIEW " + std::to_string(process_id);

  for (const auto& peer : curr_view) {
    struct sockaddr_in peer_addr;
    peer_addr.sin_family = AF_INET;
    peer_addr.sin_port = htons(port);
    inet_pton(AF_INET, peer.first.c_str(), &peer_addr.sin_addr);
    sendto(sockfd, serialized_message.c_str(), serialized_message.size(), 0,
           (struct sockaddr*)&peer_addr, sizeof(peer_addr));
  }
}

void ReliableBroadcast::sendMsgToPeer(const Message& message,
                                      const std::string& peer) {
  std::string serialized_message = "MSG " + std::to_string(message.seq_num) +
                                   " " + std::to_string(message.sender_id) +
                                   " " + message.content;
  struct sockaddr_in peer_addr;
  peer_addr.sin_family = AF_INET;
  peer_addr.sin_port = htons(port);
  inet_pton(AF_INET, peer.c_str(), &peer_addr.sin_addr);
  sendto(sockfd, serialized_message.c_str(), serialized_message.size(), 0,
         (struct sockaddr*)&peer_addr, sizeof(peer_addr));
}

void ReliableBroadcast::sendViewChangeToPeer(const ViewChangeMessage& message,
                                             const std::string& peer) {
  std::string serialized_message;
  for (auto member : message.members) {
    serialized_message +=
        " " + member.first + " " + std::to_string(member.second);
  }
  serialized_message =
      "VIEW_CHANGE " + std::to_string(message.process_id) + serialized_message;

  struct sockaddr_in peer_addr;
  peer_addr.sin_family = AF_INET;
  peer_addr.sin_port = htons(port);
  inet_pton(AF_INET, peer.c_str(), &peer_addr.sin_addr);
  sendto(sockfd, serialized_message.c_str(), serialized_message.size(), 0,
         (struct sockaddr*)&peer_addr, sizeof(peer_addr));
}

void ReliableBroadcast::sendJoinMessage() {
  std::string local_ip = getLocalIP();
  std::string discovery_message =
      "JOIN " + std::to_string(process_id) + " " + local_ip;
  struct sockaddr_in broadcast_addr;
  broadcast_addr.sin_family = AF_INET;
  broadcast_addr.sin_port = htons(port);
  broadcast_addr.sin_addr.s_addr = htonl(INADDR_BROADCAST);
  int broadcast_enable = 1;
  setsockopt(sockfd, SOL_SOCKET, SO_BROADCAST, &broadcast_enable,
             sizeof(broadcast_enable));
  sendto(sockfd, discovery_message.c_str(), discovery_message.size(), 0,
         (struct sockaddr*)&broadcast_addr, sizeof(broadcast_addr));
}

void ReliableBroadcast::HeartbeaThread() {
  while (running) {
    {
      std::lock_guard<std::mutex> lock(send_mtx);
      if (process_id == 0) {
        new_view = curr_view;
        bool node_left = false;
        for (auto peer : curr_view) {
          if (peer.second != 0 && --ttl[peer.second] == 0) {
            node_left = true;
            cerr << "[DEBUG] " << curr_timestamp() << peer.first << " "
                 << peer.second << " has left" << endl;
            new_view.erase(
                std::remove_if(new_view.begin(), new_view.end(),
                               [peer](const std::pair<std::string, int>& p) {
                                 return p.second == peer.second;
                               }),
                new_view.end());
          }
        }
        if (node_left) {
          curr_view = new_view;
          ViewChangeMessage view_change(process_id, new_view);
          for (auto peer : curr_view) {
            sendViewChangeToPeer(view_change, peer.first);
            for (auto msg : pending) {
              sendMsgToPeer(msg, peer.first);
            }
            sendFlushToPeer(peer.first);
          }
        }
      }

      else {
        std::string heart_beat_message =
            "HEART_BEAT " + std::to_string(process_id) + " " + getLocalIP();
        struct sockaddr_in broadcast_addr;
        broadcast_addr.sin_family = AF_INET;
        broadcast_addr.sin_port = htons(port);
        broadcast_addr.sin_addr.s_addr = htonl(INADDR_BROADCAST);
        int broadcast_enable = 1;
        setsockopt(sockfd, SOL_SOCKET, SO_BROADCAST, &broadcast_enable,
                   sizeof(broadcast_enable));
        sendto(sockfd, heart_beat_message.c_str(), heart_beat_message.size(), 0,
               (struct sockaddr*)&broadcast_addr, sizeof(broadcast_addr));
      }
    }
    std::this_thread::sleep_for(std::chrono::seconds(3));
  }
}

std::string ReliableBroadcast::getLocalIP() {
  struct ifaddrs *ifaddr, *ifa;
  char host[NI_MAXHOST];
  std::string ip;

  if (getifaddrs(&ifaddr) == -1) {
    perror("getifaddrs");
    exit(EXIT_FAILURE);
  }

  for (ifa = ifaddr; ifa != NULL; ifa = ifa->ifa_next) {
    if (ifa->ifa_addr == NULL) continue;

    int family = ifa->ifa_addr->sa_family;
    if (family == AF_INET) {
      if (getnameinfo(ifa->ifa_addr, sizeof(struct sockaddr_in), host,
                      NI_MAXHOST, NULL, 0, NI_NUMERICHOST) == 0) {
        if (strcmp(ifa->ifa_name, "lo") != 0) {  // Ignore localhost
          ip = host;
          break;
        }
      }
    }
  }

  freeifaddrs(ifaddr);
  return ip;
}
