/*
 * Copyright 2022 LiveKit
 *
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

#ifndef LIBWEBRTC_RTC_DESKTOP_CAPTURER_IMPL_HXX
#define LIBWEBRTC_RTC_DESKTOP_CAPTURER_IMPL_HXX

#include <atomic>
#include <string>

#include "api/video/i420_buffer.h"
#include "api/video/video_frame.h"
#include "include/rtc_desktop_capturer.h"
#include "include/rtc_desktop_device.h"
#include "include/rtc_types.h"
#include "modules/desktop_capture/desktop_and_cursor_composer.h"
#include "modules/desktop_capture/desktop_capture_options.h"
#include "modules/desktop_capture/desktop_capturer.h"
#include "modules/desktop_capture/desktop_frame.h"
#include "rtc_base/thread.h"
#ifdef WEBRTC_WIN
#include "rtc_base/win/scoped_com_initializer.h"
#endif
#if defined(WEBRTC_USE_PIPEWIRE)
#include "modules/desktop_capture/delegated_source_list_controller.h"
#include "modules/desktop_capture/desktop_capture_types.h"
#endif
#include "src/internal/vcm_capturer.h"
#include "src/internal/video_capturer.h"

#if defined(WEBRTC_USE_PIPEWIRE)
typedef struct _GMainContext GMainContext;
namespace webrtc {
class BaseCapturerPipeWire;
}
#endif

namespace libwebrtc {

#if defined(WEBRTC_USE_PIPEWIRE)
// What a PipeWire capturer captures: a source the ScreenCast portal's picker
// chooses (optionally restored from an earlier pick), or a stream of a
// portal session the application opened itself.
struct PipeWireCaptureRequest {
  // For the picker.
  webrtc::CaptureType picker_type = webrtc::CaptureType::kAnyScreenContent;
  // The id of an earlier pick to restore, or 0 to ask.
  webrtc::DesktopCapturer::SourceId restore_id = 0;
  // For an application's own session: the stream, instead of the picker.
  bool external_stream = false;
  int pipewire_fd = -1;
  uint32_t stream_node_id = 0;
  std::string session_handle;
};
#endif

class RTCDesktopCapturerImpl : public RTCDesktopCapturer,
                               public webrtc::DesktopCapturer::Callback,
#if defined(WEBRTC_USE_PIPEWIRE)
                               public webrtc::DelegatedSourceListController::
                                   Observer,
#endif
                               public webrtc::internal::VideoCapturer {
 public:
#if defined(WEBRTC_USE_PIPEWIRE)
  // Whether screens and windows are captured through the ScreenCast portal
  // and PipeWire (a Wayland session with PipeWire available).
  static bool UsesSystemPicker();

  RTCDesktopCapturerImpl(const PipeWireCaptureRequest& request,
                         webrtc::Thread* signaling_thread,
                         const RTCDesktopCapturerOptions& options);
#endif
  RTCDesktopCapturerImpl(DesktopType type,
                         webrtc::DesktopCapturer::SourceId source_id,
                         webrtc::Thread* signaling_thread,
                         scoped_refptr<MediaSource> source, bool showCursor = true);
  RTCDesktopCapturerImpl(DesktopType type,
                         webrtc::DesktopCapturer::SourceId source_id,
                         webrtc::Thread* signaling_thread,
                         scoped_refptr<MediaSource> source,
                         const RTCDesktopCapturerOptions& options);
  ~RTCDesktopCapturerImpl();

  void RegisterDesktopCapturerObserver(
      DesktopCapturerObserver* observer) override {
    observer_ = observer;
  }

  void DeRegisterDesktopCapturerObserver() override { observer_ = nullptr; }
  CaptureState Start(uint32_t fps) override;

  CaptureState Start(uint32_t fps, uint32_t x, uint32_t y, uint32_t w,
                     uint32_t h) override;

  void Stop() override;

  bool IsRunning() override;

  scoped_refptr<MediaSource> source() override { return source_; }

  bool IsCaptureBorderHidden() override { return border_hidden_; }

  SystemPickerResult GetSystemPickerResult() override {
    return picker_result_.load();
  }

 protected:
  virtual void OnCaptureResult(
      webrtc::DesktopCapturer::Result result,
      std::unique_ptr<webrtc::DesktopFrame> frame) override;

 private:
#if defined(WEBRTC_USE_PIPEWIRE)
  // DelegatedSourceListController::Observer, called on thread_ while the
  // thread's GLib main context is iterated.
  void OnSelection() override;
  void OnCancelled() override;
  void OnError() override;

  void CreatePipeWireCapturer(const PipeWireCaptureRequest& request,
                              bool show_cursor);
  // Dispatches what is pending on thread_'s GLib main context: the portal's
  // D-Bus replies and signals. Never blocks.
  void DispatchGLibEvents();
  void DestroyPipeWireCapturer();
#endif
  void CaptureFrame();
  // Converts a frame whose rows may be padded (stride > width * 4) and whose
  // size can differ from the source's (Windows.Graphics.Capture, PipeWire).
  void OnWgcFrame(const webrtc::DesktopFrame& frame);
  webrtc::DesktopCaptureOptions options_;
  std::unique_ptr<webrtc::DesktopCapturer> capturer_;
  std::unique_ptr<webrtc::Thread> thread_;
  webrtc::scoped_refptr<webrtc::I420Buffer> i420_buffer_;
  CaptureState capture_state_ = CS_STOPPED;
  DesktopType type_;
  webrtc::DesktopCapturer::SourceId source_id_;
  DesktopCapturerObserver* observer_ = nullptr;
  uint32_t capture_delay_ = 1000;  // 1s
  webrtc::DesktopCapturer::Result result_ =
      webrtc::DesktopCapturer::Result::SUCCESS;
  webrtc::Thread* signaling_thread_ = nullptr;
  scoped_refptr<MediaSource> source_;
  uint32_t x_ = 0;
  uint32_t y_ = 0;
  uint32_t w_ = 0;
  uint32_t h_ = 0;
  bool focus_window_ = true;
  // Whether capturer_ captures with Windows.Graphics.Capture. Set on thread_
  // in the constructor.
  bool use_wgc_ = false;
  // Whether the WGC capture border is off. Set on thread_ in the constructor.
  bool border_hidden_ = false;
  std::atomic<SystemPickerResult> picker_result_{kPickerNotUsed};
  // Whether frames need the stride-aware conversion (OnWgcFrame).
  bool strided_frames_ = false;
#if defined(WEBRTC_USE_PIPEWIRE)
  // Set on thread_ in the constructor, for PipeWire capturers only. The
  // portal's GDBus callbacks are delivered on thread_ through this context,
  // instead of the process's default context (often the UI thread's).
  GMainContext* glib_context_ = nullptr;
  webrtc::BaseCapturerPipeWire* pipewire_capturer_ = nullptr;
#endif
#ifdef WEBRTC_WIN
  // WGC needs COM on the thread it runs on. Created and destroyed on thread_.
  std::unique_ptr<webrtc::ScopedCOMInitializer> com_initializer_;
#endif
};

}  // namespace libwebrtc

#endif  // LIBWEBRTC_RTC_DESKTOP_CAPTURER_IMPL_HXX
