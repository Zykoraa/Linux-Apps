// betterbanana GUI - the bus EQ editor and the AutoEq headphone browser.
//
// Split out of mainwindow.h because it grew its own world: an interactive
// curve, a numeric band table, named profiles on disk, and a search over the
// AutoEq measurement database. Everything here edits one bus's BusParams
// directly; the engine picks changes up on its next block.
#pragma once

#include "../common/protocol.h"

#include <QDialog>
#include <QString>
#include <QVector>
#include <QWidget>

class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QLabel;
class QLineEdit;
class QListWidget;
class QNetworkAccessManager;
class QPushButton;

namespace bb { struct EqProfile; }

// Interactive magnitude plot of one bus's EQ. Each enabled band gets a numbered
// handle: drag it to move frequency and gain, wheel over it to change Q, right
// click to bypass it. The curve is drawn with the engine's own Biquad, so what
// is on screen is what is being applied.
class EqCurve : public QWidget {
    Q_OBJECT
public:
    EqCurve(bb::Shared* shm, int bus, QWidget* parent = nullptr);

    void setSelected(int band);
    int  selected() const { return m_sel; }

signals:
    void bandEdited(int band);      // dragged or wheeled: values already written
    void bandSelected(int band);
    void bandToggled(int band);     // right-clicked: on/off already written

protected:
    void paintEvent(QPaintEvent*) override;
    void mousePressEvent(QMouseEvent*) override;
    void mouseMoveEvent(QMouseEvent*) override;
    void mouseReleaseEvent(QMouseEvent*) override;
    void mouseDoubleClickEvent(QMouseEvent*) override;
    void wheelEvent(QWheelEvent*) override;
    void leaveEvent(QEvent*) override;

private:
    QRectF plotRect() const;
    double xForFreq(double f) const;
    double freqForX(double x) const;
    double yForDb(double db) const;
    double dbForY(double y) const;
    QPointF handlePos(int band) const;
    int  bandAt(const QPoint& p) const;

    bb::Shared* m_shm;
    int m_bus;
    int m_sel   = 0;
    int m_drag  = -1;
    int m_hover = -1;
};

// The full editor for one bus: profile bar, curve, twelve band rows, preamp.
class BusEqDialog : public QDialog {
    Q_OBJECT
public:
    BusEqDialog(bb::Shared* shm, int bus, QWidget* parent = nullptr);

private slots:
    void onProfileChosen(int index);
    void saveProfile();
    void deleteProfile();
    void importProfile();
    void exportProfile();
    void openAutoEq();
    void autoPreamp();
    void flatten();

private:
    struct BandRow {
        QWidget*        holder = nullptr;
        QCheckBox*      on     = nullptr;
        QComboBox*      type   = nullptr;
        QDoubleSpinBox* freq   = nullptr;
        QDoubleSpinBox* gain   = nullptr;
        QDoubleSpinBox* q      = nullptr;
        QLabel*         num    = nullptr;
    };

    void buildProfileCombo(const QString& select = QString());
    void applyProfile(const bb::EqProfile& prof, const QString& label);
    void pullFromShm();            // shm -> widgets, without re-entering them
    void pushBand(int k);          // widgets -> shm, for one band
    void refreshRowEnables(int k);
    void highlight(int band);

    bb::Shared* m_shm;
    int         m_bus;
    bool        m_updating = false;

    EqCurve*     m_curve   = nullptr;
    QComboBox*   m_profile = nullptr;
    QPushButton* m_delete  = nullptr;
    QPushButton* m_eqOn    = nullptr;
    QDoubleSpinBox* m_preamp = nullptr;
    QLabel*      m_note     = nullptr;
    QVector<BandRow> m_rows;
};

// Search the AutoEq database and apply a measured headphone profile to a bus.
// The index is cached on disk, so searching is instant and works offline once
// it has been fetched; only applying a profile needs the network.
class AutoEqDialog : public QDialog {
    Q_OBJECT
public:
    AutoEqDialog(bb::Shared* shm, int bus, QWidget* parent = nullptr);

    // Name of the profile that was applied, empty if none was.
    QString applied() const { return m_applied; }

    // Re-applies the profile remembered for `device` onto `bus`, if there is
    // one. Returns the profile name, or an empty string. Used by the mixer when
    // a bus is pointed at a different pair of headphones.
    static QString applyRemembered(bb::Shared* shm, int bus, const QString& device);

private slots:
    void refreshIndex();
    void applyFilter();
    void applySelected();

private:
    struct Entry {
        QString name;      // "Sennheiser HD 650"
        QString path;      // "oratory1990/over-ear/Sennheiser%20HD%20650"
        QString source;    // "oratory1990"
        QString rig;       // "GRAS 43AG-7", often empty
    };

    bool loadIndex(QString* err = nullptr);
    void setStatus(const QString& text, bool busy = false);

    bb::Shared* m_shm;
    int         m_bus;
    QString     m_applied;

    QLineEdit*   m_search  = nullptr;
    QListWidget* m_list    = nullptr;
    QLabel*      m_status  = nullptr;
    QCheckBox*   m_remember = nullptr;
    QPushButton* m_apply   = nullptr;
    QPushButton* m_refresh = nullptr;
    QNetworkAccessManager* m_net = nullptr;
    QVector<Entry> m_all;
    QString m_device;      // node name of the sink this bus drives, if any
};
