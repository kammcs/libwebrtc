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

#include "rtc_desktop_capturer_impl.h"

#include <algorithm>
#include <memory>

#include "api/sequence_checker.h"
#include "rtc_base/checks.h"
#include "rtc_base/logging.h"
#include "third_party/libyuv/include/libyuv.h"
#ifdef WEBRTC_WIN
#include "modules/desktop_capture/win/window_capture_utils.h"
#if defined(RTC_ENABLE_WIN_WGC)
#include <windows.foundation.metadata.h>
#include <windows.graphics.capture.h>
#include <windows.security.authorization.appcapabilityaccess.h>
#include <wrl/client.h>
#include <wrl/event.h>

#include <cwchar>

#include "modules/desktop_capture/win/wgc_capturer_win.h"
#include "rtc_base/win/get_activation_factory.h"
#include "rtc_base/win/hstring.h"
#endif
#endif
#if defined(WEBRTC_USE_PIPEWIRE)
#include <glib.h>

#include <chrono>
#include <thread>

#include "modules/desktop_capture/desktop_capturer_differ_wrapper.h"
#include "modules/desktop_capture/linux/wayland/base_capturer_pipewire.h"
#include "src/internal/pipewire_stream_portal.h"
#endif

namespace libwebrtc {

enum { kCaptureDelay = 33, kCaptureMessageId = 1000 };

namespace {

RTCDesktopCapturerOptions OptionsWithCursor(bool show_cursor) {
  RTCDesktopCapturerOptions options;
  options.show_cursor = show_cursor;
  return options;
}

#if defined(WEBRTC_USE_PIPEWIRE)
// The source of a PipeWire capturer: its id is the key of the portal's
// restore token, which RTCDesktopDevice::CreateSystemPickerCapturer() takes
// back as restore_id. There's no name or thumbnail; the portal shows those.
class PipeWireMediaSource : public MediaSource {
 public:
  PipeWireMediaSource(webrtc::DesktopCapturer::SourceId id, DesktopType type)
      : id_(id), type_(type) {}

  string id() const override { return string(std::to_string(id_)); }
  string name() const override { return string(); }
  portable::vector<unsigned char> thumbnail() const override {
    return portable::vector<unsigned char>();
  }
  DesktopType type() const override { return type_; }
  bool UpdateThumbnail() override { return false; }

