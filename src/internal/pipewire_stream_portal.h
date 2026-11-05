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

#ifndef LIBWEBRTC_INTERNAL_PIPEWIRE_STREAM_PORTAL_H_
#define LIBWEBRTC_INTERNAL_PIPEWIRE_STREAM_PORTAL_H_

#if defined(WEBRTC_USE_PIPEWIRE)

#include <gio/gio.h>

#include <cstdint>
#include <string>

#include "modules/desktop_capture/linux/wayland/screen_capture_portal_interface.h"
#include "modules/desktop_capture/linux/wayland/screencast_portal.h"

namespace libwebrtc {

// A portal for BaseCapturerPipeWire that doesn't open a portal session: it
// hands over a PipeWire stream of a session the application opened itself
// (for example a RemoteDesktop session that also selected ScreenCast
// sources). Start() reports the stream at once. When a session handle is
// given, the session's Closed signal ends the capture.
//
// Start(), Stop() and the destructor must run on one thread, whose
// thread-default GLib main context is iterated: the Closed signal is
// delivered there.
class PipeWireStreamPortal
    : public webrtc::xdg_portal::ScreenCapturePortalInterface {
 public:
  // Duplicates `pipewire_fd`; the caller keeps its own descriptor.
  PipeWireStreamPortal(int pipewire_fd,
                       uint32_t stream_node_id,
                       std::string session_handle);
  ~PipeWireStreamPortal() override;

  PipeWireStreamPortal(const PipeWireStreamPortal&) = delete;
  PipeWireStreamPortal& operator=(const PipeWireStreamPortal&) = delete;

  // Must be set before Start(), to the capturer that owns this portal.
  void set_notifier(webrtc::ScreenCastPortal::PortalNotifier* notifier) {
    notifier_ = notifier;
  }

  // ScreenCapturePortalInterface.
  void Start() override;
  void Stop() override;

 private:
  static void OnSessionClosed(GDBusConnection* connection,
                              const char* sender_name,
                              const char* object_path,
                              const char* interface_name,
                              const char* signal_name,
                              GVariant* parameters,
                              gpointer user_data);

  webrtc::ScreenCastPortal::PortalNotifier* notifier_ = nullptr;
  int pipewire_fd_ = -1;
  const uint32_t stream_node_id_;
  const std::string session_handle_;
  GDBusConnection* connection_ = nullptr;
  guint session_closed_signal_id_ = 0;
};

}  // namespace libwebrtc

#endif  // defined(WEBRTC_USE_PIPEWIRE)

#endif  // LIBWEBRTC_INTERNAL_PIPEWIRE_STREAM_PORTAL_H_
