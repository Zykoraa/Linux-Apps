// betterbanana GUI - main window, input strips, bus section, tape deck.
// All Q_OBJECT classes live in this header; the Makefile runs moc on it.
#pragma once

#include "../common/protocol.h"
#include "widgets.h"
#include "knob.h"

#include <QMainWindow>
#include <QDialog>
#include <QWidget>
#include <QString>
#include <QStringList>
#include <QVector>
#include <QSet>

class QComboBox;
class QSlider;
class QLabel;
class QPushButton;
class QLineEdit;
class QSpinBox;
class QCheckBox;
class QTimer;
class QGridLayout;

// One input strip: 3 hardware, then VAIO and AUX.
class StripWidget : public QWidget {
    Q_OBJECT
public:
    StripWidget(bb::Shared* shm, int index, bool hardware, const QString& title,
                QWidget* parent = nullptr);

    void refreshMeters();
    void setDeviceList(const QStringList& ids, const QStringList& labels);
    void setDeviceValue(const QString& id);
    QString deviceValue() const;
    void pullFromShm();

signals:
    void routingChanged(int hwIndex, const QString& nodeName);

private:
    Knob* addKnob(QGridLayout* g, int col, const QString& name,
                  int lo, int hi, int def, bool bipolar);

    bb::Shared* m_shm;
    int   m_index;
    bool  m_hardware;

    QComboBox*   m_device = nullptr;
    Knob*        m_gate   = nullptr;
    Knob*        m_comp   = nullptr;
    Knob*        m_aud    = nullptr;
    Knob*        m_eqLo   = nullptr;
    Knob*        m_eqMid  = nullptr;
    Knob*        m_eqHi   = nullptr;
    XYPad*       m_pan    = nullptr;
    QPushButton* m_mono   = nullptr;
    QPushButton* m_solo   = nullptr;
    QPushButton* m_mute   = nullptr;
    Fader*       m_fader  = nullptr;
    QLabel*      m_gainLbl = nullptr;
    LevelMeter*   m_meter  = nullptr;
    QLabel*       m_header = nullptr;
    ReductionBar* m_gateGr = nullptr;
    ReductionBar* m_compGr = nullptr;
    ReductionBar* m_duckGr = nullptr;
    QVector<QPushButton*> m_busBtns;
};

// One output bus: A1..A3 drive hardware, B1/B2 are virtual sources.
class BusWidget : public QWidget {
    Q_OBJECT
public:
    BusWidget(bb::Shared* shm, int index, bool hardware, const QString& title,
              QWidget* parent = nullptr);

    void refreshMeters();
    void setDeviceList(const QStringList& ids, const QStringList& labels);
    void setDeviceValue(const QString& id);
    QString deviceValue() const;
    void pullFromShm();

signals:
    void routingChanged(int busIndex, const QString& nodeName);
    void eqEditRequested(int busIndex);

private:
    bb::Shared* m_shm;
    int   m_index;
    bool  m_hardware;

    QComboBox*   m_device = nullptr;
    QPushButton* m_eq     = nullptr;
    QPushButton* m_mono   = nullptr;
    QPushButton* m_mute   = nullptr;
    Fader*       m_fader  = nullptr;
    QLabel*      m_gainLbl = nullptr;
    LevelMeter*  m_meter  = nullptr;
    QLabel*      m_header = nullptr;
};

// The tape deck: records a bus to WAV and plays a WAV back into the matrix.
class RecorderWidget : public QWidget {
    Q_OBJECT
public:
    explicit RecorderWidget(bb::Shared* shm, QWidget* parent = nullptr);
    void refresh();

private:
    void sendCmd(int cmd);
    void writePaths();

    bb::Shared* m_shm;
    QLineEdit*   m_recPath  = nullptr;
    QLineEdit*   m_playPath = nullptr;
    QComboBox*   m_srcBus   = nullptr;
    QPushButton* m_rec      = nullptr;
    QPushButton* m_play     = nullptr;
    QPushButton* m_stop     = nullptr;
    QPushButton* m_loop     = nullptr;
    QLabel*      m_status   = nullptr;
    Knob*        m_gain     = nullptr;
    QVector<QPushButton*> m_busBtns;
};