 private:
  const webrtc::DesktopCapturer::SourceId id_;
  const DesktopType type_;
};
#endif

#if defined(WEBRTC_WIN) && defined(RTC_ENABLE_WIN_WGC)
// Whether Windows lets this app capture without the WGC capture border: the
// system has GraphicsCaptureSession.IsBorderRequired (Windows 11) and
// GraphicsCaptureAccess grants borderless capture. Runs on a COM initialized
// thread.
bool CanCaptureWithoutBorder() {
  using Microsoft::WRL::ComPtr;
  namespace WGC = ABI::Windows::Graphics::Capture;
  namespace Access = ABI::Windows::Security::Authorization::AppCapabilityAccess;
  namespace WF = ABI::Windows::Foundation;

  if (!webrtc::ResolveCoreWinRTDelayload() ||
      !webrtc::ResolveCoreWinRTStringDelayload()) {
    return false;
  }

  ComPtr<WF::Metadata::IApiInformationStatics> api_info;
  HRESULT hr = webrtc::GetActivationFactory<
      WF::Metadata::IApiInformationStatics,
      RuntimeClass_Windows_Foundation_Metadata_ApiInformation>(&api_info);
  if (FAILED(hr)) {
    return false;
  }
  static const wchar_t kSessionType[] =
      L"Windows.Graphics.Capture.GraphicsCaptureSession";
  static const wchar_t kBorderProperty[] = L"IsBorderRequired";
  HSTRING session_type = nullptr;
  HSTRING border_property = nullptr;
  boolean has_border_property = false;
  if (SUCCEEDED(webrtc::CreateHstring(kSessionType, wcslen(kSessionType),
                                      &session_type)) &&
      SUCCEEDED(webrtc::CreateHstring(kBorderProperty, wcslen(kBorderProperty),
                                      &border_property))) {
    if (FAILED(api_info->IsPropertyPresent(session_type, border_property,
                                           &has_border_property))) {
      has_border_property = false;
    }
  }
  if (session_type) {
    webrtc::DeleteHstring(session_type);
  }
  if (border_property) {
    webrtc::DeleteHstring(border_property);
  }
  if (!has_border_property) {
    return false;
  }

  ComPtr<WGC::IGraphicsCaptureAccessStatics> access;
  hr = webrtc::GetActivationFactory<
      WGC::IGraphicsCaptureAccessStatics,
      RuntimeClass_Windows_Graphics_Capture_GraphicsCaptureAccess>(&access);
  if (FAILED(hr)) {
    return false;
  }
  ComPtr<WF::IAsyncOperation<Access::AppCapabilityAccessStatus>> request;
  if (FAILED(access->RequestAccessAsync(
          WGC::GraphicsCaptureAccessKind_Borderless, &request))) {
    return false;
  }
  // Desktop apps get an answer without a prompt. The event is shared with
  // the completion handler, which may run after a timeout here.
  auto done = std::make_shared<Microsoft::WRL::Wrappers::Event>(
      CreateEventEx(nullptr, nullptr, 0, EVENT_ALL_ACCESS));
  if (!done->IsValid()) {
    return false;
  }
  auto handler = Microsoft::WRL::Callback<
      WF::IAsyncOperationCompletedHandler<Access::AppCapabilityAccessStatus>>(
      [done](WF::IAsyncOperation<Access::AppCapabilityAccessStatus>*,
             WF::AsyncStatus) {
        SetEvent(done->Get());
        return S_OK;
      });
  if (!handler || FAILED(request->put_Completed(handler.Get())) ||
      WaitForSingleObject(done->Get(), 5000) != WAIT_OBJECT_0) {
    return false;
  }
  Access::AppCapabilityAccessStatus status;
  if (FAILED(request->GetResults(&status))) {
    return false;
  }
  return status == Access::AppCapabilityAccessStatus_Allowed;
}
#endif

}  // namespace

RTCDesktopCapturerImpl::RTCDesktopCapturerImpl(
    DesktopType type, webrtc::DesktopCapturer::SourceId source_id,
    webrtc::Thread* signaling_thread, scoped_refptr<MediaSource> source,
    bool showCursor)
    : RTCDesktopCapturerImpl(type, source_id, signaling_thread, source,
                             OptionsWithCursor(showCursor)) {}

RTCDesktopCapturerImpl::RTCDesktopCapturerImpl(
    DesktopType type, webrtc::DesktopCapturer::SourceId source_id,
    webrtc::Thread* signaling_thread, scoped_refptr<MediaSource> source,
    const RTCDesktopCapturerOptions& options)
    : thread_(webrtc::Thread::Create()),
      source_id_(source_id),
      signaling_thread_(signaling_thread),
      source_(source),
      focus_window_(options.focus_window) {
  RTC_DCHECK(thread_);
  type_ = type;
  thread_->Start();
  options_ = webrtc::DesktopCaptureOptions::CreateDefault();
  options_.set_detect_updated_region(true);
#ifdef WEBRTC_WIN
  options_.set_allow_directx_capturer(true);
#if defined(RTC_ENABLE_WIN_WGC)
  if (type == kWindow && options.allow_wgc_window_capturer) {
    options_.set_allow_wgc_window_capturer(true);
    options_.set_wgc_require_border(options.wgc_border_required);
  }
#endif
#endif
#if defined(WEBRTC_USE_PIPEWIRE)
  // In a Wayland session, windows can be captured only through PipeWire as
  // well: without it there's no window capturer at all.
  options_.set_allow_pipewire(true);
#endif
  const bool showCursor = options.show_cursor;
  const bool wants_borderless = !options.wgc_border_required;
  thread_->BlockingCall([this, type, showCursor, wants_borderless] {
#if defined(WEBRTC_WIN) && defined(RTC_ENABLE_WIN_WGC)
    if (options_.allow_wgc_window_capturer()) {
      // WGC creates WinRT objects and a DispatcherQueue on the thread it
      // captures on, which must already be COM initialized.
      com_initializer_ = std::make_unique<webrtc::ScopedCOMInitializer>(
          webrtc::ScopedCOMInitializer::kMTA);
      use_wgc_ = com_initializer_->Succeeded() &&
                 webrtc::IsWgcSupported(webrtc::CaptureType::kWindow);
      if (!use_wgc_) {
        options_.set_allow_wgc_window_capturer(false);
      }
      // Ask WGC to drop its border only when Windows allows it, so the
      // border state is known rather than a request that may silently fail.
      border_hidden_ =
          use_wgc_ && wants_borderless && CanCaptureWithoutBorder();
      options_.set_wgc_require_border(!border_hidden_);
      RTC_LOG(LS_INFO) << "RTCDesktopCapturerImpl: window capture with "
                       << (use_wgc_ ? "Windows.Graphics.Capture"
                                    : "the default capturer")
                       << (border_hidden_ ? ", without the capture border"
                                          : "");
    }
#else
    (void)wants_borderless;
#endif
    std::unique_ptr<webrtc::DesktopCapturer> capturer =
        type == kScreen
            ? webrtc::DesktopCapturer::CreateScreenCapturer(options_)
            : webrtc::DesktopCapturer::CreateWindowCapturer(options_);
    if (!capturer) {
      // No capturer for this kind of source on this system; Start() fails.
      RTC_LOG(LS_ERROR) << "RTCDesktopCapturerImpl: no "
                        << (type == kScreen ? "screen" : "window")
                        << " capturer on this system";
      return;
    }
    if (type == kScreen && !showCursor) {
      capturer_ =
          webrtc::DesktopAndCursorComposer::CreateWithoutMouseCursorMonitor(
              std::move(capturer));
    } else {
      capturer_ = std::make_unique<webrtc::DesktopAndCursorComposer>(
          std::move(capturer), options_);
    }
  });
}

#if defined(WEBRTC_USE_PIPEWIRE)
// static
bool RTCDesktopCapturerImpl::UsesSystemPicker() {
  return webrtc::BaseCapturerPipeWire::IsSupported();
}

RTCDesktopCapturerImpl::RTCDesktopCapturerImpl(
    const PipeWireCaptureRequest& request, webrtc::Thread* signaling_thread,
    const RTCDesktopCapturerOptions& options)
    : thread_(webrtc::Thread::Create()),
      source_id_(-1),
      signaling_thread_(signaling_thread),
      focus_window_(false) {
  RTC_DCHECK(thread_);
  type_ = request.picker_type == webrtc::CaptureType::kWindow ? kWindow
                                                               : kScreen;
  strided_frames_ = true;
  if (!request.external_stream) {
    picker_result_ = kPickerPending;
  }
  thread_->Start();
  options_ = webrtc::DesktopCaptureOptions::CreateDefault();
  options_.set_detect_updated_region(true);
  options_.set_allow_pipewire(true);
  // The portal draws the cursor into the frames ("embedded"), or sends it
  // as metadata that isn't drawn.
  options_.set_prefer_cursor_embedded(options.show_cursor);
  const bool show_cursor = options.show_cursor;
  thread_->BlockingCall([this, &request, show_cursor] {
    // Everything the portal does on this capturer's behalf (GDBus replies
    // and signals) is delivered to this thread through its own GLib main
    // context, which CaptureFrame() iterates. The application's main loop,
    // often the UI thread, is never involved and never waited on.
    glib_context_ = g_main_context_new();
    g_main_context_push_thread_default(glib_context_);
    CreatePipeWireCapturer(request, show_cursor);
  });
}

void RTCDesktopCapturerImpl::CreatePipeWireCapturer(
    const PipeWireCaptureRequest& request, bool show_cursor) {
  RTC_DCHECK_RUN_ON(thread_.get());
  (void)show_cursor;
  if (!webrtc::BaseCapturerPipeWire::IsSupported()) {
    RTC_LOG(LS_ERROR) << "RTCDesktopCapturerImpl: PipeWire capture isn't "
                         "available";
    return;
  }
  std::unique_ptr<webrtc::BaseCapturerPipeWire> pipewire;
  if (request.external_stream) {
    auto portal = std::make_unique<PipeWireStreamPortal>(
        request.pipewire_fd, request.stream_node_id, request.session_handle);
    PipeWireStreamPortal* portal_ptr = portal.get();
    pipewire = std::make_unique<webrtc::BaseCapturerPipeWire>(
        options_, std::move(portal));
    portal_ptr->set_notifier(pipewire.get());
  } else {
    pipewire = std::make_unique<webrtc::BaseCapturerPipeWire>(
        options_, request.picker_type);
    pipewire->Observe(this);
  }

  // The source id is the key of the portal's restore token: the token of
  // the pick is stored under it once the user has picked.
  webrtc::DesktopCapturer::SourceId id = request.restore_id;
  if (id) {
    pipewire->SelectSource(id);
  } else {
    webrtc::DesktopCapturer::SourceList sources;
    pipewire->GetSourceList(&sources);
    if (!sources.empty()) {
      id = sources.front().id;
    }
  }
  source_ = new RefCountedObject<PipeWireMediaSource>(id, type_);

  pipewire_capturer_ = pipewire.get();
  std::unique_ptr<webrtc::DesktopCapturer> capturer = std::move(pipewire);
  if (options_.detect_updated_region()) {
    capturer =
        std::make_unique<webrtc::DesktopCapturerDifferWrapper>(
            std::move(capturer));
  }
  capturer_ = std::move(capturer);
}

void RTCDesktopCapturerImpl::DispatchGLibEvents() {
  RTC_DCHECK_RUN_ON(thread_.get());
  if (!glib_context_) {
    return;
  }
  // Bounded, so a flood of events can't starve the capture loop.
  for (int i = 0; i < 100 && g_main_context_iteration(glib_context_, FALSE);
       ++i) {
  }
}

void RTCDesktopCapturerImpl::DestroyPipeWireCapturer() {
  RTC_DCHECK_RUN_ON(thread_.get());
  // Destroying the capturer closes the portal session (or stops watching
  // the application's) and cancels the portal calls in flight.
  pipewire_capturer_ = nullptr;
  capturer_.reset();
  if (!glib_context_) {
    return;
  }
  // Let the cancelled calls finish on this context for a moment, so their
  // callbacks run (as no-ops) and release what they hold.
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::milliseconds(100);
  while (std::chrono::steady_clock::now() < deadline) {
    if (g_main_context_iteration(glib_context_, FALSE)) {
      continue;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  g_main_context_pop_thread_default(glib_context_);
  g_main_context_unref(glib_context_);
  glib_context_ = nullptr;
}

void RTCDesktopCapturerImpl::OnSelection() {
  RTC_DCHECK_RUN_ON(thread_.get());
  picker_result_ = kPickerSelected;
  if (observer_) {
    signaling_thread_->BlockingCall([this]() { observer_->OnStart(this); });
  }
}

void RTCDesktopCapturerImpl::OnCancelled() {
  RTC_DCHECK_RUN_ON(thread_.get());
  // The next CaptureFrame() reports ERROR_PERMANENT, which calls the
  // observer's OnError().
  picker_result_ = kPickerCancelled;
}

void RTCDesktopCapturerImpl::OnError() {
  RTC_DCHECK_RUN_ON(thread_.get());
  picker_result_ = kPickerFailed;
}
#endif  // defined(WEBRTC_USE_PIPEWIRE)

RTCDesktopCapturerImpl::~RTCDesktopCapturerImpl() {
#if defined(WEBRTC_USE_PIPEWIRE)
  if (glib_context_) {
    // The portal and its GLib context belong to thread_.
    thread_->BlockingCall([this] {
      capture_state_ = CS_STOPPED;
      DestroyPipeWireCapturer();
    });
  }
#endif
#ifdef WEBRTC_WIN
  if (com_initializer_) {
    // Release the capturer's WinRT objects, then COM, on the thread that
    // created them.
    thread_->BlockingCall([this] {
      capture_state_ = CS_STOPPED;
      capturer_.reset();
      com_initializer_.reset();
    });
  }
#endif
  thread_->Stop();
  capturer_.reset();
}

RTCDesktopCapturerImpl::CaptureState RTCDesktopCapturerImpl::Start(
    uint32_t fps, uint32_t x, uint32_t y, uint32_t w, uint32_t h) {
  x_ = x;
  y_ = y;
  w_ = w;
  h_ = h;
  if (!w_ || !h) {
    x_ = 0;
    y_ = 0;
  }
  return Start(fps);
}

RTCDesktopCapturerImpl::CaptureState RTCDesktopCapturerImpl::Start(
    uint32_t fps) {
  if (capture_state_ == CS_RUNNING) {
    return capture_state_;
  }

  if (!capturer_) {
    capture_state_ = CS_FAILED;
    return capture_state_;
  }

  if (fps == 0) {
    capture_state_ = CS_FAILED;
    return capture_state_;
  }

  if (fps >= 60) {
    capture_delay_ = uint32_t(1000.0 / 60.0);
  } else {
    capture_delay_ = uint32_t(1000.0 / fps);
  }

  if (source_id_ != -1) {
    // WGC creates its capture item when the source is selected: do it on
    // the thread it captures on.
    const bool selected = use_wgc_ ? thread_->BlockingCall([this] {
      return capturer_->SelectSource(source_id_);
    })
                                   : capturer_->SelectSource(source_id_);
    if (!selected) {
      capture_state_ = CS_FAILED;
      return capture_state_;
    }
    if (type_ == kWindow && focus_window_) {
      if (!capturer_->FocusOnSelectedSource()) {
        capture_state_ = CS_FAILED;
        return capture_state_;
      }
    }
  }

  const bool started = thread_->BlockingCall([this] {
    // A PipeWire capturer's portal is gone once it was stopped.
    if (!capturer_) {
      return false;
    }
    capturer_->Start(this);
#if defined(WEBRTC_USE_PIPEWIRE)
    DispatchGLibEvents();
#endif
    return true;
  });
  if (!started) {
    capture_state_ = CS_FAILED;
    return capture_state_;
  }
  capture_state_ = CS_RUNNING;
  thread_->PostTask([this] { CaptureFrame(); });
  // A capturer behind the system picker reports OnStart() once the user has
  // picked a source (OnSelection()).
  if (observer_ && picker_result_.load() == kPickerNotUsed) {
    signaling_thread_->BlockingCall([&, this]() { observer_->OnStart(this); });
  }
  return capture_state_;
}

void RTCDesktopCapturerImpl::Stop() {
  if (observer_) {
    if (!signaling_thread_->IsCurrent()) {
      signaling_thread_->BlockingCall([&, this]() { observer_->OnStop(this); });
    } else {
      observer_->OnStop(this);
    }
  }
  capture_state_ = CS_STOPPED;
#if defined(WEBRTC_USE_PIPEWIRE)
  if (glib_context_) {
    // End the portal session now, so the system's "sharing" indicator goes
    // away even while this capturer is still referenced. Posted rather than
    // blocking: Stop() may be called from an observer callback that
    // thread_ is waiting on. The destructor's blocking call on thread_ runs
    // after this task.
    thread_->PostTask([this] {
      if (capture_state_ == CS_STOPPED) {
        pipewire_capturer_ = nullptr;
        capturer_.reset();
      }
    });
  }
#endif
}

bool RTCDesktopCapturerImpl::IsRunning() {
  return capture_state_ == CS_RUNNING;
}

#ifdef WEBRTC_WIN
int filterException(int code, PEXCEPTION_POINTERS ex) {
  return EXCEPTION_EXECUTE_HANDLER;
}
#endif

void RTCDesktopCapturerImpl::OnCaptureResult(
    webrtc::DesktopCapturer::Result result,
    std::unique_ptr<webrtc::DesktopFrame> frame) {
  if (result != result_) {
    if (result == webrtc::DesktopCapturer::Result::ERROR_PERMANENT) {
      if (observer_) {
        signaling_thread_->BlockingCall(
            [&, this]() { observer_->OnError(this); });
      }
      capture_state_ = CS_FAILED;
      return;
    }

    if (result == webrtc::DesktopCapturer::Result::ERROR_TEMPORARY) {
      result_ = result;
      if (observer_) {
        signaling_thread_->BlockingCall(
            [&, this]() { observer_->OnPaused(this); });
      }
      return;
    }

    if (result == webrtc::DesktopCapturer::Result::SUCCESS) {
      result_ = result;
      if (observer_) {
        signaling_thread_->BlockingCall(
            [&, this]() { observer_->OnStart(this); });
      }
    }
  }

  if (result == webrtc::DesktopCapturer::Result::ERROR_TEMPORARY) {
    return;
  }

  if (use_wgc_ || strided_frames_) {
    OnWgcFrame(*frame);
    return;
  }

  int width = frame->size().width();
  int height = frame->size().height();
#ifdef WEBRTC_WIN
  webrtc::DesktopRect rect_ = webrtc::DesktopRect::MakeWH(width, height);

  if (type_ != kScreen) {
    webrtc::GetWindowRect(reinterpret_cast<HWND>(source_id_), &rect_);
  }

  __try
#endif
  {
    width = w_ > 0 ? w_ : width;
    height = h_ > 0 ? h_ : height;
    if (!i420_buffer_ || !i420_buffer_.get() ||
        i420_buffer_->width() * i420_buffer_->height() != width * height) {
      i420_buffer_ = webrtc::I420Buffer::Create(width, height);
    }

    libyuv::ConvertToI420(frame->data(), 0, i420_buffer_->MutableDataY(),
                          i420_buffer_->StrideY(), i420_buffer_->MutableDataU(),
                          i420_buffer_->StrideU(), i420_buffer_->MutableDataV(),
                          i420_buffer_->StrideV(), x_, y_,
#ifdef WEBRTC_WIN
                          rect_.width(), rect_.height(),
#else
                          width, height,
#endif
                          width, height, libyuv::kRotate0, libyuv::FOURCC_ARGB);

    OnFrame(webrtc::VideoFrame(i420_buffer_, 0, webrtc::TimeMillis(),
                               webrtc::kVideoRotation_0));
  }
#ifdef WEBRTC_WIN
  __except (filterException(GetExceptionCode(), GetExceptionInformation())) {
  }
#endif
}

void RTCDesktopCapturerImpl::OnWgcFrame(const webrtc::DesktopFrame& frame) {
  // A WGC frame has the window's current size, which can differ from its
  // window rectangle, and its rows may be padded (stride > width * 4), so it
  // is converted as it is.
  const int frame_width = frame.size().width();
  const int frame_height = frame.size().height();
  const int x = std::min<int>(x_, frame_width);
  const int y = std::min<int>(y_, frame_height);
  const int width =
      w_ > 0 ? std::min<int>(w_, frame_width - x) : frame_width - x;
  const int height =
      h_ > 0 ? std::min<int>(h_, frame_height - y) : frame_height - y;
  if (width <= 0 || height <= 0) {
    return;
  }
  if (!i420_buffer_ || i420_buffer_->width() != width ||
      i420_buffer_->height() != height) {
    i420_buffer_ = webrtc::I420Buffer::Create(width, height);
  }
  const uint8_t* src = frame.data() + y * frame.stride() +
                       x * webrtc::DesktopFrame::kBytesPerPixel;
  libyuv::ARGBToI420(src, frame.stride(), i420_buffer_->MutableDataY(),
                     i420_buffer_->StrideY(), i420_buffer_->MutableDataU(),
                     i420_buffer_->StrideU(), i420_buffer_->MutableDataV(),
                     i420_buffer_->StrideV(), width, height);
  OnFrame(webrtc::VideoFrame(i420_buffer_, 0, webrtc::TimeMillis(),
                             webrtc::kVideoRotation_0));
}

void RTCDesktopCapturerImpl::CaptureFrame() {
  RTC_DCHECK_RUN_ON(thread_.get());
#if defined(WEBRTC_USE_PIPEWIRE)
  DispatchGLibEvents();
#endif
  if (capture_state_ == CS_RUNNING && capturer_) {
    capturer_->CaptureFrame();
    thread_->PostDelayedHighPrecisionTask(
        [this]() { CaptureFrame(); },
        webrtc::TimeDelta::Millis(capture_delay_));
  }
}

}  // namespace libwebrtc
