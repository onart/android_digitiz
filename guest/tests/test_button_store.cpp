#include <doctest/doctest.h>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

#include "buttons/ButtonStore.hpp"

using namespace digitiz::guest;

namespace {

// A directory of its own per test, so one leaving a file behind cannot change
// what another one sees.
class TempDir {
public:
    explicit TempDir(const char* name)
        : path_(std::filesystem::temp_directory_path() / ("digitiz_test_" + std::string(name))) {
        std::filesystem::remove_all(path_);
        std::filesystem::create_directories(path_);
    }
    ~TempDir() { std::filesystem::remove_all(path_); }

    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;

    std::string str() const { return path_.string(); }
    std::filesystem::path file() const { return path_ / kButtonsFileName; }

private:
    std::filesystem::path path_;
};

CustomButton point(std::string label, int x, int y) {
    CustomButton b;
    b.kind = ButtonKind::Point;
    b.label = std::move(label);
    b.target = digitiz::core::Recti{x, y, 0, 0};
    return b;
}

std::string read_file(const std::filesystem::path& p) {
    std::ifstream in(p);
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

std::vector<std::string> labels_of(const ButtonStore& store) {
    std::vector<std::string> out;
    for (const CustomButton& b : store.buttons()) {
        out.push_back(b.label);
    }
    return out;
}

} // namespace

TEST_CASE("a button survives a round trip through the file") {
    const TempDir dir("roundtrip");

    {
        ButtonStore store;
        store.load(dir.str().c_str());
        CustomButton region;
        region.kind = ButtonKind::Region;
        region.label = "toolbar";
        region.target = digitiz::core::Recti{1600, -40, 320, 900};
        store.add(region);

        CustomButton shortcut;
        shortcut.kind = ButtonKind::Shortcut;
        shortcut.label = "save";
        shortcut.modifiers = 0x03;
        shortcut.key = "s";
        store.add(shortcut);
    }

    ButtonStore reloaded;
    reloaded.load(dir.str().c_str());
    REQUIRE(reloaded.size() == 2);

    CHECK(reloaded.buttons()[0].kind == ButtonKind::Region);
    CHECK(reloaded.buttons()[0].label == "toolbar");
    // Negative coordinates are the normal case on a multi-monitor desktop, so
    // they have to survive the text format.
    CHECK(reloaded.buttons()[0].target.y == -40);
    CHECK(reloaded.buttons()[0].target.w == 320);

    CHECK(reloaded.buttons()[1].kind == ButtonKind::Shortcut);
    CHECK(reloaded.buttons()[1].modifiers == 0x03);
    CHECK(reloaded.buttons()[1].key == "s");
}

TEST_CASE("a tab in a label would split the line, so it never reaches the file") {
    const TempDir dir("sanitize");

    ButtonStore store;
    store.load(dir.str().c_str());
    store.add(point("two\twords\nhere", 10, 20));

    CHECK(store.buttons()[0].label == "twowordshere");

    ButtonStore reloaded;
    reloaded.load(dir.str().c_str());
    REQUIRE(reloaded.size() == 1);
    CHECK(reloaded.buttons()[0].label == "twowordshere");
    CHECK(reloaded.buttons()[0].target.x == 10);
}

TEST_CASE("a malformed line is skipped rather than taking the file down with it") {
    const TempDir dir("malformed");
    {
        std::ofstream out(dir.file());
        out << "# comment\n";
        out << "0\tgood\t10\t20\t0\t0\t0\t\n";
        out << "not a button at all\n";
        out << "9\tunknown kind\t1\t2\t3\t4\t0\t\n";
        out << "\n";
        out << "2\tsave\t0\t0\t0\t0\t1\ts\n";
    }

    ButtonStore store;
    store.load(dir.str().c_str());
    REQUIRE(store.size() == 2);
    CHECK(store.buttons()[0].label == "good");
    CHECK(store.buttons()[1].label == "save");
}

TEST_CASE("editing replaces in place and keeps the order") {
    const TempDir dir("replace");
    ButtonStore store;
    store.load(dir.str().c_str());
    store.add(point("a", 1, 1));
    store.add(point("b", 2, 2));
    store.add(point("c", 3, 3));

    store.replace(1, point("B", 9, 9));
    CHECK(labels_of(store) == std::vector<std::string>{"a", "B", "c"});
    CHECK(store.buttons()[1].target.x == 9);

    // Out of range is ignored, not clamped onto a neighbour.
    store.replace(7, point("nope", 0, 0));
    store.replace(-1, point("nope", 0, 0));
    CHECK(labels_of(store) == std::vector<std::string>{"a", "B", "c"});
}

TEST_CASE("moving a button clamps at the ends instead of wrapping") {
    const TempDir dir("move");
    ButtonStore store;
    store.load(dir.str().c_str());
    store.add(point("a", 0, 0));
    store.add(point("b", 0, 0));
    store.add(point("c", 0, 0));

    store.move(2, -1);
    CHECK(labels_of(store) == std::vector<std::string>{"a", "c", "b"});

    // Already first: moving earlier is a no-op, not a jump to the end. The
    // menu offers the command whatever position the button is in.
    store.move(0, -1);
    CHECK(labels_of(store) == std::vector<std::string>{"a", "c", "b"});

    store.move(2, 5);
    CHECK(labels_of(store) == std::vector<std::string>{"a", "c", "b"});

    store.move(0, 2);
    CHECK(labels_of(store) == std::vector<std::string>{"c", "b", "a"});
}

TEST_CASE("removing writes the file back immediately") {
    const TempDir dir("remove");
    ButtonStore store;
    store.load(dir.str().c_str());
    store.add(point("keep", 1, 2));
    store.add(point("drop", 3, 4));

    store.remove(1);
    CHECK(store.size() == 1);

    // Every mutation persists on the spot: the app can be killed at any time
    // and there is no later moment to save in.
    const std::string text = read_file(dir.file());
    CHECK(text.find("keep") != std::string::npos);
    CHECK(text.find("drop") == std::string::npos);

    store.remove(4); // out of range
    CHECK(store.size() == 1);
}

TEST_CASE("with nowhere to save, buttons still work for this run") {
    ButtonStore store;
    store.load(nullptr);
    store.add(point("volatile", 5, 6));
    CHECK(store.size() == 1);
    CHECK(store.buttons()[0].label == "volatile");
}
