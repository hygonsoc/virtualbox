/* $Id$ */
/** @file
 *
 * VBox frontends: Qt GUI ("VirtualBox"):
 * UIGMachinePreview class implementation
 */

/*
 * Copyright (C) 2010-2013 Oracle Corporation
 *
 * This file is part of VirtualBox Open Source Edition (OSE), as
 * available from http://www.virtualbox.org. This file is free software;
 * you can redistribute it and/or modify it under the terms of the GNU
 * General Public License (GPL) as published by the Free Software
 * Foundation, in version 2 as it comes in the "COPYING" file of the
 * VirtualBox OSE distribution. VirtualBox OSE is distributed in the
 * hope that it will be useful, but WITHOUT ANY WARRANTY of any kind.
 */

/* Qt includes: */
#include <QGraphicsSceneContextMenuEvent>
#include <QMenu>
#include <QPainter>
#include <QTimer>

/* GUI includes: */
#include "UIGMachinePreview.h"
#include "UIVirtualBoxEventHandler.h"
#include "UIExtraDataManager.h"
#include "UIImageTools.h"
#include "UIConverter.h"
#include "UIIconPool.h"
#include "VBoxGlobal.h"

/* COM includes: */
#include "CConsole.h"
#include "CDisplay.h"

UIGMachinePreview::UIGMachinePreview(QIGraphicsWidget *pParent)
    : QIWithRetranslateUI4<QIGraphicsWidget>(pParent)
    , m_pUpdateTimer(new QTimer(this))
    , m_pUpdateTimerMenu(0)
    , m_iMargin(0)
    , m_pbgEmptyImage(0)
    , m_pbgFullImage(0)
    , m_pPreviewImg(0)
{
    /* Setup contents: */
    setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);

    /* Create session instance: */
    m_session.createInstance(CLSID_Session);

    /* Create bg images: */
    m_pbgEmptyImage = new QPixmap(UIIconPool::pixmap(":/preview_empty_228x168px.png"));
    m_pbgFullImage = new QPixmap(UIIconPool::pixmap(":/preview_full_228x168px.png"));

    /* Create the context menu: */
    m_pUpdateTimerMenu = new QMenu;
    QActionGroup *pUpdateTimeG = new QActionGroup(this);
    pUpdateTimeG->setExclusive(true);
    for(int i = 0; i < PreviewUpdateIntervalType_Max; ++i)
    {
        QAction *pUpdateTime = new QAction(pUpdateTimeG);
        pUpdateTime->setData(i);
        pUpdateTime->setCheckable(true);
        pUpdateTimeG->addAction(pUpdateTime);
        m_pUpdateTimerMenu->addAction(pUpdateTime);
        m_actions[static_cast<PreviewUpdateIntervalType>(i)] = pUpdateTime;
    }
    m_pUpdateTimerMenu->insertSeparator(m_actions[static_cast<PreviewUpdateIntervalType>(PreviewUpdateIntervalType_500ms)]);

    /* Initialize with the new update interval: */
    setUpdateInterval(gEDataManager->selectorWindowPreviewUpdateInterval(), false);

    /* Setup connections: */
    connect(m_pUpdateTimer, SIGNAL(timeout()), this, SLOT(sltRecreatePreview()));
    connect(gVBoxEvents, SIGNAL(sigMachineStateChange(QString, KMachineState)),
            this, SLOT(sltMachineStateChange(QString)));

    /* Retranslate the UI */
    retranslateUi();
}

UIGMachinePreview::~UIGMachinePreview()
{
    /* Close any open session: */
    if (m_session.GetState() == KSessionState_Locked)
        m_session.UnlockMachine();
    delete m_pbgEmptyImage;
    delete m_pbgFullImage;
    if (m_pPreviewImg)
        delete m_pPreviewImg;
    if (m_pUpdateTimerMenu)
        delete m_pUpdateTimerMenu;
}

