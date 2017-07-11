//  Copyright (c) 2017-present The pika_hub Authors.  All rights reserved.
//  This source code is licensed under the BSD-style license found in the
//  LICENSE file in the root directory of this source tree. An additional grant
//  of patent rights can be found in the PATENTS file in the same directory.

#include <string>
#include <thread>

#include "src/pika_hub_trysync.h"
#include "pink/include/redis_cli.h"
#include "slash/include/slash_string.h"
#include "slash/include/slash_status.h"

bool PikaHubTrysync::Send(pink::PinkCli* cli,
    const std::map<int32_t, PikaStatus>::iterator& iter) {
  pink::RedisCmdArgsType argv;
  std::string wbuf_str;

  argv.clear();
  std::string tbuf_str;
  argv.push_back("internaltrysync");
  argv.push_back(local_ip_);
  argv.push_back(std::to_string(local_port_));
  argv.push_back(std::to_string(iter->second.rcv_number));
  argv.push_back(std::to_string(iter->second.rcv_offset));

  pink::SerializeRedisCommand(argv, &tbuf_str);

  wbuf_str.append(tbuf_str);

  slash::Status s = cli->Send(&wbuf_str);
  if (!s.ok()) {
    Error(info_log_, "Connect master %d,%s:%d, Send, error: %s",
      iter->first, iter->second.ip.c_str(), iter->second.port,
      s.ToString().c_str());
    return false;
  }
  return true;
}

bool PikaHubTrysync::Recv(pink::PinkCli* cli,
    const std::map<int32_t, PikaStatus>::iterator& iter) {
  slash::Status s;
  std::string reply;

  pink::RedisCmdArgsType argv;
  s = cli->Recv(&argv);
  if (!s.ok()) {
    Error(info_log_, "Connect master %d,%s:%d, Recv, error: %s",
      iter->first, iter->second.ip.c_str(), iter->second.port,
      strerror(errno));
    return false;
  }

  reply = slash::StringToLower(argv[0]);

  if (reply != "ok") {
    Error(info_log_, "Connect master %d,%s:%d, Recv, logic error: %s",
      iter->first, iter->second.ip.c_str(), iter->second.port,
      reply.c_str());
    return false;
  }
  iter->second.should_trysync = false;
  return true;
}

void PikaHubTrysync::Trysync(const std::map<int32_t, PikaStatus>::
    iterator& iter) {
  pink::PinkCli* cli = pink::NewRedisCli();
  cli->set_connect_timeout(1500);
  std::string master_ip;
  if ((cli->Connect(iter->second.ip, iter->second.port)).ok()) {
    cli->set_send_timeout(3000);
    cli->set_recv_timeout(3000);
    if (Send(cli, iter) && Recv(cli, iter)) {
      Info(info_log_, "Trysync %d,%s:%d success", iter->first,
          iter->second.ip.c_str(), iter->second.port);
    }
    cli->Close();
    delete cli;
  } else {
    Error(info_log_, "Trysync %d,%s:%d failed", iter->first,
          iter->second.ip.c_str(), iter->second.port);
  }
}

void* PikaHubTrysync::ThreadMain() {
  while (!should_stop()) {
    {
    rocksutil::MutexLock l(pika_mutex_);
    for (auto it = pika_servers_->begin(); it != pika_servers_->end(); it++) {
      if (it->second.should_delete) {
        delete static_cast<BinlogSender*>(it->second.sender);
        it = pika_servers_->erase(it);
      }
      if (it->second.should_trysync && it->second.sender == nullptr) {
        Trysync(it);
      }
    }
    }
    std::this_thread::sleep_for(std::chrono::seconds(2));
  }
  return nullptr;
}
