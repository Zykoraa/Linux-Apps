// betterbanana GUI - custom-painted controls that follow the active theme.
#pragma once

#include <QWidget>
#include <QString>
#include <functional>

// Rotary knob with a 270-degree sweep. Values are integers (tenths of a unit,
// as elsewhere in the GUI); `bipolar` fills the arc outward from the default,
// which is what you want for EQ cut/boost.
class Knob : public QWidget {
    Q_OBJECT
public:
    Knob(int lo, int hi, int def, bool bipolar, QString suffix, QWidget* parent = nullptr);

    int  value() const { return m_value; }
    void setValue(int v);
    void setDecimals(int d)     { m_decimals = d; update(); }
    // Overrides the printed value, for non-linear scales such as EQ frequency.
    void setFormatter(std::function<QString(int)> f) { m_fmt = std::move(f); update(); }
    void setScale(double s)     { m_scale = s;    update(); }
    // A second arc outside the value arc, 0..1, for gain reduction the engine
    // already measures. Drawn only when > 0.
    void setReduction(float r);
    bool isDragging() const     { return m_dragging; }

    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;

signals:
    void valueChanged(int v);

protected:
    void paintEvent(QPaintEvent*) override;
    void mousePressEvent(QMouseEvent*) override;
    void mouseMoveEvent(QMouseEvent*) override;
    void mouseReleaseEvent(QMouseEvent*) override;
    void mouseDoubleClickEvent(QMouseEvent*) override;
    void wheelEvent(QWheelEvent*) override;
    void keyPressEvent(QKeyEvent*) override;
    void enterEvent(QEnterEvent*) override;
    void leaveEvent(QEvent*) override;

private:
    QString shownText() const;
    int     stepFor(bool fine) const;
    void    showValueTip();

    int     m_lo, m_hi, m_def, m_value;
    bool    m_bipolar;
    QString m_suffix;
    int     m_decimals = 1;
    double  m_scale = 0.1;      // display value = raw * scale
    float   m_reduction = 0.0f;
    std::function<QString(int)> m_fmt;
    bool    m_dragging = false;
    bool    m_hover = false;
    int     m_dragStartY = 0, m_dragStartVal = 0;
};

// Mixer fader. Linear in dB, with a marked unity position, a labelled dB scale,
// and double-click to snap back to 0 dB.
class Fader : public QWidget {
    Q_OBJECT
public:
    Fader(int lo, int hi, int def, QWidget* parent = nullptr);

    int  value() const { return m_value; }
    void setValue(int v);
    bool isDragging() const { return m_dragging; }

    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;

signals:
    void valueChanged(int v);

protected:
    void paintEvent(QPaintEvent*) override;
    void mousePressEvent(QMouseEvent*) override;
    void mouseMoveEvent(QMouseEvent*) override;
    void mouseReleaseEvent(QMouseEvent*) override;
    void mouseDoubleClickEvent(QMouseEvent*) override;
    void wheelEvent(QWheelEvent*) override;
    void keyPressEvent(QKeyEvent*) override;
    void enterEvent(QEnterEvent*) override;
    void leaveEvent(QEvent*) override;

private:
    QRectF grooveRect() const;
    QRectF capRect() const;
    double valueToY(int v) const;
    int    yToValue(double y) const;
    void   showValueTip();

    int  m_lo, m_hi, m_def, m_value;
    bool m_dragging = false;
    bool m_hover = false;
    bool m_fine = false;
    int  m_dragStartY = 0, m_dragStartVal = 0;
};

// Voicemeeter's Intellipan. Only X does anything: the engine reads pan_x and
// has never read pan_y, so the Y axis is pinned rather than offering feedback
// for an operation that does not exist.
class XYPad : public QWidget {
    Q_OBJECT
public:
    explicit XYPad(QWidget* parent = nullptr);

    int  xValue() const { return m_x; }
    int  yValue() const { return m_y; }
    void setValues(int x, int y);
    bool isDragging() const { return m_dragging; }

    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;

signals:
    void valuesChanged(int x, int y);

protected:
    void paintEvent(QPaintEvent*) override;
    void mousePressEvent(QMouseEvent*) override;
    void mouseMoveEvent(QMouseEvent*) override;
    void mouseReleaseEvent(QMouseEvent*) override;
    void mouseDoubleClickEvent(QMouseEvent*) override;
    void keyPressEvent(QKeyEvent*) override;
    void enterEvent(QEnterEvent*) override;
    void leaveEvent(QEvent*) override;

private:
    void applyPos(const QPoint& p);
    int  m_x = 0, m_y = 0;      // -100 .. 100
    bool m_dragging = false;
    bool m_hover = false;
};
