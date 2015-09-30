/*
 * Copyright (C) 2013-2015 Canonical, Ltd.
 *
 * This program is free software: you can redistribute it and/or modify it under
 * the terms of the GNU Lesser General Public License version 3, as published by
 * the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranties of MERCHANTABILITY,
 * SATISFACTORY QUALITY, or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

// local
#include "application.h"
#include "session.h"
#include "mirsurfaceitem.h"
#include "logging.h"
#include "ubuntukeyboardinfo.h"
#include "tracepoints.h" // generated from tracepoints.tp
#include "timestamp.h"

// common
#include <debughelpers.h>

// Qt
#include <QDebug>
#include <QGuiApplication>
#include <QMutexLocker>
#include <QQmlEngine>
#include <QQuickWindow>
#include <QScreen>
#include <private/qsgdefaultimagenode_p.h>
#include <QTimer>
#include <QSGTextureProvider>

#include <QRunnable>

namespace qtmir {

namespace {

class MirSurfaceItemReleaseResourcesJob : public QRunnable
{
public:
    MirSurfaceItemReleaseResourcesJob() : textureProvider(nullptr) {}
    void run() {
        delete textureProvider;
        textureProvider = nullptr;
    }
    QObject *textureProvider;
};

} // namespace {

class MirTextureProvider : public QSGTextureProvider
{
    Q_OBJECT
public:
    MirTextureProvider(QSharedPointer<QSGTexture> texture) : t(texture) {}
    QSGTexture *texture() const {
        if (t)
            t->setFiltering(smooth ? QSGTexture::Linear : QSGTexture::Nearest);
        return t.data();
    }

    bool smooth;

    void releaseTexture() {
        t.reset();
    }

    void setTexture(QSharedPointer<QSGTexture> newTexture) {
        t = newTexture;
    }

private:
    QSharedPointer<QSGTexture> t;
};

MirSurfaceItem::MirSurfaceItem(QQuickItem *parent)
    : MirSurfaceItemInterface(parent)
    , m_surface(nullptr)
    , m_textureProvider(nullptr)
    , m_lastTouchEvent(nullptr)
    , m_lastFrameNumberRendered(nullptr)
    , m_surfaceWidth(0)
    , m_surfaceHeight(0)
    , m_orientationAngle(nullptr)
    , m_consumesInput(false)
{
    qCDebug(QTMIR_SURFACES) << "MirSurfaceItem::MirSurfaceItem";

    setSmooth(true);
    setFlag(QQuickItem::ItemHasContents, true); //so scene graph will render this item

    if (!UbuntuKeyboardInfo::instance()) {
        new UbuntuKeyboardInfo;
    }

    m_updateMirSurfaceSizeTimer.setSingleShot(true);
    m_updateMirSurfaceSizeTimer.setInterval(1);
    connect(&m_updateMirSurfaceSizeTimer, &QTimer::timeout, this, &MirSurfaceItem::updateMirSurfaceSize);

    connect(this, &QQuickItem::activeFocusChanged, this, &MirSurfaceItem::updateMirSurfaceFocus);
}

MirSurfaceItem::~MirSurfaceItem()
{
    qCDebug(QTMIR_SURFACES) << "MirSurfaceItem::~MirSurfaceItem - this=" << this;

    setSurface(nullptr);

    delete m_lastTouchEvent;
    delete m_lastFrameNumberRendered;
    delete m_orientationAngle;
    delete m_textureProvider;
}

Mir::Type MirSurfaceItem::type() const
{
    if (m_surface) {
        return m_surface->type();
    } else {
        return Mir::UnknownType;
    }
}

Mir::OrientationAngle MirSurfaceItem::orientationAngle() const
{
    if (m_orientationAngle) {
        Q_ASSERT(!m_surface);
        return *m_orientationAngle;
    } else if (m_surface) {
        return m_surface->orientationAngle();
    } else {
        return Mir::Angle0;
    }
}

void MirSurfaceItem::setOrientationAngle(Mir::OrientationAngle angle)
{
    qCDebug(QTMIR_SURFACES, "MirSurfaceItem::setOrientationAngle(%d)", angle);

    if (m_surface) {
        Q_ASSERT(!m_orientationAngle);
        m_surface->setOrientationAngle(angle);
    } else if (!m_orientationAngle) {
        m_orientationAngle = new Mir::OrientationAngle;
        *m_orientationAngle = angle;
        Q_EMIT orientationAngleChanged(angle);
    } else if (*m_orientationAngle != angle) {
        *m_orientationAngle = angle;
        Q_EMIT orientationAngleChanged(angle);
    }
}

QString MirSurfaceItem::name() const
{
    if (m_surface) {
        return m_surface->name();
    } else {
        return QString();
    }
}

bool MirSurfaceItem::live() const
{
    return m_surface && m_surface->live();
}

// Called from the rendering (scene graph) thread
QSGTextureProvider *MirSurfaceItem::textureProvider() const
{
    QMutexLocker mutexLocker(const_cast<QMutex*>(&m_mutex));
    const_cast<MirSurfaceItem *>(this)->ensureTextureProvider();
    return m_textureProvider;
}

void MirSurfaceItem::ensureTextureProvider()
{
    if (!m_surface) {
        return;
    }

    if (!m_textureProvider) {
        m_textureProvider = new MirTextureProvider(m_surface->texture());
    } else if (!m_textureProvider->texture()) {
        m_textureProvider->setTexture(m_surface->texture());
    }
}

QSGNode *MirSurfaceItem::updatePaintNode(QSGNode *oldNode, UpdatePaintNodeData *)    // called by render thread
{
    QMutexLocker mutexLocker(&m_mutex);

    if (!m_surface) {
        delete oldNode;
        return 0;
    }

    ensureTextureProvider();

    m_surface->updateTexture();

    if (m_surface->numBuffersReadyForCompositor() > 0) {
        QTimer::singleShot(0, this, SLOT(update()));
    }

    if (!m_textureProvider->texture()) {
        delete oldNode;
        return 0;
    }

    m_textureProvider->smooth = smooth();

    QSGDefaultImageNode *node = static_cast<QSGDefaultImageNode*>(oldNode);
    if (!node) {
        node = new QSGDefaultImageNode;
        node->setTexture(m_textureProvider->texture());

        node->setMipmapFiltering(QSGTexture::None);
        node->setHorizontalWrapMode(QSGTexture::ClampToEdge);
        node->setVerticalWrapMode(QSGTexture::ClampToEdge);
        node->setSubSourceRect(QRectF(0, 0, 1, 1));
    } else {
        if (!m_lastFrameNumberRendered  || (*m_lastFrameNumberRendered != m_surface->currentFrameNumber())) {
            node->markDirty(QSGNode::DirtyMaterial);
        }
    }

    node->setTargetRect(QRectF(0, 0, width(), height()));
    node->setInnerTargetRect(QRectF(0, 0, width(), height()));

    node->setFiltering(smooth() ? QSGTexture::Linear : QSGTexture::Nearest);
    node->setAntialiasing(antialiasing());

    node->update();

    if (!m_lastFrameNumberRendered) {
        m_lastFrameNumberRendered = new unsigned int;
    }
    *m_lastFrameNumberRendered = m_surface->currentFrameNumber();

    return node;
}

void MirSurfaceItem::mousePressEvent(QMouseEvent *event)
{
    if (m_consumesInput && m_surface && m_surface->live()) {
        if (type() == Mir::InputMethodType) {
            // FIXME: Hack to get the VKB use case working while we don't have the proper solution in place.
            if (isMouseInsideUbuntuKeyboard(event)) {
                m_surface->mousePressEvent(event);
            } else {
                event->ignore();
            }
        } else {
            m_surface->mousePressEvent(event);
        }
    } else {
        event->ignore();
    }
}

void MirSurfaceItem::mouseMoveEvent(QMouseEvent *event)
{
    if (m_consumesInput && m_surface && m_surface->live()) {
        m_surface->mouseMoveEvent(event);
    } else {
        event->ignore();
    }
}

void MirSurfaceItem::mouseReleaseEvent(QMouseEvent *event)
{
    if (m_consumesInput && m_surface && m_surface->live()) {
        m_surface->mouseReleaseEvent(event);
    } else {
        event->ignore();
    }
}

void MirSurfaceItem::wheelEvent(QWheelEvent *event)
{
    Q_UNUSED(event);
}

void MirSurfaceItem::hoverEnterEvent(QHoverEvent *event)
{
    if (m_consumesInput && m_surface && m_surface->live()) {
        m_surface->hoverEnterEvent(event);
    } else {
        event->ignore();
    }
}

void MirSurfaceItem::hoverLeaveEvent(QHoverEvent *event)
{
    if (m_consumesInput && m_surface && m_surface->live()) {
        m_surface->hoverLeaveEvent(event);
    } else {
        event->ignore();
    }
}

void MirSurfaceItem::hoverMoveEvent(QHoverEvent *event)
{
    if (m_consumesInput && m_surface && m_surface->live()) {
        m_surface->hoverMoveEvent(event);
    } else {
        event->ignore();
    }
}

void MirSurfaceItem::keyPressEvent(QKeyEvent *event)
{
    if (m_consumesInput && m_surface && m_surface->live()) {
        m_surface->keyPressEvent(event);
    } else {
        event->ignore();
    }
}

void MirSurfaceItem::keyReleaseEvent(QKeyEvent *event)
{
    if (m_consumesInput && m_surface && m_surface->live()) {
        m_surface->keyReleaseEvent(event);
    } else {
        event->ignore();
    }
}

QString MirSurfaceItem::appId() const
{
    if (m_surface) {
        return m_surface->appId();
    } else {
        return QString("-");
    }
}

void MirSurfaceItem::endCurrentTouchSequence(ulong timestamp)
{
    Q_ASSERT(m_lastTouchEvent);
    Q_ASSERT(m_lastTouchEvent->type != QEvent::TouchEnd);
    Q_ASSERT(m_lastTouchEvent->touchPoints.count() > 0);

    TouchEvent touchEvent = *m_lastTouchEvent;
    touchEvent.timestamp = timestamp;

    // Remove all already released touch points
    int i = 0;
    while (i < touchEvent.touchPoints.count()) {
        if (touchEvent.touchPoints[i].state() == Qt::TouchPointReleased) {
            touchEvent.touchPoints.removeAt(i);
        } else {
            ++i;
        }
    }

    // And release the others one by one as Mir expects one press/release per event
    while (touchEvent.touchPoints.count() > 0) {
        touchEvent.touchPoints[0].setState(Qt::TouchPointReleased);

        touchEvent.updateTouchPointStatesAndType();

        m_surface->touchEvent(touchEvent.modifiers, touchEvent.touchPoints,
                               touchEvent.touchPointStates, touchEvent.timestamp);

        *m_lastTouchEvent = touchEvent;

        touchEvent.touchPoints.removeAt(0);
    }
}

void MirSurfaceItem::validateAndDeliverTouchEvent(int eventType,
            ulong timestamp,
            Qt::KeyboardModifiers mods,
            const QList<QTouchEvent::TouchPoint> &touchPoints,
            Qt::TouchPointStates touchPointStates)
{
    if (eventType == QEvent::TouchBegin && m_lastTouchEvent && m_lastTouchEvent->type != QEvent::TouchEnd) {
        qCWarning(QTMIR_SURFACES) << qPrintable(QString("MirSurfaceItem(%1) - Got a QEvent::TouchBegin while "
            "there's still an active/unfinished touch sequence.").arg(appId()));
        // Qt forgot to end the last touch sequence. Let's do it ourselves.
        endCurrentTouchSequence(timestamp);
    }

    m_surface->touchEvent(mods, touchPoints, touchPointStates, timestamp);

    if (!m_lastTouchEvent) {
        m_lastTouchEvent = new TouchEvent;
    }
    m_lastTouchEvent->type = eventType;
    m_lastTouchEvent->timestamp = timestamp;
    m_lastTouchEvent->touchPoints = touchPoints;
    m_lastTouchEvent->touchPointStates = touchPointStates;

    tracepoint(qtmir, touchEventConsume_end, uncompressTimestamp<ulong>(timestamp).count());
}

void MirSurfaceItem::touchEvent(QTouchEvent *event)
{
    tracepoint(qtmir, touchEventConsume_start, uncompressTimestamp<ulong>(event->timestamp()).count());

    bool accepted = processTouchEvent(event->type(),
            event->timestamp(),
            event->modifiers(),
            event->touchPoints(),
            event->touchPointStates());
    event->setAccepted(accepted);
}

bool MirSurfaceItem::processTouchEvent(
        int eventType,
        ulong timestamp,
        Qt::KeyboardModifiers mods,
        const QList<QTouchEvent::TouchPoint> &touchPoints,
        Qt::TouchPointStates touchPointStates)
{

    if (!m_consumesInput || !m_surface || !m_surface->live()) {
        return false;
    }

    bool accepted = true;
    if (type() == Mir::InputMethodType && eventType == QEvent::TouchBegin) {
        // FIXME: Hack to get the VKB use case working while we don't have the proper solution in place.
        if (hasTouchInsideUbuntuKeyboard(touchPoints)) {
            validateAndDeliverTouchEvent(eventType, timestamp, mods, touchPoints, touchPointStates);
        } else {
            accepted = false;
        }

    } else {
        // NB: If we are getting QEvent::TouchUpdate or QEvent::TouchEnd it's because we've
        // previously accepted the corresponding QEvent::TouchBegin
        validateAndDeliverTouchEvent(eventType, timestamp, mods, touchPoints, touchPointStates);
    }
    return accepted;
}

bool MirSurfaceItem::hasTouchInsideUbuntuKeyboard(const QList<QTouchEvent::TouchPoint> &touchPoints)
{
    UbuntuKeyboardInfo *ubuntuKeyboardInfo = UbuntuKeyboardInfo::instance();

    for (int i = 0; i < touchPoints.count(); ++i) {
        QPoint pos = touchPoints.at(i).pos().toPoint();
        if (pos.x() >= ubuntuKeyboardInfo->x()
                && pos.x() <= (ubuntuKeyboardInfo->x() + ubuntuKeyboardInfo->width())
                && pos.y() >= ubuntuKeyboardInfo->y()
                && pos.y() <= (ubuntuKeyboardInfo->y() + ubuntuKeyboardInfo->height())) {
            return true;
        }
    }
    return false;
}

bool MirSurfaceItem::isMouseInsideUbuntuKeyboard(const QMouseEvent *event)
{
    UbuntuKeyboardInfo *ubuntuKeyboardInfo = UbuntuKeyboardInfo::instance();

    const QPointF &pos = event->localPos();

    return pos.x() >= ubuntuKeyboardInfo->x()
        && pos.x() <= (ubuntuKeyboardInfo->x() + ubuntuKeyboardInfo->width())
        && pos.y() >= ubuntuKeyboardInfo->y()
        && pos.y() <= (ubuntuKeyboardInfo->y() + ubuntuKeyboardInfo->height());
}

Mir::State MirSurfaceItem::surfaceState() const
{
    if (m_surface) {
        return m_surface->state();
    } else {
        return Mir::UnknownState;
    }
}

void MirSurfaceItem::setSurfaceState(Mir::State state)
{
    if (m_surface) {
        m_surface->setState(state);
    }
}

void MirSurfaceItem::scheduleMirSurfaceSizeUpdate()
{
    if (!m_updateMirSurfaceSizeTimer.isActive()) {
        m_updateMirSurfaceSizeTimer.start();
    }
}

void MirSurfaceItem::updateMirSurfaceSize()
{
    if (!m_surface || !m_surface->live() || (m_surfaceWidth <= 0 && m_surfaceHeight <= 0)) {
        return;
    }

    // If one dimension is not set, fallback to the current value
    int width = m_surfaceWidth > 0 ? m_surfaceWidth : m_surface->size().width();
    int height = m_surfaceHeight > 0 ? m_surfaceHeight : m_surface->size().height();

    m_surface->resize(width, height);
}

void MirSurfaceItem::updateMirSurfaceFocus(bool focused)
{
    if (m_surface && m_consumesInput && m_surface->live()) {
        m_surface->setFocus(focused);
    }
}

void MirSurfaceItem::invalidateSceneGraph()
{
    delete m_textureProvider;
    m_textureProvider = nullptr;
}

void MirSurfaceItem::TouchEvent::updateTouchPointStatesAndType()
{
    touchPointStates = 0;
    for (int i = 0; i < touchPoints.count(); ++i) {
        touchPointStates |= touchPoints.at(i).state();
    }

    if (touchPointStates == Qt::TouchPointReleased) {
        type = QEvent::TouchEnd;
    } else if (touchPointStates == Qt::TouchPointPressed) {
        type = QEvent::TouchBegin;
    } else {
        type = QEvent::TouchUpdate;
    }
}

bool MirSurfaceItem::consumesInput() const
{
    return m_consumesInput;
}

void MirSurfaceItem::setConsumesInput(bool value)
{
    if (m_consumesInput == value) {
        return;
    }

    m_consumesInput = value;
    if (m_consumesInput) {
        setAcceptedMouseButtons(Qt::LeftButton | Qt::MiddleButton | Qt::RightButton |
            Qt::ExtraButton1 | Qt::ExtraButton2 | Qt::ExtraButton3 | Qt::ExtraButton4 |
            Qt::ExtraButton5 | Qt::ExtraButton6 | Qt::ExtraButton7 | Qt::ExtraButton8 |
            Qt::ExtraButton9 | Qt::ExtraButton10 | Qt::ExtraButton11 |
            Qt::ExtraButton12 | Qt::ExtraButton13);
        setAcceptHoverEvents(true);
    } else {
        setAcceptedMouseButtons(Qt::NoButton);
        setAcceptHoverEvents(false);
    }

    Q_EMIT consumesInputChanged(value);
}

unity::shell::application::MirSurfaceInterface* MirSurfaceItem::surface() const
{
    return m_surface;
}

void MirSurfaceItem::setSurface(unity::shell::application::MirSurfaceInterface *unitySurface)
{
    QMutexLocker mutexLocker(&m_mutex);

    auto surface = static_cast<qtmir::MirSurfaceInterface*>(unitySurface);
    qCDebug(QTMIR_SURFACES).nospace() << "MirSurfaceItem::setSurface surface=" << surface;

    if (surface == m_surface) {
        return;
    }

    if (m_surface) {
        disconnect(m_surface, nullptr, this, nullptr);

        if (hasActiveFocus() && m_consumesInput && m_surface->live()) {
            m_surface->setFocus(false);
        }

        m_surface->decrementViewCount();

        if (!m_surface->isBeingDisplayed() && window()) {
            disconnect(window(), nullptr, m_surface, nullptr);
        }
        if (m_textureProvider) {
            m_textureProvider->releaseTexture();
        }
    }

    m_surface = surface;

    if (m_surface) {
        m_surface->incrementViewCount();

        // When a new mir frame gets posted we notify the QML engine that this item needs redrawing,
        // schedules call to updatePaintNode() from the rendering thread
        connect(m_surface, &MirSurfaceInterface::framesPosted, this, &QQuickItem::update);

        connect(m_surface, &MirSurfaceInterface::stateChanged, this, &MirSurfaceItem::surfaceStateChanged);
        connect(m_surface, &MirSurfaceInterface::liveChanged, this, &MirSurfaceItem::liveChanged);
        connect(m_surface, &MirSurfaceInterface::sizeChanged, this, &MirSurfaceItem::onActualSurfaceSizeChanged);

        connect(window(), &QQuickWindow::frameSwapped, m_surface, &MirSurfaceInterface::onCompositorSwappedBuffers,
            (Qt::ConnectionType) (Qt::DirectConnection | Qt::UniqueConnection));

        Q_EMIT typeChanged(m_surface->type());
        Q_EMIT liveChanged(true);
        Q_EMIT surfaceStateChanged(m_surface->state());

        updateMirSurfaceSize();
        setImplicitSize(m_surface->size().width(), m_surface->size().height());

        if (m_orientationAngle) {
            m_surface->setOrientationAngle(*m_orientationAngle);
            connect(m_surface, &MirSurfaceInterface::orientationAngleChanged, this, &MirSurfaceItem::orientationAngleChanged);
            delete m_orientationAngle;
            m_orientationAngle = nullptr;
        } else {
            connect(m_surface, &MirSurfaceInterface::orientationAngleChanged, this, &MirSurfaceItem::orientationAngleChanged);
            Q_EMIT orientationAngleChanged(m_surface->orientationAngle());
        }

        if (m_consumesInput) {
            m_surface->setFocus(hasActiveFocus());
        }
    }

    update();

    Q_EMIT surfaceChanged(m_surface);
}

void MirSurfaceItem::releaseResources()
{
    if (m_textureProvider) {
        Q_ASSERT(window());

        MirSurfaceItemReleaseResourcesJob *job = new MirSurfaceItemReleaseResourcesJob;
        job->textureProvider = m_textureProvider;
        m_textureProvider = nullptr;
        window()->scheduleRenderJob(job, QQuickWindow::AfterSynchronizingStage);
    }
}

int MirSurfaceItem::surfaceWidth() const
{
    return m_surfaceWidth;
}

void MirSurfaceItem::setSurfaceWidth(int value)
{
    if (value != m_surfaceWidth) {
        m_surfaceWidth = value;
        scheduleMirSurfaceSizeUpdate();
        Q_EMIT surfaceWidthChanged(value);
    }
}

void MirSurfaceItem::onActualSurfaceSizeChanged(const QSize &size)
{
    setImplicitSize(size.width(), size.height());
}

int MirSurfaceItem::surfaceHeight() const
{
    return m_surfaceHeight;
}

void MirSurfaceItem::setSurfaceHeight(int value)
{
    if (value != m_surfaceHeight) {
        m_surfaceHeight = value;
        scheduleMirSurfaceSizeUpdate();
        Q_EMIT surfaceHeightChanged(value);
    }
}

} // namespace qtmir

#include "mirsurfaceitem.moc"
