/*
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "src/internal/pipewire_stream_portal.h"

#if defined(WEBRTC_USE_PIPEWIRE)

#include <fcntl.h>
#include <unistd.h>

#include <utility>

#include "modules/portal/portal_request_response.h"
#include "rtc_base/logging.h"

namespace libwebrtc {

namespace {

constexpr char kDesktopBusName[] = "org.freedesktop.portal.Desktop";
constexpr char kSessionInterfaceName[] = "org.freedesktop.portal.Session";

}  // namespace

PipeWireStreamPortal::PipeWireStreamPortal(int pipewire_fd,
                                           uint32_t stream_node_id,
                                           std::string session_handle)
    : stream_node_id_(stream_node_id),
      session_handle_(std::move(session_handle)) {
  if (pipewire_fd >= 0) {
    pipewire_fd_ = fcntl(pipewire_fd, F_DUPFD_CLOEXEC, 0);
    if (pipewire_fd_ < 0) {
      RTC_LOG(LS_ERROR) << "PipeWireStreamPortal: can't duplicate the "
                           "PipeWire descriptor";
    }
  }
}

PipeWireStreamPortal::~PipeWireStreamPortal() {
  Stop();
  if (pipewire_fd_ >= 0) {
    close(pipewire_fd_);
    pipewire_fd_ = -1;
  }
}

void PipeWireStreamPortal::Start() {
  if (!notifier_) {
    return;
  }
  if (pipewire_fd_ < 0 || stream_node_id_ == 0) {
    notifier_->OnScreenCastRequestResult(
        webrtc::xdg_portal::RequestResponse::kError, stream_node_id_,
        pipewire_fd_);
    return;
  }

  if (!session_handle_.empty() && !session_closed_signal_id_) {
    // The application's own portal calls use the same shared session bus
    // connection, so this is the connection that owns the session.
    GError* error = nullptr;
    connection_ = g_bus_get_sync(G_BUS_TYPE_SESSION, nullptr, &error);
    if (connection_) {
      session_closed_signal_id_ = g_dbus_connection_signal_subscribe(
          connection_, kDesktopBusName, kSessionInterfaceName, "Closed",
          session_handle_.c_str(), /*arg0=*/nullptr, G_DBUS_SIGNAL_FLAGS_NONE,
          &PipeWireStreamPortal::OnSessionClosed, this,
          /*user_data_free_func=*/nullptr);
    } else {
      RTC_LOG(LS_WARNING) << "PipeWireStreamPortal: no session bus, the "
                             "session's end won't be noticed: "
                          << (error ? error->message : "");
      g_clear_error(&error);
    }
  }

  // BaseCapturerPipeWire duplicates the descriptor again for PipeWire.
  notifier_->OnScreenCastRequestResult(
      webrtc::xdg_portal::RequestResponse::kSuccess, stream_node_id_,
      pipewire_fd_);
}

void PipeWireStreamPortal::Stop() {
  if (connection_) {
    if (session_closed_signal_id_) {
      g_dbus_connection_signal_unsubscribe(connection_,
                                           session_closed_signal_id_);
      session_closed_signal_id_ = 0;
    }
    g_object_unref(connection_);
    connection_ = nullptr;
  }
}

// static
void PipeWireStreamPortal::OnSessionClosed(GDBusConnection* connection,
                                           const char* sender_name,
                                           const char* object_path,
                                           const char* interface_name,
                                           const char* signal_name,
                                           GVariant* parameters,
                                           gpointer user_data) {
  auto* that = static_cast<PipeWireStreamPortal*>(user_data);
  RTC_LOG(LS_INFO) << "PipeWireStreamPortal: the portal session closed";
  if (that->notifier_) {
    that->notifier_->OnScreenCastSessionClosed();
  }
}

}  // namespace libwebrtc

#endif  // defined(WEBRTC_USE_PIPEWIRE)
