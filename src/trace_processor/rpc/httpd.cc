/*
 * Copyright (C) 2019 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "perfetto/base/build_config.h"

#if PERFETTO_BUILDFLAG(PERFETTO_TP_HTTPD)

#include "src/trace_processor/rpc/httpd.h"

#include "perfetto/ext/base/http/http_server.h"
#include "perfetto/ext/base/string_utils.h"
#include "perfetto/ext/base/string_view.h"
#include "perfetto/ext/base/unix_task_runner.h"
#include "perfetto/ext/base/utils.h"
#include "perfetto/protozero/scattered_heap_buffer.h"
#include "perfetto/trace_processor/trace_processor.h"
#include "src/trace_processor/rpc/rpc.h"

#include "protos/perfetto/trace_processor/trace_processor.pbzero.h"

namespace perfetto {
namespace trace_processor {

namespace {

constexpr int kBindPort = 9001;

// Sets the Access-Control-Allow-Origin: $origin on the following origins.
// This affects only browser clients that use CORS. Other HTTP clients (e.g. the
// python API) don't look at CORS headers.
const char* kAllowedCORSOrigins[] = {
    "https://ui.perfetto.dev",
    "http://localhost:10000",
    "http://localhost:8080/perfetto",
    "http://localhost:8080/brperfanalyzer",
    "http://127.0.0.1:10000",
    "http://127.0.0.1:8080/perfetto",
    "http://127.0.0.1:8080/brperfanalyzer",
    "http://171.28.150.7:10000",
    "http://172.28.150.7:8080/perfetto",
    "http://172.28.150.7:8080/brperfanalyzer",    
    "http://192.168.2.124:10000",
    "http://192.168.2.124:8080/perfetto",
    "http://192.168.2.124:8080/brperfanalyzer",
    "http://10.10.254.101:10000",
    "http://10.10.254.101:8080/perfetto",
    "http://10.10.254.101:8080/brperfanalyzer",
};

class Httpd : public base::HttpRequestHandler {
 public:
  explicit Httpd(std::unique_ptr<TraceProcessor>);
  ~Httpd() override;
  void Run(int port);

 private:
  // HttpRequestHandler implementation.
  void OnHttpRequest(const base::HttpRequest&) override;
  void OnWebsocketMessage(const base::WebsocketMessage&) override;

  void ServeHelpPage(const base::HttpRequest&);

  Rpc trace_processor_rpc_;
  base::UnixTaskRunner task_runner_;
  base::HttpServer http_srv_;
};

base::HttpServerConnection* g_cur_conn;

base::StringView Vec2Sv(const std::vector<uint8_t>& v) {
  return base::StringView(reinterpret_cast<const char*>(v.data()), v.size());
}

// Used both by websockets and /rpc chunked HTTP endpoints.
void SendRpcChunk(const void* data, uint32_t len) {
  if (data == nullptr) {
    // Unrecoverable RPC error case.
    if (!g_cur_conn->is_websocket())
      g_cur_conn->SendResponseBody("0\r\n\r\n", 5);
    g_cur_conn->Close();
    return;
  }
  if (g_cur_conn->is_websocket()) {
    g_cur_conn->SendWebsocketMessage(data, len);
  } else {
    base::StackString<32> chunk_hdr("%x\r\n", len);
    g_cur_conn->SendResponseBody(chunk_hdr.c_str(), chunk_hdr.len());
    g_cur_conn->SendResponseBody(data, len);
    g_cur_conn->SendResponseBody("\r\n", 2);
  }
}

Httpd::Httpd(std::unique_ptr<TraceProcessor> preloaded_instance)
    : trace_processor_rpc_(std::move(preloaded_instance)),
      http_srv_(&task_runner_, this) {}
Httpd::~Httpd() = default;

void Httpd::Run(int port) {
  PERFETTO_ILOG("[HTTP] Starting RPC server on localhost:%d", port);
  PERFETTO_LOG(
      "[HTTP] This server can be used by reloading https://ui.perfetto.dev and "
      "clicking on YES on the \"Trace Processor native acceleration\" dialog "
      "or through the Python API (see "
      "https://perfetto.dev/docs/analysis/trace-processor#python-api).");

  for (size_t i = 0; i < base::ArraySize(kAllowedCORSOrigins); ++i)
    http_srv_.AddAllowedOrigin(kAllowedCORSOrigins[i]);
  http_srv_.Start(port);
  task_runner_.Run();
}

void Httpd::OnHttpRequest(const base::HttpRequest& req) {
  base::HttpServerConnection& conn = *req.conn;
  if (req.uri == "/") {
    // If a user tries to open http://127.0.0.1:9001/ show a minimal help page.
    return ServeHelpPage(req);
  }

  static int last_req_id = 0;
  auto seq_hdr = req.GetHeader("x-seq-id").value_or(base::StringView());
  int seq_id = base::StringToInt32(seq_hdr.ToStdString()).value_or(0);

  if (seq_id) {
    if (last_req_id && seq_id != last_req_id + 1 && seq_id != 1)
      PERFETTO_ELOG("HTTP Request out of order");
    last_req_id = seq_id;
  }

  // This is the default. Overridden by the /query handler for chunked replies.
  char transfer_encoding_hdr[255] = "Transfer-Encoding: identity";
  std::initializer_list<const char*> headers = {
      "Cache-Control: no-cache",               //
      "Content-Type: application/x-protobuf",  //
      transfer_encoding_hdr,                   //
  };

  if (req.uri == "/status") {
    auto status = trace_processor_rpc_.GetStatus();
    return conn.SendResponse("200 OK", headers, Vec2Sv(status));
  }

  if (req.uri == "/websocket" && req.is_websocket_handshake) {
    // Will trigger OnWebsocketMessage() when is received.
    // It returns a 403 if the origin is not in kAllowedCORSOrigins.
    return conn.UpgradeToWebsocket(req);
  }

  // --- Everything below this line is a legacy endpoint not used by the UI.
  // There are two generations of pre-websocket legacy-ness:
  // 1. The /rpc based endpoint. This is based on a chunked transfer, doing one
  //    POST request for each RPC invocation. All RPC methods are multiplexed
  //    into this one. This is still used by the python API.
  // 2. The REST API, with one enpoint per RPC method (/parse, /query, ...).
  //    This is unused and will be removed at some point.

  if (req.uri == "/rpc") {
    // Start the chunked reply.
    base::StringCopy(transfer_encoding_hdr, "Transfer-Encoding: chunked",
                     sizeof(transfer_encoding_hdr));
    conn.SendResponseHeaders("200 OK", headers,
                             base::HttpServerConnection::kOmitContentLength);
    PERFETTO_CHECK(g_cur_conn == nullptr);
    g_cur_conn = req.conn;
    trace_processor_rpc_.SetRpcResponseFunction(SendRpcChunk);
    // OnRpcRequest() will call SendRpcChunk() one or more times.
    trace_processor_rpc_.OnRpcRequest(req.body.data(), req.body.size());
    trace_processor_rpc_.SetRpcResponseFunction(nullptr);
    g_cur_conn = nullptr;

    // Terminate chunked stream.
    conn.SendResponseBody("0\r\n\r\n", 5);
    return;
  }

  if (req.uri == "/parse") {
    base::Status status = trace_processor_rpc_.Parse(
        reinterpret_cast<const uint8_t*>(req.body.data()), req.body.size());
    protozero::HeapBuffered<protos::pbzero::AppendTraceDataResult> result;
    if (!status.ok()) {
      result->set_error(status.c_message());
    }
    return conn.SendResponse("200 OK", headers,
                             Vec2Sv(result.SerializeAsArray()));
  }

  if (req.uri == "/notify_eof") {
    trace_processor_rpc_.NotifyEndOfFile();
    return conn.SendResponse("200 OK", headers);
  }

  if (req.uri == "/restore_initial_tables") {
    trace_processor_rpc_.RestoreInitialTables();
    return conn.SendResponse("200 OK", headers);
  }

  // New endpoint, returns data in batches using chunked transfer encoding.
  // The batch size is determined by |cells_per_batch_| and
  // |batch_split_threshold_| in query_result_serializer.h.
  // This is temporary, it will be switched to WebSockets soon.
  if (req.uri == "/query") {
    std::vector<uint8_t> response;

    // Start the chunked reply.
    base::StringCopy(transfer_encoding_hdr, "Transfer-Encoding: chunked",
                     sizeof(transfer_encoding_hdr));
    conn.SendResponseHeaders("200 OK", headers,
                             base::HttpServerConnection::kOmitContentLength);

    // |on_result_chunk| will be called nested within the same callstack of the
    // rpc.Query() call. No further calls will be made once Query() returns.
    auto on_result_chunk = [&](const uint8_t* buf, size_t len, bool has_more) {
      PERFETTO_DLOG("Sending response chunk, len=%zu eof=%d", len, !has_more);
      char chunk_hdr[32];
      auto hdr_len = static_cast<size_t>(sprintf(chunk_hdr, "%zx\r\n", len));
      conn.SendResponseBody(chunk_hdr, hdr_len);
      conn.SendResponseBody(buf, len);
      conn.SendResponseBody("\r\n", 2);
      if (!has_more) {
        hdr_len = static_cast<size_t>(sprintf(chunk_hdr, "0\r\n\r\n"));
        conn.SendResponseBody(chunk_hdr, hdr_len);
      }
    };
    trace_processor_rpc_.Query(
        reinterpret_cast<const uint8_t*>(req.body.data()), req.body.size(),
        on_result_chunk);
    return;
  }

  if (req.uri == "/compute_metric") {
    std::vector<uint8_t> res = trace_processor_rpc_.ComputeMetric(
        reinterpret_cast<const uint8_t*>(req.body.data()), req.body.size());
    return conn.SendResponse("200 OK", headers, Vec2Sv(res));
  }

  if (req.uri == "/enable_metatrace") {
    trace_processor_rpc_.EnableMetatrace();
    return conn.SendResponse("200 OK", headers);
  }

  if (req.uri == "/disable_and_read_metatrace") {
    std::vector<uint8_t> res = trace_processor_rpc_.DisableAndReadMetatrace();
    return conn.SendResponse("200 OK", headers, Vec2Sv(res));
  }

  return conn.SendResponseAndClose("404 Not Found", headers);
}

void Httpd::OnWebsocketMessage(const base::WebsocketMessage& msg) {
  PERFETTO_CHECK(g_cur_conn == nullptr);
  g_cur_conn = msg.conn;
  trace_processor_rpc_.SetRpcResponseFunction(SendRpcChunk);
  // OnRpcRequest() will call SendRpcChunk() one or more times.
  trace_processor_rpc_.OnRpcRequest(msg.data.data(), msg.data.size());
  trace_processor_rpc_.SetRpcResponseFunction(nullptr);
  g_cur_conn = nullptr;
}

}  // namespace

void RunHttpRPCServer(std::unique_ptr<TraceProcessor> preloaded_instance,
                      std::string port_number) {
  Httpd srv(std::move(preloaded_instance));
  base::Optional<int> port_opt = base::StringToInt32(port_number);
  int port = port_opt.has_value() ? *port_opt : kBindPort;
  srv.Run(port);
}

void Httpd::ServeHelpPage(const base::HttpRequest& req) {
  static const char kPage[] = R"(Perfetto Trace Processor RPC Server


This service can be used in two ways:

1. Open or reload https://ui.perfetto.dev/

It will automatically try to connect and use the server on localhost:9001 when
available. Click YES when prompted to use Trace Processor Native Acceleration
in the UI dialog.
See https://perfetto.dev/docs/visualization/large-traces for more.


2. Python API.

Example: perfetto.TraceProcessor(addr='localhost:9001')
See https://perfetto.dev/docs/analysis/trace-processor#python-api for more.


For questions:
https://perfetto.dev/docs/contributing/getting-started#community
)";

  std::initializer_list<const char*> headers{"Content-Type: text/plain"};
  req.conn->SendResponse("200 OK", headers, kPage);
}

}  // namespace trace_processor
}  // namespace perfetto

#endif  // PERFETTO_TP_HTTPD
