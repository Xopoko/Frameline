#pragma once

#include "capture.h"

#include <functional>

namespace RegionSelector {

// Must run on the GUI thread. A null image denotes cancellation or an unavailable frame.
void start(const QList<CaptureFrame> &frames, std::function<void(QImage)> done);

} // namespace RegionSelector
