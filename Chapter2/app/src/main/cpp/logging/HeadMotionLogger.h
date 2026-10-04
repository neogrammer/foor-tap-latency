#pragma once

// Head pose logger for foot-strike detection experiments.
//
// Records raw pose + XrTime once per frame into memory, writes CSV on shutdown.
// Nothing is smoothed or differentiated here on purpose -- derive velocity and
// acceleration offline so you can change your mind about the filter later
// without re-running the experiment.

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

class HeadMotionLogger {
public:
    struct Sample {
        XrTime   time;
        float    px, py, pz;
        float    ox, oy, oz, ow;
        uint64_t flags;
    };

    // Default cap: ~5 minutes at 120 Hz. ~40 bytes/sample, so under 1.5 MB.
    explicit HeadMotionLogger(size_t maxSamples = 36000)
            : m_max(maxSamples) {
        m_samples.reserve(m_max);
    }

    void Record(XrTime t, const XrSpaceLocation &loc) {
        if (m_samples.size() >= m_max) {
            return;
        }
        m_samples.push_back({
            t,
            loc.pose.position.x, loc.pose.position.y, loc.pose.position.z,
            loc.pose.orientation.x, loc.pose.orientation.y,
            loc.pose.orientation.z, loc.pose.orientation.w,
            static_cast<uint64_t>(loc.locationFlags)
        });
    }

    bool WriteCSV(const std::string &path) const {
        if (m_samples.empty()) {
            return false;
        }
        FILE *f = fopen(path.c_str(), "w");
        if (!f) {
            return false;
        }
        fprintf(f, "xr_time_ns,pos_x,pos_y,pos_z,quat_x,quat_y,quat_z,quat_w,flags\n");
        for (const Sample &s : m_samples) {
            fprintf(f, "%lld,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,%llu\n",
                    static_cast<long long>(s.time),
                    s.px, s.py, s.pz,
                    s.ox, s.oy, s.oz, s.ow,
                    static_cast<unsigned long long>(s.flags));
        }
        fclose(f);
        return true;
    }

    size_t Count() const { return m_samples.size(); }
    bool   Full()  const { return m_samples.size() >= m_max; }
    void   Clear()       { m_samples.clear(); }

private:
    std::vector<Sample> m_samples;
    size_t              m_max;
};
