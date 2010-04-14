/* $Id$ */
/** @file
 *
 * VBox frontends: Qt GUI ("VirtualBox"):
 * VBoxGlobal class implementation
 */

/*
 * Copyright (C) 2006-2010 Sun Microsystems, Inc.
 *
 * This file is part of VirtualBox Open Source Edition (OSE), as
 * available from http://www.virtualbox.org. This file is free software;
 * you can redistribute it and/or modify it under the terms of the GNU
 * General Public License (GPL) as published by the Free Software
 * Foundation, in version 2 as it comes in the "COPYING" file of the
 * VirtualBox OSE distribution. VirtualBox OSE is distributed in the
 * hope that it will be useful, but WITHOUT ANY WARRANTY of any kind.
 *
 * Please contact Sun Microsystems, Inc., 4150 Network Circle, Santa
 * Clara, CA 95054 USA or visit http://www.sun.com if you need
 * additional information or have any questions.
 */

#include "VBoxGlobal.h"
#include <VBox/VBoxHDD.h>
#include <VBox/version.h>

#include "VBoxDefs.h"
#include "VBoxSelectorWnd.h"
#include "VBoxConsoleWnd.h"
#include "VBoxProblemReporter.h"
#include "QIHotKeyEdit.h"
#include "QIMessageBox.h"
#include "QIDialogButtonBox.h"

#ifdef VBOX_WITH_NEW_RUNTIME_CORE
# include "UIMachine.h"
# include "UISession.h"
#endif
#ifdef VBOX_WITH_REGISTRATION
# include "UIRegistrationWzd.h"
#endif
#include "VBoxUpdateDlg.h"

#ifdef VBOX_WITH_VIDEOHWACCEL
#include "VBoxFrameBuffer.h"
#endif

/* Qt includes */
#include <QProgressDialog>
#include <QLibraryInfo>
#include <QFileDialog>
#include <QToolTip>
#include <QTranslator>
#include <QDesktopWidget>
#include <QDesktopServices>
#include <QMutex>
#include <QToolButton>
#include <QProcess>
#include <QThread>
#include <QPainter>
#include <QSettings>
#include <QTimer>
#include <QDir>
#include <QHelpEvent>
#include <QLocale>

#include <math.h>

#ifdef Q_WS_X11
# ifndef VBOX_OSE
#  include "VBoxLicenseViewer.h"
# endif /* VBOX_OSE */
# include <QTextBrowser>
# include <QScrollBar>
# include <QX11Info>
# include "VBoxX11Helper.h"
#endif

#ifdef Q_WS_MAC
# include "VBoxUtils-darwin.h"
#endif /* Q_WS_MAC */

#if defined (Q_WS_WIN)
#include "shlobj.h"
#include <QEventLoop>
#endif

#if defined (Q_WS_X11)
#undef BOOL /* typedef CARD8 BOOL in Xmd.h conflicts with #define BOOL PRBool
             * in COMDefs.h. A better fix would be to isolate X11-specific
             * stuff by placing XX* helpers below to a separate source file. */
#include <X11/X.h>
#include <X11/Xmd.h>
#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <X11/extensions/Xinerama.h>
#define BOOL PRBool
#endif

#include <VBox/sup.h>
#include <VBox/com/Guid.h>

#include <iprt/asm.h>
#include <iprt/err.h>
#include <iprt/param.h>
#include <iprt/path.h>
#include <iprt/env.h>
#include <iprt/file.h>
#include <iprt/ldr.h>
#include <iprt/system.h>

#ifdef VBOX_GUI_WITH_SYSTRAY
#include <iprt/process.h>

#if defined(RT_OS_WINDOWS) || defined(RT_OS_OS2)
#define HOSTSUFF_EXE ".exe"
#else /* !RT_OS_WINDOWS */
#define HOSTSUFF_EXE ""
#endif /* !RT_OS_WINDOWS */
#endif

#if defined (Q_WS_X11)
#include <iprt/mem.h>
#endif

//#define VBOX_WITH_FULL_DETAILS_REPORT /* hidden for now */

//#warning "port me: check this"
/// @todo bird: Use (U)INT_PTR, (U)LONG_PTR, DWORD_PTR, or (u)intptr_t.
#if defined(Q_OS_WIN64)
typedef __int64 Q_LONG;             /* word up to 64 bit signed */
typedef unsigned __int64 Q_ULONG;   /* word up to 64 bit unsigned */
#else
typedef long Q_LONG;                /* word up to 64 bit signed */
typedef unsigned long Q_ULONG;      /* word up to 64 bit unsigned */
#endif

// VBoxMediaEnumEvent
/////////////////////////////////////////////////////////////////////////////

class VBoxMediaEnumEvent : public QEvent
{
public:

    /** Constructs a regular enum event */
    VBoxMediaEnumEvent (const VBoxMedium &aMedium,
                        VBoxMediaList::iterator &aIterator)
        : QEvent ((QEvent::Type) VBoxDefs::MediaEnumEventType)
        , mMedium (aMedium), mIterator (aIterator), mLast (false)
        {}
    /** Constructs the last enum event */
    VBoxMediaEnumEvent (VBoxMediaList::iterator &aIterator)
        : QEvent ((QEvent::Type) VBoxDefs::MediaEnumEventType)
        , mIterator (aIterator), mLast (true)
        {}

    /** Last enumerated medium (not valid when #last is true) */
    const VBoxMedium mMedium;
    /** Opaque iterator provided by the event sender (guaranteed to be
     *  the same variable for all media in the single enumeration procedure) */
    VBoxMediaList::iterator &mIterator;
    /** Whether this is the last event for the given enumeration or not */
    const bool mLast;
};

// VirtualBox callback class
/////////////////////////////////////////////////////////////////////////////

class VBoxCallback : VBOX_SCRIPTABLE_IMPL(IVirtualBoxCallback)
{
public:

    VBoxCallback (VBoxGlobal &aGlobal)
        : mGlobal (aGlobal)
        , mIsRegDlgOwner (false)
        , mIsUpdDlgOwner (false)
#ifdef VBOX_GUI_WITH_SYSTRAY
        , mIsTrayIconOwner (false)
#endif
    {
#if defined (Q_OS_WIN32)
        refcnt = 0;
#endif
    }

    virtual ~VBoxCallback() {}

    NS_DECL_ISUPPORTS

#if defined (Q_OS_WIN32)
    STDMETHOD_(ULONG, AddRef)()
    {
        return ::InterlockedIncrement (&refcnt);
    }
    STDMETHOD_(ULONG, Release)()
    {
        long cnt = ::InterlockedDecrement (&refcnt);
        if (cnt == 0)
            delete this;
        return cnt;
    }
#endif
    VBOX_SCRIPTABLE_DISPATCH_IMPL(IVirtualBoxCallback)

    // IVirtualBoxCallback methods

    // Note: we need to post custom events to the GUI event queue
    // instead of doing what we need directly from here because on Win32
    // these callback methods are never called on the main GUI thread.
    // Another reason to handle events asynchronously is that internally
    // most callback interface methods are called from under the initiator
    // object's lock, so accessing the initiator object (for example, reading
    // some property) directly from the callback method will definitely cause
    // a deadlock.

    STDMETHOD(OnMachineStateChange) (IN_BSTR id, MachineState_T state)
    {
        postEvent (new VBoxMachineStateChangeEvent (QString::fromUtf16(id),
                                                    (KMachineState) state));
        return S_OK;
    }

    STDMETHOD(OnMachineDataChange) (IN_BSTR id)
    {
        postEvent (new VBoxMachineDataChangeEvent (QString::fromUtf16(id)));
        return S_OK;
    }

    STDMETHOD(OnExtraDataCanChange)(IN_BSTR id,
                                    IN_BSTR key, IN_BSTR value,
                                    BSTR *error, BOOL *allowChange)
    {
        if (!error || !allowChange)
            return E_INVALIDARG;

        if (com::asGuidStr(id).isEmpty())
        {
            /* it's a global extra data key someone wants to change */
            QString sKey = QString::fromUtf16 (key);
            QString sVal = QString::fromUtf16 (value);
            if (sKey.startsWith ("GUI/"))
            {
                if (sKey == VBoxDefs::GUI_RegistrationDlgWinID)
                {
                    if (mIsRegDlgOwner)
                    {
                        if (sVal.isEmpty() ||
                            sVal == QString ("%1")
                                .arg ((qulonglong) vboxGlobal().mainWindow()->winId()))
                            *allowChange = TRUE;
                        else
                            *allowChange = FALSE;
                    }
                    else
                        *allowChange = TRUE;
                    return S_OK;
                }

                if (sKey == VBoxDefs::GUI_UpdateDlgWinID)
                {
                    if (mIsUpdDlgOwner)
                    {
                        if (sVal.isEmpty() ||
                            sVal == QString ("%1")
                                .arg ((qulonglong) vboxGlobal().mainWindow()->winId()))
                            *allowChange = TRUE;
                        else
                            *allowChange = FALSE;
                    }
                    else
                        *allowChange = TRUE;
                    return S_OK;
                }
#ifdef VBOX_GUI_WITH_SYSTRAY
                if (sKey == VBoxDefs::GUI_TrayIconWinID)
                {
                    if (mIsTrayIconOwner)
                    {
                        if (sVal.isEmpty() ||
                            sVal == QString ("%1")
                                .arg ((qulonglong) vboxGlobal().mainWindow()->winId()))
                            *allowChange = TRUE;
                        else
                            *allowChange = FALSE;
                    }
                    else
                        *allowChange = TRUE;
                    return S_OK;
                }
#endif
                /* try to set the global setting to check its syntax */
                VBoxGlobalSettings gs (false /* non-null */);
                if (gs.setPublicProperty (sKey, sVal))
                {
                    /* this is a known GUI property key */
                    if (!gs)
                    {
                        /* disallow the change when there is an error*/
                        *error = SysAllocString ((const OLECHAR *)
                            (gs.lastError().isNull() ? 0 : gs.lastError().utf16()));
                        *allowChange = FALSE;
                    }
                    else
                        *allowChange = TRUE;
                    return S_OK;
                }
            }
        }

        /* not interested in this key -- never disagree */
        *allowChange = TRUE;
        return S_OK;
    }

    STDMETHOD(OnExtraDataChange) (IN_BSTR id,
                                  IN_BSTR key, IN_BSTR value)
    {
        if (com::asGuidStr(id).isEmpty())
        {
            QString sKey = QString::fromUtf16 (key);
            QString sVal = QString::fromUtf16 (value);
            if (sKey.startsWith ("GUI/"))
            {
                if (sKey == VBoxDefs::GUI_RegistrationDlgWinID)
                {
                    if (sVal.isEmpty())
                    {
                        mIsRegDlgOwner = false;
                        QApplication::postEvent (&mGlobal, new VBoxCanShowRegDlgEvent (true));
                    }
                    else if (sVal == QString ("%1")
                             .arg ((qulonglong) vboxGlobal().mainWindow()->winId()))
                    {
                        mIsRegDlgOwner = true;
                        QApplication::postEvent (&mGlobal, new VBoxCanShowRegDlgEvent (true));
                    }
                    else
                        QApplication::postEvent (&mGlobal, new VBoxCanShowRegDlgEvent (false));
                }
                if (sKey == VBoxDefs::GUI_UpdateDlgWinID)
                {
                    if (sVal.isEmpty())
                    {
                        mIsUpdDlgOwner = false;
                        QApplication::postEvent (&mGlobal, new VBoxCanShowUpdDlgEvent (true));
                    }
                    else if (sVal == QString ("%1")
                             .arg ((qulonglong) vboxGlobal().mainWindow()->winId()))
                    {
                        mIsUpdDlgOwner = true;
                        QApplication::postEvent (&mGlobal, new VBoxCanShowUpdDlgEvent (true));
                    }
                    else
                        QApplication::postEvent (&mGlobal, new VBoxCanShowUpdDlgEvent (false));
                }
                if (sKey == "GUI/LanguageID")
                    QApplication::postEvent (&mGlobal, new VBoxChangeGUILanguageEvent (sVal));
#ifdef VBOX_GUI_WITH_SYSTRAY
                if (sKey == "GUI/MainWindowCount")
                    QApplication::postEvent (&mGlobal, new VBoxMainWindowCountChangeEvent (sVal.toInt()));
                if (sKey == VBoxDefs::GUI_TrayIconWinID)
                {
                    if (sVal.isEmpty())
                    {
                        mIsTrayIconOwner = false;
                        QApplication::postEvent (&mGlobal, new VBoxCanShowTrayIconEvent (true));
                    }
                    else if (sVal == QString ("%1")
                             .arg ((qulonglong) vboxGlobal().mainWindow()->winId()))
                    {
                        mIsTrayIconOwner = true;
                        QApplication::postEvent (&mGlobal, new VBoxCanShowTrayIconEvent (true));
                    }
                    else
                        QApplication::postEvent (&mGlobal, new VBoxCanShowTrayIconEvent (false));
                }
                if (sKey == "GUI/TrayIcon/Enabled")
                    QApplication::postEvent (&mGlobal, new VBoxChangeTrayIconEvent ((sVal.toLower() == "true") ? true : false));
#endif
#ifdef Q_WS_MAC
                if (sKey == VBoxDefs::GUI_PresentationModeEnabled)
                {
                    /* Default to true if it is an empty value */
                    QString testStr = sVal.toLower();
                    bool f = (testStr.isEmpty() || testStr == "false");
                    QApplication::postEvent (&mGlobal, new VBoxChangePresentationModeEvent (f));
                }
#endif

                mMutex.lock();
                mGlobal.gset.setPublicProperty (sKey, sVal);
                mMutex.unlock();
                Assert (!!mGlobal.gset);
            }
        }
#ifdef Q_WS_MAC
        else if (mGlobal.isVMConsoleProcess())
        {
            /* Check for the currently running machine */
            if (QString::fromUtf16(id) == mGlobal.vmUuid)
            {
                QString strKey = QString::fromUtf16(key);
                QString strVal = QString::fromUtf16(value);
                // TODO_NEW_CORE: we should cleanup
                // VBoxChangeDockIconUpdateEvent to have no parameters. So it
                // could really be use for both events and the consumer should
                // ask per GetExtraData how the current values are.
                if (   strKey == VBoxDefs::GUI_RealtimeDockIconUpdateEnabled
                    || strKey == VBoxDefs::GUI_RealtimeDockIconUpdateMonitor)
                {
                    /* Default to true if it is an empty value */
                    bool f = strVal.toLower() == "false" ? false : true;
                    QApplication::postEvent(&mGlobal, new VBoxChangeDockIconUpdateEvent(f));
                }
            }
        }
#endif /* Q_WS_MAC */
        return S_OK;
    }

    STDMETHOD(OnMediumRegistered) (IN_BSTR id, DeviceType_T type,
                                   BOOL registered)
    {
        /** @todo */
        Q_UNUSED (id);
        Q_UNUSED (type);
        Q_UNUSED (registered);
        return S_OK;
    }

    STDMETHOD(OnMachineRegistered) (IN_BSTR id, BOOL registered)
    {
        postEvent (new VBoxMachineRegisteredEvent (QString::fromUtf16(id),
                                                   registered));
        return S_OK;
    }

    STDMETHOD(OnSessionStateChange) (IN_BSTR id, SessionState_T state)
    {
        postEvent (new VBoxSessionStateChangeEvent (QString::fromUtf16(id),
                                                    (KSessionState) state));
        return S_OK;
    }

    STDMETHOD(OnSnapshotTaken) (IN_BSTR aMachineId, IN_BSTR aSnapshotId)
    {
        postEvent (new VBoxSnapshotEvent (QString::fromUtf16(aMachineId),
                                          QString::fromUtf16(aSnapshotId),
                                          VBoxSnapshotEvent::Taken));
        return S_OK;
    }

    STDMETHOD(OnSnapshotDeleted) (IN_BSTR aMachineId, IN_BSTR aSnapshotId)
    {
        postEvent (new VBoxSnapshotEvent (QString::fromUtf16(aMachineId),
                                          QString::fromUtf16(aSnapshotId),
                                          VBoxSnapshotEvent::Deleted));
        return S_OK;
    }

    STDMETHOD(OnSnapshotChange) (IN_BSTR aMachineId, IN_BSTR aSnapshotId)
    {
        postEvent (new VBoxSnapshotEvent (QString::fromUtf16(aMachineId),
                                          QString::fromUtf16(aSnapshotId),
                                          VBoxSnapshotEvent::Changed));
        return S_OK;
    }

    STDMETHOD(OnGuestPropertyChange) (IN_BSTR /* id */,
                                      IN_BSTR /* key */,
                                      IN_BSTR /* value */,
                                      IN_BSTR /* flags */)
    {
        return S_OK;
    }

private:

    void postEvent (QEvent *e)
    {
        // currently, we don't post events if we are in the VM execution
        // console mode, to save some CPU ticks (so far, there was no need
        // to handle VirtualBox callback events in the execution console mode)
        if (!mGlobal.isVMConsoleProcess())
            QApplication::postEvent (&mGlobal, e);
    }

    VBoxGlobal &mGlobal;

    /** protects #OnExtraDataChange() */
    QMutex mMutex;

    bool mIsRegDlgOwner;
    bool mIsUpdDlgOwner;
#ifdef VBOX_GUI_WITH_SYSTRAY
    bool mIsTrayIconOwner;
#endif

#if defined (Q_OS_WIN32)
private:
    long refcnt;
#endif
};

#if !defined (Q_OS_WIN32)
NS_DECL_CLASSINFO (VBoxCallback)
NS_IMPL_THREADSAFE_ISUPPORTS1_CI (VBoxCallback, IVirtualBoxCallback)
#endif

// VBoxGlobal
////////////////////////////////////////////////////////////////////////////////

static bool sVBoxGlobalInited = false;
static bool sVBoxGlobalInCleanup = false;

/** @internal
 *
 *  Special routine to do VBoxGlobal cleanup when the application is being
 *  terminated. It is called before some essential Qt functionality (for
 *  instance, QThread) becomes unavailable, allowing us to use it from
 *  VBoxGlobal::cleanup() if necessary.
 */
static void vboxGlobalCleanup()
{
    Assert (!sVBoxGlobalInCleanup);
    sVBoxGlobalInCleanup = true;
    vboxGlobal().cleanup();
}

/** @internal
 *
 *  Determines the rendering mode from the argument. Sets the appropriate
 *  default rendering mode if the argumen is NULL.
 */
static VBoxDefs::RenderMode vboxGetRenderMode (const char *aModeStr)
{
    VBoxDefs::RenderMode mode = VBoxDefs::InvalidRenderMode;

#if defined (Q_WS_MAC) && defined (VBOX_GUI_USE_QUARTZ2D)
    mode = VBoxDefs::Quartz2DMode;
# ifdef RT_ARCH_X86
    /* Quartz2DMode doesn't refresh correctly on 32-bit Snow Leopard, use image mode. */
//    char szRelease[80];
//    if (    RT_SUCCESS (RTSystemQueryOSInfo (RTSYSOSINFO_RELEASE, szRelease, sizeof (szRelease)))
//        &&  !strncmp (szRelease, "10.", 3))
//        mode = VBoxDefs::QImageMode;
# endif
#elif (defined (Q_WS_WIN32) || defined (Q_WS_PM) || defined (Q_WS_X11)) && defined (VBOX_GUI_USE_QIMAGE)
    mode = VBoxDefs::QImageMode;
#elif defined (Q_WS_X11) && defined (VBOX_GUI_USE_SDL)
    mode = VBoxDefs::SDLMode;
#elif defined (VBOX_GUI_USE_QIMAGE)
    mode = VBoxDefs::QImageMode;
#else
# error "Cannot determine the default render mode!"
#endif

    if (aModeStr)
    {
        if (0) ;
#if defined (VBOX_GUI_USE_QIMAGE)
        else if (::strcmp (aModeStr, "image") == 0)
            mode = VBoxDefs::QImageMode;
#endif
#if defined (VBOX_GUI_USE_SDL)
        else if (::strcmp (aModeStr, "sdl") == 0)
            mode = VBoxDefs::SDLMode;
#endif
#if defined (VBOX_GUI_USE_DDRAW)
        else if (::strcmp (aModeStr, "ddraw") == 0)
            mode = VBoxDefs::DDRAWMode;
#endif
#if defined (VBOX_GUI_USE_QUARTZ2D)
        else if (::strcmp (aModeStr, "quartz2d") == 0)
            mode = VBoxDefs::Quartz2DMode;
#endif
#if defined (VBOX_GUI_USE_QGLFB)
        else if (::strcmp (aModeStr, "qgl") == 0)
            mode = VBoxDefs::QGLMode;
#endif
//#if defined (VBOX_GUI_USE_QGL)
//        else if (::strcmp (aModeStr, "qgloverlay") == 0)
//            mode = VBoxDefs::QGLOverlayMode;
//#endif

    }

    return mode;
}

/** @class VBoxGlobal
 *
 *  The VBoxGlobal class incapsulates the global VirtualBox data.
 *
 *  There is only one instance of this class per VirtualBox application,
 *  the reference to it is returned by the static instance() method, or by
 *  the global vboxGlobal() function, that is just an inlined shortcut.
 */

VBoxGlobal::VBoxGlobal()
    : mValid (false)
    , mSelectorWnd (NULL), mConsoleWnd (NULL)
#ifdef VBOX_WITH_NEW_RUNTIME_CORE
    , m_pVirtualMachine(0)
#endif
    , mMainWindow (NULL)
#ifdef VBOX_WITH_REGISTRATION
    , mRegDlg (NULL)
#endif
    , mUpdDlg (NULL)
#ifdef VBOX_GUI_WITH_SYSTRAY
    , mIsTrayMenu (false)
    , mIncreasedWindowCounter (false)
#endif
    , mMediaEnumThread (NULL)
    , mIsKWinManaged (false)
    , mVerString ("1.0")
{
}

//
// Public members
/////////////////////////////////////////////////////////////////////////////

/**
 *  Returns a reference to the global VirtualBox data, managed by this class.
 *
 *  The main() function of the VBox GUI must call this function soon after
 *  creating a QApplication instance but before opening any of the main windows
 *  (to let the VBoxGlobal initialization procedure use various Qt facilities),
 *  and continue execution only when the isValid() method of the returned
 *  instancereturns true, i.e. do something like:
 *
 *  @code
 *  if ( !VBoxGlobal::instance().isValid() )
 *      return 1;
 *  @endcode
 *  or
 *  @code
 *  if ( !vboxGlobal().isValid() )
 *      return 1;
 *  @endcode
 *
 *  @note Some VBoxGlobal methods can be used on a partially constructed
 *  VBoxGlobal instance, i.e. from constructors and methods called
 *  from the VBoxGlobal::init() method, which obtain the instance
 *  using this instance() call or the ::vboxGlobal() function. Currently, such
 *  methods are:
 *      #vmStateText, #vmTypeIcon, #vmTypeText, #vmTypeTextList, #vmTypeFromText.
 *
 *  @see ::vboxGlobal
 */
VBoxGlobal &VBoxGlobal::instance()
{
    static VBoxGlobal vboxGlobal_instance;

    if (!sVBoxGlobalInited)
    {
        /* check that a QApplication instance is created */
        if (qApp)
        {
            sVBoxGlobalInited = true;
            vboxGlobal_instance.init();
            /* add our cleanup handler to the list of Qt post routines */
            qAddPostRoutine (vboxGlobalCleanup);
        }
        else
            AssertMsgFailed (("Must construct a QApplication first!"));
    }
    return vboxGlobal_instance;
}

VBoxGlobal::~VBoxGlobal()
{
    qDeleteAll (mOsTypeIcons);
    qDeleteAll (mVMStateIcons);
    qDeleteAll (mVMStateColors);
}

/* static */
QString VBoxGlobal::qtRTVersionString()
{
    return QString::fromLatin1 (qVersion());
}

/* static */
uint VBoxGlobal::qtRTVersion()
{
    QString rt_ver_str = VBoxGlobal::qtRTVersionString();
    return (rt_ver_str.section ('.', 0, 0).toInt() << 16) +
           (rt_ver_str.section ('.', 1, 1).toInt() << 8) +
           rt_ver_str.section ('.', 2, 2).toInt();
}

