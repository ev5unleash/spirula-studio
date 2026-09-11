#pragma once

// What the sensors in a chosen input actually hold, read off the GUI thread.
//
// A video carries its IMU and GPS in a track (sfm/core/Telemetry.h); a folder
// of photographs carries GPS in each file's EXIF. The panel asks about a path
// on every frame it draws the row, so the first ask queues the read and every
// later one returns whatever is known by then.

#include <condition_variable>
#include <deque>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <utility>

namespace gui {

struct TelemetryInfo {
    bool done = false;      // false while the read is still queued or running
    bool video = false;
    // A video's streams, and the carrier they came in ("" when none did).
    bool gyro = false, accel = false, attitude = false, gps = false;
    std::string carrier;
    // A photo folder: files whose EXIF carries a position, out of those read.
    int with_gps = 0, photos = 0;

    bool any() const { return gyro || accel || attitude || gps || with_gps; }
};

class TelemetryProbe {
public:
    ~TelemetryProbe();

    // What is known about `path`, queueing the read on the first call.
    TelemetryInfo get(const std::string& path, bool is_video);

private:
    void run();

    std::mutex _mu;
    std::condition_variable _cv;
    std::map<std::string, TelemetryInfo> _known;
    std::deque<std::pair<std::string, bool>> _queue;
    std::thread _worker;
    bool _quit = false;
};

}  // namespace gui
