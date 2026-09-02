// betterbanana GUI - the parametric EQ editor and the AutoEq headphone browser.
//
// Split out of mainwindow.h because it grew its own world: an interactive
// curve over a live spectrum, a numeric band table, named profiles on disk,
// undo, and a search over the AutoEq measurement database. Everything here
// edits one EqParams block directly - a bus's or an input strip's, they are the
// same struct - and the engine picks changes up on its next block.
#pragma once

#include "../common/protocol.h"
#include "../common/eqprofile.h"
#include "dialogbits.h"

#include <QByteArray>
#include <QDialog>
#include <QElapsedTimer>
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
class QNetworkReply;
class QPainter;
class QProgressBar;
class QPushButton;
class QScrollArea;
class QShowEvent;
class QSplitter;
class QTimer;
class QUrl;

// Interactive magnitude plot of one EQ block, drawn over the engine's live
// spectrum of the signal it sits in. Each enabled band gets a numbered handle:
// drag it to move frequency and gain, wheel over it to change Q, right click to
// bypass it. The curve is drawn with the engine's own Biquad, so what is on
// screen is what is being applied.
class EqCurve : public QWidget {
    Q_OBJECT
public:
    // `specSource` is what the engine should analyse behind the curve, or
    // bb::kSpecNone to draw no spectrum at all.
    EqCurve(bb::Shared* shm, bb::EqParams* eq, int specSource, QWidget* parent = nullptr);

    void setSelected(int band);
    int  selected() const { return m_sel; }

    // True once the engine has published bins for our source.
    bool spectrumLive() const;

signals:
    void editStarted(int band);     // a gesture is about to change something
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
    QRectF cardRect() const;        // the whole widget, as a plate
    QRectF plotRect() const;        // the well the curve is drawn in
    QRectF readoutRect() const;     // the strip above it
    double xForFreq(double f) const;
    double freqForX(double x) const;
    double yForDb(double db) const;
    double dbForY(double y) const;
    double yForSpec(double dbfs) const;
    void   drawSpectrum(QPainter& p, const QRectF& r) const;
    void   drawGrid(QPainter& p, const QRectF& r) const;
    void   drawReadout(QPainter& p) const;
    void   fanHandles() const;
    QPointF handlePos(int band) const;   // where the band's values put it
    QPointF drawPos(int band) const;     // where it is drawn, after fanning
    int  bandAt(const QPoint& p) const;

    bb::Shared*   m_shm;
    bb::EqParams* m_eq;
    int m_spec  = bb::kSpecNone;
    int m_sel   = 0;
    int m_drag  = -1;
    int m_hover = -1;

    // The dB range the frame spans, chosen to fit whatever is drawn. It used to
    // be a fixed 18 while a band reaches 24 and the preamp another 24 below
    // that, so any real correction flattened against the frame with nothing to
    // say it had been truncated.
    double m_range = 18.0;

    // Peak hold over the engine's spectrum, and the clock its fall is paced by.
    // Mutable because drawSpectrum is const, and the alternative is to update
    // the trace in paintEvent - two call frames from the code that reads it.
    mutable float m_hold[bb::kSpecBins];
    mutable QElapsedTimer m_holdClock;

    // Vertical offsets that pull coincident handles apart. The Telephone preset
    // puts two bands on 300 Hz and two on 3400 Hz, and the second of each pair
    // was drawn exactly under the first, so it could never be clicked.
    mutable double m_fan[bb::kEqBands];
};

// The full editor for one EQ block: profile bar, curve, twelve band rows,
// preamp, undo.
class EqEditorDialog : public QDialog {
    Q_OBJECT
public:
    // `bus` is the bus index when this edits a bus, or -1 for an input strip.
    // Only a bus gets the headphone-correction browser and the per-device
    // memory that goes with it.
    EqEditorDialog(bb::Shared* shm, bb::EqParams* eq, int specSource,
                   const QString& title, int bus, QWidget* parent = nullptr);
    ~EqEditorDialog() override;

protected:
    void showEvent(QShowEvent*) override;

private slots:
    void onProfileChosen(int index);
    void saveProfile();
    void deleteProfile();
    void importProfile();
    void exportProfile();
    void openAutoEq();
    void autoPreamp();
    void flatten();
    void undo();

private:
    struct BandRow {
        QWidget*        holder = nullptr;   // the row's own plate, behind it
        QCheckBox*      on     = nullptr;
        QComboBox*      type   = nullptr;
        QDoubleSpinBox* freq   = nullptr;
        QDoubleSpinBox* gain   = nullptr;
        QDoubleSpinBox* q      = nullptr;
        QLabel*         num    = nullptr;
        // What was last handed to each setStyleSheet. A drag re-pulls the whole
        // block on every mouse move, and re-stating an unchanged sheet
        // re-polishes the widget for nothing on the thread that also has to
        // keep the curve at 20 fps.
        QString         plateCss, chipCss;
    };

    // Records the state to come back to. `tag` is a band index, or -1 for a
    // whole-block change; successive touches of the same band inside half a
    // second are one gesture, not thirty edits.
    void snapshot(int tag);

    void buildProfileCombo(const QString& select = QString());
    void applyProfile(const bb::EqProfile& prof, const QString& label);
    void pullFromShm();            // shm -> widgets, without re-entering them
    void pushBand(int k);          // widgets -> shm, for one band
    void refreshRowEnables(int k);
    void restyleRows();            // zebra, selection plate and band chips
    void highlight(int band);

    bb::Shared*   m_shm;
    bb::EqParams* m_eq;
    int           m_bus;           // -1 when this edits a strip
    int           m_spec;
    QString       m_title;
    bool          m_updating = false;
    bool          m_tabular  = false;   // feature tags re-stated after polish
    int           m_selRow   = 0;

    QVector<bb::EqSnapshot> m_undo;
    uint32_t      m_specSeen = 0;
    int           m_lastTag = -2;
    QElapsedTimer m_since;

    EqCurve*     m_curve   = nullptr;
    QSplitter*   m_split   = nullptr;
    QScrollArea* m_table   = nullptr;
    QComboBox*   m_profile = nullptr;
    QPushButton* m_delete  = nullptr;
    QPushButton* m_undoBtn = nullptr;
    QPushButton* m_eqOn    = nullptr;
    QDoubleSpinBox* m_preamp = nullptr;
    bbdlg::StatusStrip m_note;
    QTimer*      m_repaint  = nullptr;
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

    // One blocking GET with a live progress bar and a working Cancel. Returns
    // an empty array and leaves the status line explaining itself on failure.
    QByteArray fetch(const QUrl& url, const QString& what);

    bb::Shared* m_shm;
    int         m_bus;
    QString     m_applied;

    QLineEdit*   m_search  = nullptr;
    QListWidget* m_list    = nullptr;
    QLabel*      m_status  = nullptr;
    QProgressBar* m_busy   = nullptr;
    QCheckBox*   m_remember = nullptr;
    QPushButton* m_apply   = nullptr;
    QPushButton* m_refresh = nullptr;
    QNetworkAccessManager* m_net = nullptr;
    QNetworkReply* m_reply = nullptr;   // non-null only while a GET is in flight
    QVector<Entry> m_all;
    QString m_device;      // node name of the sink this bus drives, if any
};