/* static */
QString VBoxGlobal::qtCTVersionString()
{
    return QString::fromLatin1 (QT_VERSION_STR);
}

/* static */
uint VBoxGlobal::qtCTVersion()
{
    QString ct_ver_str = VBoxGlobal::qtCTVersionString();
    return (ct_ver_str.section ('.', 0, 0).toInt() << 16) +
           (ct_ver_str.section ('.', 1, 1).toInt() << 8) +
           ct_ver_str.section ('.', 2, 2).toInt();
}

/**
 *  Sets the new global settings and saves them to the VirtualBox server.
 */
bool VBoxGlobal::setSettings (const VBoxGlobalSettings &gs)
{
    gs.save (mVBox);

    if (!mVBox.isOk())
    {
        vboxProblem().cannotSaveGlobalConfig (mVBox);
        return false;
    }

    /* We don't assign gs to our gset member here, because VBoxCallback
     * will update gset as necessary when new settings are successfullly
     * sent to the VirtualBox server by gs.save(). */

    return true;
}

/**
 *  Returns a reference to the main VBox VM Selector window.
 *  The reference is valid until application termination.
 *
 *  There is only one such a window per VirtualBox application.
 */
VBoxSelectorWnd &VBoxGlobal::selectorWnd()
{
#if defined (VBOX_GUI_SEPARATE_VM_PROCESS)
    AssertMsg (!vboxGlobal().isVMConsoleProcess(),
               ("Must NOT be a VM console process"));
#endif

    Assert (mValid);

    if (!mSelectorWnd)
    {
        /*
         *  We pass the address of mSelectorWnd to the constructor to let it be
         *  initialized right after the constructor is called. It is necessary
         *  to avoid recursion, since this method may be (and will be) called
         *  from the below constructor or from constructors/methods it calls.
         */
        VBoxSelectorWnd *w = new VBoxSelectorWnd (&mSelectorWnd, 0);
        Assert (w == mSelectorWnd);
        NOREF(w);
    }

    return *mSelectorWnd;
}


QWidget *VBoxGlobal::vmWindow()
{
    if (isVMConsoleProcess())
    {
#ifdef VBOX_WITH_NEW_RUNTIME_CORE
        if (m_pVirtualMachine)
            return m_pVirtualMachine->mainWindow();
        else
#endif /* VBOX_WITH_NEW_RUNTIME_CORE */
            return &consoleWnd();
    }
    return NULL;
}

/**
 *  Returns a reference to the main VBox VM Console window.
 *  The reference is valid until application termination.
 *
 *  There is only one such a window per VirtualBox application.
 */
VBoxConsoleWnd &VBoxGlobal::consoleWnd()
{
#if defined (VBOX_GUI_SEPARATE_VM_PROCESS)
    AssertMsg (vboxGlobal().isVMConsoleProcess(),
               ("Must be a VM console process"));
#endif

    Assert (mValid);

    if (!mConsoleWnd)
    {
        /*
         *  We pass the address of mConsoleWnd to the constructor to let it be
         *  initialized right after the constructor is called. It is necessary
         *  to avoid recursion, since this method may be (and will be) called
         *  from the below constructor or from constructors/methods it calls.
         */
        VBoxConsoleWnd *w = new VBoxConsoleWnd (&mConsoleWnd, 0);
        Assert (w == mConsoleWnd);
        NOREF(w);
    }

    return *mConsoleWnd;
}

#ifdef VBOX_WITH_NEW_RUNTIME_CORE
bool VBoxGlobal::createVirtualMachine(const CSession &session)
{
    if (!m_pVirtualMachine && !session.isNull())
    {
        UIMachine *pVirtualMachine = new UIMachine(&m_pVirtualMachine, session);
        Assert(pVirtualMachine == m_pVirtualMachine);
        NOREF(pVirtualMachine);
        return true;
    }
    return false;
}

UIMachine* VBoxGlobal::virtualMachine()
{
    return m_pVirtualMachine;
}
#endif

bool VBoxGlobal::brandingIsActive (bool aForce /* = false*/)
{
    if (aForce)
        return true;

    if (mBrandingConfig.isEmpty())
    {
        mBrandingConfig = QDir(QApplication::applicationDirPath()).absolutePath();
        mBrandingConfig += "/custom/custom.ini";
    }
    return QFile::exists (mBrandingConfig);
}

/**
  * Gets a value from the custom .ini file
  */
QString VBoxGlobal::brandingGetKey (QString aKey)
{
    QSettings s(mBrandingConfig, QSettings::IniFormat);
    return s.value(QString("%1").arg(aKey)).toString();
}

#ifdef VBOX_GUI_WITH_SYSTRAY

/**
 *  Returns true if the current instance a systray menu only (started with
 *  "-systray" parameter).
 */
bool VBoxGlobal::isTrayMenu() const
{
    return mIsTrayMenu;
}

void VBoxGlobal::setTrayMenu(bool aIsTrayMenu)
{
    mIsTrayMenu = aIsTrayMenu;
}

/**
 *  Spawns a new selector window (process).
 */
void VBoxGlobal::trayIconShowSelector()
{
    /* Get the path to the executable. */
    char path [RTPATH_MAX];
    RTPathAppPrivateArch (path, RTPATH_MAX);
    size_t sz = strlen (path);
    path [sz++] = RTPATH_DELIMITER;
    path [sz] = 0;
    char *cmd = path + sz;
    sz = RTPATH_MAX - sz;

    int rc = 0;
    RTPROCESS pid = NIL_RTPROCESS;
    RTENV env = RTENV_DEFAULT;

    const char VirtualBox_exe[] = "VirtualBox" HOSTSUFF_EXE;
    Assert (sz >= sizeof (VirtualBox_exe));
    strcpy (cmd, VirtualBox_exe);
    const char * args[] = {path, 0 };
# ifdef RT_OS_WINDOWS
    rc = RTProcCreate (path, args, env, 0, &pid);
# else
    rc = RTProcCreate (path, args, env, RTPROC_FLAGS_DAEMONIZE_DEPRECATED, &pid);
# endif
    if (RT_FAILURE (rc))
        LogRel(("Systray: Failed to start new selector window! Path=%s, rc=%Rrc\n", path, rc));
}

/**
 *  Tries to install the tray icon using the current instance (singleton).
 *  Returns true if this instance is the tray icon, false if not.
 */
bool VBoxGlobal::trayIconInstall()
{
    int rc = 0;
    QString strTrayWinID = mVBox.GetExtraData (VBoxDefs::GUI_TrayIconWinID);
    if (false == strTrayWinID.isEmpty())
    {
        /* Check if current tray icon is alive by writing some bogus value. */
        mVBox.SetExtraData (VBoxDefs::GUI_TrayIconWinID, "0");
        if (mVBox.isOk())
        {
            /* Current tray icon died - clean up. */
            mVBox.SetExtraData (VBoxDefs::GUI_TrayIconWinID, NULL);
            strTrayWinID.clear();
        }
    }

    /* Is there already a tray icon or is tray icon not active? */
    if (   (mIsTrayMenu == false)
        && (vboxGlobal().settings().trayIconEnabled())
        && (QSystemTrayIcon::isSystemTrayAvailable())
        && (strTrayWinID.isEmpty()))
    {
        /* Get the path to the executable. */
        char path [RTPATH_MAX];
        RTPathAppPrivateArch (path, RTPATH_MAX);
        size_t sz = strlen (path);
        path [sz++] = RTPATH_DELIMITER;
        path [sz] = 0;
        char *cmd = path + sz;
        sz = RTPATH_MAX - sz;

        RTPROCESS pid = NIL_RTPROCESS;
        RTENV env = RTENV_DEFAULT;

        const char VirtualBox_exe[] = "VirtualBox" HOSTSUFF_EXE;
        Assert (sz >= sizeof (VirtualBox_exe));
        strcpy (cmd, VirtualBox_exe);
        const char * args[] = {path, "-systray", 0 };
# ifdef RT_OS_WINDOWS /** @todo drop this once the RTProcCreate bug has been fixed */
        rc = RTProcCreate (path, args, env, 0, &pid);
# else
        rc = RTProcCreate (path, args, env, RTPROC_FLAGS_DAEMONIZE_DEPRECATED, &pid);
# endif

        if (RT_FAILURE (rc))
        {
            LogRel(("Systray: Failed to start systray window! Path=%s, rc=%Rrc\n", path, rc));
            return false;
        }
    }

    if (mIsTrayMenu)
    {
        // Use this selector for displaying the tray icon
        mVBox.SetExtraData (VBoxDefs::GUI_TrayIconWinID,
                            QString ("%1").arg ((qulonglong) vboxGlobal().mainWindow()->winId()));

        /* The first process which can grab this "mutex" will win ->
         * It will be the tray icon menu then. */
        if (mVBox.isOk())
        {
            emit trayIconShow (*(new VBoxShowTrayIconEvent (true)));
            return true;
        }
    }

    return false;
}

#endif

#ifdef Q_WS_X11
QList<QRect> XGetDesktopList()
{
    /* Prepare empty resulting list: */
    QList<QRect> result;

    /* Get current display: */
    Display* pDisplay = QX11Info::display();

    /* If thats Xinerama desktop: */
    if (XineramaIsActive(pDisplay))
    {
        /* Reading Xinerama data: */
        int iScreens = 0;
        XineramaScreenInfo *pScreensData = XineramaQueryScreens(pDisplay, &iScreens);

        /* Fill resulting list: */
        for (int i = 0; i < iScreens; ++ i)
            result << QRect(pScreensData[i].x_org, pScreensData[i].y_org,
                            pScreensData[i].width, pScreensData[i].height);

        /* Free screens data: */
        XFree(pScreensData);
    }

    /* Return resulting list: */
    return result;
}

QList<Window> XGetWindowIDList()
{
    /* Get current display: */
    Display *pDisplay = QX11Info::display();

    /* Get virtual desktop window: */
    Window window = QX11Info::appRootWindow();

    /* Get 'client list' atom: */
    Atom propNameAtom = XInternAtom(pDisplay, "_NET_CLIENT_LIST", True /* only if exists */);

    /* Prepare empty resulting list: */
    QList<Window> result;

    /* If atom does not exists return empty list: */
    if (propNameAtom == None)
        return result;

    /* Get atom value: */
    Atom realAtomType = None;
    int iRealFormat = 0;
    unsigned long uItemsCount = 0;
    unsigned long uBytesAfter = 0;
    unsigned char *pData = 0;
    int rc = XGetWindowProperty(pDisplay, window, propNameAtom,
                                0, 0x7fffffff /*LONG_MAX*/, False /* delete */,
                                XA_WINDOW, &realAtomType, &iRealFormat,
                                &uItemsCount, &uBytesAfter, &pData);

    /* If get property is failed return empty list: */
    if (rc != Success)
        return result;

    /* Fill resulting list with win ids: */
    Window *pWindowData = reinterpret_cast<Window*>(pData);
    for (ulong i = 0; i < uItemsCount; ++ i)
        result << pWindowData[i];

    /* Releasing resources: */
    XFree(pData);

    /* Return resulting list: */
    return result;
}

QList<ulong> XGetStrut(Window window)
{
    /* Get current display: */
    Display *pDisplay = QX11Info::display();

    /* Get 'strut' atom: */
    Atom propNameAtom = XInternAtom(pDisplay, "_NET_WM_STRUT_PARTIAL", True /* only if exists */);

    /* Prepare empty resulting list: */
    QList<ulong> result;

    /* If atom does not exists return empty list: */
    if (propNameAtom == None)
        return result;

    /* Get atom value: */
    Atom realAtomType = None;
    int iRealFormat = 0;
    ulong uItemsCount = 0;
    ulong uBytesAfter = 0;
    unsigned char *pData = 0;
    int rc = XGetWindowProperty(pDisplay, window, propNameAtom,
                                0, LONG_MAX, False /* delete */,
                                XA_CARDINAL, &realAtomType, &iRealFormat,
                                &uItemsCount, &uBytesAfter, &pData);

    /* If get property is failed return empty list: */
    if (rc != Success)
        return result;

    /* Fill resulting list with strut shifts: */
    ulong *pStrutsData = reinterpret_cast<ulong*>(pData);
    for (ulong i = 0; i < uItemsCount; ++ i)
        result << pStrutsData[i];

    /* Releasing resources: */
    XFree(pData);

    /* Return resulting list: */
    return result;
}
#endif /* ifdef Q_WS_X11 */

const QRect VBoxGlobal::availableGeometry(int iScreen) const
{
    /* Prepare empty result: */
    QRect result;

#ifdef Q_WS_X11

    /* Get current display: */
    Display* pDisplay = QX11Info::display();

    /* Get current application desktop: */
    QDesktopWidget *pDesktopWidget = QApplication::desktop();

    /* If thats virtual desktop: */
    if (pDesktopWidget->isVirtualDesktop())
    {
        /* If thats Xinerama desktop: */
        if (XineramaIsActive(pDisplay))
        {
            /* Get desktops list: */
            QList<QRect> desktops = XGetDesktopList();

            /* Combine to get full virtual region: */
            QRegion virtualRegion;
            foreach (QRect desktop, desktops)
                virtualRegion += desktop;
            virtualRegion = virtualRegion.boundingRect();

            /* Remember initial virtual desktop: */
            QRect virtualDesktop = virtualRegion.boundingRect();
            //AssertMsgFailed(("LOG... Virtual desktop is: %dx%dx%dx%d\n", virtualDesktop.x(), virtualDesktop.y(),
            //                                                             virtualDesktop.width(), virtualDesktop.height()));

            /* Set available geometry to screen geometry initially: */
            result = desktops[iScreen];

            /* Feat available geometry of virtual desktop to respect all the struts: */
            QList<Window> list = XGetWindowIDList();
            for (int i = 0; i < list.size(); ++ i)
            {
                /* Get window: */
                Window wid = list[i];
                QList<ulong> struts = XGetStrut(wid);

                /* If window has strut: */
                if (struts.size())
                {
                    ulong uLeftShift = struts[0];
                    ulong uLeftFromY = struts[4];
                    ulong uLeftToY = struts[5];

                    ulong uRightShift = struts[1];
                    ulong uRightFromY = struts[6];
                    ulong uRightToY = struts[7];

                    ulong uTopShift = struts[2];
                    ulong uTopFromX = struts[8];
                    ulong uTopToX = struts[9];

                    ulong uBottomShift = struts[3];
                    ulong uBottomFromX = struts[10];
                    ulong uBottomToX = struts[11];

                    if (uLeftShift)
                    {
                        QRect sr(QPoint(0, uLeftFromY),
                                 QSize(uLeftShift, uLeftToY - uLeftFromY + 1));

                        //AssertMsgFailed(("LOG... Subtract left strut: top-left: %dx%d, size: %dx%d\n", sr.x(), sr.y(), sr.width(), sr.height()));
                        virtualRegion -= sr;
                    }

                    if (uRightShift)
                    {
                        QRect sr(QPoint(virtualDesktop.x() + virtualDesktop.width() - uRightShift, uRightFromY),
                                 QSize(virtualDesktop.x() + virtualDesktop.width(), uRightToY - uRightFromY + 1));

                        //AssertMsgFailed(("LOG... Subtract right strut: top-left: %dx%d, size: %dx%d\n", sr.x(), sr.y(), sr.width(), sr.height()));
                        virtualRegion -= sr;
                    }

                    if (uTopShift)
                    {
                        QRect sr(QPoint(uTopFromX, 0),
                                 QSize(uTopToX - uTopFromX + 1, uTopShift));

                        //AssertMsgFailed(("LOG... Subtract top strut: top-left: %dx%d, size: %dx%d\n", sr.x(), sr.y(), sr.width(), sr.height()));
                        virtualRegion -= sr;
                    }

                    if (uBottomShift)
                    {
                        QRect sr(QPoint(uBottomFromX, virtualDesktop.y() + virtualDesktop.height() - uBottomShift),
                                 QSize(uBottomToX - uBottomFromX + 1, uBottomShift));

                        //AssertMsgFailed(("LOG... Subtract bottom strut: top-left: %dx%d, size: %dx%d\n", sr.x(), sr.y(), sr.width(), sr.height()));
                        virtualRegion -= sr;
                    }
                }
            }

            /* Get final available geometry: */
            result = (virtualRegion & result).boundingRect();
        }
    }

    /* If result is still NULL: */
    if (result.isNull())
    {
        /* Use QT default functionality: */
        result = pDesktopWidget->availableGeometry(iScreen);
    }

    //AssertMsgFailed(("LOG... Final geometry: %dx%dx%dx%d\n", result.x(), result.y(), result.width(), result.height()));

#else /* ifdef Q_WS_X11 */

    result = QApplication::desktop()->availableGeometry(iScreen);

#endif /* ifndef Q_WS_X11 */

    return result;
}

/**
 *  Returns the list of few guest OS types, queried from
 *  IVirtualBox corresponding to every family id.
 */
QList <CGuestOSType> VBoxGlobal::vmGuestOSFamilyList() const
{
    QList <CGuestOSType> result;
    for (int i = 0 ; i < mFamilyIDs.size(); ++ i)
        result << mTypes [i][0];
    return result;
}

/**
 *  Returns the list of all guest OS types, queried from
 *  IVirtualBox corresponding to passed family id.
 */
QList <CGuestOSType> VBoxGlobal::vmGuestOSTypeList (const QString &aFamilyId) const
{
    AssertMsg (mFamilyIDs.contains (aFamilyId), ("Family ID incorrect: '%s'.", aFamilyId.toLatin1().constData()));
    return mFamilyIDs.contains (aFamilyId) ?
           mTypes [mFamilyIDs.indexOf (aFamilyId)] : QList <CGuestOSType>();
}

/**
 *  Returns the icon corresponding to the given guest OS type id.
 */
QPixmap VBoxGlobal::vmGuestOSTypeIcon (const QString &aTypeId) const
{
    static const QPixmap none;
    QPixmap *p = mOsTypeIcons.value (aTypeId);
    AssertMsg (p, ("Icon for type '%s' must be defined.", aTypeId.toLatin1().constData()));
    return p ? *p : none;
}

/**
 *  Returns the guest OS type object corresponding to the given type id of list
 *  containing OS types related to OS family determined by family id attribute.
 *  If the index is invalid a null object is returned.
 */
CGuestOSType VBoxGlobal::vmGuestOSType (const QString &aTypeId,
             const QString &aFamilyId /* = QString::null */) const
{
    QList <CGuestOSType> list;
    if (mFamilyIDs.contains (aFamilyId))
    {
        list = mTypes [mFamilyIDs.indexOf (aFamilyId)];
    }
    else
    {
        for (int i = 0; i < mFamilyIDs.size(); ++ i)
            list += mTypes [i];
    }
    for (int j = 0; j < list.size(); ++ j)
        if (!list [j].GetId().compare (aTypeId))
            return list [j];
    AssertMsgFailed (("Type ID incorrect: '%s'.", aTypeId.toLatin1().constData()));
    return CGuestOSType();
}

/**
 *  Returns the description corresponding to the given guest OS type id.
 */
QString VBoxGlobal::vmGuestOSTypeDescription (const QString &aTypeId) const
{
    for (int i = 0; i < mFamilyIDs.size(); ++ i)
    {
        QList <CGuestOSType> list (mTypes [i]);
        for ( int j = 0; j < list.size(); ++ j)
            if (!list [j].GetId().compare (aTypeId))
                return list [j].GetDescription();
    }
    return QString::null;
}

/**
 * Returns a string representation of the given channel number on the given storage bus.
 * Complementary to #toStorageChannel (KStorageBus, const QString &) const.
 */
QString VBoxGlobal::toString (KStorageBus aBus, LONG aChannel) const
{
    QString channel;

    switch (aBus)
    {
        case KStorageBus_IDE:
        {
            if (aChannel == 0 || aChannel == 1)
            {
                channel = mStorageBusChannels [aChannel];
                break;
            }
            AssertMsgFailed (("Invalid IDE channel %d\n", aChannel));
            break;
        }
        case KStorageBus_SATA:
        case KStorageBus_SCSI:
        {
            channel = mStorageBusChannels [2].arg (aChannel);
            break;
        }
        case KStorageBus_Floppy:
        {
            AssertMsgFailed (("Floppy have no channels, only devices\n"));
            break;
        }
        default:
        {
            AssertMsgFailed (("Invalid bus type %d\n", aBus));
            break;
        }
    }

    Assert (!channel.isNull());
    return channel;
}

/**
 * Returns a channel number on the given storage bus corresponding to the given string representation.
 * Complementary to #toString (KStorageBus, LONG) const.
 */
LONG VBoxGlobal::toStorageChannel (KStorageBus aBus, const QString &aChannel) const
{
    LONG channel = 0;

    switch (aBus)
    {
        case KStorageBus_IDE:
        {
            QLongStringHash::const_iterator it = qFind (mStorageBusChannels.begin(), mStorageBusChannels.end(), aChannel);
            AssertMsgBreak (it != mStorageBusChannels.end(), ("No value for {%s}\n", aChannel.toLatin1().constData()));
            channel = it.key();
            break;
        }
        case KStorageBus_SATA:
        case KStorageBus_SCSI:
        {
            QString tpl = mStorageBusChannels [2].arg ("");
            if (aChannel.startsWith (tpl))
            {
                channel = aChannel.right (aChannel.length() - tpl.length()).toLong();
                break;
            }
            AssertMsgFailed (("Invalid channel {%s}\n", aChannel.toLatin1().constData()));
            break;
        }
        case KStorageBus_Floppy:
        {
            channel = 0;
            break;
        }
        default:
        {
            AssertMsgFailed (("Invalid bus type %d\n", aBus));
            break;
        }
    }

    return channel;
}

/**
 * Returns a string representation of the given device number of the given channel on the given storage bus.
 * Complementary to #toStorageDevice (KStorageBus, LONG, const QString &) const.
 */
QString VBoxGlobal::toString (KStorageBus aBus, LONG aChannel, LONG aDevice) const
{
    NOREF (aChannel);

    QString device;

    switch (aBus)
    {
        case KStorageBus_IDE:
        {
            if (aDevice == 0 || aDevice == 1)
            {
                device = mStorageBusDevices [aDevice];
                break;
            }
            AssertMsgFailed (("Invalid device %d\n", aDevice));
            break;
        }
        case KStorageBus_SATA:
        case KStorageBus_SCSI:
        {
            AssertMsgFailed (("SATA & SCSI have no devices, only channels\n"));
            break;
        }
        case KStorageBus_Floppy:
        {
            AssertMsgBreak (aChannel == 0, ("Invalid channel %d\n", aChannel));
            device = mStorageBusDevices [2].arg (aDevice);
            break;
        }
        default:
        {
            AssertMsgFailed (("Invalid bus type %d\n", aBus));
            break;
        }
    }

    Assert (!device.isNull());
    return device;
}

/**
 * Returns a device number of the given channel on the given storage bus corresponding to the given string representation.
 * Complementary to #toString (KStorageBus, LONG, LONG) const.
 */
LONG VBoxGlobal::toStorageDevice (KStorageBus aBus, LONG aChannel, const QString &aDevice) const
{
    NOREF (aChannel);

    LONG device = 0;

    switch (aBus)
    {
        case KStorageBus_IDE:
        {
            QLongStringHash::const_iterator it = qFind (mStorageBusDevices.begin(), mStorageBusDevices.end(), aDevice);
            AssertMsgBreak (it != mStorageBusDevices.end(), ("No value for {%s}", aDevice.toLatin1().constData()));
            device = it.key();
            break;
        }
        case KStorageBus_SATA:
        case KStorageBus_SCSI:
        {
            device = 0;
            break;
        }
        case KStorageBus_Floppy:
        {
            AssertMsgBreak (aChannel == 0, ("Invalid channel %d\n", aChannel));
            QString tpl = mStorageBusDevices [2].arg ("");
            if (aDevice.startsWith (tpl))
            {
                device = aDevice.right (aDevice.length() - tpl.length()).toLong();
                break;
            }
            AssertMsgFailed (("Invalid device {%s}\n", aDevice.toLatin1().constData()));
            break;
        }
        default:
        {
            AssertMsgFailed (("Invalid bus type %d\n", aBus));
            break;
        }
    }

    return device;
}

