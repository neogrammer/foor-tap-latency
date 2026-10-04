#pragma once

// Skeletal hand tracking, plus Meta's simultaneous hands-and-controllers mode.
//
// Why both: the only controller the participant can reach is lying on the floor
// under their foot. Without hand tracking there is nothing to drive the dialogs
// with. Hands do the UI, the foot does the timed input, and each sensor does the
// job it is actually good at.
//
// Hand tracking is camera-based, lower rate and higher latency than a controller.
// It is fine for pressing a button. NEVER time anything off a hand joint.
//
// Everything here degrades quietly. If an extension is missing at compile time or
// unsupported at runtime, Available() returns false and the caller skips it. On a
// deadline that matters more than elegance.

#include <cstring>

#ifndef XR_HAND_JOINT_COUNT_EXT
#define TAPSTUDY_NO_HAND_TRACKING 1
#endif

class HandTracking {
public:
    static constexpr int kMaxJoints = 26;

    struct Joint {
        float x = 0, y = 0, z = 0;
        float radius = 0;
        bool  valid = false;
    };

    struct Hand {
        bool  active = false;
        int   count  = 0;
        Joint joints[kMaxJoints];

        // XR_FB_hand_tracking_aim. The runtime's own pointing ray and pinch
        // detection, built to stay still while the fingers close. Computing the
        // ray from knuckle to fingertip looks correct until you pinch, at which
        // point the finger curls and takes the ray with it.
        bool  aimValid     = false;
        bool  aimPinching  = false;
        float pinchStrength = 0.0f;
        float aimX = 0, aimY = 0, aimZ = 0;      // ray origin
        float aimDX = 0, aimDY = 0, aimDZ = -1;  // unit direction
    };

    bool Init(XrInstance instance, XrSession session) {
#ifdef TAPSTUDY_NO_HAND_TRACKING
        (void)instance; (void)session;
        return false;
#else
        m_instance = instance;
        m_session  = session;

        if (xrGetInstanceProcAddr(instance, "xrCreateHandTrackerEXT",
                (PFN_xrVoidFunction *)&m_create) != XR_SUCCESS || !m_create) return false;
        if (xrGetInstanceProcAddr(instance, "xrDestroyHandTrackerEXT",
                (PFN_xrVoidFunction *)&m_destroy) != XR_SUCCESS || !m_destroy) return false;
        if (xrGetInstanceProcAddr(instance, "xrLocateHandJointsEXT",
                (PFN_xrVoidFunction *)&m_locate) != XR_SUCCESS || !m_locate) return false;

        for (int h = 0; h < 2; h++) {
            XrHandTrackerCreateInfoEXT ci{XR_TYPE_HAND_TRACKER_CREATE_INFO_EXT};
            ci.hand         = (h == 0) ? XR_HAND_LEFT_EXT : XR_HAND_RIGHT_EXT;
            ci.handJointSet = XR_HAND_JOINT_SET_DEFAULT_EXT;
            if (m_create(session, &ci, &m_tracker[h]) != XR_SUCCESS) {
                m_tracker[h] = XR_NULL_HANDLE;
                return false;
            }
        }
        m_available = true;
        return true;
#endif
    }

    // Ask the runtime to keep tracking hands while a controller is also present.
    // Without this, Quest switches to controllers-only the moment it sees one,
    // and the dialogs become unusable.
    bool EnableSimultaneous() {
#if defined(TAPSTUDY_NO_HAND_TRACKING) || !defined(XR_META_simultaneous_hands_and_controllers)
        return false;
#else
        PFN_xrResumeSimultaneousHandsAndControllersTrackingMETA resume = nullptr;
        if (xrGetInstanceProcAddr(m_instance,
                "xrResumeSimultaneousHandsAndControllersTrackingMETA",
                (PFN_xrVoidFunction *)&resume) != XR_SUCCESS || !resume) return false;

        XrSimultaneousHandsAndControllersTrackingResumeInfoMETA info{
            XR_TYPE_SIMULTANEOUS_HANDS_AND_CONTROLLERS_TRACKING_RESUME_INFO_META};
        m_simultaneous = (resume(m_session, &info) == XR_SUCCESS);
        return m_simultaneous;
#endif
    }

