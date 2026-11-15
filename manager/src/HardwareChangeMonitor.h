#pragma once

#include <QAbstractNativeEventFilter>
#include <QObject>
#include <QTimer>

namespace Ucal::Manager {

// Platform notifications are only refresh hints. Device identity and presence
// continue to come from backend plugins, never from a vendor-specific PnP list.
class HardwareChangeMonitor final : public QObject, public QAbstractNativeEventFilter {
    Q_OBJECT
public:
    explicit HardwareChangeMonitor(QObject* parent = nullptr,
                                   int debounceMs = 500, int settleMs = 2000);
    ~HardwareChangeMonitor() override;
    bool nativeEventFilter(const QByteArray& eventType, void* message, long* result) override;
    quint64 generation() const { return generation_; }
    bool isDebouncing() const { return debounce_.isActive(); }

public slots:
    void observeChange();

signals:
    void changeObserved();
    void refreshRequested();

private:
    QTimer debounce_;
    QTimer settle_;
    quint64 generation_ = 0;
};

}  // namespace Ucal::Manager