/**
 * Returns a full string representation of the given device of the given channel on the given storage bus.
 * This method does not uses any separate string tags related to bus, channel, device, it has own
 * separately translated string tags allowing to translate a full slot name into human readable format
 * to be consistent with i18n.
 * Complementary to #toStorageSlot (const QString &) const.
 */
QString VBoxGlobal::toString (StorageSlot aSlot) const
{
    switch (aSlot.bus)
    {
        case KStorageBus_IDE:
        case KStorageBus_SATA:
        case KStorageBus_SCSI:
        case KStorageBus_Floppy:
        case KStorageBus_SAS:
            break;

        default:
        {
            AssertMsgFailed (("Invalid bus type %d\n", aSlot.bus));
            break;
        }
    }

    int maxPort = virtualBox().GetSystemProperties().GetMaxPortCountForStorageBus (aSlot.bus);
    int maxDevice = virtualBox().GetSystemProperties().GetMaxDevicesPerPortForStorageBus (aSlot.bus);
    if (aSlot.port < 0 || aSlot.port > maxPort)
        AssertMsgFailed (("Invalid port %d\n", aSlot.port));
    if (aSlot.device < 0 || aSlot.device > maxDevice)
        AssertMsgFailed (("Invalid device %d\n", aSlot.device));

    QString result;
    switch (aSlot.bus)
    {
        case KStorageBus_IDE:
        {
            result = mSlotTemplates [aSlot.port * maxDevice + aSlot.device];
            break;
        }
        case KStorageBus_SATA:
        {
            result = mSlotTemplates [4].arg (aSlot.port);
            break;
        }
        case KStorageBus_SCSI:
        {
            result = mSlotTemplates [5].arg (aSlot.port);
            break;
        }
        case KStorageBus_Floppy:
        {
            result = mSlotTemplates [6].arg (aSlot.device);
            break;
        }
        case KStorageBus_SAS:
        {
            result = mSlotTemplates [5].arg (aSlot.port);
            break;
        }
        default:
        {
            AssertMsgFailed (("Invalid bus type %d\n", aSlot.bus));
            break;
        }
    }
    return result;
}

/**
 * Returns a StorageSlot based on the given device of the given channel on the given storage bus.
 * Complementary to #toFullString (StorageSlot) const.
 */
StorageSlot VBoxGlobal::toStorageSlot (const QString &aSlot) const
{
    int index = -1;
    QRegExp regExp;
    for (int i = 0; i < mSlotTemplates.size(); ++ i)
    {
        regExp = QRegExp (i >= 0 && i <= 3 ? mSlotTemplates [i] : mSlotTemplates [i].arg ("(\\d+)"));
        if (regExp.indexIn (aSlot) != -1)
        {
            index = i;
            break;
        }
    }

    StorageSlot result;
    switch (index)
    {
        case 0:
        case 1:
        case 2:
        case 3:
        {
            result.bus = KStorageBus_IDE;
            int maxPort = virtualBox().GetSystemProperties().GetMaxPortCountForStorageBus (result.bus);
            result.port = index / maxPort;
            result.device = index % maxPort;
            break;
        }
        case 4:
        {
            result.bus = KStorageBus_SATA;
            int maxPort = virtualBox().GetSystemProperties().GetMaxPortCountForStorageBus (result.bus);
            result.port = regExp.cap (1).toInt();
            if (result.port < 0 || result.port > maxPort)
                AssertMsgFailed (("Invalid port %d\n", result.port));
            break;
        }
        case 5:
        {
            result.bus = KStorageBus_SCSI;
            int maxPort = virtualBox().GetSystemProperties().GetMaxPortCountForStorageBus (result.bus);
            result.port = regExp.cap (1).toInt();
            if (result.port < 0 || result.port > maxPort)
                AssertMsgFailed (("Invalid port %d\n", result.port));
            break;
        }
        case 6:
        {
            result.bus = KStorageBus_Floppy;
            int maxDevice = virtualBox().GetSystemProperties().GetMaxDevicesPerPortForStorageBus (result.bus);
            result.device = regExp.cap (1).toInt();
            if (result.device < 0 || result.device > maxDevice)
                AssertMsgFailed (("Invalid device %d\n", result.device));
            break;
        }
        default:
            break;
    }
    return result;
}

/**
 * Returns the list of all device types (VirtualBox::DeviceType COM enum).
 */
QStringList VBoxGlobal::deviceTypeStrings() const
{
    static QStringList list;
    if (list.empty())
        for (QULongStringHash::const_iterator it = mDeviceTypes.begin();
             it != mDeviceTypes.end(); ++ it)
            list += it.value();
    return list;
}

struct PortConfig
{
    const char *name;
    const ulong IRQ;
    const ulong IOBase;
};

static const PortConfig kComKnownPorts[] =
{
    { "COM1", 4, 0x3F8 },
    { "COM2", 3, 0x2F8 },
    { "COM3", 4, 0x3E8 },
    { "COM4", 3, 0x2E8 },
    /* must not contain an element with IRQ=0 and IOBase=0 used to cause
     * toCOMPortName() to return the "User-defined" string for these values. */
};

static const PortConfig kLptKnownPorts[] =
{
    { "LPT1", 7, 0x3BC },
    { "LPT2", 5, 0x378 },
    { "LPT3", 5, 0x278 },
    /* must not contain an element with IRQ=0 and IOBase=0 used to cause
     * toLPTPortName() to return the "User-defined" string for these values. */
};

/**
 *  Returns the list of the standard COM port names (i.e. "COMx").
 */
QStringList VBoxGlobal::COMPortNames() const
{
    QStringList list;
    for (size_t i = 0; i < RT_ELEMENTS (kComKnownPorts); ++ i)
        list << kComKnownPorts [i].name;

    return list;
}

/**
 *  Returns the list of the standard LPT port names (i.e. "LPTx").
 */
QStringList VBoxGlobal::LPTPortNames() const
{
    QStringList list;
    for (size_t i = 0; i < RT_ELEMENTS (kLptKnownPorts); ++ i)
        list << kLptKnownPorts [i].name;

    return list;
}

/**
 *  Returns the name of the standard COM port corresponding to the given
 *  parameters, or "User-defined" (which is also returned when both
 *  @a aIRQ and @a aIOBase are 0).
 */
QString VBoxGlobal::toCOMPortName (ulong aIRQ, ulong aIOBase) const
{
    for (size_t i = 0; i < RT_ELEMENTS (kComKnownPorts); ++ i)
        if (kComKnownPorts [i].IRQ == aIRQ &&
            kComKnownPorts [i].IOBase == aIOBase)
            return kComKnownPorts [i].name;

    return mUserDefinedPortName;
}

/**
 *  Returns the name of the standard LPT port corresponding to the given
 *  parameters, or "User-defined" (which is also returned when both
 *  @a aIRQ and @a aIOBase are 0).
 */
QString VBoxGlobal::toLPTPortName (ulong aIRQ, ulong aIOBase) const
{
    for (size_t i = 0; i < RT_ELEMENTS (kLptKnownPorts); ++ i)
        if (kLptKnownPorts [i].IRQ == aIRQ &&
            kLptKnownPorts [i].IOBase == aIOBase)
            return kLptKnownPorts [i].name;

    return mUserDefinedPortName;
}

/**
 *  Returns port parameters corresponding to the given standard COM name.
 *  Returns @c true on success, or @c false if the given port name is not one
 *  of the standard names (i.e. "COMx").
 */
bool VBoxGlobal::toCOMPortNumbers (const QString &aName, ulong &aIRQ,
                                   ulong &aIOBase) const
{
    for (size_t i = 0; i < RT_ELEMENTS (kComKnownPorts); ++ i)
        if (strcmp (kComKnownPorts [i].name, aName.toUtf8().data()) == 0)
        {
            aIRQ = kComKnownPorts [i].IRQ;
            aIOBase = kComKnownPorts [i].IOBase;
            return true;
        }

    return false;
}

/**
 *  Returns port parameters corresponding to the given standard LPT name.
 *  Returns @c true on success, or @c false if the given port name is not one
 *  of the standard names (i.e. "LPTx").
 */
bool VBoxGlobal::toLPTPortNumbers (const QString &aName, ulong &aIRQ,
                                   ulong &aIOBase) const
{
    for (size_t i = 0; i < RT_ELEMENTS (kLptKnownPorts); ++ i)
        if (strcmp (kLptKnownPorts [i].name, aName.toUtf8().data()) == 0)
        {
            aIRQ = kLptKnownPorts [i].IRQ;
            aIOBase = kLptKnownPorts [i].IOBase;
            return true;
        }

    return false;
}

/**
 * Searches for the given hard disk in the list of known media descriptors and
 * calls VBoxMedium::details() on the found desriptor.
 *
 * If the requeststed hard disk is not found (for example, it's a new hard disk
 * for a new VM created outside our UI), then media enumeration is requested and
 * the search is repeated. We assume that the secont attempt always succeeds and
 * assert otherwise.
 *
 * @note Technically, the second attempt may fail if, for example, the new hard
 *       passed to this method disk gets removed before #startEnumeratingMedia()
 *       succeeds. This (unexpected object uninitialization) is a generic
 *       problem though and needs to be addressed using exceptions (see also the
 *       @todo in VBoxMedium::details()).
 */
QString VBoxGlobal::details (const CMedium &aMedium, bool aPredictDiff)
{
    CMedium cmedium (aMedium);
    VBoxMedium medium;

    if (!findMedium (cmedium, medium))
    {
        /* Medium may be new and not already in the media list, request refresh */
        startEnumeratingMedia();
        if (!findMedium (cmedium, medium))
            /* Medium might be deleted already, return null string */
            return QString();
    }

    return medium.detailsHTML (true /* aNoDiffs */, aPredictDiff);
}

/**
 *  Returns the details of the given USB device as a single-line string.
 */
QString VBoxGlobal::details (const CUSBDevice &aDevice) const
{
    QString sDetails;
    QString m = aDevice.GetManufacturer().trimmed();
    QString p = aDevice.GetProduct().trimmed();
    if (m.isEmpty() && p.isEmpty())
    {
        sDetails =
            tr ("Unknown device %1:%2", "USB device details")
            .arg (QString().sprintf ("%04hX", aDevice.GetVendorId()))
            .arg (QString().sprintf ("%04hX", aDevice.GetProductId()));
    }
    else
    {
        if (p.toUpper().startsWith (m.toUpper()))
            sDetails = p;
        else
            sDetails = m + " " + p;
    }
    ushort r = aDevice.GetRevision();
    if (r != 0)
        sDetails += QString().sprintf (" [%04hX]", r);

    return sDetails.trimmed();
}

/**
 *  Returns the multi-line description of the given USB device.
 */
QString VBoxGlobal::toolTip (const CUSBDevice &aDevice) const
{
    QString tip =
        tr ("<nobr>Vendor ID: %1</nobr><br>"
            "<nobr>Product ID: %2</nobr><br>"
            "<nobr>Revision: %3</nobr>", "USB device tooltip")
        .arg (QString().sprintf ("%04hX", aDevice.GetVendorId()))
        .arg (QString().sprintf ("%04hX", aDevice.GetProductId()))
        .arg (QString().sprintf ("%04hX", aDevice.GetRevision()));

    QString ser = aDevice.GetSerialNumber();
    if (!ser.isEmpty())
        tip += QString (tr ("<br><nobr>Serial No. %1</nobr>", "USB device tooltip"))
                        .arg (ser);

    /* add the state field if it's a host USB device */
    CHostUSBDevice hostDev (aDevice);
    if (!hostDev.isNull())
    {
        tip += QString (tr ("<br><nobr>State: %1</nobr>", "USB device tooltip"))
                        .arg (vboxGlobal().toString (hostDev.GetState()));
    }

    return tip;
}

/**
 *  Returns the multi-line description of the given USB filter
 */
QString VBoxGlobal::toolTip (const CUSBDeviceFilter &aFilter) const
{
    QString tip;

    QString vendorId = aFilter.GetVendorId();
    if (!vendorId.isEmpty())
        tip += tr ("<nobr>Vendor ID: %1</nobr>", "USB filter tooltip")
                   .arg (vendorId);

    QString productId = aFilter.GetProductId();
    if (!productId.isEmpty())
        tip += tip.isEmpty() ? "":"<br/>" + tr ("<nobr>Product ID: %2</nobr>", "USB filter tooltip")
                                                .arg (productId);

    QString revision = aFilter.GetRevision();
    if (!revision.isEmpty())
        tip += tip.isEmpty() ? "":"<br/>" + tr ("<nobr>Revision: %3</nobr>", "USB filter tooltip")
                                                .arg (revision);

    QString product = aFilter.GetProduct();
    if (!product.isEmpty())
        tip += tip.isEmpty() ? "":"<br/>" + tr ("<nobr>Product: %4</nobr>", "USB filter tooltip")
                                                .arg (product);

    QString manufacturer = aFilter.GetManufacturer();
    if (!manufacturer.isEmpty())
        tip += tip.isEmpty() ? "":"<br/>" + tr ("<nobr>Manufacturer: %5</nobr>", "USB filter tooltip")
                                                .arg (manufacturer);

    QString serial = aFilter.GetSerialNumber();
    if (!serial.isEmpty())
        tip += tip.isEmpty() ? "":"<br/>" + tr ("<nobr>Serial No.: %1</nobr>", "USB filter tooltip")
                                                .arg (serial);

    QString port = aFilter.GetPort();
    if (!port.isEmpty())
        tip += tip.isEmpty() ? "":"<br/>" + tr ("<nobr>Port: %1</nobr>", "USB filter tooltip")
                                                .arg (port);

    /* add the state field if it's a host USB device */
    CHostUSBDevice hostDev (aFilter);
    if (!hostDev.isNull())
    {
        tip += tip.isEmpty() ? "":"<br/>" + tr ("<nobr>State: %1</nobr>", "USB filter tooltip")
                                                .arg (vboxGlobal().toString (hostDev.GetState()));
    }

    return tip;
}

/**
 * Returns a details report on a given VM represented as a HTML table.
 *
 * @param aMachine      Machine to create a report for.
 * @param aWithLinks    @c true if section titles should be hypertext links.
 */
QString VBoxGlobal::detailsReport (const CMachine &aMachine, bool aWithLinks)
{
    /* Details templates */
    static const char *sTableTpl =
        "<table border=0 cellspacing=1 cellpadding=0>%1</table>";
    static const char *sSectionHrefTpl =
        "<tr><td width=22 rowspan=%1 align=left><img src='%2'></td>"
            "<td colspan=3><b><a href='%3'><nobr>%4</nobr></a></b></td></tr>"
            "%5"
        "<tr><td colspan=3><font size=1>&nbsp;</font></td></tr>";
    static const char *sSectionBoldTpl =
        "<tr><td width=22 rowspan=%1 align=left><img src='%2'></td>"
            "<td colspan=3><!-- %3 --><b><nobr>%4</nobr></b></td></tr>"
            "%5"
        "<tr><td colspan=3><font size=1>&nbsp;</font></td></tr>";
    static const char *sSectionItemTpl1 =
        "<tr><td width=40%><nobr><i>%1</i></nobr></td><td/><td/></tr>";
    static const char *sSectionItemTpl2 =
        "<tr><td width=40%><nobr>%1:</nobr></td><td/><td>%2</td></tr>";
    static const char *sSectionItemTpl3 =
        "<tr><td width=40%><nobr>%1</nobr></td><td/><td/></tr>";

    const QString &sectionTpl = aWithLinks ? sSectionHrefTpl : sSectionBoldTpl;

    /* Compose details report */
    QString report;

    /* General */
    {
        QString item = QString (sSectionItemTpl2).arg (tr ("Name", "details report"),
                                                       aMachine.GetName())
                     + QString (sSectionItemTpl2).arg (tr ("OS Type", "details report"),
                                                       vmGuestOSTypeDescription (aMachine.GetOSTypeId()));

        report += sectionTpl
                  .arg (2 + 2) /* rows */
                  .arg (":/machine_16px.png", /* icon */
                        "#general", /* link */
                        tr ("General", "details report"), /* title */
                        item); /* items */
    }

    /* System */
    {
        /* BIOS Settings holder */
        CBIOSSettings biosSettings = aMachine.GetBIOSSettings();

        /* System details row count: */
        int iRowCount = 2; /* Memory & CPU details rows initially. */

        /* Boot order */
        QString bootOrder;
        for (ulong i = 1; i <= mVBox.GetSystemProperties().GetMaxBootPosition(); ++ i)
        {
            KDeviceType device = aMachine.GetBootOrder (i);
            if (device == KDeviceType_Null)
                continue;
            if (!bootOrder.isEmpty())
                bootOrder += ", ";
            bootOrder += toString (device);
        }
        if (bootOrder.isEmpty())
            bootOrder = toString (KDeviceType_Null);

        iRowCount += 1; /* Boot-order row. */

#ifdef VBOX_WITH_FULL_DETAILS_REPORT
        /* ACPI */
        QString acpi = biosSettings.GetACPIEnabled()
            ? tr ("Enabled", "details report (ACPI)")
            : tr ("Disabled", "details report (ACPI)");

        /* IO APIC */
        QString ioapic = biosSettings.GetIOAPICEnabled()
            ? tr ("Enabled", "details report (IO APIC)")
            : tr ("Disabled", "details report (IO APIC)");

        /* PAE/NX */
        QString pae = aMachine.GetCpuProperty(KCpuPropertyType_PAE)
            ? tr ("Enabled", "details report (PAE/NX)")
            : tr ("Disabled", "details report (PAE/NX)");

        iRowCount += 3; /* Full report rows. */
#endif /* VBOX_WITH_FULL_DETAILS_REPORT */

        /* VT-x/AMD-V */
        QString virt = aMachine.GetHWVirtExProperty(KHWVirtExPropertyType_Enabled)
            ? tr ("Enabled", "details report (VT-x/AMD-V)")
            : tr ("Disabled", "details report (VT-x/AMD-V)");

        /* Nested Paging */
        QString nested = aMachine.GetHWVirtExProperty(KHWVirtExPropertyType_NestedPaging)
            ? tr ("Enabled", "details report (Nested Paging)")
            : tr ("Disabled", "details report (Nested Paging)");

        /* VT-x/AMD-V availability: */
        bool fVTxAMDVSupported = virtualBox().GetHost().GetProcessorFeature(KProcessorFeature_HWVirtEx);

        if (fVTxAMDVSupported)
            iRowCount += 2; /* VT-x/AMD-V items. */

        QString item = QString (sSectionItemTpl2).arg (tr ("Base Memory", "details report"),
                                                       tr ("<nobr>%1 MB</nobr>", "details report"))
                       .arg (aMachine.GetMemorySize())
                     + QString (sSectionItemTpl2).arg (tr ("Processor(s)", "details report"),
                                                       tr ("<nobr>%1</nobr>", "details report"))
                       .arg (aMachine.GetCPUCount())
                     + QString (sSectionItemTpl2).arg (tr ("Boot Order", "details report"), bootOrder)
#ifdef VBOX_WITH_FULL_DETAILS_REPORT
                     + QString (sSectionItemTpl2).arg (tr ("ACPI", "details report"), acpi)
                     + QString (sSectionItemTpl2).arg (tr ("IO APIC", "details report"), ioapic)
                     + QString (sSectionItemTpl2).arg (tr ("PAE/NX", "details report"), pae)
#endif /* VBOX_WITH_FULL_DETAILS_REPORT */
                     ;

        if (fVTxAMDVSupported)
                item += QString (sSectionItemTpl2).arg (tr ("VT-x/AMD-V", "details report"), virt)
                     +  QString (sSectionItemTpl2).arg (tr ("Nested Paging", "details report"), nested);

        report += sectionTpl
                  .arg (2 + iRowCount) /* rows */
                  .arg (":/chipset_16px.png", /* icon */
                        "#system", /* link */
                        tr ("System", "details report"), /* title */
                        item); /* items */
    }

    /* Display */
    {
        /* Rows including section header and footer */
        int rows = 2;

        /* Video tab */
        QString acc3d = aMachine.GetAccelerate3DEnabled()
            ? tr ("Enabled", "details report (3D Acceleration)")
            : tr ("Disabled", "details report (3D Acceleration)");

        QString item = QString (sSectionItemTpl2)
                       .arg (tr ("Video Memory", "details report"),
                             tr ("<nobr>%1 MB</nobr>", "details report"))
                       .arg (aMachine.GetVRAMSize())
                     + QString (sSectionItemTpl2)
                       .arg (tr ("3D Acceleration", "details report"), acc3d);

        rows += 2;

#ifdef VBOX_WITH_VIDEOHWACCEL
        QString acc2dVideo = aMachine.GetAccelerate2DVideoEnabled()
            ? tr ("Enabled", "details report (2D Video Acceleration)")
            : tr ("Disabled", "details report (2D Video Acceleration)");

        item += QString (sSectionItemTpl2)
                .arg (tr ("2D Video Acceleration", "details report"), acc2dVideo);
        ++ rows;
#endif

        /* VRDP tab */
        CVRDPServer srv = aMachine.GetVRDPServer();
        if (!srv.isNull())
        {
            if (srv.GetEnabled())
                item += QString (sSectionItemTpl2)
                        .arg (tr ("Remote Display Server Port", "details report (VRDP Server)"))
                        .arg (srv.GetPorts());
            else
                item += QString (sSectionItemTpl2)
                        .arg (tr ("Remote Display Server", "details report (VRDP Server)"))
                        .arg (tr ("Disabled", "details report (VRDP Server)"));
            ++ rows;
        }

        report += sectionTpl
            .arg (rows) /* rows */
            .arg (":/vrdp_16px.png", /* icon */
                  "#display", /* link */
                  tr ("Display", "details report"), /* title */
                  item); /* items */
    }

    /* Storage */
    {
        /* Rows including section header and footer */
        int rows = 2;

        QString item;

        CStorageControllerVector controllers = aMachine.GetStorageControllers();
        foreach (const CStorageController &controller, controllers)
        {
            item += QString (sSectionItemTpl3).arg (controller.GetName());
            ++ rows;

            CMediumAttachmentVector attachments = aMachine.GetMediumAttachmentsOfController (controller.GetName());
            foreach (const CMediumAttachment &attachment, attachments)
            {
                CMedium medium = attachment.GetMedium();
                if (attachment.isOk())
                {
                    /* Append 'device slot name' with 'device type name' for CD/DVD devices only */
                    QString strDeviceType = attachment.GetType() == KDeviceType_DVD ? tr("(CD/DVD)") : QString();
                    if (!strDeviceType.isNull()) strDeviceType.prepend(' ');
                    item += QString (sSectionItemTpl2)
                            .arg (QString ("&nbsp;&nbsp;") +
                                  toString (StorageSlot (controller.GetBus(),
                                                         attachment.GetPort(),
                                                         attachment.GetDevice())) +
                                  strDeviceType)
                            .arg (details (medium, false));
                    ++ rows;
                }
            }
        }

        if (item.isNull())
        {
            item = QString (sSectionItemTpl1)
                   .arg (tr ("Not Attached", "details report (Storage)"));
            ++ rows;
        }

        report += sectionTpl
            .arg (rows) /* rows */
            .arg (":/attachment_16px.png", /* icon */
                  "#storage", /* link */
                  tr ("Storage", "details report"), /* title */
                  item); /* items */
    }

    /* Audio */
    {
        QString item;

        CAudioAdapter audio = aMachine.GetAudioAdapter();
        int rows = audio.GetEnabled() ? 3 : 2;
        if (audio.GetEnabled())
            item = QString (sSectionItemTpl2)
                   .arg (tr ("Host Driver", "details report (audio)"),
                         toString (audio.GetAudioDriver())) +
                   QString (sSectionItemTpl2)
                   .arg (tr ("Controller", "details report (audio)"),
                         toString (audio.GetAudioController()));
        else
            item = QString (sSectionItemTpl1)
                   .arg (tr ("Disabled", "details report (audio)"));

        report += sectionTpl
            .arg (rows + 1) /* rows */
            .arg (":/sound_16px.png", /* icon */
                  "#audio", /* link */
                  tr ("Audio", "details report"), /* title */
                  item); /* items */
    }

    /* Network */
    {
        QString item;

        ulong count = mVBox.GetSystemProperties().GetNetworkAdapterCount();
        int rows = 2; /* including section header and footer */
        for (ulong slot = 0; slot < count; slot ++)
        {
            CNetworkAdapter adapter = aMachine.GetNetworkAdapter (slot);
            if (adapter.GetEnabled())
            {
                KNetworkAttachmentType type = adapter.GetAttachmentType();
                QString attType = toString (adapter.GetAdapterType())
                                  .replace (QRegExp ("\\s\\(.+\\)"), " (%1)");
                /* don't use the adapter type string for types that have
                 * an additional symbolic network/interface name field, use
                 * this name instead */
                if (type == KNetworkAttachmentType_Bridged)
                    attType = attType.arg (tr ("Bridged adapter, %1",
                        "details report (network)").arg (adapter.GetHostInterface()));
                else if (type == KNetworkAttachmentType_Internal)
                    attType = attType.arg (tr ("Internal network, '%1'",
                        "details report (network)").arg (adapter.GetInternalNetwork()));
                else if (type == KNetworkAttachmentType_HostOnly)
                    attType = attType.arg (tr ("Host-only adapter, '%1'",
                        "details report (network)").arg (adapter.GetHostInterface()));
                                                               /* ENABLE VDE */
                else if (type == KNetworkAttachmentType_VDE)
                    attType = attType.arg (tr ("VDE network, '%1'",
                        "details report (network)").arg (adapter.GetVDENetwork()));
                                                               /* /ENABLE VDE */
                else
                    attType = attType.arg (vboxGlobal().toString (type));

                item += QString (sSectionItemTpl2)
                        .arg (tr ("Adapter %1", "details report (network)")
                              .arg (adapter.GetSlot() + 1))
                        .arg (attType);
                ++ rows;
            }
        }
        if (item.isNull())
        {
            item = QString (sSectionItemTpl1)
                   .arg (tr ("Disabled", "details report (network)"));
            ++ rows;
        }

        report += sectionTpl
            .arg (rows) /* rows */
            .arg (":/nw_16px.png", /* icon */
                  "#network", /* link */
                  tr ("Network", "details report"), /* title */
                  item); /* items */
    }

    /* Serial Ports */
    {
        QString item;

        ulong count = mVBox.GetSystemProperties().GetSerialPortCount();
        int rows = 2; /* including section header and footer */
        for (ulong slot = 0; slot < count; slot ++)
        {
            CSerialPort port = aMachine.GetSerialPort (slot);
            if (port.GetEnabled())
            {
                KPortMode mode = port.GetHostMode();
                QString data =
                    toCOMPortName (port.GetIRQ(), port.GetIOBase()) + ", ";
                if (mode == KPortMode_HostPipe ||
                    mode == KPortMode_HostDevice ||
                    mode == KPortMode_RawFile)
                    data += QString ("%1 (<nobr>%2</nobr>)")
                            .arg (vboxGlobal().toString (mode))
                            .arg (QDir::toNativeSeparators (port.GetPath()));
                else
                    data += toString (mode);

                item += QString (sSectionItemTpl2)
                        .arg (tr ("Port %1", "details report (serial ports)")
                              .arg (port.GetSlot() + 1))
                        .arg (data);
                ++ rows;
            }
        }
        if (item.isNull())
        {
            item = QString (sSectionItemTpl1)
                   .arg (tr ("Disabled", "details report (serial ports)"));
            ++ rows;
        }

        report += sectionTpl
            .arg (rows) /* rows */
            .arg (":/serial_port_16px.png", /* icon */
                  "#serialPorts", /* link */
                  tr ("Serial Ports", "details report"), /* title */
                  item); /* items */
    }

    /* Parallel Ports */
    {
        QString item;

        ulong count = mVBox.GetSystemProperties().GetParallelPortCount();
        int rows = 2; /* including section header and footer */
        for (ulong slot = 0; slot < count; slot ++)
        {
            CParallelPort port = aMachine.GetParallelPort (slot);
            if (port.GetEnabled())
            {
                QString data =
                    toLPTPortName (port.GetIRQ(), port.GetIOBase()) +
                    QString (" (<nobr>%1</nobr>)")
                    .arg (QDir::toNativeSeparators (port.GetPath()));

                item += QString (sSectionItemTpl2)
                        .arg (tr ("Port %1", "details report (parallel ports)")
                              .arg (port.GetSlot() + 1))
                        .arg (data);
                ++ rows;
            }
        }
        if (item.isNull())
        {
            item = QString (sSectionItemTpl1)
                   .arg (tr ("Disabled", "details report (parallel ports)"));
            ++ rows;
        }

        /* Temporary disabled */
        QString dummy = sectionTpl /* report += sectionTpl */
            .arg (rows) /* rows */
            .arg (":/parallel_port_16px.png", /* icon */
                  "#parallelPorts", /* link */
                  tr ("Parallel Ports", "details report"), /* title */
                  item); /* items */
    }

    /* USB */
    {
        QString item;

        CUSBController ctl = aMachine.GetUSBController();
        if (   !ctl.isNull()
            && ctl.GetProxyAvailable())
        {
            /* the USB controller may be unavailable (i.e. in VirtualBox OSE) */

            if (ctl.GetEnabled())
            {
                CUSBDeviceFilterVector coll = ctl.GetDeviceFilters();
                uint active = 0;
                for (int i = 0; i < coll.size(); ++i)
                    if (coll[i].GetActive())
                        active ++;

                item = QString (sSectionItemTpl2)
                       .arg (tr ("Device Filters", "details report (USB)"),
                             tr ("%1 (%2 active)", "details report (USB)")
                                 .arg (coll.size()).arg (active));
            }
            else
                item = QString (sSectionItemTpl1)
                       .arg (tr ("Disabled", "details report (USB)"));

            report += sectionTpl
                .arg (2 + 1) /* rows */
                .arg (":/usb_16px.png", /* icon */
                      "#usb", /* link */
                      tr ("USB", "details report"), /* title */
                      item); /* items */
        }
    }

    /* Shared Folders */
    {
        QString item;

        ulong count = aMachine.GetSharedFolders().size();
        if (count > 0)
        {
            item = QString (sSectionItemTpl2)
                   .arg (tr ("Shared Folders", "details report (shared folders)"))
                   .arg (count);
        }
        else
            item = QString (sSectionItemTpl1)
                   .arg (tr ("None", "details report (shared folders)"));

        report += sectionTpl
            .arg (2 + 1) /* rows */
            .arg (":/shared_folder_16px.png", /* icon */
                  "#sfolders", /* link */
                  tr ("Shared Folders", "details report"), /* title */
                  item); /* items */
    }

    return QString (sTableTpl). arg (report);
}

