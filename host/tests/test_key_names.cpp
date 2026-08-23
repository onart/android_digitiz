#include <doctest/doctest.h>

#include "input/KeyNames.hpp"

using namespace digitiz::host;

namespace {

std::uint16_t code_of(std::string_view name) {
    std::uint16_t code = 0;
    REQUIRE(key_name_to_code(name, code));
    return code;
}

bool known(std::string_view name) {
    std::uint16_t code = 0;
    return key_name_to_code(name, code);
}

} // namespace

TEST_CASE("letters and digits are their own virtual keys") {
    CHECK(code_of("a") == 'A');
    CHECK(code_of("z") == 'Z');
    CHECK(code_of("0") == '0');
    CHECK(code_of("9") == '9');
    // Uppercase is not what the guest sends, but accepting it costs nothing
    // and refusing it would be a silently dead shortcut.
    CHECK(code_of("A") == 'A');
}

TEST_CASE("function keys run the whole range") {
    CHECK(code_of("f1") == 0x70);
    CHECK(code_of("f12") == 0x7B);
    CHECK(code_of("f24") == 0x87);
    CHECK_FALSE(known("f0"));
    CHECK_FALSE(known("f25"));
}

TEST_CASE("numpad digits do not collide with the top row") {
    CHECK(code_of("numpad0") == 0x60);
    CHECK(code_of("numpad9") == 0x69);
    CHECK(code_of("0") != code_of("numpad0"));
}

TEST_CASE("named keys resolve, including the aliases") {
    CHECK(code_of("escape") == code_of("esc"));
    CHECK(code_of("enter") == code_of("return"));
    CHECK(code_of("delete") == code_of("del"));
    CHECK(known("pageup"));
    CHECK(known("printscreen"));
}

TEST_CASE("names that start like a prefix still reach the table") {
    // "numpadplus" begins with "numpad" and "f..." names begin with "f"; the
    // indexed parse has to decline rather than swallow them.
    CHECK(known("numpadplus"));
    CHECK(known("numpaddivide"));
}

TEST_CASE("an unknown name is refused rather than approximated") {
    CHECK_FALSE(known(""));
    CHECK_FALSE(known("ctrl"));
    CHECK_FALSE(known("f99"));
    CHECK_FALSE(known("numpad"));
    // "f" on its own is the letter, not a malformed function key.
    CHECK(code_of("f") == 'F');
    CHECK_FALSE(known("banana"));
    // No modifier parsing here: the wire carries those in their own field.
    CHECK_FALSE(known("ctrl+s"));
}