// 8 in / 8 out VBAN streams, matching Banana's VBAN dialog.
class VbanDialog : public QDialog {
    Q_OBJECT
public:
    explicit VbanDialog(bb::Shared* shm, QWidget* parent = nullptr);

private:
    void apply();

    bb::Shared* m_shm;
    struct OutRow { QCheckBox* on; QLineEdit* name; QLineEdit* host; QSpinBox* port; QComboBox* bus; };
    struct InRow  { QCheckBox* on; QLineEdit* name; QSpinBox* port; };
    OutRow m_out[bb::kVbanStreams];
    InRow  m_in [bb::kVbanStreams];
};

// Sidechain ducker: key strips pull down every strip that has a duck depth.
class DuckDialog : public QDialog {
    Q_OBJECT
public:
    explicit DuckDialog(bb::Shared* shm, QWidget* parent = nullptr);
private slots:
    void refresh();
private:
    bb::Shared* m_shm;
    QPushButton* m_on = nullptr;
    QLabel*      m_env = nullptr;
    QTimer*      m_timer = nullptr;
};

// Per-application routing: move a playing app onto a BetterBanana strip, or
// point a recording app at a BetterBanana bus. WirePlumber remembers the choice,
// so it survives the app restarting.
class AppsDialog : public QDialog {
    Q_OBJECT
public:
    explicit AppsDialog(QWidget* parent = nullptr);

private slots:
    void refresh();

private:
    struct Row {
        int          index = -1;
        bool         playback = true;
        QString      app;                       // key the saved rule is stored under
        QLabel*      name = nullptr;
        QComboBox*   target = nullptr;
        QString      extra;                     // placeholder shown when the
                                                // live target is not in the list
        class QPushButton* forget = nullptr;    // shown only while a rule exists
        QWidget*     holder = nullptr;
    };
    void rebuild(bool playback, const QVector<struct StreamInfo>& streams,
                 const QStringList& devIds, const QStringList& devLabels);
    void rebuildRemembered();

    class QVBoxLayout* m_playLay = nullptr;
    class QVBoxLayout* m_capLay  = nullptr;
    class QVBoxLayout* m_memLay  = nullptr;
    QLabel* m_playEmpty = nullptr;
    QLabel* m_capEmpty  = nullptr;
    QLabel* m_memEmpty  = nullptr;
    QVector<Row> m_rows;
    QVector<QWidget*> m_memRows;
    QStringList m_memShown;
    QTimer* m_timer = nullptr;
};

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(bb::Shared* shm, QWidget* parent = nullptr);

    void openVbanDialog();
    void openAppsDialog();
    void openBusEq(int bus);
    void openDuckDialog();

private slots:
    void tick();
    void refreshDevices();
    void applyTheme(int index);
    void refreshAutostart();

private:
    void writeRouting();
    void applyDeviceEq(int bus, const QString& device);
    void openMicAnalyzer(const QString& source, const QString& label);
    void populateAnalyzerMenu(class QMenu* menu);
    void readRouting();
    void applyAppRules();
    void buildMenus();

    bb::Shared* m_shm;
    QVector<StripWidget*> m_strips;
    QVector<BusWidget*>   m_buses;
    RecorderWidget*       m_recorder = nullptr;
    QTimer*  m_timer = nullptr;
    QLabel*  m_status = nullptr;
    uint32_t m_lastHeartbeat = 0;
    int      m_stallTicks = 0;
    int      m_syncTicks = 0;
    QVector<QAction*> m_themeActions;
    QAction* m_autoEngine = nullptr;
    QAction* m_autoGui = nullptr;
    AppsDialog* m_apps = nullptr;
    QSet<int>   m_ruledStreams;
    int         m_ruleTicks = 0;

    QString  m_hwIn[bb::kHwStrips];
    QString  m_busOut[bb::kPhysBuses];
};