QString VBoxGlobal::platformInfo()
{
    QString platform;

#if defined (Q_OS_WIN)
    platform = "win";
#elif defined (Q_OS_LINUX)
    platform = "linux";
#elif defined (Q_OS_MACX)
    platform = "macosx";
#elif defined (Q_OS_OS2)
    platform = "os2";
#elif defined (Q_OS_FREEBSD)
    platform = "freebsd";
#elif defined (Q_OS_SOLARIS)
    platform = "solaris";
#else
    platform = "unknown";
#endif

    /* The format is <system>.<bitness> */
    platform += QString (".%1").arg (ARCH_BITS);

    /* Add more system information */
#if defined (Q_OS_WIN)
    OSVERSIONINFO versionInfo;
    ZeroMemory (&versionInfo, sizeof (OSVERSIONINFO));
    versionInfo.dwOSVersionInfoSize = sizeof (OSVERSIONINFO);
    GetVersionEx (&versionInfo);
    int major = versionInfo.dwMajorVersion;
    int minor = versionInfo.dwMinorVersion;
    int build = versionInfo.dwBuildNumber;
    QString sp = QString::fromUtf16 ((ushort*)versionInfo.szCSDVersion);

    QString distrib;
    if (major == 6)
        distrib = QString ("Windows Vista %1");
    else if (major == 5)
    {
        if (minor == 2)
            distrib = QString ("Windows Server 2003 %1");
        else if (minor == 1)
            distrib = QString ("Windows XP %1");
        else if (minor == 0)
            distrib = QString ("Windows 2000 %1");
        else
            distrib = QString ("Unknown %1");
    }
    else if (major == 4)
    {
        if (minor == 90)
            distrib = QString ("Windows Me %1");
        else if (minor == 10)
            distrib = QString ("Windows 98 %1");
        else if (minor == 0)
            distrib = QString ("Windows 95 %1");
        else
            distrib = QString ("Unknown %1");
    }
    else /** @todo Windows Server 2008 == vista? Probably not... */
        distrib = QString ("Unknown %1");
    distrib = distrib.arg (sp);
    QString version = QString ("%1.%2").arg (major).arg (minor);
    QString kernel = QString ("%1").arg (build);
    platform += QString (" [Distribution: %1 | Version: %2 | Build: %3]")
        .arg (distrib).arg (version).arg (kernel);
#elif defined (Q_OS_LINUX)
    /* Get script path */
    char szAppPrivPath [RTPATH_MAX];
    int rc = RTPathAppPrivateNoArch (szAppPrivPath, sizeof (szAppPrivPath)); NOREF(rc);
    AssertRC (rc);
    /* Run script */
    QByteArray result =
        Process::singleShot (QString (szAppPrivPath) + "/VBoxSysInfo.sh");
    if (!result.isNull())
        platform += QString (" [%1]").arg (QString (result).trimmed());
#else
    /* Use RTSystemQueryOSInfo. */
    char szTmp[256];
    QStringList components;
    int vrc = RTSystemQueryOSInfo (RTSYSOSINFO_PRODUCT, szTmp, sizeof (szTmp));
    if ((RT_SUCCESS (vrc) || vrc == VERR_BUFFER_OVERFLOW) && szTmp[0] != '\0')
        components << QString ("Product: %1").arg (szTmp);
    vrc = RTSystemQueryOSInfo (RTSYSOSINFO_RELEASE, szTmp, sizeof (szTmp));
    if ((RT_SUCCESS (vrc) || vrc == VERR_BUFFER_OVERFLOW) && szTmp[0] != '\0')
        components << QString ("Release: %1").arg (szTmp);
    vrc = RTSystemQueryOSInfo (RTSYSOSINFO_VERSION, szTmp, sizeof (szTmp));
    if ((RT_SUCCESS (vrc) || vrc == VERR_BUFFER_OVERFLOW) && szTmp[0] != '\0')
        components << QString ("Version: %1").arg (szTmp);
    vrc = RTSystemQueryOSInfo (RTSYSOSINFO_SERVICE_PACK, szTmp, sizeof (szTmp));
    if ((RT_SUCCESS (vrc) || vrc == VERR_BUFFER_OVERFLOW) && szTmp[0] != '\0')
        components << QString ("SP: %1").arg (szTmp);
    if (!components.isEmpty())
        platform += QString (" [%1]").arg (components.join (" | "));
#endif

    return platform;
}

#if defined(Q_WS_X11) && !defined(VBOX_OSE)
double VBoxGlobal::findLicenseFile (const QStringList &aFilesList, QRegExp aPattern, QString &aLicenseFile) const
{
    double maxVersionNumber = 0;
    aLicenseFile = "";
    for (int index = 0; index < aFilesList.count(); ++ index)
    {
        aPattern.indexIn (aFilesList [index]);
        QString version = aPattern.cap (1);
        if (maxVersionNumber < version.toDouble())
        {
            maxVersionNumber = version.toDouble();
            aLicenseFile = aFilesList [index];
        }
    }
    return maxVersionNumber;
}

bool VBoxGlobal::showVirtualBoxLicense()
{
    /* get the apps doc path */
    int size = 256;
    char *buffer = (char*) RTMemTmpAlloc (size);
    RTPathAppDocs (buffer, size);
    QString path (buffer);
    RTMemTmpFree (buffer);
    QDir docDir (path);
    docDir.setFilter (QDir::Files);
    docDir.setNameFilters (QStringList ("License-*.html"));

    /* Make sure that the language is in two letter code.
     * Note: if languageId() returns an empty string lang.name() will
     * return "C" which is an valid language code. */
    QLocale lang (VBoxGlobal::languageId());

    QStringList filesList = docDir.entryList();
    QString licenseFile;
    /* First try to find a localized version of the license file. */
    double versionNumber = findLicenseFile (filesList, QRegExp (QString ("License-([\\d\\.]+)-%1.html").arg (lang.name())), licenseFile);
    /* If there wasn't a localized version of the currently selected language,
     * search for the generic one. */
    if (versionNumber == 0)
        versionNumber = findLicenseFile (filesList, QRegExp ("License-([\\d\\.]+).html"), licenseFile);
    /* Check the version again. */
    if (!versionNumber)
    {
        vboxProblem().cannotFindLicenseFiles (path);
        return false;
    }

    /* compose the latest license file full path */
    QString latestVersion = QString::number (versionNumber);
    QString latestFilePath = docDir.absoluteFilePath (licenseFile);

    /* check for the agreed license version */
    QString licenseAgreed = virtualBox().GetExtraData (VBoxDefs::GUI_LicenseKey);
    if (licenseAgreed == latestVersion)
        return true;

    VBoxLicenseViewer licenseDialog (latestFilePath);
    bool result = licenseDialog.exec() == QDialog::Accepted;
    if (result)
        virtualBox().SetExtraData (VBoxDefs::GUI_LicenseKey, latestVersion);
    return result;
}
#endif /* defined(Q_WS_X11) && !defined(VBOX_OSE) */

/**
 *  Opens a direct session for a machine with the given ID.
 *  This method does user-friendly error handling (display error messages, etc.).
 *  and returns a null CSession object in case of any error.
 *  If this method succeeds, don't forget to close the returned session when
 *  it is no more necessary.
 *
 *  @param aId          Machine ID.
 *  @param aExisting    @c true to open an existing session with the machine
 *                      which is already running, @c false to open a new direct
 *                      session.
 */
CSession VBoxGlobal::openSession (const QString &aId, bool aExisting /* = false */)
{
    CSession session;
    session.createInstance (CLSID_Session);
    if (session.isNull())
    {
        vboxProblem().cannotOpenSession (session);
        return session;
    }

    if (aExisting)
        mVBox.OpenExistingSession (session, aId);
    else
    {
        mVBox.OpenSession (session, aId);
        CMachine machine = session.GetMachine ();
        /* Make sure that the language is in two letter code.
         * Note: if languageId() returns an empty string lang.name() will
         * return "C" which is an valid language code. */
        QLocale lang (VBoxGlobal::languageId());
        machine.SetGuestPropertyValue ("/VirtualBox/HostInfo/GUI/LanguageID", lang.name());
    }

    if (!mVBox.isOk())
    {
        CMachine machine = CVirtualBox (mVBox).GetMachine (aId);
        vboxProblem().cannotOpenSession (mVBox, machine);
        session.detach();
    }

    return session;
}

/**
 *  Starts a machine with the given ID.
 */
bool VBoxGlobal::startMachine(const QString &strId)
{
    AssertReturn(mValid, false);

    CSession session = vboxGlobal().openSession(strId);
    if (session.isNull())
        return false;

#ifdef VBOX_WITH_NEW_RUNTIME_CORE
# ifndef VBOX_FORCE_NEW_RUNTIME_CORE_ALWAYS
    if (session.GetMachine().GetMonitorCount() > 1)
# endif /* VBOX_FORCE_NEW_RUNTIME_CORE_ALWAYS */
        return createVirtualMachine(session);
# ifndef VBOX_FORCE_NEW_RUNTIME_CORE_ALWAYS
    else
# endif /* VBOX_FORCE_NEW_RUNTIME_CORE_ALWAYS */
#endif /* VBOX_WITH_NEW_RUNTIME_CORE */
        return consoleWnd().openView(session);
}

/**
 * Appends the NULL medium to the media list.
 * For using with VBoxGlobal::startEnumeratingMedia() only.
 */
static void addNullMediumToList (VBoxMediaList &aList, VBoxMediaList::iterator aWhere)
{
    VBoxMedium medium;
    aList.insert (aWhere, medium);
}

/**
 * Appends the given list of mediums to the media list.
 * For using with VBoxGlobal::startEnumeratingMedia() only.
 */
static void addMediumsToList (const CMediumVector &aVector,
                              VBoxMediaList &aList,
                              VBoxMediaList::iterator aWhere,
                              VBoxDefs::MediumType aType,
                              VBoxMedium *aParent = 0)
{
    VBoxMediaList::iterator first = aWhere;

    for (CMediumVector::ConstIterator it = aVector.begin(); it != aVector.end(); ++ it)
    {
        CMedium cmedium (*it);
        VBoxMedium medium (cmedium, aType, aParent);

        /* Search for a proper alphabetic position */
        VBoxMediaList::iterator jt = first;
        for (; jt != aWhere; ++ jt)
            if ((*jt).name().localeAwareCompare (medium.name()) > 0)
                break;

        aList.insert (jt, medium);

        /* Adjust the first item if inserted before it */
        if (jt == first)
            -- first;
    }
}

/**
 * Appends the given list of hard disks and all their children to the media list.
 * For using with VBoxGlobal::startEnumeratingMedia() only.
 */
static void addHardDisksToList (const CMediumVector &aVector,
                                VBoxMediaList &aList,
                                VBoxMediaList::iterator aWhere,
                                VBoxMedium *aParent = 0)
{
    VBoxMediaList::iterator first = aWhere;

    /* First pass: Add siblings sorted */
    for (CMediumVector::ConstIterator it = aVector.begin(); it != aVector.end(); ++ it)
    {
        CMedium cmedium (*it);
        VBoxMedium medium (cmedium, VBoxDefs::MediumType_HardDisk, aParent);

        /* Search for a proper alphabetic position */
        VBoxMediaList::iterator jt = first;
        for (; jt != aWhere; ++ jt)
            if ((*jt).name().localeAwareCompare (medium.name()) > 0)
                break;

        aList.insert (jt, medium);

        /* Adjust the first item if inserted before it */
        if (jt == first)
            -- first;
    }

    /* Second pass: Add children */
    for (VBoxMediaList::iterator it = first; it != aWhere;)
    {
        CMediumVector children = (*it).medium().GetChildren();
        VBoxMedium *parent = &(*it);

        ++ it; /* go to the next sibling before inserting children */
        addHardDisksToList (children, aList, it, parent);
    }
}

/**
 * Starts a thread that asynchronously enumerates all currently registered
 * media.
 *
 * Before the enumeration is started, the current media list (a list returned by
 * #currentMediaList()) is populated with all registered media and the
 * #mediumEnumStarted() signal is emitted. The enumeration thread then walks this
 * list, checks for media acessiblity and emits #mediumEnumerated() signals of
 * each checked medium. When all media are checked, the enumeration thread is
 * stopped and the #mediumEnumFinished() signal is emitted.
 *
 * If the enumeration is already in progress, no new thread is started.
 *
 * The media list returned by #currentMediaList() is always sorted
 * alphabetically by the location attribute and comes in the following order:
 * <ol>
 *  <li>All hard disks. If a hard disk has children, these children
 *      (alphabetically sorted) immediately follow their parent and terefore
 *      appear before its next sibling hard disk.</li>
 *  <li>All CD/DVD images.</li>
 *  <li>All Floppy images.</li>
 * </ol>
 *
 * Note that #mediumEnumerated() signals are emitted in the same order as
 * described above.
 *
 * @sa #currentMediaList()
 * @sa #isMediaEnumerationStarted()
 */
void VBoxGlobal::startEnumeratingMedia()
{
    AssertReturnVoid (mValid);

    /* check if already started but not yet finished */
    if (mMediaEnumThread != NULL)
        return;

    /* ignore the request during application termination */
    if (sVBoxGlobalInCleanup)
        return;

    /* composes a list of all currently known media & their children */
    mMediaList.clear();
    addNullMediumToList (mMediaList, mMediaList.end());
    addHardDisksToList (mVBox.GetHardDisks(), mMediaList, mMediaList.end());
    addMediumsToList (mVBox.GetHost().GetDVDDrives(), mMediaList, mMediaList.end(), VBoxDefs::MediumType_DVD);
    addMediumsToList (mVBox.GetDVDImages(), mMediaList, mMediaList.end(), VBoxDefs::MediumType_DVD);
    addMediumsToList (mVBox.GetHost().GetFloppyDrives(), mMediaList, mMediaList.end(), VBoxDefs::MediumType_Floppy);
    addMediumsToList (mVBox.GetFloppyImages(), mMediaList, mMediaList.end(), VBoxDefs::MediumType_Floppy);

    /* enumeration thread class */
    class MediaEnumThread : public QThread
    {
    public:

        MediaEnumThread (VBoxMediaList &aList)
            : mVector (aList.size())
            , mSavedIt (aList.begin())
        {
            int i = 0;
            for (VBoxMediaList::const_iterator it = aList.begin();
                 it != aList.end(); ++ it)
                mVector [i ++] = *it;
        }

        virtual void run()
        {
            LogFlow (("MediaEnumThread started.\n"));
            COMBase::InitializeCOM();

            CVirtualBox mVBox = vboxGlobal().virtualBox();
            QObject *self = &vboxGlobal();

            /* Enumerate the list */
            for (int i = 0; i < mVector.size() && !sVBoxGlobalInCleanup; ++ i)
            {
                mVector [i].blockAndQueryState();
                QApplication::
                    postEvent (self,
                               new VBoxMediaEnumEvent (mVector [i], mSavedIt));
            }

            /* Post the end-of-enumeration event */
            if (!sVBoxGlobalInCleanup)
                QApplication::postEvent (self, new VBoxMediaEnumEvent (mSavedIt));

            COMBase::CleanupCOM();
            LogFlow (("MediaEnumThread finished.\n"));
        }

    private:

        QVector <VBoxMedium> mVector;
        VBoxMediaList::iterator mSavedIt;
    };

    mMediaEnumThread = new MediaEnumThread (mMediaList);
    AssertReturnVoid (mMediaEnumThread);

    /* emit mediumEnumStarted() after we set mMediaEnumThread to != NULL
     * to cause isMediaEnumerationStarted() to return TRUE from slots */
    emit mediumEnumStarted();

    mMediaEnumThread->start();
}

/**
 * Adds a new medium to the current media list and emits the #mediumAdded()
 * signal.
 *
 * @sa #currentMediaList()
 */
void VBoxGlobal::addMedium (const VBoxMedium &aMedium)
{
    /* Note that we maitain the same order here as #startEnumeratingMedia() */

    VBoxMediaList::iterator it = mMediaList.begin();

    if (aMedium.type() == VBoxDefs::MediumType_HardDisk)
    {
        VBoxMediaList::iterator itParent = mMediaList.end();

        for (; it != mMediaList.end(); ++ it)
        {
            /* skip null medium that come first */
            if ((*it).isNull()) continue;

            if ((*it).type() != VBoxDefs::MediumType_HardDisk)
                break;

            if (aMedium.parent() != NULL && itParent == mMediaList.end())
            {
                if (&*it == aMedium.parent())
                    itParent = it;
            }
            else
            {
                /* break if met a parent's sibling (will insert before it) */
                if (aMedium.parent() != NULL &&
                    (*it).parent() == (*itParent).parent())
                    break;

                /* compare to aMedium's siblings */
                if ((*it).parent() == aMedium.parent() &&
                    (*it).name().localeAwareCompare (aMedium.name()) > 0)
                    break;
            }
        }

        AssertReturnVoid (aMedium.parent() == NULL || itParent != mMediaList.end());
    }
    else
    {
        for (; it != mMediaList.end(); ++ it)
        {
            /* skip null medium that come first */
            if ((*it).isNull()) continue;

            /* skip HardDisks that come first */
            if ((*it).type() == VBoxDefs::MediumType_HardDisk)
                continue;

            /* skip DVD when inserting Floppy */
            if (aMedium.type() == VBoxDefs::MediumType_Floppy &&
                (*it).type() == VBoxDefs::MediumType_DVD)
                continue;

            if ((*it).name().localeAwareCompare (aMedium.name()) > 0 ||
                (aMedium.type() == VBoxDefs::MediumType_DVD &&
                 (*it).type() == VBoxDefs::MediumType_Floppy))
                break;
        }
    }

    it = mMediaList.insert (it, aMedium);

    emit mediumAdded (*it);
}

