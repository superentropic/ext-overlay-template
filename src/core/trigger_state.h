#pragma once

namespace TriggerState {

inline bool IsTriggerActive(int triggerKey, bool requireKey, bool keyHeld) {
    // Valid key-gated semantics:
    // - no key requirement => always active
    // - key required but no valid key configured => never active
    // - key required and configured => key must be held
    if (!requireKey) {
        return true;
    }
    if (triggerKey == 0) {
        return false;
    }
    return keyHeld;
}

} // namespace TriggerState
