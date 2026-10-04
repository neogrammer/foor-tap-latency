#pragma once

// OpenXR input for the foot-tap study.
//
// There are two controllers doing two different jobs:
//
//   FLOOR controller  sits on the ground. A foot (or a contraption on a foot)
//                     presses something on it. Its timestamp is the measurement.
//   HELD controller   stays in the participant's hand and drives the dialogs.
//
// The tap action is bound to nearly every button on both controllers on purpose.
// A foot does not aim well, so whatever gets pressed should count. The two roles
// are separated at query time by subaction path, not by binding, so which hand is
// on the floor can be switched at runtime without touching the bindings.
//
// IMPORTANT for timing: XrActionStateBoolean carries lastChangeTime, which is the
// runtime's own timestamp for the state change. It is usually finer than the frame
// interval. Always prefer it over the frame's predictedDisplayTime, and fall back
// only when it comes back as zero.

#include <vector>
#include <cstring>

class XRInput {
public:
    struct Button {
        bool   pressed      = false;  // current state
        bool   justPressed  = false;  // rising edge this frame
        XrTime changeTime   = 0;      // runtime timestamp of the change
    };

    bool Init(XrInstance instance, XrSession session) {
        m_instance = instance;
        m_session  = session;

        XrActionSetCreateInfo asci{XR_TYPE_ACTION_SET_CREATE_INFO};
        strncpy(asci.actionSetName,          "tapstudy", XR_MAX_ACTION_SET_NAME_SIZE);
        strncpy(asci.localizedActionSetName, "Tap Study", XR_MAX_LOCALIZED_ACTION_SET_NAME_SIZE);
        asci.priority = 0;
        if (xrCreateActionSet(instance, &asci, &m_actionSet) != XR_SUCCESS) return false;

        if (xrStringToPath(instance, "/user/hand/left",  &m_handPath[0]) != XR_SUCCESS) return false;
        if (xrStringToPath(instance, "/user/hand/right", &m_handPath[1]) != XR_SUCCESS) return false;

        if (!MakeBool("tap",     "Tap",     m_tapAction))     return false;
        if (!MakeBool("confirm", "Confirm", m_confirmAction)) return false;
        if (!MakeBool("nav_next","Next",    m_nextAction))    return false;
        if (!MakeBool("nav_prev","Previous",m_prevAction))    return false;

        if (!SuggestTouchBindings()) return false;

        XrSessionActionSetsAttachInfo attach{XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO};
        attach.countActionSets = 1;
        attach.actionSets      = &m_actionSet;
        if (xrAttachSessionActionSets(session, &attach) != XR_SUCCESS) return false;

        return true;
    }

    void Destroy() {
        if (m_tapAction)     xrDestroyAction(m_tapAction);
        if (m_confirmAction) xrDestroyAction(m_confirmAction);
        if (m_nextAction)    xrDestroyAction(m_nextAction);
        if (m_prevAction)    xrDestroyAction(m_prevAction);
        if (m_actionSet)     xrDestroyActionSet(m_actionSet);
        m_tapAction = m_confirmAction = m_nextAction = m_prevAction = XR_NULL_HANDLE;
        m_actionSet = XR_NULL_HANDLE;
    }

    // Call once per frame, after xrWaitFrame and before you use any state.
    void Sync(XrTime frameTime) {
        XrActiveActionSet active{};
        active.actionSet     = m_actionSet;
        active.subactionPath = XR_NULL_PATH;

        XrActionsSyncInfo sync{XR_TYPE_ACTIONS_SYNC_INFO};
        sync.countActiveActionSets = 1;
        sync.activeActionSets      = &active;
        if (xrSyncActions(m_session, &sync) != XR_SUCCESS) return;

        const int floorIdx = m_floorIsLeft ? 0 : 1;
        const int heldIdx  = m_floorIsLeft ? 1 : 0;

        Poll(m_tapAction,     m_handPath[floorIdx], m_tap,     frameTime);
        Poll(m_confirmAction, m_handPath[heldIdx],  m_confirm, frameTime);
        Poll(m_nextAction,    m_handPath[heldIdx],  m_next,    frameTime);
        Poll(m_prevAction,    m_handPath[heldIdx],  m_prev,    frameTime);

        // Diagnostic: is anything at all being pressed on either controller?
        Button a{}, b{};
        Poll(m_tapAction, m_handPath[0], a, frameTime);
        Poll(m_tapAction, m_handPath[1], b, frameTime);
        m_anyLeft  = a.pressed;
        m_anyRight = b.pressed;
    }

    // Which physical controller is on the floor. Flip it without rebuilding.
    void SetFloorHandLeft(bool isLeft) { m_floorIsLeft = isLeft; }
    bool FloorHandIsLeft() const       { return m_floorIsLeft; }