/**
 * Updates the medium in the current media list and emits the #mediumUpdated()
 * signal.
 *
 * @sa #currentMediaList()
 */
void VBoxGlobal::updateMedium (const VBoxMedium &aMedium)
{
    VBoxMediaList::Iterator it;
    for (it = mMediaList.begin(); it != mMediaList.end(); ++ it)
        if ((*it).id() == aMedium.id())
            break;

    AssertReturnVoid (it != mMediaList.end());

    if (&*it != &aMedium)
        *it = aMedium;

    emit mediumUpdated (*it);
}

/**
 * Removes the medium from the current media list and emits the #mediumRemoved()
 * signal.
 *
 * @sa #currentMediaList()
 */
void VBoxGlobal::removeMedium (VBoxDefs::MediumType aType, const QString &aId)
{
    VBoxMediaList::Iterator it;
    for (it = mMediaList.begin(); it != mMediaList.end(); ++ it)
        if ((*it).id() == aId)
            break;

    AssertReturnVoid (it != mMediaList.end());

#if DEBUG
    /* sanity: must be no children */
    {
        VBoxMediaList::Iterator jt = it;
        ++ jt;
        AssertReturnVoid (jt == mMediaList.end() || (*jt).parent() != &*it);
    }
#endif

    VBoxMedium *pParent = (*it).parent();

    /* remove the medium from the list to keep it in sync with the server "for
     * free" when the medium is deleted from one of our UIs */
    mMediaList.erase (it);

    emit mediumRemoved (aType, aId);

    /* also emit the parent update signal because some attributes like
     * isReadOnly() may have been changed after child removal */
    if (pParent != NULL)
    {
        pParent->refresh();
        emit mediumUpdated (*pParent);
    }
}

/**
 *  Searches for a VBoxMedum object representing the given COM medium object.
 *
 *  @return true if found and false otherwise.
 */
bool VBoxGlobal::findMedium (const CMedium &aObj, VBoxMedium &aMedium) const
{
    for (VBoxMediaList::ConstIterator it = mMediaList.begin(); it != mMediaList.end(); ++ it)
    {
        if (((*it).medium().isNull() && aObj.isNull()) ||
            (!(*it).medium().isNull() && !aObj.isNull() && (*it).medium().GetId() == aObj.GetId()))
        {
            aMedium = (*it);
            return true;
        }
    }
    return false;
}

/**
 *  Searches for a VBoxMedum object with the given medium id attribute.
 *
 *  @return VBoxMedum if found which is invalid otherwise.
 */
VBoxMedium VBoxGlobal::findMedium (const QString &aMediumId) const
{
    for (VBoxMediaList::ConstIterator it = mMediaList.begin(); it != mMediaList.end(); ++ it)
        if ((*it).id() == aMediumId)
            return *it;
    return VBoxMedium();
}

#ifdef VBOX_GUI_WITH_SYSTRAY
/**
 *  Returns the number of current running Fe/Qt4 main windows.
 *
 *  @return Number of running main windows.
 */
int VBoxGlobal::mainWindowCount ()
{
    return mVBox.GetExtraData (VBoxDefs::GUI_MainWindowCount).toInt();
}
#endif

/**
 *  Native language name of the currently installed translation.
 *  Returns "English" if no translation is installed
 *  or if the translation file is invalid.
 */
QString VBoxGlobal::languageName() const
{

    return qApp->translate ("@@@", "English",
                            "Native language name");
}

/**
 *  Native language country name of the currently installed translation.
 *  Returns "--" if no translation is installed or if the translation file is
 *  invalid, or if the language is independent on the country.
 */
QString VBoxGlobal::languageCountry() const
{
    return qApp->translate ("@@@", "--",
                            "Native language country name "
                            "(empty if this language is for all countries)");
}

/**
 *  Language name of the currently installed translation, in English.
 *  Returns "English" if no translation is installed
 *  or if the translation file is invalid.
 */
QString VBoxGlobal::languageNameEnglish() const
{

    return qApp->translate ("@@@", "English",
                            "Language name, in English");
}

/**
 *  Language country name of the currently installed translation, in English.
 *  Returns "--" if no translation is installed or if the translation file is
 *  invalid, or if the language is independent on the country.
 */
QString VBoxGlobal::languageCountryEnglish() const
{
    return qApp->translate ("@@@", "--",
                            "Language country name, in English "
                            "(empty if native country name is empty)");
}

/**
 *  Comma-separated list of authors of the currently installed translation.
 *  Returns "Sun Microsystems, Inc." if no translation is installed or if the
 *  translation file is invalid, or if the translation is supplied by Sun
 *  Microsystems, inc.
 */
QString VBoxGlobal::languageTranslators() const
{
    return qApp->translate ("@@@", "Oracle Corporation",
                            "Comma-separated list of translators");
}

/**
 *  Changes the language of all global string constants according to the
 *  currently installed translations tables.
 */
void VBoxGlobal::retranslateUi()
{
    mMachineStates [KMachineState_PoweredOff] = tr ("Powered Off", "MachineState");
    mMachineStates [KMachineState_Saved] =      tr ("Saved", "MachineState");
    mMachineStates [KMachineState_Teleported] = tr ("Teleported", "MachineState");
    mMachineStates [KMachineState_Aborted] =    tr ("Aborted", "MachineState");
    mMachineStates [KMachineState_Running] =    tr ("Running", "MachineState");
    mMachineStates [KMachineState_Paused] =     tr ("Paused", "MachineState");
    mMachineStates [KMachineState_Stuck] =      tr ("Guru Meditation", "MachineState");
    mMachineStates [KMachineState_Teleporting] = tr ("Teleporting", "MachineState");
    mMachineStates [KMachineState_LiveSnapshotting] = tr ("Taking Live Snapshot", "MachineState");
    mMachineStates [KMachineState_Starting] =   tr ("Starting", "MachineState");
    mMachineStates [KMachineState_Stopping] =   tr ("Stopping", "MachineState");
    mMachineStates [KMachineState_Saving] =     tr ("Saving", "MachineState");
    mMachineStates [KMachineState_Restoring] =  tr ("Restoring", "MachineState");
    mMachineStates [KMachineState_TeleportingPausedVM] = tr ("Teleporting Paused VM", "MachineState");
    mMachineStates [KMachineState_TeleportingIn] = tr ("Teleporting", "MachineState");
    mMachineStates [KMachineState_RestoringSnapshot] = tr ("Restoring Snapshot", "MachineState");
    mMachineStates [KMachineState_DeletingSnapshot] = tr ("Deleting Snapshot", "MachineState");
    mMachineStates [KMachineState_SettingUp] =  tr ("Setting Up", "MachineState");

    mSessionStates [KSessionState_Closed] =     tr ("Closed", "SessionState");
    mSessionStates [KSessionState_Open] =       tr ("Open", "SessionState");
    mSessionStates [KSessionState_Spawning] =   tr ("Spawning", "SessionState");
    mSessionStates [KSessionState_Closing] =    tr ("Closing", "SessionState");

    mDeviceTypes [KDeviceType_Null] =           tr ("None", "DeviceType");
    mDeviceTypes [KDeviceType_Floppy] =         tr ("Floppy", "DeviceType");
    mDeviceTypes [KDeviceType_DVD] =            tr ("CD/DVD-ROM", "DeviceType");
    mDeviceTypes [KDeviceType_HardDisk] =       tr ("Hard Disk", "DeviceType");
    mDeviceTypes [KDeviceType_Network] =        tr ("Network", "DeviceType");
    mDeviceTypes [KDeviceType_USB] =            tr ("USB", "DeviceType");
    mDeviceTypes [KDeviceType_SharedFolder] =   tr ("Shared Folder", "DeviceType");

    mStorageBuses [KStorageBus_IDE] =       tr ("IDE", "StorageBus");
    mStorageBuses [KStorageBus_SATA] =      tr ("SATA", "StorageBus");
    mStorageBuses [KStorageBus_SCSI] =      tr ("SCSI", "StorageBus");
    mStorageBuses [KStorageBus_Floppy] =    tr ("Floppy", "StorageBus");
    mStorageBuses [KStorageBus_SAS] =      tr ("SAS", "StorageBus");

    mStorageBusChannels [0] = tr ("Primary", "StorageBusChannel");
    mStorageBusChannels [1] = tr ("Secondary", "StorageBusChannel");
    mStorageBusChannels [2] = tr ("Port %1", "StorageBusChannel");

    mStorageBusDevices [0] = tr ("Master", "StorageBusDevice");
    mStorageBusDevices [1] = tr ("Slave", "StorageBusDevice");
    mStorageBusDevices [2] = tr ("Device %1", "StorageBusDevice");

    mSlotTemplates [0] = tr ("IDE Primary Master", "New Storage UI : Slot Name");
    mSlotTemplates [1] = tr ("IDE Primary Slave", "New Storage UI : Slot Name");
    mSlotTemplates [2] = tr ("IDE Secondary Master", "New Storage UI : Slot Name");
    mSlotTemplates [3] = tr ("IDE Secondary Slave", "New Storage UI : Slot Name");
    mSlotTemplates [4] = tr ("SATA Port %1", "New Storage UI : Slot Name");
    mSlotTemplates [5] = tr ("SCSI Port %1", "New Storage UI : Slot Name");
    mSlotTemplates [6] = tr ("Floppy Device %1", "New Storage UI : Slot Name");

    mDiskTypes [KMediumType_Normal] =           tr ("Normal", "DiskType");
    mDiskTypes [KMediumType_Immutable] =        tr ("Immutable", "DiskType");
    mDiskTypes [KMediumType_Writethrough] =     tr ("Writethrough", "DiskType");
    mDiskTypes_Differencing =                   tr ("Differencing", "DiskType");

    mVRDPAuthTypes [KVRDPAuthType_Null] =       tr ("Null", "VRDPAuthType");
    mVRDPAuthTypes [KVRDPAuthType_External] =   tr ("External", "VRDPAuthType");
    mVRDPAuthTypes [KVRDPAuthType_Guest] =      tr ("Guest", "VRDPAuthType");

    mPortModeTypes [KPortMode_Disconnected] =   tr ("Disconnected", "PortMode");
    mPortModeTypes [KPortMode_HostPipe] =       tr ("Host Pipe", "PortMode");
    mPortModeTypes [KPortMode_HostDevice] =     tr ("Host Device", "PortMode");
    mPortModeTypes [KPortMode_RawFile] =        tr ("Raw File", "PortMode");

    mUSBFilterActionTypes [KUSBDeviceFilterAction_Ignore] =
        tr ("Ignore", "USBFilterActionType");
    mUSBFilterActionTypes [KUSBDeviceFilterAction_Hold] =
        tr ("Hold", "USBFilterActionType");

    mAudioDriverTypes [KAudioDriverType_Null] =
        tr ("Null Audio Driver", "AudioDriverType");
    mAudioDriverTypes [KAudioDriverType_WinMM] =
        tr ("Windows Multimedia", "AudioDriverType");
    mAudioDriverTypes [KAudioDriverType_SolAudio] =
        tr ("Solaris Audio", "AudioDriverType");
    mAudioDriverTypes [KAudioDriverType_OSS] =
        tr ("OSS Audio Driver", "AudioDriverType");
    mAudioDriverTypes [KAudioDriverType_ALSA] =
        tr ("ALSA Audio Driver", "AudioDriverType");
    mAudioDriverTypes [KAudioDriverType_DirectSound] =
        tr ("Windows DirectSound", "AudioDriverType");
    mAudioDriverTypes [KAudioDriverType_CoreAudio] =
        tr ("CoreAudio", "AudioDriverType");
    mAudioDriverTypes [KAudioDriverType_Pulse] =
        tr ("PulseAudio", "AudioDriverType");

    mAudioControllerTypes [KAudioControllerType_AC97] =
        tr ("ICH AC97", "AudioControllerType");
    mAudioControllerTypes [KAudioControllerType_SB16] =
        tr ("SoundBlaster 16", "AudioControllerType");

    mNetworkAdapterTypes [KNetworkAdapterType_Am79C970A] =
        tr ("PCnet-PCI II (Am79C970A)", "NetworkAdapterType");
    mNetworkAdapterTypes [KNetworkAdapterType_Am79C973] =
        tr ("PCnet-FAST III (Am79C973)", "NetworkAdapterType");
    mNetworkAdapterTypes [KNetworkAdapterType_I82540EM] =
        tr ("Intel PRO/1000 MT Desktop (82540EM)", "NetworkAdapterType");
    mNetworkAdapterTypes [KNetworkAdapterType_I82543GC] =
        tr ("Intel PRO/1000 T Server (82543GC)", "NetworkAdapterType");
    mNetworkAdapterTypes [KNetworkAdapterType_I82545EM] =
        tr ("Intel PRO/1000 MT Server (82545EM)", "NetworkAdapterType");
#ifdef VBOX_WITH_VIRTIO
    mNetworkAdapterTypes [KNetworkAdapterType_Virtio] =
        tr ("Paravirtualized Network (virtio-net)", "NetworkAdapterType");
#endif /* VBOX_WITH_VIRTIO */

    mNetworkAttachmentTypes [KNetworkAttachmentType_Null] =
        tr ("Not attached", "NetworkAttachmentType");
    mNetworkAttachmentTypes [KNetworkAttachmentType_NAT] =
        tr ("NAT", "NetworkAttachmentType");
    mNetworkAttachmentTypes [KNetworkAttachmentType_Bridged] =
        tr ("Bridged Adapter", "NetworkAttachmentType");
    mNetworkAttachmentTypes [KNetworkAttachmentType_Internal] =
        tr ("Internal Network", "NetworkAttachmentType");
    mNetworkAttachmentTypes [KNetworkAttachmentType_HostOnly] =
        tr ("Host-only Adapter", "NetworkAttachmentType");
               /* ENABLE VDE */
    mNetworkAttachmentTypes [KNetworkAttachmentType_VDE] =
        tr ("VDE Adapter", "NetworkAttachmentType");
               /* /ENABLE VDE */

    mClipboardTypes [KClipboardMode_Disabled] =
        tr ("Disabled", "ClipboardType");
    mClipboardTypes [KClipboardMode_HostToGuest] =
        tr ("Host To Guest", "ClipboardType");
    mClipboardTypes [KClipboardMode_GuestToHost] =
        tr ("Guest To Host", "ClipboardType");
    mClipboardTypes [KClipboardMode_Bidirectional] =
        tr ("Bidirectional", "ClipboardType");

    mStorageControllerTypes [KStorageControllerType_PIIX3] =
        tr ("PIIX3", "StorageControllerType");
    mStorageControllerTypes [KStorageControllerType_PIIX4] =
        tr ("PIIX4", "StorageControllerType");
    mStorageControllerTypes [KStorageControllerType_ICH6] =
        tr ("ICH6", "StorageControllerType");
    mStorageControllerTypes [KStorageControllerType_IntelAhci] =
        tr ("AHCI", "StorageControllerType");
    mStorageControllerTypes [KStorageControllerType_LsiLogic] =
        tr ("Lsilogic", "StorageControllerType");
    mStorageControllerTypes [KStorageControllerType_BusLogic] =
        tr ("BusLogic", "StorageControllerType");
    mStorageControllerTypes [KStorageControllerType_I82078] =
        tr ("I82078", "StorageControllerType");
    mStorageControllerTypes [KStorageControllerType_LsiLogicSas] =
        tr ("LsiLogic SAS", "StorageControllerType");

    mUSBDeviceStates [KUSBDeviceState_NotSupported] =
        tr ("Not supported", "USBDeviceState");
    mUSBDeviceStates [KUSBDeviceState_Unavailable] =
        tr ("Unavailable", "USBDeviceState");
    mUSBDeviceStates [KUSBDeviceState_Busy] =
        tr ("Busy", "USBDeviceState");
    mUSBDeviceStates [KUSBDeviceState_Available] =
        tr ("Available", "USBDeviceState");
    mUSBDeviceStates [KUSBDeviceState_Held] =
        tr ("Held", "USBDeviceState");
    mUSBDeviceStates [KUSBDeviceState_Captured] =
        tr ("Captured", "USBDeviceState");

    mUserDefinedPortName = tr ("User-defined", "serial port");

    mWarningIcon = standardIcon (QStyle::SP_MessageBoxWarning, 0).pixmap (16, 16);
    Assert (!mWarningIcon.isNull());

    mErrorIcon = standardIcon (QStyle::SP_MessageBoxCritical, 0).pixmap (16, 16);
    Assert (!mErrorIcon.isNull());

    /* refresh media properties since they contain some translations too  */
    for (VBoxMediaList::iterator it = mMediaList.begin();
         it != mMediaList.end(); ++ it)
        it->refresh();

#if defined (Q_WS_PM) || defined (Q_WS_X11)
    /* As PM and X11 do not (to my knowledge) have functionality for providing
     * human readable key names, we keep a table of them, which must be
     * updated when the language is changed. */
    QIHotKeyEdit::retranslateUi();
#endif
}

// public static stuff
////////////////////////////////////////////////////////////////////////////////

/* static */
bool VBoxGlobal::isDOSType (const QString &aOSTypeId)
{
    if (aOSTypeId.left (3) == "dos" ||
        aOSTypeId.left (3) == "win" ||
        aOSTypeId.left (3) == "os2")
        return true;

    return false;
}

const char *gVBoxLangSubDir = "/nls";
const char *gVBoxLangFileBase = "VirtualBox_";
const char *gVBoxLangFileExt = ".qm";
const char *gVBoxLangIDRegExp = "(([a-z]{2})(?:_([A-Z]{2}))?)|(C)";
const char *gVBoxBuiltInLangName   = "C";

class VBoxTranslator : public QTranslator
{
public:

    VBoxTranslator (QObject *aParent = 0)
        : QTranslator (aParent) {}

    bool loadFile (const QString &aFileName)
    {
        QFile file (aFileName);
        if (!file.open (QIODevice::ReadOnly))
            return false;
        mData = file.readAll();
        return load ((uchar*) mData.data(), mData.size());
    }

private:

    QByteArray mData;
};

static VBoxTranslator *sTranslator = 0;
static QString sLoadedLangId = gVBoxBuiltInLangName;

/**
 *  Returns the loaded (active) language ID.
 *  Note that it may not match with VBoxGlobalSettings::languageId() if the
 *  specified language cannot be loaded.
 *  If the built-in language is active, this method returns "C".
 *
 *  @note "C" is treated as the built-in language for simplicity -- the C
 *  locale is used in unix environments as a fallback when the requested
 *  locale is invalid. This way we don't need to process both the "built_in"
 *  language and the "C" language (which is a valid environment setting)
 *  separately.
 */
/* static */
QString VBoxGlobal::languageId()
{
    return sLoadedLangId;
}

/**
 *  Loads the language by language ID.
 *
 *  @param aLangId Language ID in in form of xx_YY. QString::null means the
 *                 system default language.
 */
/* static */
void VBoxGlobal::loadLanguage (const QString &aLangId)
{
    QString langId = aLangId.isNull() ?
        VBoxGlobal::systemLanguageId() : aLangId;
    QString languageFileName;
    QString selectedLangId = gVBoxBuiltInLangName;

    /* If C is selected we change it temporary to en. This makes sure any extra
     * "en" translation file will be loaded. This is necessary for loading the
     * plural forms of some of our translations. */
    bool fResetToC = false;
    if (langId == "C")
    {
        langId = "en";
        fResetToC = true;
    }

    char szNlsPath[RTPATH_MAX];
    int rc;

    rc = RTPathAppPrivateNoArch(szNlsPath, sizeof(szNlsPath));
    AssertRC (rc);

    QString nlsPath = QString(szNlsPath) + gVBoxLangSubDir;
    QDir nlsDir (nlsPath);

    Assert (!langId.isEmpty());
    if (!langId.isEmpty() && langId != gVBoxBuiltInLangName)
    {
        QRegExp regExp (gVBoxLangIDRegExp);
        int pos = regExp.indexIn (langId);
        /* the language ID should match the regexp completely */
        AssertReturnVoid (pos == 0);

        QString lang = regExp.cap (2);

        if (nlsDir.exists (gVBoxLangFileBase + langId + gVBoxLangFileExt))
        {
            languageFileName = nlsDir.absoluteFilePath (gVBoxLangFileBase + langId +
                                                        gVBoxLangFileExt);
            selectedLangId = langId;
        }
        else if (nlsDir.exists (gVBoxLangFileBase + lang + gVBoxLangFileExt))
        {
            languageFileName = nlsDir.absoluteFilePath (gVBoxLangFileBase + lang +
                                                        gVBoxLangFileExt);
            selectedLangId = lang;
        }
        else
        {
            /* Never complain when the default language is requested.  In any
             * case, if no explicit language file exists, we will simply
             * fall-back to English (built-in). */
            if (!aLangId.isNull() && langId != "en")
                vboxProblem().cannotFindLanguage (langId, nlsPath);
            /* selectedLangId remains built-in here */
            AssertReturnVoid (selectedLangId == gVBoxBuiltInLangName);
        }
    }

    /* delete the old translator if there is one */
    if (sTranslator)
    {
        /* QTranslator destructor will call qApp->removeTranslator() for
         * us. It will also delete all its child translations we attach to it
         * below, so we don't have to care about them specially. */
        delete sTranslator;
    }

    /* load new language files */
    sTranslator = new VBoxTranslator (qApp);
    Assert (sTranslator);
    bool loadOk = true;
    if (sTranslator)
    {
        if (selectedLangId != gVBoxBuiltInLangName)
        {
            Assert (!languageFileName.isNull());
            loadOk = sTranslator->loadFile (languageFileName);
        }
        /* we install the translator in any case: on failure, this will
         * activate an empty translator that will give us English
         * (built-in) */
        qApp->installTranslator (sTranslator);
    }
    else
        loadOk = false;

    if (loadOk)
        sLoadedLangId = selectedLangId;
    else
    {
        vboxProblem().cannotLoadLanguage (languageFileName);
        sLoadedLangId = gVBoxBuiltInLangName;
    }

    /* Try to load the corresponding Qt translation */
    if (sLoadedLangId != gVBoxBuiltInLangName)
    {
#ifdef Q_OS_UNIX
        /* We use system installations of Qt on Linux systems, so first, try
         * to load the Qt translation from the system location. */
        languageFileName = QLibraryInfo::location(QLibraryInfo::TranslationsPath) + "/qt_" +
                           sLoadedLangId + gVBoxLangFileExt;
        QTranslator *qtSysTr = new QTranslator (sTranslator);
        Assert (qtSysTr);
        if (qtSysTr && qtSysTr->load (languageFileName))
            qApp->installTranslator (qtSysTr);
        /* Note that the Qt translation supplied by Sun is always loaded
         * afterwards to make sure it will take precedence over the system
         * translation (it may contain more decent variants of translation
         * that better correspond to VirtualBox UI). We need to load both
         * because a newer version of Qt may be installed on the user computer
         * and the Sun version may not fully support it. We don't do it on
         * Win32 because we supply a Qt library there and therefore the
         * Sun translation is always the best one. */
#endif
        languageFileName =  nlsDir.absoluteFilePath (QString ("qt_") +
                                                     sLoadedLangId +
                                                     gVBoxLangFileExt);
        QTranslator *qtTr = new QTranslator (sTranslator);
        Assert (qtTr);
        if (qtTr && (loadOk = qtTr->load (languageFileName)))
            qApp->installTranslator (qtTr);
        /* The below message doesn't fit 100% (because it's an additional
         * language and the main one won't be reset to built-in on failure)
         * but the load failure is so rare here that it's not worth a separate
         * message (but still, having something is better than having none) */
        if (!loadOk && !aLangId.isNull())
            vboxProblem().cannotLoadLanguage (languageFileName);
    }
    if (fResetToC)
        sLoadedLangId = "C";
}

