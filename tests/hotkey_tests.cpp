#include "../src/hotkeys.hpp"
#include <cassert>
#include <iostream>
using teletype_hotkeys::Action;
using teletype_hotkeys::match;
int main() {
    // Telegram's normal focus navigation must remain untouched.
    assert(match(9, false, false, false, false, false, true, false) == Action::None);
    assert(match(9, false, false, true, false, false, true, false) == Action::None);
    // Correction and acceptance use distinct, explicit modifier combinations.
    assert(match('G', true, true, false, false, false, false, false) == Action::FixDraft);
    assert(match('J', true, true, false, false, false, true, false) == Action::AcceptSuggestion);
    assert(match('J', true, true, false, false, false, false, false) == Action::None);
    assert(match('J', true, true, false, false, false, true, true) == Action::None);
    // No accidental capture of shortcuts with extra modifiers or AltGr input.
    for (auto key : {'G', 'J'}) {
        assert(match(key, true, true, true, false, false, true, false) == Action::None);
        assert(match(key, true, true, false, true, false, true, false) == Action::None);
        assert(match(key, true, true, false, false, true, true, false) == Action::None);
        assert(match(key, true, false, false, false, false, true, false) == Action::None);
    }
    std::cout << "Hotkey tests passed\n";
}
