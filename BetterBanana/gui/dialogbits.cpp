#include "dialogbits.h"
#include "metrics.h"
#include "theme.h"

#include <QBoxLayout>
#include <QDialog>
#include <QEvent>
#include <QFrame>
#include <QPainter>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QSettings>
#include <QTimer>
#include <QVBoxLayout>

namespace {

class HeaderRule : public QWidget {
public:
    HeaderRule() { setFixedHeight(1); setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed); }
protected:
    void paintEvent(QPaintEvent*) override
    {
        QPainter p(this);
        p.fillRect(rect(), theme().border);
    }
};

} // namespace

namespace bbdlg {

void chrome(QBoxLayout* root)
{
    root->setContentsMargins(bbui::px(16), bbui::px(14), bbui::px(16), bbui::px(14));
    root->setSpacing(bbui::px(10));
}

QWidget* header(const QString& title, const QString& subtitle)
{
    auto* w = new QWidget;
    auto* v = new QVBoxLayout(w);
    v->setContentsMargins(0, 0, 0, 0);
    v->setSpacing(bbui::px(2));

    auto* t = new QLabel(title);
    t->setProperty("role", "display");
    v->addWidget(t);

    if (!subtitle.isEmpty()) {
        auto* s = new QLabel(subtitle);
        s->setProperty("role", "prose");
        s->setWordWrap(true);            // no hard-coded newlines: it reflows
        v->addWidget(s);
    }

    // A painted rule, not a styled one: a per-widget stylesheet would bake in
    // whichever theme happened to be live when the dialog was built, and
    // applyTheme's qApp->setStyleSheet() does not clear a widget's own sheet.
    v->addSpacing(bbui::px(4));
    v->addWidget(new HeaderRule);
    return w;
}

QBoxLayout* buttonRow(QPushButton* primary, QPushButton* close)
{
    auto* row = new QHBoxLayout;
    row->setSpacing(bbui::px(8));
    row->addStretch(1);
    if (primary) {
        primary->setProperty("cta", "primary");
        row->addWidget(primary);
    }
    if (close) row->addWidget(close);
    return row;
}

void tameDefaults(QDialog* d, QPushButton* keepDefault)
{
    for (QPushButton* b : d->findChildren<QPushButton*>()) {
        b->setAutoDefault(false);
        b->setDefault(false);
    }
    if (keepDefault) {
        keepDefault->setAutoDefault(true);
        keepDefault->setDefault(true);
    }
}

// ---------------------------------------------------------------------------
StatusStrip::StatusStrip(QWidget* parent)
    : m_label(new QLabel(parent))
{
    m_label->setProperty("role", "caption");
    m_label->setMinimumHeight(bbui::px(14));
}

void StatusStrip::say(const QString& text, int ms)
{
    m_label->setText(text);
    QLabel* l = m_label;
    const QString shown = text;
    QTimer::singleShot(ms, l, [l, shown] {
        if (l->text() == shown) l->clear();
    });
}

void StatusStrip::clear() { m_label->clear(); }

// ---------------------------------------------------------------------------
namespace {

// Saves on close without needing a subclass or a moc'd helper.
class GeometryKeeper : public QObject {
public:
    GeometryKeeper(QDialog* d, QString key) : QObject(d), m_d(d), m_key(std::move(key))
    {
        const QByteArray g = QSettings("betterbanana", "gui")
                                 .value("geometry/" + m_key).toByteArray();
        if (!g.isEmpty()) d->restoreGeometry(g);
        d->installEventFilter(this);
    }

protected:
    bool eventFilter(QObject* o, QEvent* e) override
    {
        if (o == m_d && (e->type() == QEvent::Close || e->type() == QEvent::Hide))
            QSettings("betterbanana", "gui").setValue("geometry/" + m_key,
                                                      m_d->saveGeometry());
        return false;
    }

private:
    QDialog* m_d;
    QString  m_key;
};

} // namespace

void rememberGeometry(QDialog* d, const QString& key) { new GeometryKeeper(d, key); }

} // namespace bbdlg
