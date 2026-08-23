#pragma once

// Turns the key name a guest sends into this OS's key code.
//
// Names travel on the wire instead of codes because numbering is a property of
// the host OS. Declared away from the table so the parsing can be tested
// without dragging windows.h into the test.

#include <cstdint>
#include <string_view>

namespace digitiz::host {

// False when the name is not one we know. The caller reports that rather than
// guessing: pressing the wrong key on the user's PC is worse than pressing
// none.
bool key_name_to_code(std::string_view name, std::uint16_t& code);

} // namespace digitiz::host