QString VBoxGlobal::helpFile() const
{
#if defined (Q_WS_WIN32)
    const QString name = "VirtualBox";
    const QString suffix = "chm";
#elif defined (Q_WS_MAC)
    const QString name = "UserManual";
    const QString suffix = "pdf";
#elif defined (Q_WS_X11)
# if defined VBOX_OSE
    const QString name = "UserManual";
    const QString suffix = "pdf";
# else
    const QString name = "VirtualBox";
    const QString suffix = "chm";
# endif
#endif
    /* Where are the docs located? */
    char szDocsPath[RTPATH_MAX];
    int rc = RTPathAppDocs (szDocsPath, sizeof (szDocsPath));
    AssertRC (rc);
    /* Make sure that the language is in two letter code.
     * Note: if languageId() returns an empty string lang.name() will
     * return "C" which is an valid language code. */
    QLocale lang (VBoxGlobal::languageId());

    /* Construct the path and the filename */
    QString manual = QString ("%1/%2_%3.%4").arg (szDocsPath)
                                            .arg (name)
                                            .arg (lang.name())
                                            .arg (suffix);
    /* Check if a help file with that name exists */
    QFileInfo fi (manual);
    if (fi.exists())
        return manual;

    /* Fall back to the standard */
    manual = QString ("%1/%2.%4").arg (szDocsPath)
                                 .arg (name)
                                 .arg (suffix);
    return manual;
}

QIcon VBoxGlobal::iconSet (const QPixmap &aNormal,
                           const QPixmap &aDisabled,
                           const QPixmap &aActive)
{
    QIcon iconSet;

    Assert (aNormal);
        iconSet.addPixmap (aNormal, QIcon::Normal);

    if (!aDisabled.isNull())
        iconSet.addPixmap (aDisabled, QIcon::Disabled);

    if (!aActive.isNull())
        iconSet.addPixmap (aActive, QIcon::Active);

    return iconSet;
}

/* static */
QIcon VBoxGlobal::iconSet (const char *aNormal,
                           const char *aDisabled /* = NULL */,
                           const char *aActive /* = NULL */)
{
    QIcon iconSet;

    Assert (aNormal != NULL);
    iconSet.addFile (aNormal, QSize(),
                     QIcon::Normal);
    if (aDisabled != NULL)
        iconSet.addFile (aDisabled, QSize(),
                         QIcon::Disabled);
    if (aActive != NULL)
        iconSet.addFile (aActive, QSize(),
                         QIcon::Active);
    return iconSet;
}

/* static */
QIcon VBoxGlobal::iconSetOnOff (const char *aNormal, const char *aNormalOff,
                                const char *aDisabled /* = NULL */,
                                const char *aDisabledOff /* = NULL */,
                                const char *aActive /* = NULL */,
                                const char *aActiveOff /* = NULL */)
{
    QIcon iconSet;

    Assert (aNormal != NULL);
    iconSet.addFile (aNormal, QSize(), QIcon::Normal, QIcon::On);
    if (aNormalOff != NULL)
        iconSet.addFile (aNormalOff, QSize(), QIcon::Normal, QIcon::Off);

    if (aDisabled != NULL)
        iconSet.addFile (aDisabled, QSize(), QIcon::Disabled, QIcon::On);
    if (aDisabledOff != NULL)
        iconSet.addFile (aDisabledOff, QSize(), QIcon::Disabled, QIcon::Off);

    if (aActive != NULL)
        iconSet.addFile (aActive, QSize(), QIcon::Active, QIcon::On);
    if (aActiveOff != NULL)
        iconSet.addFile (aActive, QSize(), QIcon::Active, QIcon::Off);

    return iconSet;
}

/* static */
QIcon VBoxGlobal::iconSetFull (const QSize &aNormalSize, const QSize &aSmallSize,
                               const char *aNormal, const char *aSmallNormal,
                               const char *aDisabled /* = NULL */,
                               const char *aSmallDisabled /* = NULL */,
                               const char *aActive /* = NULL */,
                               const char *aSmallActive /* = NULL */)
{
    QIcon iconSet;

    Assert (aNormal != NULL);
    Assert (aSmallNormal != NULL);
    iconSet.addFile (aNormal, aNormalSize, QIcon::Normal);
    iconSet.addFile (aSmallNormal, aSmallSize, QIcon::Normal);

    if (aSmallDisabled != NULL)
    {
        iconSet.addFile (aDisabled, aNormalSize, QIcon::Disabled);
        iconSet.addFile (aSmallDisabled, aSmallSize, QIcon::Disabled);
    }

    if (aSmallActive != NULL)
    {
        iconSet.addFile (aActive, aNormalSize, QIcon::Active);
        iconSet.addFile (aSmallActive, aSmallSize, QIcon::Active);
    }

    return iconSet;
}

QIcon VBoxGlobal::standardIcon (QStyle::StandardPixmap aStandard, QWidget *aWidget /* = NULL */)
{
    QStyle *style = aWidget ? aWidget->style(): QApplication::style();
    if (!style)
        return QIcon();
#ifdef Q_WS_MAC
    /* At least in Qt 4.3.4/4.4 RC1 SP_MessageBoxWarning is the application
     * icon. So change this to the critical icon. (Maybe this would be
     * fixed in a later Qt version) */
    if (aStandard == QStyle::SP_MessageBoxWarning)
        return style->standardIcon (QStyle::SP_MessageBoxCritical, 0, aWidget);
#endif /* Q_WS_MAC */
    return style->standardIcon (aStandard, 0, aWidget);
}

/**
 *  Replacement for QToolButton::setTextLabel() that handles the shortcut
 *  letter (if it is present in the argument string) as if it were a setText()
 *  call: the shortcut letter is used to automatically assign an "Alt+<letter>"
 *  accelerator key sequence to the given tool button.
 *
 *  @note This method preserves the icon set if it was assigned before. Only
 *  the text label and the accelerator are changed.
 *
 *  @param aToolButton  Tool button to set the text label on.
 *  @param aTextLabel   Text label to set.
 */
/* static */
void VBoxGlobal::setTextLabel (QToolButton *aToolButton,
                               const QString &aTextLabel)
{
    AssertReturnVoid (aToolButton != NULL);

    /* remember the icon set as setText() will kill it */
    QIcon iset = aToolButton->icon();
    /* re-use the setText() method to detect and set the accelerator */
    aToolButton->setText (aTextLabel);
    QKeySequence accel = aToolButton->shortcut();
    aToolButton->setText (aTextLabel);
    aToolButton->setIcon (iset);
    /* set the accel last as setIconSet() would kill it */
    aToolButton->setShortcut (accel);
}

/**
 *  Performs direct and flipped search of position for \a aRectangle to make sure
 *  it is fully contained inside \a aBoundRegion region by moving & resizing
 *  \a aRectangle if necessary. Selects the minimum shifted result between direct
 *  and flipped variants.
 */
/* static */
QRect VBoxGlobal::normalizeGeometry (const QRect &aRectangle, const QRegion &aBoundRegion,
                                     bool aCanResize /* = true */)
{
    /* Direct search for normalized rectangle */
    QRect var1 (getNormalized (aRectangle, aBoundRegion, aCanResize));

    /* Flipped search for normalized rectangle */
    QRect var2 (flip (getNormalized (flip (aRectangle).boundingRect(),
                                     flip (aBoundRegion), aCanResize)).boundingRect());

    /* Calculate shift from starting position for both variants */
    double length1 = sqrt (pow ((double) (var1.x() - aRectangle.x()), (double) 2) +
                           pow ((double) (var1.y() - aRectangle.y()), (double) 2));
    double length2 = sqrt (pow ((double) (var2.x() - aRectangle.x()), (double) 2) +
                           pow ((double) (var2.y() - aRectangle.y()), (double) 2));

    /* Return minimum shifted variant */
    return length1 > length2 ? var2 : var1;
}

/**
 *  Ensures that the given rectangle \a aRectangle is fully contained within the
 *  region \a aBoundRegion by moving \a aRectangle if necessary. If \a aRectangle is
 *  larger than \a aBoundRegion, top left corner of \a aRectangle is aligned with the
 *  top left corner of maximum available rectangle and, if \a aCanResize is true,
 *  \a aRectangle is shrinked to become fully visible.
 */
/* static */
QRect VBoxGlobal::getNormalized (const QRect &aRectangle, const QRegion &aBoundRegion,
                                 bool /* aCanResize = true */)
{
    /* Storing available horizontal sub-rectangles & vertical shifts */
    int windowVertical = aRectangle.center().y();
    QVector <QRect> rectanglesVector (aBoundRegion.rects());
    QList <QRect> rectanglesList;
    QList <int> shiftsList;
    foreach (QRect currentItem, rectanglesVector)
    {
        int currentDelta = qAbs (windowVertical - currentItem.center().y());
        int shift2Top = currentItem.top() - aRectangle.top();
        int shift2Bot = currentItem.bottom() - aRectangle.bottom();

        int itemPosition = 0;
        foreach (QRect item, rectanglesList)
        {
            int delta = qAbs (windowVertical - item.center().y());
            if (delta > currentDelta) break; else ++ itemPosition;
        }
        rectanglesList.insert (itemPosition, currentItem);

        int shift2TopPos = 0;
        foreach (int shift, shiftsList)
            if (qAbs (shift) > qAbs (shift2Top)) break; else ++ shift2TopPos;
        shiftsList.insert (shift2TopPos, shift2Top);

        int shift2BotPos = 0;
        foreach (int shift, shiftsList)
            if (qAbs (shift) > qAbs (shift2Bot)) break; else ++ shift2BotPos;
        shiftsList.insert (shift2BotPos, shift2Bot);
    }

    /* Trying to find the appropriate place for window */
    QRect result;
    for (int i = -1; i < shiftsList.size(); ++ i)
    {
        /* Move to appropriate vertical */
        QRect rectangle (aRectangle);
        if (i >= 0) rectangle.translate (0, shiftsList [i]);

        /* Search horizontal shift */
        int maxShift = 0;
        foreach (QRect item, rectanglesList)
        {
            QRect trectangle (rectangle.translated (item.left() - rectangle.left(), 0));
            if (!item.intersects (trectangle))
                continue;

            if (rectangle.left() < item.left())
            {
                int shift = item.left() - rectangle.left();
                maxShift = qAbs (shift) > qAbs (maxShift) ? shift : maxShift;
            }
            else if (rectangle.right() > item.right())
            {
                int shift = item.right() - rectangle.right();
                maxShift = qAbs (shift) > qAbs (maxShift) ? shift : maxShift;
            }
        }

        /* Shift across the horizontal direction */
        rectangle.translate (maxShift, 0);

        /* Check the translated rectangle to feat the rules */
        if (aBoundRegion.united (rectangle) == aBoundRegion)
            result = rectangle;

        if (!result.isNull()) break;
    }

    if (result.isNull())
    {
        /* Resize window to feat desirable size
         * using max of available rectangles */
        QRect maxRectangle;
        quint64 maxSquare = 0;
        foreach (QRect item, rectanglesList)
        {
            quint64 square = item.width() * item.height();
            if (square > maxSquare)
            {
                maxSquare = square;
                maxRectangle = item;
            }
        }

        result = aRectangle;
        result.moveTo (maxRectangle.x(), maxRectangle.y());
        if (maxRectangle.right() < result.right())
            result.setRight (maxRectangle.right());
        if (maxRectangle.bottom() < result.bottom())
            result.setBottom (maxRectangle.bottom());
    }

    return result;
}

/**
 *  Returns the flipped (transposed) region.
 */
/* static */
QRegion VBoxGlobal::flip (const QRegion &aRegion)
{
    QRegion result;
    QVector <QRect> rectangles (aRegion.rects());
    foreach (QRect rectangle, rectangles)
        result += QRect (rectangle.y(), rectangle.x(),
                         rectangle.height(), rectangle.width());
    return result;
}

/**
 *  Aligns the center of \a aWidget with the center of \a aRelative.
 *
 *  If necessary, \a aWidget's position is adjusted to make it fully visible
 *  within the available desktop area. If \a aWidget is bigger then this area,
 *  it will also be resized unless \a aCanResize is false or there is an
 *  inappropriate minimum size limit (in which case the top left corner will be
 *  simply aligned with the top left corner of the available desktop area).
 *
 *  \a aWidget must be a top-level widget. \a aRelative may be any widget, but
 *  if it's not top-level itself, its top-level widget will be used for
 *  calculations. \a aRelative can also be NULL, in which case \a aWidget will
 *  be centered relative to the available desktop area.
 */
/* static */
void VBoxGlobal::centerWidget (QWidget *aWidget, QWidget *aRelative,
                               bool aCanResize /* = true */)
{
    AssertReturnVoid (aWidget);
    AssertReturnVoid (aWidget->isTopLevel());

    QRect deskGeo, parentGeo;
    QWidget *w = aRelative;
    if (w)
    {
        w = w->window();
        deskGeo = QApplication::desktop()->availableGeometry (w);
        parentGeo = w->frameGeometry();
        /* On X11/Gnome, geo/frameGeo.x() and y() are always 0 for top level
         * widgets with parents, what a shame. Use mapToGlobal() to workaround. */
        QPoint d = w->mapToGlobal (QPoint (0, 0));
        d.rx() -= w->geometry().x() - w->x();
        d.ry() -= w->geometry().y() - w->y();
        parentGeo.moveTopLeft (d);
    }
    else
    {
        deskGeo = QApplication::desktop()->availableGeometry();
        parentGeo = deskGeo;
    }

    /* On X11, there is no way to determine frame geometry (including WM
     * decorations) before the widget is shown for the first time. Stupidly
     * enumerate other top level widgets to find the thickest frame. The code
     * is based on the idea taken from QDialog::adjustPositionInternal(). */

    int extraw = 0, extrah = 0;

    QWidgetList list = QApplication::topLevelWidgets();
    QListIterator<QWidget*> it (list);
    while ((extraw == 0 || extrah == 0) && it.hasNext())
    {
        int framew, frameh;
        QWidget *current = it.next();
        if (!current->isVisible())
            continue;

        framew = current->frameGeometry().width() - current->width();
        frameh = current->frameGeometry().height() - current->height();

        extraw = qMax (extraw, framew);
        extrah = qMax (extrah, frameh);
    }

    /// @todo (r=dmik) not sure if we really need this
#if 0
    /* sanity check for decoration frames. With embedding, we
     * might get extraordinary values */
    if (extraw == 0 || extrah == 0 || extraw > 20 || extrah > 50)
    {
        extrah = 50;
        extraw = 20;
    }
#endif

    /* On non-X11 platforms, the following would be enough instead of the
     * above workaround: */
    // QRect geo = frameGeometry();
    QRect geo = QRect (0, 0, aWidget->width() + extraw,
                             aWidget->height() + extrah);

    geo.moveCenter (QPoint (parentGeo.x() + (parentGeo.width() - 1) / 2,
                            parentGeo.y() + (parentGeo.height() - 1) / 2));

    /* ensure the widget is within the available desktop area */
    QRect newGeo = normalizeGeometry (geo, deskGeo, aCanResize);
#ifdef Q_WS_MAC
    /* No idea why, but Qt doesn't respect if there is a unified toolbar on the
     * ::move call. So manually add the height of the toolbar before setting
     * the position. */
    if (w)
        newGeo.translate (0, ::darwinWindowToolBarHeight (aWidget));
#endif /* Q_WS_MAC */

    aWidget->move (newGeo.topLeft());

    if (aCanResize &&
        (geo.width() != newGeo.width() || geo.height() != newGeo.height()))
        aWidget->resize (newGeo.width() - extraw, newGeo.height() - extrah);
}

/**
 *  Returns the decimal separator for the current locale.
 */
/* static */
QChar VBoxGlobal::decimalSep()
{
    return QLocale::system().decimalPoint();
}

/**
 *  Returns the regexp string that defines the format of the human-readable
 *  size representation, <tt>####[.##] B|KB|MB|GB|TB|PB</tt>.
 *
 *  This regexp will capture 5 groups of text:
 *  - cap(1): integer number in case when no decimal point is present
 *            (if empty, it means that decimal point is present)
 *  - cap(2): size suffix in case when no decimal point is present (may be empty)
 *  - cap(3): integer number in case when decimal point is present (may be empty)
 *  - cap(4): fraction number (hundredth) in case when decimal point is present
 *  - cap(5): size suffix in case when decimal point is present (note that
 *            B cannot appear there)
 */
/* static */
QString VBoxGlobal::sizeRegexp()
{
    QString regexp =
        QString ("^(?:(?:(\\d+)(?:\\s?([KMGTP]?B))?)|(?:(\\d*)%1(\\d{1,2})(?:\\s?([KMGTP]B))))$")
                 .arg (decimalSep());
    return regexp;
}

/**
 *  Parses the given size string that should be in form of
 *  <tt>####[.##] B|KB|MB|GB|TB|PB</tt> and returns the size value
 *  in bytes. Zero is returned on error.
 */
/* static */
quint64 VBoxGlobal::parseSize (const QString &aText)
{
    QRegExp regexp (sizeRegexp());
    int pos = regexp.indexIn (aText);
    if (pos != -1)
    {
        QString intgS = regexp.cap (1);
        QString hundS;
        QString suff = regexp.cap (2);
        if (intgS.isEmpty())
        {
            intgS = regexp.cap (3);
            hundS = regexp.cap (4);
            suff = regexp.cap (5);
        }

        quint64 denom = 0;
        if (suff.isEmpty() || suff == "B")
            denom = 1;
        else if (suff == "KB")
            denom = _1K;
        else if (suff == "MB")
            denom = _1M;
        else if (suff == "GB")
            denom = _1G;
        else if (suff == "TB")
            denom = _1T;
        else if (suff == "PB")
            denom = _1P;

        quint64 intg = intgS.toULongLong();
        if (denom == 1)
            return intg;

        quint64 hund = hundS.leftJustified (2, '0').toULongLong();
        hund = hund * denom / 100;
        intg = intg * denom + hund;
        return intg;
    }
    else
        return 0;
}

/**
 * Formats the given @a aSize value in bytes to a human readable string
 * in form of <tt>####[.##] B|KB|MB|GB|TB|PB</tt>.
 *
 * The @a aMode and @a aDecimal parameters are used for rounding the resulting
 * number when converting the size value to KB, MB, etc gives a fractional part:
 * <ul>
 * <li>When \a aMode is FormatSize_Round, the result is rounded to the
 *     closest number containing \a aDecimal decimal digits.
 * </li>
 * <li>When \a aMode is FormatSize_RoundDown, the result is rounded to the
 *     largest number with \a aDecimal decimal digits that is not greater than
 *     the result. This guarantees that converting the resulting string back to
 *     the integer value in bytes will not produce a value greater that the
 *     initial size parameter.
 * </li>
 * <li>When \a aMode is FormatSize_RoundUp, the result is rounded to the
 *     smallest number with \a aDecimal decimal digits that is not less than the
 *     result. This guarantees that converting the resulting string back to the
 *     integer value in bytes will not produce a value less that the initial
 *     size parameter.
 * </li>
 * </ul>
 *
 * @param aSize     Size value in bytes.
 * @param aMode     Conversion mode.
 * @param aDecimal  Number of decimal digits in result.
 * @return          Human-readable size string.
 */
/* static */
QString VBoxGlobal::formatSize (quint64 aSize, uint aDecimal /* = 2 */,
                                VBoxDefs::FormatSize aMode /* = FormatSize_Round */)
{
    static const char *Suffixes [] = { "B", "KB", "MB", "GB", "TB", "PB", NULL };

    quint64 denom = 0;
    int suffix = 0;

    if (aSize < _1K)
    {
        denom = 1;
        suffix = 0;
    }
    else if (aSize < _1M)
    {
        denom = _1K;
        suffix = 1;
    }
    else if (aSize < _1G)
    {
        denom = _1M;
        suffix = 2;
    }
    else if (aSize < _1T)
    {
        denom = _1G;
        suffix = 3;
    }
    else if (aSize < _1P)
    {
        denom = _1T;
        suffix = 4;
    }
    else
    {
        denom = _1P;
        suffix = 5;
    }

    quint64 intg = aSize / denom;
    quint64 decm = aSize % denom;
    quint64 mult = 1;
    for (uint i = 0; i < aDecimal; ++ i) mult *= 10;

    QString number;
    if (denom > 1)
    {
        if (decm)
        {
            decm *= mult;
            /* not greater */
            if (aMode == VBoxDefs::FormatSize_RoundDown)
                decm = decm / denom;
            /* not less */
            else if (aMode == VBoxDefs::FormatSize_RoundUp)
                decm = (decm + denom - 1) / denom;
            /* nearest */
            else decm = (decm + denom / 2) / denom;
        }
        /* check for the fractional part overflow due to rounding */
        if (decm == mult)
        {
            decm = 0;
            ++ intg;
            /* check if we've got 1024 XB after rounding and scale down if so */
            if (intg == 1024 && Suffixes [suffix + 1] != NULL)
            {
                intg /= 1024;
                ++ suffix;
            }
        }
        number = QString::number (intg);
        if (aDecimal) number += QString ("%1%2").arg (decimalSep())
            .arg (QString::number (decm).rightJustified (aDecimal, '0'));
    }
    else
    {
        number = QString::number (intg);
    }

    return QString ("%1 %2").arg (number).arg (Suffixes [suffix]);
}

/**
 *  Returns the required video memory in bytes for the current desktop
 *  resolution at maximum possible screen depth in bpp.
 */
/* static */
quint64 VBoxGlobal::requiredVideoMemory (CMachine *aMachine /* = 0 */, int cMonitors /* = 1 */)
{
    QSize desktopRes = QApplication::desktop()->screenGeometry().size();
    QDesktopWidget *pDW = QApplication::desktop();
    /* We create a list of the size of all available host monitors. This list
     * is sorted by value and by starting with the biggest one, we calculate
     * the memory requirements for every guest screen. This is of course not
     * correct, but as we can't predict on which host screens the user will
     * open the guest windows, this is the best assumption we can do, cause it
     * is the worst case. */
    QVector<int> screenSize(qMax(cMonitors, pDW->numScreens()), 0);
    for (int i = 0; i < pDW->numScreens(); ++i)
    {
        QRect r = pDW->screenGeometry(i);
        screenSize[i] = r.width() * r.height();
    }
    /* Now sort the vector */
    qSort(screenSize.begin(), screenSize.end(), qGreater<int>());
    /* For the case that there are more guest screens configured then host
     * screens available, replace all zeros with the greatest value in the
     * vector. */
    for (int i = 0; i < screenSize.size(); ++i)
        if (screenSize.at(i) == 0)
            screenSize.replace(i, screenSize.at(0));

    quint64 needBits = 0;
    for (int i = 0; i < cMonitors; ++i)
    {
        /* Calculate summary required memory amount in bits */
        needBits += (screenSize.at(i) * /* with x height */
                     32 + /* we will take the maximum possible bpp for now */
                     8 * _1M) + /* current cache per screen - may be changed in future */
                    8 * 4096; /* adapter info */
    }
    /* Translate value into megabytes with rounding to highest side */
    quint64 needMBytes = needBits % (8 * _1M) ? needBits / (8 * _1M) + 1 :
                         needBits / (8 * _1M) /* convert to megabytes */;

    if (aMachine && !aMachine->isNull())
    {
       QString typeId = aMachine->GetOSTypeId();
       if (typeId.startsWith("Windows"))
       {
           /* Windows guests need offscreen VRAM too for graphics acceleration features. */
           needMBytes *= 2;
       }
    }

    return needMBytes * _1M;
}

/**
 * Puts soft hyphens after every path component in the given file name.
 *
 * @param aFileName File name (must be a full path name).
 */
/* static */
QString VBoxGlobal::locationForHTML (const QString &aFileName)
{
/// @todo (dmik) remove?
//    QString result = QDir::toNativeSeparators (fn);
//#ifdef Q_OS_LINUX
//    result.replace ('/', "/<font color=red>&shy;</font>");
//#else
//    result.replace ('\\', "\\<font color=red>&shy;</font>");
//#endif
//    return result;
    QFileInfo fi (aFileName);
    return fi.fileName();
}

/**
 *  Reformats the input string @a aStr so that:
 *  - strings in single quotes will be put inside <nobr> and marked
 *    with blue color;
 *  - UUIDs be put inside <nobr> and marked
 *    with green color;
 *  - replaces new line chars with </p><p> constructs to form paragraphs
 *    (note that <p> and </p> are not appended to the beginnign and to the
 *     end of the string respectively, to allow the result be appended
 *     or prepended to the existing paragraph).
 *
 *  If @a aToolTip is true, colouring is not applied, only the <nobr> tag
 *  is added. Also, new line chars are replaced with <br> instead of <p>.
 */
