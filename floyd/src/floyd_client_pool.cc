// Copyright (c) 2015-present, Qihoo, Inc.  All rights reserved.
// This source code is licensed under the BSD-style license found in the
// LICENSE file in the root directory of this source tree. An additional grant
// of patent rights can be found in the PATENTS file in the same directory.

#include "floyd/src/floyd_client_pool.h"

#include <unistd.h>
#include "floyd/src/logger.h"
#include "floyd/include/floyd_options.h"

#include "slash/include/slash_string.h"

namespace floyd {

static std::string CmdType(const CmdRequest& req);

ClientPool::ClientPool(Logger* info_log, int timeout_ms, int retry)
  : info_log_(info_log),
    timeout_ms_(timeout_ms),
    retry_(retry) {
}

Status ClientPool::SendAndRecv(const std::string& server, const CmdRequest& req, CmdResponse* res) {
  LOGV(DEBUG_LEVEL, info_log_, "ClientPool::SendAndRecv Send %s command to server %s", CmdType(req).c_str(), server.c_str());
  Client *client = GetClient(server);
  pink::PinkCli* cli = client->cli;

  Status ret = Status::Incomplete("Not send");
  slash::MutexLock l(&client->mu);
  // TODO(anan) concurrent by epoll
  for (int i = 0; !ret.ok() && i < retry_; i++) {
    ret = UpHoldCli(client);
    if (!ret.ok()) {
      LOGV(WARN_LEVEL, info_log_, "Client::SendAndRecv %s cmd to %s, Connect Failed %s",
          CmdType(req).c_str(), server.c_str(), ret.ToString().c_str());
      continue;
    }

    ret = cli->Send((void*)&req);
    if (!ret.ok()) {
      LOGV(WARN_LEVEL, info_log_, "Client::SendAndRecv %s cmd to %s, Send return %s",
          CmdType(req).c_str(), server.c_str(), ret.ToString().c_str());
      cli->Close();
      continue;
    }

    ret = cli->Recv(res);
    if (!ret.ok()) {
      LOGV(WARN_LEVEL, info_log_, "Client::SendAndRecv %s cmd to %s, Recv return %s",
          CmdType(req).c_str(), server.c_str(), ret.ToString().c_str());
      cli->Close();
      continue;
    }
  }
  return ret;
}

ClientPool::~ClientPool() {
  slash::MutexLock l(&mu_);
  for (auto& iter : client_map_) {
    delete iter.second;
  }
  LOGV(DEBUG_LEVEL, info_log_, "ClientPool dtor");
}

Client* ClientPool::GetClient(const std::string& server) {
  slash::MutexLock l(&mu_);
  auto iter = client_map_.find(server);
  if (iter == client_map_.end()) {
    std::string ip;
    int port;
    slash::ParseIpPortString(server, ip, port);
    Client* client = new Client(ip, port);
    client_map_[server] = client;
    return client;
  } else {
    return iter->second;
  }
}

Status ClientPool::UpHoldCli(Client *client) {
  if (client == NULL || client->cli == NULL) {
    return Status::Corruption("null PinkCli");
  }

  Status ret;
  pink::PinkCli* cli = client->cli;
  if (!cli->Available()) {
    ret = cli->Connect();
    if (ret.ok()) {
      cli->set_send_timeout(timeout_ms_);
      cli->set_recv_timeout(timeout_ms_);
    }
  }
  return ret;
}

static std::string CmdType(const CmdRequest& cmd) {
  std::string ret;
  switch (cmd.type()) {
    case Type::kRead: {
      ret = "Read";
      break;
    }
    case Type::kWrite: {
      ret = "Write";
      break;
    }
    case Type::kDirtyWrite: {
      ret = "DirtyWrite";
      break;
    }
    case Type::kDelete: {
      ret = "Delete";
      break;
    }
    case Type::kRequestVote: {
      ret = "RequestVote";
      break;
    }
    case Type::kAppendEntries: {
      ret = "AppendEntries";
      break;
    }
    case Type::kServerStatus: {
      ret = "ServerStatus";
      break;
    }
    default:
      ret = "UnknownCmd";
  }
  return ret;
}

} // namespace floyd
