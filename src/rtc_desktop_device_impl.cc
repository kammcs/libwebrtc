#include "rtc_desktop_device_impl.h"

#include <cstdlib>

#include "rtc_base/thread.h"
#include "rtc_desktop_capturer.h"
#include "rtc_desktop_media_list.h"
#include "rtc_video_device_impl.h"

namespace libwebrtc {

namespace {

RTCDesktopCapturerOptions OptionsWithCursor(bool show_cursor) {
  RTCDesktopCapturerOptions options;
  options.show_cursor = show_cursor;
  return options;
}

}  // namespace

RTCDesktopDeviceImpl::RTCDesktopDeviceImpl(webrtc::Thread* signaling_thread)
    : signaling_thread_(signaling_thread) {}

RTCDesktopDeviceImpl::~RTCDesktopDeviceImpl() {}

scoped_refptr<RTCDesktopCapturer> RTCDesktopDeviceImpl::CreateDesktopCapturer(
    scoped_refptr<MediaSource> source, bool showCursor) {
  return CreateDesktopCapturerWithOptions(source, OptionsWithCursor(showCursor));
}

scoped_refptr<RTCDesktopCapturer>
RTCDesktopDeviceImpl::CreateDesktopCapturerWithOptions(
    scoped_refptr<MediaSource> source,
    const RTCDesktopCapturerOptions& options) {
  MediaSourceImpl* source_impl = static_cast<MediaSourceImpl*>(source.get());
  if (HasSystemPicker()) {
    // The source is a media list's placeholder: let the system picker
    // choose a source of its type.
    const bool screen = source_impl->type() == kScreen;
    return CreateSystemPickerCapturer(screen, !screen, string(), options);
  }
  return new RefCountedObject<RTCDesktopCapturerImpl>(
      source_impl->type(), source_impl->source_id(), signaling_thread_, source,
      options);
}

scoped_refptr<RTCDesktopMediaList> RTCDesktopDeviceImpl::GetDesktopMediaList(
    DesktopType type) {
  if (desktop_media_lists_.find(type) == desktop_media_lists_.end()) {
    desktop_media_lists_[type] =
        new RefCountedObject<RTCDesktopMediaListImpl>(type, signaling_thread_);
  }
  return desktop_media_lists_[type];
}

bool RTCDesktopDeviceImpl::HasSystemPicker() {
#if defined(WEBRTC_USE_PIPEWIRE)
  return RTCDesktopCapturerImpl::UsesSystemPicker();
#else
  return false;
#endif
}

scoped_refptr<RTCDesktopCapturer>
RTCDesktopDeviceImpl::CreateSystemPickerCapturer(
    bool screens, bool windows, const string restore_id,
    const RTCDesktopCapturerOptions& options) {
#if defined(WEBRTC_USE_PIPEWIRE)
  if (!HasSystemPicker() || (!screens && !windows)) {
    return nullptr;
  }
  PipeWireCaptureRequest request;
  request.picker_type = screens && windows ? webrtc::CaptureType::kAnyScreenContent
                        : screens          ? webrtc::CaptureType::kScreen
                                           : webrtc::CaptureType::kWindow;
  const std::string id = restore_id.std_string();
  if (!id.empty()) {
    char* end = nullptr;
    const long long value = std::strtoll(id.c_str(), &end, 10);
    if (end && *end == '\0' && value > 0) {
      request.restore_id =
          static_cast<webrtc::DesktopCapturer::SourceId>(value);
    }
  }
  return new RefCountedObject<RTCDesktopCapturerImpl>(
      request, signaling_thread_, options);
#else
  return nullptr;
#endif
}

scoped_refptr<RTCDesktopCapturer>
RTCDesktopDeviceImpl::CreatePipeWireStreamCapturer(
    int pipewire_fd, uint32_t stream_node_id, const string session_handle,
    const RTCDesktopCapturerOptions& options) {
#if defined(WEBRTC_USE_PIPEWIRE)
  if (pipewire_fd < 0 || stream_node_id == 0) {
    return nullptr;
  }
  PipeWireCaptureRequest request;
  request.external_stream = true;
  request.pipewire_fd = pipewire_fd;
  request.stream_node_id = stream_node_id;
  request.session_handle = session_handle.std_string();
  return new RefCountedObject<RTCDesktopCapturerImpl>(
      request, signaling_thread_, options);
#else
  return nullptr;
#endif
}

}  // namespace libwebrtc