/* static */
QString VBoxGlobal::highlight (const QString &aStr, bool aToolTip /* = false */)
{
    QString strFont;
    QString uuidFont;
    QString endFont;
    if (!aToolTip)
    {
        strFont = "<font color=#0000CC>";
        uuidFont = "<font color=#008000>";
        endFont = "</font>";
    }

    QString text = aStr;

    /* replace special entities, '&' -- first! */
    text.replace ('&', "&amp;");
    text.replace ('<', "&lt;");
    text.replace ('>', "&gt;");
    text.replace ('\"', "&quot;");

    /* mark strings in single quotes with color */
    QRegExp rx = QRegExp ("((?:^|\\s)[(]?)'([^']*)'(?=[:.-!);]?(?:\\s|$))");
    rx.setMinimal (true);
    text.replace (rx,
        QString ("\\1%1<nobr>'\\2'</nobr>%2").arg (strFont).arg (endFont));

    /* mark UUIDs with color */
    text.replace (QRegExp (
        "((?:^|\\s)[(]?)"
        "(\\{[0-9A-Fa-f]{8}-[0-9A-Fa-f]{4}-[0-9A-Fa-f]{4}-[0-9A-Fa-f]{4}-[0-9A-Fa-f]{12}\\})"
        "(?=[:.-!);]?(?:\\s|$))"),
        QString ("\\1%1<nobr>\\2</nobr>%2").arg (uuidFont).arg (endFont));

    /* split to paragraphs at \n chars */
    if (!aToolTip)
        text.replace ('\n', "</p><p>");
    else
        text.replace ('\n', "<br>");

    return text;
}

/* static */
QString VBoxGlobal::replaceHtmlEntities(QString strText)
{
    return strText
        .replace('&', "&amp;")
        .replace('<', "&lt;")
        .replace('>', "&gt;")
        .replace('\"', "&quot;");
}

/**
 *  Reformats the input string @a aStr so that:
 *  - strings in single quotes will be put inside <nobr> and marked
 *    with bold style;
 *  - UUIDs be put inside <nobr> and marked
 *    with italic style;
 *  - replaces new line chars with </p><p> constructs to form paragraphs
 *    (note that <p> and </p> are not appended to the beginnign and to the
 *     end of the string respectively, to allow the result be appended
 *     or prepended to the existing paragraph).
 */
/* static */
QString VBoxGlobal::emphasize (const QString &aStr)
{
    QString strEmphStart ("<b>");
    QString strEmphEnd ("</b>");
    QString uuidEmphStart ("<i>");
    QString uuidEmphEnd ("</i>");

    QString text = aStr;

    /* replace special entities, '&' -- first! */
    text.replace ('&', "&amp;");
    text.replace ('<', "&lt;");
    text.replace ('>', "&gt;");
    text.replace ('\"', "&quot;");

    /* mark strings in single quotes with bold style */
    QRegExp rx = QRegExp ("((?:^|\\s)[(]?)'([^']*)'(?=[:.-!);]?(?:\\s|$))");
    rx.setMinimal (true);
    text.replace (rx,
        QString ("\\1%1<nobr>'\\2'</nobr>%2").arg (strEmphStart).arg (strEmphEnd));

    /* mark UUIDs with italic style */
    text.replace (QRegExp (
        "((?:^|\\s)[(]?)"
        "(\\{[0-9A-Fa-f]{8}-[0-9A-Fa-f]{4}-[0-9A-Fa-f]{4}-[0-9A-Fa-f]{4}-[0-9A-Fa-f]{12}\\})"
        "(?=[:.-!);]?(?:\\s|$))"),
        QString ("\\1%1<nobr>\\2</nobr>%2").arg (uuidEmphStart).arg (uuidEmphEnd));

    /* split to paragraphs at \n chars */
    text.replace ('\n', "</p><p>");

    return text;
}

/**
 *  This does exactly the same as QLocale::system().name() but corrects its
 *  wrong behavior on Linux systems (LC_NUMERIC for some strange reason takes
 *  precedence over any other locale setting in the QLocale::system()
 *  implementation). This implementation first looks at LC_ALL (as defined by
 *  SUS), then looks at LC_MESSAGES which is designed to define a language for
 *  program messages in case if it differs from the language for other locale
 *  categories. Then it looks for LANG and finally falls back to
 *  QLocale::system().name().
 *
 *  The order of precedence is well defined here:
 *  http://opengroup.org/onlinepubs/007908799/xbd/envvar.html
 *
 *  @note This method will return "C" when the requested locale is invalid or
 *  when the "C" locale is set explicitly.
 */
/* static */
QString VBoxGlobal::systemLanguageId()
{
#if defined (Q_WS_MAC)
    /* QLocale return the right id only if the user select the format of the
     * language also. So we use our own implementation */
    return ::darwinSystemLanguage();
#elif defined (Q_OS_UNIX)
    const char *s = RTEnvGet ("LC_ALL");
    if (s == 0)
        s = RTEnvGet ("LC_MESSAGES");
    if (s == 0)
        s = RTEnvGet ("LANG");
    if (s != 0)
        return QLocale (s).name();
#endif
    return  QLocale::system().name();
}

#if defined (Q_WS_X11)

static char *XXGetProperty (Display *aDpy, Window aWnd,
                            Atom aPropType, const char *aPropName)
{
    Atom propNameAtom = XInternAtom (aDpy, aPropName,
                                     True /* only_if_exists */);
    if (propNameAtom == None)
        return NULL;

    Atom actTypeAtom = None;
    int actFmt = 0;
    unsigned long nItems = 0;
    unsigned long nBytesAfter = 0;
    unsigned char *propVal = NULL;
    int rc = XGetWindowProperty (aDpy, aWnd, propNameAtom,
                                 0, LONG_MAX, False /* delete */,
                                 aPropType, &actTypeAtom, &actFmt,
                                 &nItems, &nBytesAfter, &propVal);
    if (rc != Success)
        return NULL;

    return reinterpret_cast <char *> (propVal);
}

static Bool XXSendClientMessage (Display *aDpy, Window aWnd, const char *aMsg,
                                 unsigned long aData0 = 0, unsigned long aData1 = 0,
                                 unsigned long aData2 = 0, unsigned long aData3 = 0,
                                 unsigned long aData4 = 0)
{
    Atom msgAtom = XInternAtom (aDpy, aMsg, True /* only_if_exists */);
    if (msgAtom == None)
        return False;

    XEvent ev;

    ev.xclient.type = ClientMessage;
    ev.xclient.serial = 0;
    ev.xclient.send_event = True;
    ev.xclient.display = aDpy;
    ev.xclient.window = aWnd;
    ev.xclient.message_type = msgAtom;

    /* always send as 32 bit for now */
    ev.xclient.format = 32;
    ev.xclient.data.l [0] = aData0;
    ev.xclient.data.l [1] = aData1;
    ev.xclient.data.l [2] = aData2;
    ev.xclient.data.l [3] = aData3;
    ev.xclient.data.l [4] = aData4;

    return XSendEvent (aDpy, DefaultRootWindow (aDpy), False,
                       SubstructureRedirectMask, &ev) != 0;
}

#endif

/**
 * Activates the specified window. If necessary, the window will be
 * de-iconified activation.
 *
 * @note On X11, it is implied that @a aWid represents a window of the same
 * display the application was started on.
 *
 * @param aWId              Window ID to activate.
 * @param aSwitchDesktop    @c true to switch to the window's desktop before
 *                          activation.
 *
 * @return @c true on success and @c false otherwise.
 */
/* static */
bool VBoxGlobal::activateWindow (WId aWId, bool aSwitchDesktop /* = true */)
{
    bool result = true;

#if defined (Q_WS_WIN32)

    if (IsIconic (aWId))
        result &= !!ShowWindow (aWId, SW_RESTORE);
    else if (!IsWindowVisible (aWId))
        result &= !!ShowWindow (aWId, SW_SHOW);

    result &= !!SetForegroundWindow (aWId);

#elif defined (Q_WS_X11)

    Display *dpy = QX11Info::display();

    if (aSwitchDesktop)
    {
        /* try to find the desktop ID using the NetWM property */
        CARD32 *desktop = (CARD32 *) XXGetProperty (dpy, aWId, XA_CARDINAL,
                                                    "_NET_WM_DESKTOP");
        if (desktop == NULL)
            /* if the NetWM propery is not supported try to find the desktop
             * ID using the GNOME WM property */
            desktop = (CARD32 *) XXGetProperty (dpy, aWId, XA_CARDINAL,
                                                "_WIN_WORKSPACE");

        if (desktop != NULL)
        {
            Bool ok = XXSendClientMessage (dpy, DefaultRootWindow (dpy),
                                           "_NET_CURRENT_DESKTOP",
                                           *desktop);
            if (!ok)
            {
                LogWarningFunc (("Couldn't switch to desktop=%08X\n",
                                 desktop));
                result = false;
            }
            XFree (desktop);
        }
        else
        {
            LogWarningFunc (("Couldn't find a desktop ID for aWId=%08X\n",
                             aWId));
            result = false;
        }
    }

    Bool ok = XXSendClientMessage (dpy, aWId, "_NET_ACTIVE_WINDOW");
    result &= !!ok;

    XRaiseWindow (dpy, aWId);

#else

    NOREF (aWId);
    NOREF (aSwitchDesktop);
    AssertFailed();
    result = false;

#endif

    if (!result)
        LogWarningFunc (("Couldn't activate aWId=%08X\n", aWId));

    return result;
}

/**
 *  Removes the acceletartor mark (the ampersand symbol) from the given string
 *  and returns the result. The string is supposed to be a menu item's text
 *  that may (or may not) contain the accelerator mark.
 *
 *  In order to support accelerators used in non-alphabet languages
 *  (e.g. Japanese) that has a form of "(&<L>)" (where <L> is a latin letter),
 *  this method first searches for this pattern and, if found, removes it as a
 *  whole. If such a pattern is not found, then the '&' character is simply
 *  removed from the string.
 *
 *  @note This function removes only the first occurense of the accelerator
 *  mark.
 *
 *  @param aText Menu item's text to remove the acceletaror mark from.
 *
 *  @return The resulting string.
 */
/* static */
QString VBoxGlobal::removeAccelMark (const QString &aText)
{
    QString result = aText;

    QRegExp accel ("\\(&[a-zA-Z]\\)");
    int pos = accel.indexIn (result);
    if (pos >= 0)
        result.remove (pos, accel.cap().length());
    else
    {
        pos = result.indexOf ('&');
        if (pos >= 0)
            result.remove (pos, 1);
    }

    return result;
}

/* static */
QString VBoxGlobal::insertKeyToActionText (const QString &aText, const QString &aKey)
{
#ifdef Q_WS_MAC
    QString key ("%1 (Host+%2)");
#else
    QString key ("%1 \tHost+%2");
#endif
    return key.arg (aText).arg (QKeySequence (aKey).toString (QKeySequence::NativeText));
}

/* static */
QString VBoxGlobal::extractKeyFromActionText (const QString &aText)
{
    QString key;
#ifdef Q_WS_MAC
    QRegExp re (".* \\(Host\\+(.+)\\)");
#else
    QRegExp re (".* \\t\\Host\\+(.+)");
#endif
    if (re.exactMatch (aText))
        key = re.cap (1);
    return key;
}

/**
 * Joins two pixmaps horizontally with 2px space between them and returns the
 * result.
 *
 * @param aPM1 Left pixmap.
 * @param aPM2 Right pixmap.
 */
/* static */
QPixmap VBoxGlobal::joinPixmaps (const QPixmap &aPM1, const QPixmap &aPM2)
{
    if (aPM1.isNull())
        return aPM2;
    if (aPM2.isNull())
        return aPM1;

    QPixmap result (aPM1.width() + aPM2.width() + 2,
                    qMax (aPM1.height(), aPM2.height()));
    result.fill (Qt::transparent);

    QPainter painter (&result);
    painter.drawPixmap (0, 0, aPM1);
    painter.drawPixmap (aPM1.width() + 2, result.height() - aPM2.height(), aPM2);
    painter.end();

    return result;
}

/**
 *  Searches for a widget that with @a aName (if it is not NULL) which inherits
 *  @a aClassName (if it is not NULL) and among children of @a aParent. If @a
 *  aParent is NULL, all top-level widgets are searched. If @a aRecursive is
 *  true, child widgets are recursively searched as well.
 */
/* static */
QWidget *VBoxGlobal::findWidget (QWidget *aParent, const char *aName,
                                 const char *aClassName /* = NULL */,
                                 bool aRecursive /* = false */)
{
    if (aParent == NULL)
    {
        QWidgetList list = QApplication::topLevelWidgets();
        QWidget* w = NULL;
        foreach(w, list)
        {
            if ((!aName || strcmp (w->objectName().toAscii().constData(), aName) == 0) &&
                (!aClassName || strcmp (w->metaObject()->className(), aClassName) == 0))
                break;
            if (aRecursive)
            {
                w = findWidget (w, aName, aClassName, aRecursive);
                if (w)
                    break;
            }
        }
        return w;
    }

    /* Find the first children of aParent with the appropriate properties.
     * Please note that this call is recursivly. */
    QList<QWidget *> list = qFindChildren<QWidget *> (aParent, aName);
    QWidget *child = NULL;
    foreach(child, list)
    {
        if (!aClassName || strcmp (child->metaObject()->className(), aClassName) == 0)
            break;
    }
    return child;
}

/**
 * Figures out which hard disk formats are currently supported by VirtualBox.
 * Returned is a list of pairs with the form
 *   <tt>{"Backend Name", "*.suffix1 .suffix2 ..."}</tt>.
 */
/* static */
QList <QPair <QString, QString> > VBoxGlobal::HDDBackends()
{
    CSystemProperties systemProperties = vboxGlobal().virtualBox().GetSystemProperties();
    QVector<CMediumFormat> mediumFormats = systemProperties.GetMediumFormats();
    QList< QPair<QString, QString> > backendPropList;
    for (int i = 0; i < mediumFormats.size(); ++ i)
    {
        /* File extensions */
        QVector <QString> fileExtensions = mediumFormats [i].GetFileExtensions();
        QStringList f;
        for (int a = 0; a < fileExtensions.size(); ++ a)
            f << QString ("*.%1").arg (fileExtensions [a]);
        /* Create a pair out of the backend description and all suffix's. */
        if (!f.isEmpty())
            backendPropList << QPair<QString, QString> (mediumFormats [i].GetName(), f.join(" "));
    }
    return backendPropList;
}

/* static */
QString VBoxGlobal::documentsPath()
{
    QString path;
#if QT_VERSION < 0x040400
    path = QDir::homePath();
#else
    path = QDesktopServices::storageLocation (QDesktopServices::DocumentsLocation);
#endif

    /* Make sure the path exists */
    QDir dir (path);
    if (dir.exists())
        return QDir::cleanPath (dir.canonicalPath());
    else
    {
        dir.setPath (QDir::homePath() + "/Documents");
        if (dir.exists())
            return QDir::cleanPath (dir.canonicalPath());
        else
            return QDir::homePath();
    }
}

#ifdef VBOX_WITH_VIDEOHWACCEL
/* static */
bool VBoxGlobal::isAcceleration2DVideoAvailable()
{
    return VBoxQGLOverlay::isAcceleration2DVideoAvailable();
}

/** additional video memory required for the best 2D support performance
 *  total amount of VRAM required is thus calculated as requiredVideoMemory + required2DOffscreenVideoMemory  */
/* static */
quint64 VBoxGlobal::required2DOffscreenVideoMemory()
{
    return VBoxQGLOverlay::required2DOffscreenVideoMemory();
}

#endif

// Public slots
////////////////////////////////////////////////////////////////////////////////

/**
 * Opens the specified URL using OS/Desktop capabilities.
 *
 * @param aURL URL to open
 *
 * @return true on success and false otherwise
 */
bool VBoxGlobal::openURL (const QString &aURL)
{
    /* Service event */
    class ServiceEvent : public QEvent
    {
        public:

            ServiceEvent (bool aResult) : QEvent (QEvent::User), mResult (aResult) {}

            bool result() const { return mResult; }

        private:

            bool mResult;
    };

    /* Service-Client object */
    class ServiceClient : public QEventLoop
    {
        public:

            ServiceClient() : mResult (false) {}

            bool result() const { return mResult; }

        private:

            bool event (QEvent *aEvent)
            {
                if (aEvent->type() == QEvent::User)
                {
                    ServiceEvent *pEvent = static_cast <ServiceEvent*> (aEvent);
                    mResult = pEvent->result();
                    pEvent->accept();
                    quit();
                    return true;
                }
                return false;
            }

            bool mResult;
    };

    /* Service-Server object */
    class ServiceServer : public QThread
    {
        public:

            ServiceServer (ServiceClient &aClient, const QString &sURL)
                : mClient (aClient), mURL (sURL) {}

        private:

            void run()
            {
                QApplication::postEvent (&mClient, new ServiceEvent (QDesktopServices::openUrl (mURL)));
            }

            ServiceClient &mClient;
            const QString &mURL;
    };

    ServiceClient client;
    ServiceServer server (client, aURL);
    server.start();
    client.exec();
    server.wait();

    bool result = client.result();

    if (!result)
        vboxProblem().cannotOpenURL (aURL);

    return result;
}

/**
 * Shows the VirtualBox registration dialog.
 *
 * @note that this method is not part of VBoxProblemReporter (like e.g.
 *       VBoxProblemReporter::showHelpAboutDialog()) because it is tied to
 *       VBoxCallback::OnExtraDataChange() handling performed by VBoxGlobal.
 *
 * @param aForce
 */
void VBoxGlobal::showRegistrationDialog (bool aForce)
{
#ifdef VBOX_WITH_REGISTRATION
    if (!aForce && !UIRegistrationWzd::hasToBeShown())
        return;

    if (mRegDlg)
    {
        /* Show the already opened registration dialog */
        mRegDlg->setWindowState (mRegDlg->windowState() & ~Qt::WindowMinimized);
        mRegDlg->raise();
        mRegDlg->activateWindow();
    }
    else
    {
        /* Store the ID of the main window to ensure that only one
         * registration dialog is shown at a time. Due to manipulations with
         * OnExtraDataCanChange() and OnExtraDataChange() signals, this extra
         * data item acts like an inter-process mutex, so the first process
         * that attempts to set it will win, the rest will get a failure from
         * the SetExtraData() call. */
        mVBox.SetExtraData (VBoxDefs::GUI_RegistrationDlgWinID,
                            QString ("%1").arg ((qulonglong) mMainWindow->winId()));

        if (mVBox.isOk())
        {
            /* We've got the "mutex", create a new registration dialog */
            UIRegistrationWzd *dlg = new UIRegistrationWzd (&mRegDlg);
            dlg->setAttribute (Qt::WA_DeleteOnClose);
            Assert (dlg == mRegDlg);
            mRegDlg->show();
        }
    }
#endif
}

/**
 * Shows the VirtualBox version check & update dialog.
 *
 * @note that this method is not part of VBoxProblemReporter (like e.g.
 *       VBoxProblemReporter::showHelpAboutDialog()) because it is tied to
 *       VBoxCallback::OnExtraDataChange() handling performed by VBoxGlobal.
 *
 * @param aForce
 */
void VBoxGlobal::showUpdateDialog (bool aForce)
{
    /* Silently check in one day after current time-stamp */
    QTimer::singleShot (24 /* hours */   * 60   /* minutes */ *
                        60 /* seconds */ * 1000 /* milliseconds */,
                        this, SLOT (perDayNewVersionNotifier()));

    bool isNecessary = VBoxUpdateDlg::isNecessary();

    if (!aForce && !isNecessary)
        return;

    if (mUpdDlg)
    {
        if (!mUpdDlg->isHidden())
        {
            mUpdDlg->setWindowState (mUpdDlg->windowState() & ~Qt::WindowMinimized);
            mUpdDlg->raise();
            mUpdDlg->activateWindow();
        }
    }
    else
    {
        /* Store the ID of the main window to ensure that only one
         * update dialog is shown at a time. Due to manipulations with
         * OnExtraDataCanChange() and OnExtraDataChange() signals, this extra
         * data item acts like an inter-process mutex, so the first process
         * that attempts to set it will win, the rest will get a failure from
         * the SetExtraData() call. */
        mVBox.SetExtraData (VBoxDefs::GUI_UpdateDlgWinID,
                            QString ("%1").arg ((qulonglong) mMainWindow->winId()));

        if (mVBox.isOk())
        {
            /* We've got the "mutex", create a new update dialog */
            VBoxUpdateDlg *dlg = new VBoxUpdateDlg (&mUpdDlg, aForce, 0);
            dlg->setAttribute (Qt::WA_DeleteOnClose);
            Assert (dlg == mUpdDlg);

            /* Update dialog always in background mode for now.
             * if (!aForce && isAutomatic) */
            mUpdDlg->search();
            /* else mUpdDlg->show(); */
        }
    }
}

void VBoxGlobal::perDayNewVersionNotifier()
{
    showUpdateDialog (false /* force show? */);
}

// Protected members
////////////////////////////////////////////////////////////////////////////////

bool VBoxGlobal::event (QEvent *e)
{
    switch (e->type())
    {
        case VBoxDefs::AsyncEventType:
        {
            VBoxAsyncEvent *ev = (VBoxAsyncEvent *) e;
            ev->handle();
            return true;
        }

        case VBoxDefs::MediaEnumEventType:
        {
            VBoxMediaEnumEvent *ev = (VBoxMediaEnumEvent*) e;

            if (!ev->mLast)
            {
                if (ev->mMedium.state() == KMediumState_Inaccessible &&
                    !ev->mMedium.result().isOk())
                    vboxProblem().cannotGetMediaAccessibility (ev->mMedium);
                Assert (ev->mIterator != mMediaList.end());
                *(ev->mIterator) = ev->mMedium;
                emit mediumEnumerated (*ev->mIterator);
                ++ ev->mIterator;
            }
            else
            {
                /* the thread has posted the last message, wait for termination */
                mMediaEnumThread->wait();
                delete mMediaEnumThread;
                mMediaEnumThread = 0;
                emit mediumEnumFinished (mMediaList);
            }

            return true;
        }

        /* VirtualBox callback events */

        case VBoxDefs::MachineStateChangeEventType:
        {
            emit machineStateChanged (*(VBoxMachineStateChangeEvent *) e);
            return true;
        }
        case VBoxDefs::MachineDataChangeEventType:
        {
            emit machineDataChanged (*(VBoxMachineDataChangeEvent *) e);
            return true;
        }
        case VBoxDefs::MachineRegisteredEventType:
        {
            emit machineRegistered (*(VBoxMachineRegisteredEvent *) e);
            return true;
        }
        case VBoxDefs::SessionStateChangeEventType:
        {
            emit sessionStateChanged (*(VBoxSessionStateChangeEvent *) e);
            return true;
        }
        case VBoxDefs::SnapshotEventType:
        {
            emit snapshotChanged (*(VBoxSnapshotEvent *) e);
            return true;
        }
        case VBoxDefs::CanShowRegDlgEventType:
        {
            emit canShowRegDlg (((VBoxCanShowRegDlgEvent *) e)->mCanShow);
            return true;
        }
        case VBoxDefs::CanShowUpdDlgEventType:
        {
            emit canShowUpdDlg (((VBoxCanShowUpdDlgEvent *) e)->mCanShow);
            return true;
        }
        case VBoxDefs::ChangeGUILanguageEventType:
        {
            loadLanguage (static_cast<VBoxChangeGUILanguageEvent*> (e)->mLangId);
            return true;
        }
#ifdef VBOX_GUI_WITH_SYSTRAY
        case VBoxDefs::MainWindowCountChangeEventType:

            emit mainWindowCountChanged (*(VBoxMainWindowCountChangeEvent *) e);
            return true;

        case VBoxDefs::CanShowTrayIconEventType:
        {
            emit trayIconCanShow (*(VBoxCanShowTrayIconEvent *) e);
            return true;
        }
        case VBoxDefs::ShowTrayIconEventType:
        {
            emit trayIconShow (*(VBoxShowTrayIconEvent *) e);
            return true;
        }
        case VBoxDefs::TrayIconChangeEventType:
        {
            emit trayIconChanged (*(VBoxChangeTrayIconEvent *) e);
            return true;
        }
#endif
#if defined(Q_WS_MAC)
        case VBoxDefs::ChangeDockIconUpdateEventType:
        {
            emit dockIconUpdateChanged (*(VBoxChangeDockIconUpdateEvent *) e);
            return true;
        }
        case VBoxDefs::ChangePresentationmodeEventType:
        {
            emit presentationModeChanged (*(VBoxChangePresentationModeEvent *) e);
            return true;
        }
#endif
        default:
            break;
    }

    return QObject::event (e);
}

