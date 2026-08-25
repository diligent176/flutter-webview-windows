#include "webview_platform.h"

#include <DispatcherQueue.h>
#include <shlobj.h>
#include <windows.graphics.capture.h>

#include <filesystem>
#include <iostream>

WebviewPlatform::WebviewPlatform()
    : rohelper_(std::make_unique<rx::RoHelper>(RO_INIT_SINGLETHREADED)) {
  if (rohelper_->WinRtAvailable()) {
    DispatcherQueueOptions options{sizeof(DispatcherQueueOptions),
                                   DQTYPE_THREAD_CURRENT, DQTAT_COM_STA};

    if (FAILED(rohelper_->CreateDispatcherQueueController(
            options, dispatcher_queue_controller_.put()))) {
      std::cerr << "Creating DispatcherQueueController failed." << std::endl;
      return;
    }

    if (!IsGraphicsCaptureSessionSupported()) {
      std::cerr << "Windows::Graphics::Capture::GraphicsCaptureSession is not "
                   "supported."
                << std::endl;
      return;
    }

    graphics_context_ = std::make_unique<GraphicsContext>(rohelper_.get());
    valid_ = graphics_context_->IsValid();
  }
}

WebviewPlatform::~WebviewPlatform() {
  // The dispatcher queue is deliberately abandoned rather than released
  // (BandBinder #2734). It was created with DQTYPE_THREAD_CURRENT, so it
  // belongs to the Flutter platform thread, not to this object: the WinRT
  // compositor built on it and WebView2's composition path both hold work
  // against it, and this destructor runs during engine teardown while that
  // thread is still pumping (WM_DESTROY has posted WM_QUIT but has not
  // dequeued it, so an in-flight WebView2 creation can still complete after
  // this point). Dropping the controller there puts the queue into a
  // half-owned state - alive on the thread, with nothing left holding the
  // only handle that can shut it down - immediately before ~RoHelper does
  // the rest of the WinRT teardown.
  //
  // ShutdownQueueAsync is not the answer either: it completes THROUGH the
  // queue it is shutting down, so it needs the very message pump that is
  // winding down. Blocking on it here risks hanging exit, and firing it and
  // forgetting flushes nothing. The queue's real owner is the thread, the
  // thread ends with the process, so the correct lifetime for the controller
  // is "never released". This leaks one COM reference on a process that is
  // exiting, and nothing about the plugin's live behavior changes: the queue
  // already outlived this object (it was never shut down before either), so
  // a second WebviewPlatform on the same thread sees exactly what it saw.
  static_cast<void>(dispatcher_queue_controller_.detach());
}

bool WebviewPlatform::IsGraphicsCaptureSessionSupported() {
  HSTRING className;
  HSTRING_HEADER classNameHeader;

  if (FAILED(rohelper_->GetStringReference(
          RuntimeClass_Windows_Graphics_Capture_GraphicsCaptureSession,
          &className, &classNameHeader))) {
    return false;
  }

  ABI::Windows::Graphics::Capture::IGraphicsCaptureSessionStatics*
      capture_session_statics;
  if (FAILED(rohelper_->GetActivationFactory(
          className,
          __uuidof(
              ABI::Windows::Graphics::Capture::IGraphicsCaptureSessionStatics),
          (void**)&capture_session_statics))) {
    return false;
  }

  boolean is_supported = false;
  if (FAILED(capture_session_statics->IsSupported(&is_supported))) {
    return false;
  }

  return !!is_supported;
}

std::optional<std::wstring> WebviewPlatform::GetDefaultDataDirectory() {
  PWSTR path_tmp;
  if (!SUCCEEDED(
          SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &path_tmp))) {
    return std::nullopt;
  }
  auto path = std::filesystem::path(path_tmp);
  CoTaskMemFree(path_tmp);

  wchar_t filename[MAX_PATH];
  GetModuleFileName(nullptr, filename, MAX_PATH);
  path /= "flutter_webview_windows";
  path /= std::filesystem::path(filename).stem();

  return path.wstring();
}
