#pragma once
// Decisions the application auto-routing rules make, kept free of the window
// so they can be tested without a PipeWire session.
#include <QJsonObject>
#include <QString>

namespace bb {

// A sink that another application runs in order to capture what plays into it:
// a recorder, OBS, or eveamp's Spotify bridge (pw-record standing in as an
// Audio/Sink). A stream sitting on one was put there by that application on
// purpose. A rule that pulled it off would fight the application, which moves
// it straight back - every new stream would bounce between the two, and play
// twice for a moment each time. BetterBanana's own sinks have exactly the same
// shape, so they are told apart by name and owner.
inline bool isAppCaptureSink(const QJsonObject& props)
{
    const QString name = props.value("node.name").toString();
    return !name.isEmpty() && !name.startsWith("bb_")
        && props.value("media.class").toString() == "Audio/Sink"
        && props.value("media.category").toString() == "Capture"
        && props.value("application.name").toString() != "BetterBanana";
}

} // namespace bb
