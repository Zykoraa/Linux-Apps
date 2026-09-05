// betterbanana GUI - main window, input strips, bus section, tape deck.
// All Q_OBJECT classes live in this header; the Makefile runs moc on it.
#pragma once

#include "../common/protocol.h"
#include "widgets.h"

#include <QElapsedTimer>
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
    // True when the assigned device is named by the state but not present.
    bool deviceMissing() const { return m_missing; }
    // Card state: something else is soloed; the engine has stopped; the strip
    // has a device named but nothing attached to it.
    void setDimmed(bool d);
    void setLive(bool live);
    void setAttached(bool a);
    void setTravel(int px);
    bool isHardware() const { return m_hardware; }
    int  meterTop() const;
    int  leadPad() const;
    void setLeadPad(int px);
    void refreshBusTips(const QStringList& names);

signals:
    void routingChanged(int hwIndex, const QString& nodeName);
    void eqEditRequested(int stripIndex);
    void fxEditRequested(int stripIndex);
    void statusMessage(const QString& text);

private:
    Knob* addKnob(QGridLayout* g, int col, const QString& name,
                  int lo, int hi, int def, bool bipolar);
    void deviceMenu(const QPoint& pos);

    bb::Shared* m_shm;
    int   m_index;
    bool  m_hardware;

    bool  m_missing = false;
    bool  m_dimmed = false;
    bool  m_attached = true;

    QComboBox*   m_device = nullptr;
    QPushButton* m_eqBtn  = nullptr;
    QPushButton* m_fxBtn  = nullptr;
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
    class QSpacerItem* m_lead = nullptr;
    class QVBoxLayout* m_root = nullptr;
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
    bool deviceMissing() const { return m_missing; }
    void setLive(bool live);
    // Every meter in the console shares one baseline, so the same dBFS is at
    // the same height whichever column you look at. A bus card carries fewer
    // rows than a strip, so it is padded to match rather than floated.
    int  meterTop() const;
    int  leadPad() const;
    void setLeadPad(int px);
    void setTravel(int px);

signals:
    void routingChanged(int busIndex, const QString& nodeName);
    void eqEditRequested(int busIndex);
    void statusMessage(const QString& text);

private:
    bb::Shared* m_shm;
    int   m_index;
    bool  m_hardware;
    bool  m_missing = false;

    QComboBox*   m_device = nullptr;
    QComboBox*   m_mode   = nullptr;   // A buses only; B1/B2 are always stereo
    QPushButton* m_eq     = nullptr;
    QPushButton* m_mono   = nullptr;
    QPushButton* m_mute   = nullptr;
    Fader*       m_fader  = nullptr;
    QLabel*      m_gainLbl = nullptr;
    QLabel*      m_lufs   = nullptr;   // short-term loudness, integrated in the tip
    LevelMeter*  m_meter  = nullptr;
    QLabel*      m_header = nullptr;
    EqThumb*     m_thumb  = nullptr;
    class QSpacerItem* m_lead = nullptr;
    class QVBoxLayout* m_root = nullptr;
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
    QLabel*      m_time     = nullptr;
    class QProgressBar* m_progress = nullptr;
    LevelMeter*  m_meter    = nullptr;
    QElapsedTimer m_pulse;
    Knob*        m_gain     = nullptr;
    QVector<QPushButton*> m_busBtns;
};

// 8 in / 8 out VBAN streams, matching Banana's VBAN dialog.
class VbanDialog : public QDialog {
    Q_OBJECT
public:
    explicit VbanDialog(bb::Shared* shm, QWidget* parent = nullptr);

protected:
    // Escape used to discard silently while "Close" wrote network configuration
    // into shared memory. Now Escape and Cancel both put back what was there.
    void reject() override;

private:
    void apply();
    bool revert();          // true when it actually had to put something back

    bb::Shared* m_shm;
    // Snapshotted by value in the constructor. Deliberately not a memcpy of
    // VbanConfig: that holds an atomic seq the engine is reading.
    bb::VbanOutCfg m_out0[bb::kVbanStreams];
    bb::VbanInCfg  m_in0 [bb::kVbanStreams];
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

// What the mixer can check about its own setup.
//
// Every check behind this dialog is a mistake that has actually been made on a
// real machine and taken hours to find, because none of them look like a fault:
// the mixer keeps running, the meters keep moving, and the audio simply goes
// somewhere else. They are all cheap to test and impossible to guess at.
class MainWindow;

// Time alignment. Two output devices almost never have the same latency, and
// PipeWire already knows the figure for each of them - for a Bluetooth sink it
// is the codec and link delay, which nothing else can see - so this reads them
// and offers to hold the early ones back.
class AlignDialog : public QDialog {
    Q_OBJECT
public:
    explicit AlignDialog(bb::Shared* shm, QWidget* parent = nullptr);

private slots:
    void refresh();

private:
    void alignOutputs();

    bb::Shared* m_shm;
    struct Row {
        QLabel* dev = nullptr;
        QLabel* lat = nullptr;
        class QDoubleSpinBox* delay = nullptr;
        QCheckBox* inc = nullptr;   // outputs only: include this one in an align
        QString    lastDev;         // so a device CHANGE can reset the tick
    };
    Row m_in [bb::kHwStrips];
    Row m_out[bb::kPhysBuses];
    QLabel* m_note = nullptr;
    QTimer* m_timer = nullptr;
};

class DiagnoseDialog : public QDialog {
    Q_OBJECT
public:
    DiagnoseDialog(bb::Shared* shm, MainWindow* owner, QWidget* parent = nullptr);

public slots:
    void recheck();

private:
    bb::Shared* m_shm;
    MainWindow* m_owner;
    class QVBoxLayout* m_list = nullptr;
    QLabel* m_summary = nullptr;
};

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(bb::Shared* shm, QWidget* parent = nullptr);

