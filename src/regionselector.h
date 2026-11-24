#pragma once

#include "capture.h"

#include <functional>

namespace RegionSelector {

using Completion = std::function<void(QImage, const std::function<void()> &dismiss)>;

// Runs on the GUI thread. A null image denotes cancellation or an unavailable frame.
// After a selection, the caller can show the next window before dismissing the overlay.
// Dismiss the overlay before entering a nested event loop. Returning also dismisses it.
void start(const QList<CaptureFrame> &frames, Completion done);

} // namespace RegionSelector
