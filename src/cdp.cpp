#include "solari/browser/cdp.hpp"
#ifdef SOLARI_WITH_WS

#include <chrono>
#include <utility>

#include "solari/browser/errors.hpp"

namespace solari::browser {

namespace {
constexpr int kConnectTimeoutMs = 15000;
}  // namespace

CdpConnection::CdpConnection(std::string cdpEndpoint, long callTimeoutMs)
    : endpoint_(std::move(cdpEndpoint)), callTimeoutMs_(callTimeoutMs) {
  if (endpoint_.empty()) throw SolariError("Solari: cdpEndpoint is required");
}

CdpConnection::~CdpConnection() { close(); }

void CdpConnection::onEvent(std::function<void(const nlohmann::json&)> handler) {
  onEvent_ = std::move(handler);
}

bool CdpConnection::connected() const {
  std::lock_guard<std::mutex> lk(m_);
  return opened_;
}

void CdpConnection::connect() {
  {
    std::lock_guard<std::mutex> lk(m_);
    opened_ = false;
    failed_ = false;
    errMsg_.clear();
  }
  auto ws = std::make_shared<ix::WebSocket>();
  {
    std::lock_guard<std::mutex> lk(m_);
    ws_ = ws;
  }
  ws->setUrl(endpoint_);
  // The signed session id in the path is the credential; no auth header needed.
  ws->disableAutomaticReconnection();

  ws->setOnMessageCallback([this](const ix::WebSocketMessagePtr& msg) {
    switch (msg->type) {
      case ix::WebSocketMessageType::Open: {
        std::lock_guard<std::mutex> lk(m_);
        opened_ = true;
        cv_.notify_all();
        break;
      }
      case ix::WebSocketMessageType::Message:
        handleMessage(msg->str);
        break;
      case ix::WebSocketMessageType::Error: {
        std::lock_guard<std::mutex> lk(m_);
        if (!opened_) {
          failed_ = true;
          errMsg_ = msg->errorInfo.reason;
          cv_.notify_all();
        }
        break;
      }
      case ix::WebSocketMessageType::Close: {
        bool wasOpen;
        {
          std::lock_guard<std::mutex> lk(m_);
          wasOpen = opened_;
          opened_ = false;
          if (!wasOpen) {
            failed_ = true;
            errMsg_ = msg->closeInfo.reason;
            cv_.notify_all();
          }
        }
        // A drop mid-flight must wake every blocked send() rather than let it
        // sit until its call timeout.
        if (wasOpen) failAll("CDP connection closed: " + msg->closeInfo.reason);
        break;
      }
      default:
        break;
    }
  });

  ws->start();

  std::unique_lock<std::mutex> lk(m_);
  const bool signalled =
      cv_.wait_for(lk, std::chrono::milliseconds(kConnectTimeoutMs),
                   [this] { return opened_ || failed_; });
  if (opened_) return;
  const std::string err = signalled ? errMsg_ : "connect timed out";
  lk.unlock();
  ws->stop();
  throw SolariError("Solari: CDP connect to " + endpoint_ + " failed: " +
                    (err.empty() ? "connect failed" : err));
}

void CdpConnection::close() {
  std::shared_ptr<ix::WebSocket> ws;
  {
    std::lock_guard<std::mutex> lk(m_);
    ws = std::move(ws_);
    ws_.reset();
    opened_ = false;
  }
  if (ws) ws->stop();
  failAll("CDP connection closed");
}

void CdpConnection::failAll(const std::string& reason) {
  {
    std::lock_guard<std::mutex> lk(m_);
    for (auto& [id, p] : pending_) {
      if (p->done) continue;
      p->done = true;
      p->reply = nlohmann::json{{"__transportError", reason}};
    }
  }
  cv_.notify_all();
}

void CdpConnection::handleMessage(const std::string& text) {
  nlohmann::json j;
  try {
    j = nlohmann::json::parse(text);
  } catch (...) {
    return;  // not JSON — nothing sane to do with it
  }
  if (!j.is_object()) return;

  auto idIt = j.find("id");
  if (idIt != j.end() && idIt->is_number_integer()) {
    const int id = idIt->get<int>();
    {
      std::lock_guard<std::mutex> lk(m_);
      auto it = pending_.find(id);
      if (it == pending_.end()) return;  // late reply to an abandoned call
      it->second->reply = std::move(j);
      it->second->done = true;
    }
    cv_.notify_all();
    return;
  }
  // No `id` -> a CDP event.
  if (onEvent_) onEvent_(j);
}

nlohmann::json CdpConnection::send(const std::string& method,
                                   const nlohmann::json& params,
                                   const std::optional<std::string>& sessionId) {
  auto p = std::make_shared<Pending>();
  int id;
  std::shared_ptr<ix::WebSocket> ws;
  {
    std::lock_guard<std::mutex> lk(m_);
    if (!opened_ || !ws_) {
      throw SolariError("Solari: CDP connection is not open (call connect() first)");
    }
    ws = ws_;  // keep the socket alive for the duration of this send
    id = nextId_++;
    pending_[id] = p;
  }

  nlohmann::json msg;
  msg["id"] = id;
  msg["method"] = method;
  msg["params"] = params.is_null() ? nlohmann::json::object() : params;
  if (sessionId) msg["sessionId"] = *sessionId;
  ws->sendText(msg.dump());

  nlohmann::json reply;
  {
    std::unique_lock<std::mutex> lk(m_);
    const bool done = cv_.wait_for(lk, std::chrono::milliseconds(callTimeoutMs_),
                                   [&p] { return p->done; });
    pending_.erase(id);
    if (!done) {
      throw SolariError("Solari: CDP \"" + method + "\" timed out after " +
                        std::to_string(callTimeoutMs_) + "ms");
    }
    reply = std::move(p->reply);
  }

  if (reply.contains("__transportError")) {
    throw SolariError("Solari: CDP \"" + method + "\" failed: " +
                      reply["__transportError"].get<std::string>());
  }
  auto err = reply.find("error");
  if (err != reply.end()) {
    // CDP's `{code,message,data}` is a protocol error, NOT an HTTP status, so
    // it is folded into the message rather than SolariError::status.
    std::string detail;
    if (err->is_object()) {
      detail = err->value("message", std::string("CDP error"));
      if (err->contains("code") && (*err)["code"].is_number_integer()) {
        detail += " (code " + std::to_string((*err)["code"].get<int>()) + ")";
      }
      if (err->contains("data") && (*err)["data"].is_string()) {
        detail += ": " + (*err)["data"].get<std::string>();
      }
    } else {
      detail = err->dump();
    }
    throw SolariError("Solari: CDP \"" + method + "\" failed: " + detail);
  }
  auto result = reply.find("result");
  if (result != reply.end()) return *result;
  return nlohmann::json::object();
}

}  // namespace solari::browser
#endif  // SOLARI_WITH_WS
