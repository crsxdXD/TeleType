#pragma once

// Pure key-matching logic shared by the Windows input hook and portable tests.
// VK values for letter keys are ASCII uppercase; no Win32 dependency is needed.
namespace teletype_hotkeys {

enum class Action { None, FixDraft, AcceptSuggestion };

inline Action match(unsigned int vk, bool ctrl, bool alt, bool shift,
                    bool win, bool rightAlt, bool hasSuggestion, bool popupError) {
    // Right Alt is AltGr on many European keyboards, and Windows often reports
    // it as Ctrl+Alt. Never intercept language input as a shortcut.
    if (!ctrl || !alt || shift || win || rightAlt) return Action::None;
    if (vk == 'G') return Action::FixDraft;
    if (vk == 'J' && hasSuggestion && !popupError) return Action::AcceptSuggestion;
    return Action::None;
}

} // namespace teletype_hotkeys
