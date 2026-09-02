// betterbanana GUI - the pieces every dialog shares.
//
// There are six dialogs and there used to be six conventions: one opened with a
// profile toolbar, two with a paragraph of prose, two with a group box; four
// different button-row shapes; not one of them called setContentsMargins; and
// none put its subject in the window, so which strip you were editing lived
// only in the title bar - which on a tiling compositor is never drawn.
//
// Deliberately Q_OBJECT-free so the Makefile needs no extra moc rule.
#pragma once

#include <QString>

class QBoxLayout;
class QDialog;
class QLabel;
class QPushButton;
class QWidget;

namespace bbdlg {

// Standard margins and spacing for a dialog's outermost layout.
void chrome(QBoxLayout* root);

// A title plate: the dialog's subject in display type, an optional subtitle
// under it, and a rule beneath. Returns the widget so a caller can insert it.
QWidget* header(const QString& title, const QString& subtitle = QString());

// A right-aligned button row with the platform's ordering. `primary` is styled
// as the call to action; every button gets autoDefault cleared, so Return
// commits the field you are typing in rather than dismissing the dialog.
QBoxLayout* buttonRow(QPushButton* primary, QPushButton* close);

// Clears autoDefault/default on every button in the dialog. Call at the end of
// a constructor: Qt hands "default" to the first button it finds, which is how
// Return in an EQ frequency field came to close the editor.
void tameDefaults(QDialog* d, QPushButton* keepDefault = nullptr);

// A status line that says something and then stops saying it. Every dialog had
// its own and none of them ever cleared.
class StatusStrip {
public:
    explicit StatusStrip(QWidget* parent);
    QLabel* widget() const { return m_label; }
    void say(const QString& text, int ms = 4000);
    void clear();

private:
    QLabel* m_label;
};

// Remember a dialog's size and position under `key`. Restores on construction,
// saves when it closes.
void rememberGeometry(QDialog* d, const QString& key);

} // namespace bbdlg