    void openVbanDialog();
    void openAppsDialog();
    void openBusEq(int bus);
    void openStripEq(int strip);
    void openStripFx(int strip);
    void openDuckDialog();
    void openDiagnoseDialog();
    void openAlignDialog();
    // Public because a diagnostic finding offers it as its fix.
    void restartEngine();

private slots:
    void tick();
    void refreshDevices();
    void applyTheme(int index);
    void refreshAutostart();
    void offerRecoveredMix();
    void undo();
    void redo();

protected:
    void closeEvent(class QCloseEvent* e) override;

private:
    void writeRouting();
    void applyDeviceEq(int bus, const QString& device);
    void applyDeviceStrip(int strip, const QString& device);
    // Undo works by watching the shared state rather than by instrumenting
    // every control, so anything that moves the mixer - including bb-ctl - is
    // undoable, and one gesture is one step.
    void snapshotTick();
    void applyState(const QByteArray& text);
    void refreshUndoActions();
    void reportMissingDevices();
    void populateStartupMenu(class QMenu* menu);
    void openMicAnalyzer(const QString& source, const QString& label);
    void populateAnalyzerMenu(class QMenu* menu);
    void readRouting();
    void applyAppRules();
    // `complete` is false when any of the five reads failed: the set of
    // already-handled streams is then left alone rather than pruned.
    void applyAppRulesWith(const QString& sinksJson, const QString& sinkInputs,
                           const QString& sinkShort, const QString& sourceOutputs,
                           const QString& sourceShort, bool complete);
    void buildMenus();
    // Loading, restarting and undo all share one idea: the mixer really did
    // change, and it now matches the thing it was changed to.
    void commitAsSettled();
    bool loadPresetFile(const QString& path);
    void rebuildPresetBar();
    void setPresetBarVisible(bool on);
    void savePresetAs();
    void presetMenu(QWidget* anchor, const QString& name, const QString& path,
                    const QPoint& pos);
    // The title carries the loaded preset and whether it has drifted, since on
    // a tiling compositor the title bar may be the only place either shows.
    void refreshTitle();
    void restoreWindowGeometry();
    void saveWindowGeometry();
    void refreshCardStates();
    void say(const QString& text, int ms = 4000);
    void showAbout();

    bb::Shared* m_shm;
    QVector<StripWidget*> m_strips;
    QVector<BusWidget*>   m_buses;
    RecorderWidget*       m_recorder = nullptr;
    QTimer*  m_timer = nullptr;
    QLabel*  m_status = nullptr;
    uint32_t m_lastHeartbeat = 0;
    int      m_stallTicks = 0;
    int      m_syncTicks = 0;
    int      m_stateTicks = 0;
    int      m_travel = 0;
    class QScrollArea* m_scroll = nullptr;
    QWidget* m_central = nullptr;
    int      m_consoleChrome = 0;   // a strip card minus its fader, measured once
    QVector<QAction*> m_themeActions;
    QAction* m_autoEngine = nullptr;
    QAction* m_autoGui = nullptr;
    QAction* m_restartAct = nullptr;   // says "Start" when nothing is running
    QAction* m_presetBarAct = nullptr;

    // The preset bar: one lit button per saved preset, Ctrl+1..9 to match.
    QWidget*                m_presetBar   = nullptr;
    class QHBoxLayout*      m_presetLay   = nullptr;
    class QFileSystemWatcher* m_presetWatch = nullptr;
    QStringList             m_presetOrder;   // what Ctrl+1..9 point at
    AppsDialog* m_apps = nullptr;
    DiagnoseDialog* m_diag = nullptr;
    AlignDialog* m_align = nullptr;
    QSet<int>   m_ruledStreams;
    bool        m_ruleBusy = false;
    int         m_ruleWait = 0;
    uint32_t    m_ruleRound = 0;
    int         m_ruleTicks = 0;

    QString  m_presetName;        // "" until a preset is loaded or saved
    bool     m_dirty = false;
    bool     m_engineLive = true;
    // Set while restartEngine() is bouncing the engine: the heartbeat is
    // expected to stop, so the alarm and the undo recorder both stand down.
    bool     m_restarting = false;
    QLabel*  m_alert = nullptr;   // the banner across the top when it is not
    QString  m_statusColour;      // so a theme change repaints it and a tick does not

    // An engine that restarts on its own - a PipeWire restart, the watchdog, a
    // crash - comes back holding the startup preset, which silently discards
    // whatever was actually set up. The pid is what gives it away, and the last
    // settled state is what can be offered back.
    int        m_enginePid = 0;
    QByteArray m_recovered;
    class QFrame* m_offer = nullptr;
    QLabel*       m_offerText = nullptr;

    QVector<QByteArray> m_undo, m_redo;
    QByteArray  m_committed;      // the last settled state
    QByteArray  m_seen;           // what the previous tick saw
    int         m_undoTicks = 0;
    QAction*    m_undoAct = nullptr;
    QAction*    m_redoAct = nullptr;

    QString  m_hwIn[bb::kHwStrips];
    QString  m_busOut[bb::kPhysBuses];
};
