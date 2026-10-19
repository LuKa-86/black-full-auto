// The port's settings as rows of the in-game PC SETTINGS pages: each row steps through its choices with left/right.
#pragma once

#include <string>
#include <vector>

namespace pcset {

// a page of rows (container entry: page in lower case). The first four are the PC SETTINGS menu's rows (the game's
// Options screen it is copied from has four); the others are opened from a link row on their parent's page.
struct Group {
    const char *page, *title;
    std::vector<int> rows;
    const char *parent = nullptr; // the page Back returns to (nullptr: the PC SETTINGS menu)
};
const std::vector<Group> &groups();
constexpr int kMenuGroups = 4;

int count();                        // rows; row n shows its value in &PC_V<n> (fe/vars.h)
const char *label(int n);           // the row's caption
void step(int n, int dir);          // left (-1) / right (+1): the next choice, applied and saved
std::wstring value(int n);          // the current choice as shown
void fill_page(int group);          // a group's page is up: its rows' values into the page's values (fe/vars.h)
void refresh_sliders();             // externObject.FEPCS<n>: every slider's position (0-100)
bool is_slider(int n);              // a bullet bar (26 positions) instead of a list of choices
int slider_pos(int n);              // the slider's position, 0-100 (also given to the page as externObject.FEPCS<n>)
void slide(int n, float pos);       // a slider moved to pos (0-100)
bool restart_only(int n);           // applies at the next start (aspect ratio, frame rate); shown on both menus
const char *link(int n);            // a row that opens a page (KEY BINDINGS, a sub-page), or nullptr

} // namespace pcset
