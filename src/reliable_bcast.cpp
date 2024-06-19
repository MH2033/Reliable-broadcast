#include "reliable_bcast.hpp"

#include <arpa/inet.h>
#include <ifaddrs.h>
#include <netdb.h>
#include <unistd.h>

#include <chrono>
#include <cstring>
#include <iostream>
#include <sstream>

ReliableBroadcast::ReliableBroadcast(int process_id, int port)
    : process_id(process_id), port(port), seq_num(0), running(true) {
  sockfd = socket(AF_INET, SOCK_DGRAM, 0);
  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_port = htons(port);
  addr.sin_addr.s_addr = htonl(INADDR_ANY);
  bind(sockfd, (struct sockaddr*)&addr, sizeof(addr));
}

void ReliableBroadcast::broadcast(const std::string& message) {
  std::lock_guard<std::mutex> lock(mtx);
  Message msg(seq_num++, process_id, message);
  sendToAll(msg);
}

void ReliableBroadcast::start() {
  std::thread(&ReliableBroadcast::receiverThread, this).detach();
  std::thread(&ReliableBroadcast::discoveryThread, this).detach();
}

void ReliableBroadcast::stop() {
  running = false;
  close(sockfd);
}

void ReliableBroadcast::deliver(const Message& message) {
  std::cout << "Delivered message from " << message.sender_id << ": "
            << message.content << std::endl;
}

void ReliableBroadcast::receiverThread() {
  char buffer[1024];
  while (running) {
    struct sockaddr_in sender_addr;
    socklen_t addr_len = sizeof(sender_addr);
    int bytes_received = recvfrom(sockfd, buffer, sizeof(buffer), 0,
                                  (struct sockaddr*)&sender_addr, &addr_len);
    if (bytes_received > 0) {
      std::lock_guard<std::mutex> lock(mtx);
      std::string received_message(buffer, bytes_received);
      std::istringstream iss(received_message);
      std::string type;
      iss >> type;
      if (type == "MSG") {
        int seq_num, sender_id;
        std::string content;
        iss >> seq_num >> sender_id;
        std::getline(iss, content);
        handleMessage(Message(seq_num, sender_id, content));
      } else if (type == "DISCOVERY") {
        int sender_id;
        std::string ip_address;
        iss >> sender_id >> ip_address;
        handleDiscoveryMessage(DiscoveryMessage(sender_id, ip_address));
      }
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
}

void ReliableBroadcast::handleDiscoveryMessage(
    const DiscoveryMessage& discovery_msg) {
  if (discovery_msg.process_id == process_id) return;  // Ignore self
  bool already_discovered = false;
  for (const auto& peer : peers) {
    if (peer == discovery_msg.ip_address) {
      already_discovered = true;
      break;
    }
  }
  if (!already_discovered) {
    peers.push_back(discovery_msg.ip_address);
    std::cout << "Discovered peer " << discovery_msg.process_id << " at "
              << discovery_msg.ip_address << std::endl;
  }
}

void ReliableBroadcast::handleMessage(const Message& message) {
  acked[message.seq_num].insert(message.sender_id);
  sendToAll(message);
  if (acked[message.seq_num].size() == peers.size()) {
    deliver(message);
  } else {
    pending.push_back(message);
  }
  for (auto it = pending.begin(); it != pending.end();) {
    if (acked[it->seq_num].size() == peers.size()) {
      deliver(*it);
      it = pending.erase(it);
    } else {
      ++it;
    }
  }
}

void ReliableBroadcast::sendToAll(const Message& message) {
  std::string serialized_message = "MSG " + std::to_string(message.seq_num) +
                                   " " + std::to_string(message.sender_id) +
                                   " " + message.content;
  for (const auto& peer : peers) {
    struct sockaddr_in peer_addr;
    peer_addr.sin_family = AF_INET;
    peer_addr.sin_port = htons(port);
    inet_pton(AF_INET, peer.c_str(), &peer_addr.sin_addr);
    sendto(sockfd, serialized_message.c_str(), serialized_message.size(), 0,
           (struct sockaddr*)&peer_addr, sizeof(peer_addr));
  }
}

void ReliableBroadcast::sendDiscoveryMessage() {
  std::string local_ip = getLocalIP();
  std::string discovery_message =
      "DISCOVERY " + std::to_string(process_id) + " " + local_ip;
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

void ReliableBroadcast::discoveryThread() {
  while (running) {
    sendDiscoveryMessage();
    std::this_thread::sleep_for(
        std::chrono::seconds(2));  // Periodically send discovery messages
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
