#ifndef LIB_WEBRTC_RTC_DESKTOP_DEVICE_HXX
#define LIB_WEBRTC_RTC_DESKTOP_DEVICE_HXX

#include "rtc_types.h"
#include <map>
#include <string>

namespace libwebrtc {

class MediaSource;
class RTCDesktopCapturer;
class RTCDesktopMediaList;

/**
 * @brief Options for a desktop capturer. The defaults behave like
 *        CreateDesktopCapturer(source, showCursor).
 */
struct RTCDesktopCapturerOptions {
  /** Draw the mouse cursor into the captured frames. */
  bool show_cursor = true;

  /**
   * Bring a shared window to the front when capture starts. Start() returns
   * CS_FAILED when the window can't be brought forward (Windows may refuse
   * to change the foreground window). Turn it off to share a window that
   * stays in the background; this needs a capturer that can read covered
   * windows, such as Windows.Graphics.Capture.
   */
  bool focus_window = true;

  /**
   * Windows only: capture windows with Windows.Graphics.Capture (WGC) where
   * the system supports it (Windows 10 1903 or later). WGC reads the window
   * from the compositor, so a covered or background window keeps sending
   * frames. Elsewhere the default window capturer is used.
   */
  bool allow_wgc_window_capturer = false;

  /**
   * Windows, WGC only: whether Windows draws its capture border around the
   * shared window. When off, the capturer asks for borderless capture
   * (GraphicsCaptureAccess::RequestAccessAsync with
   * GraphicsCaptureAccessKind::Borderless) and removes the border only if
   * that is allowed and the system supports it (Windows 11); otherwise the
   * border stays. RTCDesktopCapturer::IsCaptureBorderHidden() tells which.
   */
  bool wgc_border_required = true;
};

class RTCDesktopDevice : public RefCountInterface {
 public:
  virtual scoped_refptr<RTCDesktopCapturer> CreateDesktopCapturer(
      scoped_refptr<MediaSource> source, bool showCursor = true) = 0;
  virtual scoped_refptr<RTCDesktopMediaList> GetDesktopMediaList(
      DesktopType type) = 0;
  /**
   * @brief Creates a capturer for source with options. Added last, and not
   *        as an overload, so the existing vtable slots keep their order.
   */
  virtual scoped_refptr<RTCDesktopCapturer> CreateDesktopCapturerWithOptions(
      scoped_refptr<MediaSource> source,
      const RTCDesktopCapturerOptions& options) = 0;

  // The methods below are added after the existing ones, so the existing
  // vtable slots keep their order.

  /**
   * @brief Whether the system's own picker chooses what is captured.
   *
   * True on Linux in a Wayland session with PipeWire, where the
   * xdg-desktop-portal ScreenCast dialog is the picker and applications
   * can't list screens or windows themselves. Desktop media lists then hold
   * one placeholder source per type, without a thumbnail, and opening them
   * never shows the portal. Use CreateSystemPickerCapturer() to share.
   * False elsewhere.
   */
  virtual bool HasSystemPicker() = 0;

  /**
   * @brief Creates a capturer whose source the system picker chooses.
   *
   * The picker (the ScreenCast portal dialog) opens when Start() is called,
   * and Start() returns without waiting for it. Once the user picks a
   * source, the observer's OnStart() is called and frames follow. When the
   * user cancels, or the portal fails, OnError() is called; then
   * GetSystemPickerResult() tells which. OnStart() may be called again
   * later, when frames resume after a pause.
   *
   * @param screens, windows Which kinds of sources the picker offers. With
   *        both, one dialog offers screens and windows.
   * @param restore_id The source()->id() of an earlier capturer from this
   *        method, to share the same source again without asking, while the
   *        application runs (the portal's transient restore token). Empty,
   *        unknown or expired ids show the picker.
   * @return nullptr when HasSystemPicker() is false.
   */
  virtual scoped_refptr<RTCDesktopCapturer> CreateSystemPickerCapturer(
      bool screens, bool windows, const string restore_id,
      const RTCDesktopCapturerOptions& options) = 0;

  /**
   * @brief Creates a capturer for a PipeWire stream of a portal session that
   *        the application opened itself.
   *
   * For example a RemoteDesktop session that also selected ScreenCast
   * sources, so the user saw one dialog for control and sharing. No dialog
   * is shown here. The capturer duplicates `pipewire_fd` (from the
   * session's OpenPipeWireRemote) and closes its copy when it is destroyed;
   * the caller keeps its own. When `session_handle` is set, the capturer
   * watches that session's Closed signal and reports OnError() when it
   * closes.
   *
   * @return nullptr when PipeWire capture isn't available.
   */
  virtual scoped_refptr<RTCDesktopCapturer> CreatePipeWireStreamCapturer(
      int pipewire_fd, uint32_t stream_node_id, const string session_handle,
      const RTCDesktopCapturerOptions& options) = 0;

 protected:
  virtual ~RTCDesktopDevice() {}
};

}  // namespace libwebrtc

#endif  // LIB_WEBRTC_RTC_VIDEO_DEVICE_HXX