    const Button &Tap()     const { return m_tap; }
    const Button &Confirm() const { return m_confirm; }
    const Button &Next()    const { return m_next; }
    const Button &Prev()    const { return m_prev; }

    bool AnyLeftPressed()  const { return m_anyLeft; }
    bool AnyRightPressed() const { return m_anyRight; }

private:
    bool MakeBool(const char *name, const char *localized, XrAction &out) {
        XrActionCreateInfo aci{XR_TYPE_ACTION_CREATE_INFO};
        aci.actionType = XR_ACTION_TYPE_BOOLEAN_INPUT;
        strncpy(aci.actionName,          name,      XR_MAX_ACTION_NAME_SIZE);
        strncpy(aci.localizedActionName, localized, XR_MAX_LOCALIZED_ACTION_NAME_SIZE);
        aci.countSubactionPaths = 2;
        aci.subactionPaths      = m_handPath;
        return xrCreateAction(m_actionSet, &aci, &out) == XR_SUCCESS;
    }

    void Add(std::vector<XrActionSuggestedBinding> &v, XrAction a, const char *path) {
        XrPath p;
        if (xrStringToPath(m_instance, path, &p) != XR_SUCCESS) return;
        v.push_back({a, p});
    }

    bool SuggestTouchBindings() {
        std::vector<XrActionSuggestedBinding> b;

        // TAP: everything a foot might land on, both hands. A boolean action bound
        // to a float input is thresholded by the runtime, which is what we want.
        const char *tapPaths[] = {
            "/user/hand/left/input/squeeze/value",
            "/user/hand/left/input/trigger/value",
            "/user/hand/left/input/thumbstick/click",
            "/user/hand/left/input/x/click",
            "/user/hand/left/input/y/click",
            "/user/hand/right/input/squeeze/value",
            "/user/hand/right/input/trigger/value",
            "/user/hand/right/input/thumbstick/click",
            "/user/hand/right/input/a/click",
            "/user/hand/right/input/b/click",
        };
        for (const char *p : tapPaths) Add(b, m_tapAction, p);

        // CONFIRM: trigger on either hand. Filtered to the held one at query time.
        Add(b, m_confirmAction, "/user/hand/left/input/trigger/value");
        Add(b, m_confirmAction, "/user/hand/right/input/trigger/value");

        // Rating selection: face buttons step the highlight.
        Add(b, m_nextAction, "/user/hand/right/input/a/click");
        Add(b, m_nextAction, "/user/hand/left/input/x/click");
        Add(b, m_prevAction, "/user/hand/right/input/b/click");
        Add(b, m_prevAction, "/user/hand/left/input/y/click");

        XrPath profile;
        if (xrStringToPath(m_instance, "/interaction_profiles/oculus/touch_controller",
                           &profile) != XR_SUCCESS) return false;

        XrInteractionProfileSuggestedBinding sb{XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING};
        sb.interactionProfile     = profile;
        sb.countSuggestedBindings = static_cast<uint32_t>(b.size());
        sb.suggestedBindings      = b.data();
        return xrSuggestInteractionProfileBindings(m_instance, &sb) == XR_SUCCESS;
    }

    void Poll(XrAction action, XrPath sub, Button &out, XrTime frameTime) {
        XrActionStateGetInfo gi{XR_TYPE_ACTION_STATE_GET_INFO};
        gi.action        = action;
        gi.subactionPath = sub;

        XrActionStateBoolean st{XR_TYPE_ACTION_STATE_BOOLEAN};
        if (xrGetActionStateBoolean(m_session, &gi, &st) != XR_SUCCESS || !st.isActive) {
            out.justPressed = false;
            out.pressed     = false;
            return;
        }
        out.justPressed = (st.changedSinceLastSync == XR_TRUE) && (st.currentState == XR_TRUE);
        out.pressed     = (st.currentState == XR_TRUE);
        // lastChangeTime is the runtime's own timestamp and beats frame time.
        out.changeTime  = (st.lastChangeTime > 0) ? st.lastChangeTime : frameTime;
    }

    XrInstance  m_instance   = XR_NULL_HANDLE;
    XrSession   m_session    = XR_NULL_HANDLE;
    XrActionSet m_actionSet  = XR_NULL_HANDLE;
    XrAction    m_tapAction     = XR_NULL_HANDLE;
    XrAction    m_confirmAction = XR_NULL_HANDLE;
    XrAction    m_nextAction    = XR_NULL_HANDLE;
    XrAction    m_prevAction    = XR_NULL_HANDLE;
    XrPath      m_handPath[2]{};

    Button m_tap, m_confirm, m_next, m_prev;
    bool   m_floorIsLeft = true;
    bool   m_anyLeft = false, m_anyRight = false;
};