bool VBoxGlobal::eventFilter (QObject *aObject, QEvent *aEvent)
{
    if (aEvent->type() == QEvent::LanguageChange &&
        aObject->isWidgetType() &&
        static_cast <QWidget *> (aObject)->isTopLevel())
    {
        /* Catch the language change event before any other widget gets it in
         * order to invalidate cached string resources (like the details view
         * templates) that may be used by other widgets. */
        QWidgetList list = QApplication::topLevelWidgets();
        if (list.first() == aObject)
        {
            /* call this only once per every language change (see
             * QApplication::installTranslator() for details) */
            retranslateUi();
        }
    }

    return QObject::eventFilter (aObject, aEvent);
}

// Private members
////////////////////////////////////////////////////////////////////////////////

void VBoxGlobal::init()
{
#ifdef DEBUG
    mVerString += " [DEBUG]";
#endif

#ifdef Q_WS_WIN
    /* COM for the main thread is initialized in main() */
#else
    HRESULT rc = COMBase::InitializeCOM();
    if (FAILED (rc))
    {
        vboxProblem().cannotInitCOM (rc);
        return;
    }
#endif

    mVBox.createInstance (CLSID_VirtualBox);
    if (!mVBox.isOk())
    {
        vboxProblem().cannotCreateVirtualBox (mVBox);
        return;
    }

    /* create default non-null global settings */
    gset = VBoxGlobalSettings (false);

    /* try to load global settings */
    gset.load (mVBox);
    if (!mVBox.isOk() || !gset)
    {
        vboxProblem().cannotLoadGlobalConfig (mVBox, gset.lastError());
        return;
    }

    /* Load the customized language as early as possible to get possible error
     * messages translated */
    QString sLanguageId = gset.languageId();
    if (!sLanguageId.isNull())
        loadLanguage (sLanguageId);

    retranslateUi();

#ifdef VBOX_GUI_WITH_SYSTRAY
    {
        /* Increase open Fe/Qt4 windows reference count. */
        int c = mVBox.GetExtraData (VBoxDefs::GUI_MainWindowCount).toInt() + 1;
        AssertMsgReturnVoid ((c >= 0) || (mVBox.isOk()),
            ("Something went wrong with the window reference count!"));
        mVBox.SetExtraData (VBoxDefs::GUI_MainWindowCount, QString ("%1").arg (c));
        mIncreasedWindowCounter = mVBox.isOk();
        AssertReturnVoid (mIncreasedWindowCounter);
    }
#endif

    /* Initialize guest OS Type list. */
    CGuestOSTypeVector coll = mVBox.GetGuestOSTypes();
    int osTypeCount = coll.size();
    AssertMsg (osTypeCount > 0, ("Number of OS types must not be zero"));
    if (osTypeCount > 0)
    {
        /* Here we assume the 'Other' type is always the first, so we
         * remember it and will append it to the list when finished. */
        CGuestOSType otherType = coll[0];
        QString otherFamilyId (otherType.GetFamilyId());

        /* Fill the lists with all the available OS Types except
         * the 'Other' type, which will be appended. */
        for (int i = 1; i < coll.size(); ++i)
        {
            CGuestOSType os = coll[i];
            QString familyId (os.GetFamilyId());
            if (!mFamilyIDs.contains (familyId))
            {
                mFamilyIDs << familyId;
                mTypes << QList <CGuestOSType> ();
            }
            mTypes [mFamilyIDs.indexOf (familyId)].append (os);
        }

        /* Append the 'Other' OS Type to the end of list. */
        if (!mFamilyIDs.contains (otherFamilyId))
        {
            mFamilyIDs << otherFamilyId;
            mTypes << QList <CGuestOSType> ();
        }
        mTypes [mFamilyIDs.indexOf (otherFamilyId)].append (otherType);
    }

    /* Fill in OS type icon dictionary. */
    static const char *kOSTypeIcons [][2] =
    {
        {"Other",           ":/os_other.png"},
        {"DOS",             ":/os_dos.png"},
        {"Netware",         ":/os_netware.png"},
        {"L4",              ":/os_l4.png"},
        {"Windows31",       ":/os_win31.png"},
        {"Windows95",       ":/os_win95.png"},
        {"Windows98",       ":/os_win98.png"},
        {"WindowsMe",       ":/os_winme.png"},
        {"WindowsNT4",      ":/os_winnt4.png"},
        {"Windows2000",     ":/os_win2k.png"},
        {"WindowsXP",       ":/os_winxp.png"},
        {"WindowsXP_64",    ":/os_winxp_64.png"},
        {"Windows2003",     ":/os_win2k3.png"},
        {"Windows2003_64",  ":/os_win2k3_64.png"},
        {"WindowsVista",    ":/os_winvista.png"},
        {"WindowsVista_64", ":/os_winvista_64.png"},
        {"Windows2008",     ":/os_win2k8.png"},
        {"Windows2008_64",  ":/os_win2k8_64.png"},
        {"Windows7",        ":/os_win7.png"},
        {"Windows7_64",     ":/os_win7_64.png"},
        {"WindowsNT",       ":/os_win_other.png"},
        {"OS2Warp3",        ":/os_os2warp3.png"},
        {"OS2Warp4",        ":/os_os2warp4.png"},
        {"OS2Warp45",       ":/os_os2warp45.png"},
        {"OS2eCS",          ":/os_os2ecs.png"},
        {"OS2",             ":/os_os2_other.png"},
        {"Linux22",         ":/os_linux22.png"},
        {"Linux24",         ":/os_linux24.png"},
        {"Linux24_64",      ":/os_linux24_64.png"},
        {"Linux26",         ":/os_linux26.png"},
        {"Linux26_64",      ":/os_linux26_64.png"},
        {"ArchLinux",       ":/os_archlinux.png"},
        {"ArchLinux_64",    ":/os_archlinux_64.png"},
        {"Debian",          ":/os_debian.png"},
        {"Debian_64",       ":/os_debian_64.png"},
        {"OpenSUSE",        ":/os_opensuse.png"},
        {"OpenSUSE_64",     ":/os_opensuse_64.png"},
        {"Fedora",          ":/os_fedora.png"},
        {"Fedora_64",       ":/os_fedora_64.png"},
        {"Gentoo",          ":/os_gentoo.png"},
        {"Gentoo_64",       ":/os_gentoo_64.png"},
        {"Mandriva",        ":/os_mandriva.png"},
        {"Mandriva_64",     ":/os_mandriva_64.png"},
        {"RedHat",          ":/os_redhat.png"},
        {"RedHat_64",       ":/os_redhat_64.png"},
        {"Turbolinux",      ":/os_turbolinux.png"},
        {"Turbolinux_64",   ":/os_turbolinux_64.png"},
        {"Ubuntu",          ":/os_ubuntu.png"},
        {"Ubuntu_64",       ":/os_ubuntu_64.png"},
        {"Xandros",         ":/os_xandros.png"},
        {"Xandros_64",      ":/os_xandros_64.png"},
        {"Oracle",          ":/os_oracle.png"},
        {"Oracle_64",       ":/os_oracle_64.png"},
        {"Linux",           ":/os_linux_other.png"},
        {"FreeBSD",         ":/os_freebsd.png"},
        {"FreeBSD_64",      ":/os_freebsd_64.png"},
        {"OpenBSD",         ":/os_openbsd.png"},
        {"OpenBSD_64",      ":/os_openbsd_64.png"},
        {"NetBSD",          ":/os_netbsd.png"},
        {"NetBSD_64",       ":/os_netbsd_64.png"},
        {"Solaris",         ":/os_solaris.png"},
        {"Solaris_64",      ":/os_solaris_64.png"},
        {"OpenSolaris",     ":/os_opensolaris.png"},
        {"OpenSolaris_64",  ":/os_opensolaris_64.png"},
        {"QNX",             ":/os_qnx.png"},
        {"MacOS",           ":/os_macosx.png"},
        {"MacOS_64",        ":/os_macosx_64.png"},
    };
    for (uint n = 0; n < SIZEOF_ARRAY (kOSTypeIcons); ++ n)
    {
        mOsTypeIcons.insert (kOSTypeIcons [n][0],
            new QPixmap (kOSTypeIcons [n][1]));
    }

    /* fill in VM state icon map */
    static const struct
    {
        KMachineState state;
        const char *name;
    }
    kVMStateIcons[] =
    {
        {KMachineState_Null, NULL},
        {KMachineState_PoweredOff, ":/state_powered_off_16px.png"},
        {KMachineState_Saved, ":/state_saved_16px.png"},
        {KMachineState_Aborted, ":/state_aborted_16px.png"},
        {KMachineState_Teleported, ":/state_saved_16px.png"},           /** @todo Live Migration: New icon? (not really important) */
        {KMachineState_Running, ":/state_running_16px.png"},
        {KMachineState_Paused, ":/state_paused_16px.png"},
        {KMachineState_Teleporting, ":/state_running_16px.png"},        /** @todo Live Migration: New icon? (not really important) */
        {KMachineState_LiveSnapshotting, ":/state_running_16px.png"},   /** @todo Live Migration: New icon? (not really important) */
        {KMachineState_Stuck, ":/state_stuck_16px.png"},
        {KMachineState_Starting, ":/state_running_16px.png"}, /// @todo (dmik) separate icon?
        {KMachineState_Stopping, ":/state_running_16px.png"}, /// @todo (dmik) separate icon?
        {KMachineState_Saving, ":/state_saving_16px.png"},
        {KMachineState_Restoring, ":/state_restoring_16px.png"},
        {KMachineState_TeleportingPausedVM, ":/state_saving_16px.png"}, /** @todo Live Migration: New icon? (not really important) */
        {KMachineState_TeleportingIn, ":/state_restoring_16px.png"},    /** @todo Live Migration: New icon? (not really important) */
        {KMachineState_RestoringSnapshot, ":/state_discarding_16px.png"},
        {KMachineState_DeletingSnapshot, ":/state_discarding_16px.png"},
        {KMachineState_SettingUp, ":/settings_16px.png"},
    };
    for (uint n = 0; n < SIZEOF_ARRAY (kVMStateIcons); n ++)
    {
        mVMStateIcons.insert (kVMStateIcons [n].state,
            new QPixmap (kVMStateIcons [n].name));
    }

    /* initialize state colors map */
    mVMStateColors.insert (KMachineState_Null,          new QColor (Qt::red));
    mVMStateColors.insert (KMachineState_PoweredOff,    new QColor (Qt::gray));
    mVMStateColors.insert (KMachineState_Saved,         new QColor (Qt::yellow));
    mVMStateColors.insert (KMachineState_Aborted,       new QColor (Qt::darkRed));
    mVMStateColors.insert (KMachineState_Teleported,    new QColor (Qt::red));
    mVMStateColors.insert (KMachineState_Running,       new QColor (Qt::green));
    mVMStateColors.insert (KMachineState_Paused,        new QColor (Qt::darkGreen));
    mVMStateColors.insert (KMachineState_Stuck,         new QColor (Qt::darkMagenta));
    mVMStateColors.insert (KMachineState_Teleporting,   new QColor (Qt::blue));
    mVMStateColors.insert (KMachineState_LiveSnapshotting, new QColor (Qt::green));
    mVMStateColors.insert (KMachineState_Starting,      new QColor (Qt::green));
    mVMStateColors.insert (KMachineState_Stopping,      new QColor (Qt::green));
    mVMStateColors.insert (KMachineState_Saving,        new QColor (Qt::green));
    mVMStateColors.insert (KMachineState_Restoring,     new QColor (Qt::green));
    mVMStateColors.insert (KMachineState_TeleportingPausedVM, new QColor (Qt::blue));
    mVMStateColors.insert (KMachineState_TeleportingIn, new QColor (Qt::blue));
    mVMStateColors.insert (KMachineState_RestoringSnapshot, new QColor (Qt::green));
    mVMStateColors.insert (KMachineState_DeletingSnapshot, new QColor (Qt::green));
    mVMStateColors.insert (KMachineState_SettingUp,     new QColor (Qt::green));

    /* online/offline snapshot icons */
    mOfflineSnapshotIcon = QPixmap (":/offline_snapshot_16px.png");
    mOnlineSnapshotIcon = QPixmap (":/online_snapshot_16px.png");

    qApp->installEventFilter (this);

    /* process command line */

    bool bForceSeamless = false;

    vm_render_mode_str = RTStrDup (virtualBox()
            .GetExtraData (VBoxDefs::GUI_RenderMode).toAscii().constData());

#ifdef Q_WS_X11
    mIsKWinManaged = X11IsWindowManagerKWin();
#endif

#ifdef VBOX_WITH_DEBUGGER_GUI
# ifdef VBOX_WITH_DEBUGGER_GUI_MENU
    mDbgEnabled = true;
# else
    mDbgEnabled = RTEnvExist("VBOX_GUI_DBG_ENABLED");
# endif
    mDbgAutoShow = mDbgAutoShowCommandLine = mDbgAutoShowStatistics
        = RTEnvExist("VBOX_GUI_DBG_AUTO_SHOW");
    mStartPaused = false;
#endif

    mShowStartVMErrors = true;
    bool startVM = false;
    QString vmNameOrUuid;

    int argc = qApp->argc();
    int i = 1;
    while (i < argc)
    {
        const char *arg = qApp->argv() [i];
        /* NOTE: the check here must match the corresponding check for the
         * options to start a VM in main.cpp and hardenedmain.cpp exactly,
         * otherwise there will be weird error messages. */
        if (   !::strcmp (arg, "--startvm")
            || !::strcmp (arg, "-startvm"))
        {
            if (++i < argc)
            {
                vmNameOrUuid = QString (qApp->argv() [i]);
                startVM = true;
            }
        }
        else if (!::strcmp(arg, "-seamless") || !::strcmp(arg, "--seamless"))
        {
            bForceSeamless = true;
        }
#ifdef VBOX_GUI_WITH_SYSTRAY
        else if (!::strcmp (arg, "-systray") || !::strcmp (arg, "--systray"))
        {
            mIsTrayMenu = true;
        }
#endif
        else if (!::strcmp (arg, "-comment") || !::strcmp (arg, "--comment"))
        {
            ++i;
        }
        else if (!::strcmp (arg, "-rmode") || !::strcmp (arg, "--rmode"))
        {
            if (++i < argc)
                vm_render_mode_str = qApp->argv() [i];
        }
#ifdef VBOX_WITH_DEBUGGER_GUI
        else if (!::strcmp (arg, "-dbg") || !::strcmp (arg, "--dbg"))
        {
            mDbgEnabled = true;
        }
        else if (!::strcmp( arg, "-debug") || !::strcmp (arg, "--debug"))
        {
            mDbgEnabled = true;
            mDbgAutoShow = mDbgAutoShowCommandLine = mDbgAutoShowStatistics = true;
            mStartPaused = true;
        }
        else if (!::strcmp (arg, "--debug-command-line"))
        {
            mDbgEnabled = true;
            mDbgAutoShow = mDbgAutoShowCommandLine = true;
            mStartPaused = true;
        }
        else if (!::strcmp (arg, "--debug-statistics"))
        {
            mDbgEnabled = true;
            mDbgAutoShow = mDbgAutoShowStatistics = true;
            mStartPaused = true;
        }
        else if (!::strcmp (arg, "-no-debug") || !::strcmp (arg, "--no-debug"))
        {
            mDbgEnabled = false;
            mDbgAutoShow = false;
            mDbgAutoShowCommandLine = false;
            mDbgAutoShowStatistics = false;
        }
        /* Not quite debug options, but they're only useful with the debugger bits. */
        else if (!::strcmp (arg, "--start-paused"))
        {
            mStartPaused = true;
        }
        else if (!::strcmp (arg, "--start-running"))
        {
            mStartPaused = false;
        }
        else if (!::strcmp (arg, "--no-startvm-errormsgbox"))
        {
            mShowStartVMErrors = false;
        }
#endif
        /** @todo add an else { msgbox(syntax error); exit(1); } here, pretty please... */
        i++;
    }

    if (startVM)
    {
        QUuid uuid = QUuid(vmNameOrUuid);
        if (!uuid.isNull())
        {
            vmUuid = vmNameOrUuid;
        }
        else
        {
            CMachine m = mVBox.FindMachine (vmNameOrUuid);
            if (m.isNull())
            {
                if (showStartVMErrors())
                    vboxProblem().cannotFindMachineByName (mVBox, vmNameOrUuid);
                return;
            }
            vmUuid = m.GetId();
        }
    }

    if (bForceSeamless && !vmUuid.isEmpty())
    {
        mVBox.GetMachine(vmUuid).SetExtraData(VBoxDefs::GUI_Seamless, "on");
    }

    vm_render_mode = vboxGetRenderMode (vm_render_mode_str);

    /* setup the callback */
    callback = CVirtualBoxCallback (new VBoxCallback (*this));
    mVBox.RegisterCallback (callback);
    AssertWrapperOk (mVBox);
    if (!mVBox.isOk())
        return;

#ifdef VBOX_WITH_DEBUGGER_GUI
    /* setup the debugger gui. */
    if (RTEnvExist("VBOX_GUI_NO_DEBUGGER"))
        mDbgEnabled = mDbgAutoShow =  mDbgAutoShowCommandLine = mDbgAutoShowStatistics = false;
    if (mDbgEnabled)
    {
        int vrc = SUPR3HardenedLdrLoadAppPriv("VBoxDbg", &mhVBoxDbg);
        if (RT_FAILURE(vrc))
        {
            mhVBoxDbg = NIL_RTLDRMOD;
            mDbgAutoShow =  mDbgAutoShowCommandLine = mDbgAutoShowStatistics = false;
            LogRel(("Failed to load VBoxDbg, rc=%Rrc\n", vrc));
        }
    }
#endif

    mValid = true;
}

/** @internal
 *
 *  This method should be never called directly. It is called automatically
 *  when the application terminates.
 */
void VBoxGlobal::cleanup()
{
    /* sanity check */
    if (!sVBoxGlobalInCleanup)
    {
        AssertMsgFailed (("Should never be called directly\n"));
        return;
    }

#ifdef VBOX_GUI_WITH_SYSTRAY
    if (mIncreasedWindowCounter)
    {
        /* Decrease open Fe/Qt4 windows reference count. */
        int c = mVBox.GetExtraData (VBoxDefs::GUI_MainWindowCount).toInt() - 1;
        AssertMsg ((c >= 0) || (mVBox.isOk()),
            ("Something went wrong with the window reference count!"));
        if (c < 0)
            c = 0;   /* Clean up the mess. */
        mVBox.SetExtraData (VBoxDefs::GUI_MainWindowCount,
                            (c > 0) ? QString ("%1").arg (c) : NULL);
        AssertWrapperOk (mVBox);
        if (c == 0)
        {
            mVBox.SetExtraData (VBoxDefs::GUI_TrayIconWinID, NULL);
            AssertWrapperOk (mVBox);
        }
    }
#endif

    if (!callback.isNull())
    {
        mVBox.UnregisterCallback (callback);
        AssertWrapperOk (mVBox);
        callback.detach();
    }

    if (mMediaEnumThread)
    {
        /* sVBoxGlobalInCleanup is true here, so just wait for the thread */
        mMediaEnumThread->wait();
        delete mMediaEnumThread;
        mMediaEnumThread = 0;
    }

#ifdef VBOX_WITH_REGISTRATION
    if (mRegDlg)
        mRegDlg->close();
#endif

    if (mConsoleWnd)
        delete mConsoleWnd;
    if (mSelectorWnd)
        delete mSelectorWnd;
#ifdef VBOX_WITH_NEW_RUNTIME_CORE
    if (m_pVirtualMachine)
        delete m_pVirtualMachine;
#endif

    /* ensure CGuestOSType objects are no longer used */
    mFamilyIDs.clear();
    mTypes.clear();

    /* media list contains a lot of CUUnknown, release them */
    mMediaList.clear();
    /* the last step to ensure we don't use COM any more */
    mVBox.detach();

    /* There may be VBoxMediaEnumEvent instances still in the message
     * queue which reference COM objects. Remove them to release those objects
     * before uninitializing the COM subsystem. */
    QApplication::removePostedEvents (this);

#ifdef Q_WS_WIN
    /* COM for the main thread is shutdown in main() */
#else
    COMBase::CleanupCOM();
#endif

    mValid = false;
}

/** @fn vboxGlobal
 *
 *  Shortcut to the static VBoxGlobal::instance() method, for convenience.
 */


/**
 *  USB Popup Menu class methods
 *  This class provides the list of USB devices attached to the host.
 */
VBoxUSBMenu::VBoxUSBMenu (QWidget *aParent) : QMenu (aParent)
{
    connect (this, SIGNAL (aboutToShow()),
             this, SLOT   (processAboutToShow()));
//    connect (this, SIGNAL (hovered (QAction *)),
//             this, SLOT   (processHighlighted (QAction *)));
}

const CUSBDevice& VBoxUSBMenu::getUSB (QAction *aAction)
{
    return mUSBDevicesMap [aAction];
}

void VBoxUSBMenu::setConsole (const CConsole &aConsole)
{
    mConsole = aConsole;
}

void VBoxUSBMenu::processAboutToShow()
{
    clear();
    mUSBDevicesMap.clear();

    CHost host = vboxGlobal().virtualBox().GetHost();

    bool isUSBEmpty = host.GetUSBDevices().size() == 0;
    if (isUSBEmpty)
    {
        QAction *action = addAction (tr ("<no devices available>", "USB devices"));
        action->setEnabled (false);
        action->setToolTip (tr ("No supported devices connected to the host PC",
                                "USB device tooltip"));
    }
    else
    {
        CHostUSBDeviceVector devvec = host.GetUSBDevices();
        for (int i = 0; i < devvec.size(); ++i)
        {
            CHostUSBDevice dev = devvec[i];
            CUSBDevice usb (dev);
            QAction *action = addAction (vboxGlobal().details (usb));
            action->setCheckable (true);
            mUSBDevicesMap [action] = usb;
            /* check if created item was alread attached to this session */
            if (!mConsole.isNull())
            {
                CUSBDevice attachedUSB =
                    mConsole.FindUSBDeviceById (usb.GetId());
                action->setChecked (!attachedUSB.isNull());
                action->setEnabled (dev.GetState() !=
                                    KUSBDeviceState_Unavailable);
            }
        }
    }
}

bool VBoxUSBMenu::event(QEvent *aEvent)
{
    /* We provide dynamic tooltips for the usb devices */
    if (aEvent->type() == QEvent::ToolTip)
    {
        QHelpEvent *helpEvent = static_cast<QHelpEvent *> (aEvent);
        QAction *action = actionAt (helpEvent->pos());
        if (action)
        {
            CUSBDevice usb = mUSBDevicesMap [action];
            if (!usb.isNull())
            {
                QToolTip::showText (helpEvent->globalPos(), vboxGlobal().toolTip (usb));
                return true;
            }
        }
    }
    return QMenu::event (aEvent);
}

/**
 *  Enable/Disable Menu class.
 *  This class provides enable/disable menu items.
 */
VBoxSwitchMenu::VBoxSwitchMenu (QWidget *aParent, QAction *aAction,
                                bool aInverted)
    : QMenu (aParent), mAction (aAction), mInverted (aInverted)
{
    /* this menu works only with toggle action */
    Assert (aAction->isCheckable());
    addAction(aAction);
    connect (this, SIGNAL (aboutToShow()),
             this, SLOT   (processAboutToShow()));
}

void VBoxSwitchMenu::setToolTip (const QString &aTip)
{
    mAction->setToolTip (aTip);
}

void VBoxSwitchMenu::processAboutToShow()
{
    QString text = mAction->isChecked() ^ mInverted ? tr ("Disable") : tr ("Enable");
    mAction->setText (text);
}