void UIGMachinePreview::setMachine(const CMachine& machine)
{
    /* Pause: */
    stop();

    /* Assign new machine: */
    m_machine = machine;

    /* Fetch machine data: */
    m_strPreviewName = tr("No preview");
    if (!m_machine.isNull())
        m_strPreviewName = m_machine.GetAccessible() ? m_machine.GetName() :
                           QApplication::translate("UIVMListView", "Inaccessible");

    /* Resume: */
    restart();
}

CMachine UIGMachinePreview::machine() const
{
    return m_machine;
}

void UIGMachinePreview::sltMachineStateChange(QString strId)
{
    /* Make sure its the event for our machine: */
    if (m_machine.isNull() || m_machine.GetId() != strId)
        return;

    /* Restart the preview: */
    restart();
}

void UIGMachinePreview::sltRecreatePreview()
{
    /* Skip invisible preview: */
    if (!isVisible())
        return;

    /* Cleanup previous image: */
    if (m_pPreviewImg)
    {
        delete m_pPreviewImg;
        m_pPreviewImg = 0;
    }

    /* Fetch actual machine-state: */
    const KMachineState machineState = m_machine.isNull() ? KMachineState_Null : m_machine.GetState();

    /* We are creating preview only for assigned and accessible VMs: */
    if (!m_machine.isNull() && machineState != KMachineState_Null &&
        m_vRect.width() > 0 && m_vRect.height() > 0)
    {
        /* Prepare image: */
        QImage image;

        /* Preview update enabled? */
        if (m_pUpdateTimer->interval() > 0)
        {
            /* Depending on machine state: */
            switch (machineState)
            {
                /* If machine is in SAVED/RESTORING state: */
                case KMachineState_Saved:
                case KMachineState_Restoring:
                {
                    /* Use the screenshot from saved-state if possible: */
                    ULONG uWidth = 0, uHeight = 0;
                    QVector<BYTE> screenData = m_machine.ReadSavedScreenshotPNGToArray(0, uWidth, uHeight);
                    if (m_machine.isOk() && !screenData.isEmpty())
                    {
                        /* Create image based on shallow copy or screenshot data,
                         * scale image down if necessary to the size possible to reflect: */
                        image = QImage::fromData(screenData.data(), screenData.size(), "PNG")
                                .scaled(imageAspectRatioSize(m_vRect.size(), QSize(uWidth, uHeight)),
                                        Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
                        /* Dim image to give it required look: */
                        dimImage(image);
                    }
                    break;
                }
                /* If machine is in RUNNING/PAUSED state: */
                case KMachineState_Running:
                case KMachineState_Paused:
                {
                    /* Make sure session state is Locked: */
                    if (m_session.GetState() != KSessionState_Locked)
                        break;

                    /* Make sure console is OK: */
                    CConsole console = m_session.GetConsole();
                    if (!m_session.isOk() || console.isNull())
                        break;
                    /* Make sure display is OK: */
                    CDisplay display = console.GetDisplay();
                    if (!console.isOk() || display.isNull())
                        break;

                    /* Calculate aspect-ratio size: */
                    ULONG uGuestWidth, uGuestHeight, uBpp;
                    LONG iOriginX, iOriginY;
                    display.GetScreenResolution(0, uGuestWidth, uGuestHeight, uBpp, iOriginX, iOriginY);
                    const QSize size = imageAspectRatioSize(m_vRect.size(), QSize(uGuestWidth, uGuestHeight));

                    /* Use direct VM content: */
                    QVector<BYTE> screenData = display.TakeScreenShotToArray(0, size.width(), size.height());
                    if (display.isOk() && !screenData.isEmpty())
                    {
                        /* Unfortunately we have to reorder the pixel data,
                         * cause the VBox API returns RGBA data,
                         * which is not a format QImage understand. */
                        uint32_t *pData = (uint32_t*)screenData.data();
                        for (int i = 0; i < screenData.size() / 4; ++i)
                        {
                            uint32_t e = pData[i];
                            pData[i] = RT_MAKE_U32_FROM_U8(RT_BYTE3(e), RT_BYTE2(e), RT_BYTE1(e), RT_BYTE4(e));
                        }
                        /* Create image based on shallow copy or reordered data: */
                        image = QImage((uchar*)pData, size.width(), size.height(), QImage::Format_RGB32);
                        /* Dim image to give it required look for PAUSED state: */
                        if (machineState == KMachineState_Paused)
                            dimImage(image);
                    }
                    break;
                }
                default:
                    break;
            }
        }

        /* If image initialized: */
        if (!image.isNull())
        {
            /* Shallow copy that image: */
            m_pPreviewImg = new QImage(image);
            /* And detach that copy: */
            m_pPreviewImg->bits();
        }
    }

    /* Redraw preview in any case: */
    update();
}

void UIGMachinePreview::resizeEvent(QGraphicsSceneResizeEvent *pEvent)
{
    recalculatePreviewRectangle();
    sltRecreatePreview();
    QIGraphicsWidget::resizeEvent(pEvent);
}

void UIGMachinePreview::showEvent(QShowEvent *pEvent)
{
    restart();
    QIGraphicsWidget::showEvent(pEvent);
}

void UIGMachinePreview::hideEvent(QHideEvent *pEvent)
{
    stop();
    QIGraphicsWidget::hideEvent(pEvent);
}

void UIGMachinePreview::contextMenuEvent(QGraphicsSceneContextMenuEvent *pEvent)
{
    QAction *pReturn = m_pUpdateTimerMenu->exec(pEvent->screenPos(), 0);
    if (pReturn)
    {
        PreviewUpdateIntervalType interval = static_cast<PreviewUpdateIntervalType>(pReturn->data().toInt());
        setUpdateInterval(interval, true);
        restart();
    }
}

void UIGMachinePreview::retranslateUi()
{
    m_actions.value(PreviewUpdateIntervalType_Disabled)->setText(tr("Update disabled"));
    m_actions.value(PreviewUpdateIntervalType_500ms)->setText(tr("Every 0.5 s"));
    m_actions.value(PreviewUpdateIntervalType_1000ms)->setText(tr("Every 1 s"));
    m_actions.value(PreviewUpdateIntervalType_2000ms)->setText(tr("Every 2 s"));
    m_actions.value(PreviewUpdateIntervalType_5000ms)->setText(tr("Every 5 s"));
    m_actions.value(PreviewUpdateIntervalType_10000ms)->setText(tr("Every 10 s"));
}

QSizeF UIGMachinePreview::sizeHint(Qt::SizeHint which, const QSizeF &constraint /* = QSizeF() */) const
{
    if (which == Qt::MinimumSize)
        return QSize(228 /* pixmap width */ + 2 * m_iMargin,
                     168 /* pixmap height */ + 2 * m_iMargin);
    return QIGraphicsWidget::sizeHint(which, constraint);
}

void UIGMachinePreview::paint(QPainter *pPainter, const QStyleOptionGraphicsItem*, QWidget*)
{
    /* Where should the content go: */
    QRect cr = contentsRect().toRect();
    if (!cr.isValid())
        return;

    /* If there is a preview image available: */
    if (m_pPreviewImg)
    {
        /* Draw black background: */
        pPainter->fillRect(m_vRect, Qt::black);

        /* Draw empty monitor frame: */
        pPainter->drawPixmap(cr.x() + m_iMargin, cr.y() + m_iMargin, *m_pbgEmptyImage);

        /* Move image to viewport center: */
        QRect imageRect(QPoint(0, 0), m_pPreviewImg->size());
        imageRect.moveCenter(m_vRect.center());
        /* Draw preview image: */
        pPainter->drawImage(imageRect.topLeft(), *m_pPreviewImg);
    }
    else
    {
        /* Draw full monitor frame: */
        pPainter->drawPixmap(cr.x() + m_iMargin, cr.y() + m_iMargin, *m_pbgFullImage);

        /* Paint preview name: */
        QFont font = pPainter->font();
        font.setBold(true);
        int fFlags = Qt::AlignCenter | Qt::TextWordWrap;
        float h = m_vRect.size().height() * .2;
        QRect r;
        /* Make a little magic to find out if the given text fits into our rectangle.
         * Decrease the font pixel size as long as it doesn't fit. */
        int cMax = 30;
        do
        {
            h = h * .8;
            font.setPixelSize((int)h);
            pPainter->setFont(font);
            r = pPainter->boundingRect(m_vRect, fFlags, m_strPreviewName);
        }
        while ((r.height() > m_vRect.height() || r.width() > m_vRect.width()) && cMax-- != 0);
        pPainter->setPen(Qt::white);
        pPainter->drawText(m_vRect, fFlags, m_strPreviewName);
    }
}

void UIGMachinePreview::setUpdateInterval(PreviewUpdateIntervalType interval, bool fSave)
{
    switch (interval)
    {
        case PreviewUpdateIntervalType_Disabled:
        {
            /* Stop the timer: */
            m_pUpdateTimer->stop();
            /* And continue with other cases: */
        }
        case PreviewUpdateIntervalType_500ms:
        case PreviewUpdateIntervalType_1000ms:
        case PreviewUpdateIntervalType_2000ms:
        case PreviewUpdateIntervalType_5000ms:
        case PreviewUpdateIntervalType_10000ms:
        {
            /* Set the timer interval: */
            m_pUpdateTimer->setInterval(gpConverter->toInternalInteger(interval));
            /* Check corresponding action: */
            m_actions[interval]->setChecked(true);
            break;
        }
        case PreviewUpdateIntervalType_Max:
            break;
    }
    if (fSave)
        gEDataManager->setSelectorWindowPreviewUpdateInterval(interval);
}

void UIGMachinePreview::recalculatePreviewRectangle()
{
    /* Contents rectangle: */
    QRect cr = contentsRect().toRect();
    m_vRect = cr.adjusted(21 + m_iMargin, 17 + m_iMargin, -21 - m_iMargin, -20 - m_iMargin);
}

void UIGMachinePreview::restart()
{
    /* Fetch the latest machine-state: */
    KMachineState machineState = m_machine.isNull() ? KMachineState_Null : m_machine.GetState();

    /* Reopen session if necessary: */
    if (m_session.GetState() == KSessionState_Locked)
        m_session.UnlockMachine();
    if (!m_machine.isNull())
    {
        /* Lock the session for the current machine: */
        if (machineState == KMachineState_Running || machineState == KMachineState_Paused)
            m_machine.LockMachine(m_session, KLockType_Shared);
    }

    /* Recreate the preview image: */
    sltRecreatePreview();

    /* Start the timer if necessary: */
    if (!m_machine.isNull())
    {
        if (m_pUpdateTimer->interval() > 0 && machineState == KMachineState_Running)
            m_pUpdateTimer->start();
    }
}

void UIGMachinePreview::stop()
{
    /* Stop the timer: */
    m_pUpdateTimer->stop();
}

/* static */
QSize UIGMachinePreview::imageAspectRatioSize(const QSize &hostSize, const QSize &guestSize)
{
    /* Calculate host/guest aspect-ratio: */
    const double dHostAspectRatio = (double)hostSize.width() / hostSize.height();
    const double dGuestAspectRatio = (double)guestSize.width() / guestSize.height();
    int iWidth = 0, iHeight = 0;
    /* Guest-screen more thin by vertical than host-screen: */
    if (dGuestAspectRatio >= dHostAspectRatio)
    {
        /* Get host width: */
        iWidth = hostSize.width();
        /* And calculate height based on guest aspect ratio: */
        iHeight = (double)iWidth / dGuestAspectRatio;
        /* But no more than host height: */
        iHeight = qMin(iHeight, hostSize.height());
    }
    /* Host-screen more thin by vertical than guest-screen: */
    else
    {
        /* Get host height: */
        iHeight = hostSize.height();
        /* And calculate width based on guest aspect ratio: */
        iWidth = (double)iHeight * dGuestAspectRatio;
        /* But no more than host width: */
        iWidth = qMin(iWidth, hostSize.width());
    }
    /* Return actual size: */
    return QSize(iWidth, iHeight);
}

