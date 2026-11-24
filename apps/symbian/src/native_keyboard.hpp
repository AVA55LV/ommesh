#ifndef OMMESH_BELLE_NATIVE_KEYBOARD_HPP
#define OMMESH_BELLE_NATIVE_KEYBOARD_HPP

class QWidget;

namespace NativeKeyboard {

constexpr char FullScreenPreference[] = "ui/fullScreenKeyboard";

bool fullScreenPreferred();
bool setSplitMode(QWidget* focusedEditor, bool split);
int splitViewBottom();

} // namespace NativeKeyboard

#endif