    void Update(XrSpace baseSpace, XrTime time) {
#ifndef TAPSTUDY_NO_HAND_TRACKING
        if (!m_available) return;
        for (int h = 0; h < 2; h++) {
            m_hand[h].active = false;
            if (m_tracker[h] == XR_NULL_HANDLE) continue;

            XrHandJointLocationEXT locs[XR_HAND_JOINT_COUNT_EXT]{};
            XrHandJointLocationsEXT out{XR_TYPE_HAND_JOINT_LOCATIONS_EXT};
            out.jointCount     = XR_HAND_JOINT_COUNT_EXT;
            out.jointLocations = locs;

#ifdef XR_FB_hand_tracking_aim
            XrHandTrackingAimStateFB aim{XR_TYPE_HAND_TRACKING_AIM_STATE_FB};
            out.next = &aim;
#endif

            XrHandJointsLocateInfoEXT li{XR_TYPE_HAND_JOINTS_LOCATE_INFO_EXT};
            li.baseSpace = baseSpace;
            li.time      = time;

            if (m_locate(m_tracker[h], &li, &out) != XR_SUCCESS) continue;
            if (out.isActive != XR_TRUE) continue;

            int n = (int)out.jointCount;
            if (n > kMaxJoints) n = kMaxJoints;
            m_hand[h].active     = true;
            m_hand[h].count      = n;
            m_hand[h].aimValid   = false;
            m_hand[h].aimPinching = false;

#ifdef XR_FB_hand_tracking_aim
            if ((aim.status & XR_HAND_TRACKING_AIM_VALID_BIT_FB) != 0) {
                const XrQuaternionf &q = aim.aimPose.orientation;
                // A pose's forward is -Z, so take the negated third column of
                // the rotation matrix.
                m_hand[h].aimDX = -2.0f * (q.x * q.z + q.w * q.y);
                m_hand[h].aimDY = -2.0f * (q.y * q.z - q.w * q.x);
                m_hand[h].aimDZ = -(1.0f - 2.0f * (q.x * q.x + q.y * q.y));
                m_hand[h].aimX  = aim.aimPose.position.x;
                m_hand[h].aimY  = aim.aimPose.position.y;
                m_hand[h].aimZ  = aim.aimPose.position.z;
                m_hand[h].pinchStrength = aim.pinchStrengthIndex;
                m_hand[h].aimPinching =
                    (aim.status & XR_HAND_TRACKING_AIM_INDEX_PINCHING_BIT_FB) != 0;
                // A system gesture (palm up, menu) must not read as a click.
                if ((aim.status & XR_HAND_TRACKING_AIM_SYSTEM_GESTURE_BIT_FB) != 0)
                    m_hand[h].aimPinching = false;
                m_hand[h].aimValid = true;
            }
#endif
            for (int j = 0; j < n; j++) {
                const bool ok =
                    (locs[j].locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT) != 0;
                m_hand[h].joints[j].valid  = ok;
                m_hand[h].joints[j].x      = locs[j].pose.position.x;
                m_hand[h].joints[j].y      = locs[j].pose.position.y;
                m_hand[h].joints[j].z      = locs[j].pose.position.z;
                m_hand[h].joints[j].radius = locs[j].radius;
            }
        }
#else
        (void)baseSpace; (void)time;
#endif
    }

    void Destroy() {
#ifndef TAPSTUDY_NO_HAND_TRACKING
        if (m_destroy) {
            for (int h = 0; h < 2; h++) {
                if (m_tracker[h] != XR_NULL_HANDLE) m_destroy(m_tracker[h]);
                m_tracker[h] = XR_NULL_HANDLE;
            }
        }
#endif
        m_available = false;
    }

    bool        Available()        const { return m_available; }
    // True when the runtime is supplying the aim ray rather than this code
    // deriving one from joint positions.
    bool        AimAvailable()     const {
        return m_hand[0].aimValid || m_hand[1].aimValid;
    }
    bool        Simultaneous()     const { return m_simultaneous; }
    const Hand &GetHand(int i)     const { return m_hand[i]; }

private:
    bool m_available    = false;
    bool m_simultaneous = false;
    Hand m_hand[2];

#ifndef TAPSTUDY_NO_HAND_TRACKING
    XrInstance m_instance = XR_NULL_HANDLE;
    XrSession  m_session  = XR_NULL_HANDLE;
    XrHandTrackerEXT m_tracker[2] = {XR_NULL_HANDLE, XR_NULL_HANDLE};
    PFN_xrCreateHandTrackerEXT  m_create  = nullptr;
    PFN_xrDestroyHandTrackerEXT m_destroy = nullptr;
    PFN_xrLocateHandJointsEXT   m_locate  = nullptr;
#endif
};
