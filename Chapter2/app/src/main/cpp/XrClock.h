#pragma once

// Bridge between XrTime and CLOCK_MONOTONIC.
//
// The audio engine schedules in CLOCK_MONOTONIC nanoseconds. Everything else in
// the study is XrTime. Without a conversion the two halves of the subtraction
// are in different units and the whole measurement is meaningless.
//
// XR_KHR_convert_timespec_time does this properly. If the runtime does not offer
// it, we sample both clocks once and carry a constant offset. That is less
// correct in principle, and on Meta's Android runtime XrTime is CLOCK_MONOTONIC
// nanoseconds anyway, so the fallback is close to exact in practice. Which path
// is in use is reported, so the writeup can say which one the data came from.

#include <ctime>

class XrClock {
public:
    bool Init(XrInstance instance) {
        m_instance = instance;

#ifdef XR_KHR_convert_timespec_time
        if (xrGetInstanceProcAddr(instance, "xrConvertTimespecTimeToTimeKHR",
                (PFN_xrVoidFunction *)&m_toXr) != XR_SUCCESS) m_toXr = nullptr;
        if (xrGetInstanceProcAddr(instance, "xrConvertTimeToTimespecTimeKHR",
                (PFN_xrVoidFunction *)&m_toTimespec) != XR_SUCCESS) m_toTimespec = nullptr;
        m_exact = (m_toXr != nullptr && m_toTimespec != nullptr);
#endif

        if (!m_exact) {
            // One-shot offset. Sampled as close together as we can manage.
            struct timespec ts{};
            clock_gettime(CLOCK_MONOTONIC, &ts);
            m_offsetNs = 0;   // filled on the first Calibrate() with a real XrTime
        }
        return true;
    }

    // Call once per frame with the frame's predictedDisplayTime. Keeps the
    // fallback offset honest; a no-op on the exact path.
    void Calibrate(XrTime frameTime) {
        if (m_exact) return;
        struct timespec ts{};
        clock_gettime(CLOCK_MONOTONIC, &ts);
        const int64_t mono = (int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec;
        const int64_t off  = (int64_t)frameTime - mono;
        // Light smoothing so a single jittery frame does not move the mapping.
        m_offsetNs = m_haveOffset ? (m_offsetNs * 7 + off) / 8 : off;
        m_haveOffset = true;
    }

    bool MonotonicToXr(int64_t monoNs, XrTime *out) const {
#ifdef XR_KHR_convert_timespec_time
        if (m_exact) {
            struct timespec ts{};
            ts.tv_sec  = (time_t)(monoNs / 1000000000LL);
            ts.tv_nsec = (long)(monoNs % 1000000000LL);
            return m_toXr(m_instance, &ts, out) == XR_SUCCESS;
        }
#endif
        if (!m_haveOffset) return false;
        *out = (XrTime)(monoNs + m_offsetNs);
        return true;
    }

    bool XrToMonotonic(XrTime t, int64_t *outMonoNs) const {
#ifdef XR_KHR_convert_timespec_time
        if (m_exact) {
            struct timespec ts{};
            if (m_toTimespec(m_instance, t, &ts) != XR_SUCCESS) return false;
            *outMonoNs = (int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec;
            return true;
        }
#endif
        if (!m_haveOffset) return false;
        *outMonoNs = (int64_t)t - m_offsetNs;
        return true;
    }

    // True when the runtime extension is doing the conversion.
    bool Exact() const { return m_exact; }

private:
    XrInstance m_instance = XR_NULL_HANDLE;
    bool    m_exact      = false;
    bool    m_haveOffset = false;
    int64_t m_offsetNs   = 0;

#ifdef XR_KHR_convert_timespec_time
    PFN_xrConvertTimespecTimeToTimeKHR m_toXr       = nullptr;
    PFN_xrConvertTimeToTimespecTimeKHR m_toTimespec = nullptr;
#endif
};
