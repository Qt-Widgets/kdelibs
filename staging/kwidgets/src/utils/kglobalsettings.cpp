/* This file is part of the KDE libraries
   Copyright (C) 2000, 2006 David Faure <faure@kde.org>
   Copyright 2008 Friedrich W. H. Kossebau <kossebau@kde.org>

   This library is free software; you can redistribute it and/or
   modify it under the terms of the GNU Library General Public
   License version 2 as published by the Free Software Foundation.

   This library is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
   Library General Public License for more details.

   You should have received a copy of the GNU Library General Public License
   along with this library; see the file COPYING.LIB.  If not, write to
   the Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
   Boston, MA 02110-1301, USA.
*/

#include "kglobalsettings.h"

#include <kconfig.h>

#include <kcolorscheme.h>

//#include <kstyle.h>

#include <QColor>
#include <QCursor>
#include <QDesktopWidget>
#include <QtCore/QDir>
#include <QFont>
#include <QFontDatabase>
#include <QFontInfo>
#include <QKeySequence>
#include <QPixmap>
#include <QPixmapCache>
#include <QApplication>
#include <QtDBus/QtDBus>
#include <QStyleFactory>
#include <qstandardpaths.h>

// next two needed so we can set their palettes
#include <QToolTip>
#include <QWhatsThis>

#ifdef Q_OS_WIN
#include <windows.h>
#include <kkernel_win.h>

static QRgb qt_colorref2qrgb(COLORREF col)
{
    return qRgb(GetRValue(col),GetGValue(col),GetBValue(col));
}
#endif
#include <config-kwidgets.h>
#if HAVE_X11
#include <X11/Xlib.h>
#include <xcb/xcb.h>
#ifdef HAVE_XCURSOR // TODO NOT DEFINED ANYMORE. Can we drop X cursor themes?
#include <X11/Xcursor/Xcursor.h>
#endif
//#include "fixx11h.h"
#include <QX11Info>
#endif

#include <stdlib.h>
#include <kconfiggroup.h>


//static QColor *_buttonBackground = 0;
static KGlobalSettings::GraphicEffects _graphicEffects = KGlobalSettings::NoEffects;

// TODO: merge this with KGlobalSettings::Private
//
// F. Kossebau: KDE5: think to make all methods static and not expose an object,
// making KGlobalSettings rather a namespace
// D. Faure: how would people connect to signals, then?
class KGlobalSettingsData
{
  public:
    // if adding a new type here also add an entry to DefaultFontData
    enum FontTypes
    {
        GeneralFont = 0,
        FixedFont,
        ToolbarFont,
        MenuFont,
        WindowTitleFont,
        TaskbarFont ,
        SmallestReadableFont,
        FontTypesCount
    };

  public:
    KGlobalSettingsData();
    ~KGlobalSettingsData();

  public:
    static KGlobalSettingsData* self();

  public: // access, is not const due to caching
    QFont font( FontTypes fontType );
    QFont largeFont( const QString& text );
    KGlobalSettings::KMouseSettings& mouseSettings();

  public:
    void dropFontSettingsCache();
    void dropMouseSettingsCache();

  protected:
    QFont* mFonts[FontTypesCount];
    QFont* mLargeFont;
    KGlobalSettings::KMouseSettings* mMouseSettings;
};

KGlobalSettingsData::KGlobalSettingsData()
  : mLargeFont( 0 ),
    mMouseSettings( 0 )
{
    for( int i=0; i<FontTypesCount; ++i )
        mFonts[i] = 0;
}

KGlobalSettingsData::~KGlobalSettingsData()
{
    for( int i=0; i<FontTypesCount; ++i )
        delete mFonts[i];
    delete mLargeFont;

    delete mMouseSettings;
}

Q_GLOBAL_STATIC( KGlobalSettingsData, globalSettingsDataSingleton )

inline KGlobalSettingsData* KGlobalSettingsData::self()
{
    return globalSettingsDataSingleton();
}


class KGlobalSettings::Private
{
    public:
        Private(KGlobalSettings *q)
            : q(q), activated(false), paletteCreated(false)
        {
            kdeFullSession = !qgetenv("KDE_FULL_SESSION").isEmpty();
        }

        QPalette createApplicationPalette(const KSharedConfigPtr &config);
        QPalette createNewApplicationPalette(const KSharedConfigPtr &config);
        void _k_slotNotifyChange(int, int);

        void propagateQtSettings();
        void kdisplaySetPalette();
        void kdisplaySetStyle();
        void kdisplaySetFont();
        void applyGUIStyle();

        /**
         * @internal
         *
         * Ensures that cursors are loaded from the theme KDE is configured
         * to use. Note that calling this function doesn't cause existing
         * cursors to be reloaded. Reloading already created cursors is
         * handled by the KCM when a cursor theme is applied.
         *
         * It is not necessary to call this function when KGlobalSettings
         * is initialized.
         */
        void applyCursorTheme();

        static void reloadStyleSettings();

        KGlobalSettings *q;
        bool activated;
        bool paletteCreated;
        bool kdeFullSession;
        QPalette applicationPalette;
};

// class for access to KGlobalSettings constructor
class KGlobalSettingsSingleton
{
public:
    KGlobalSettings object;
};

Q_GLOBAL_STATIC(KGlobalSettingsSingleton, s_self)

KGlobalSettings* KGlobalSettings::self()
{
    return &s_self()->object;
}

KGlobalSettings::KGlobalSettings()
    : QObject(0), d(new Private(this))
{
}

KGlobalSettings::~KGlobalSettings()
{
    delete d;
}

void KGlobalSettings::activate()
{
    activate(ApplySettings | ListenForChanges);
}

void KGlobalSettings::activate(ActivateOptions options)
{
    if (!d->activated) {
        d->activated = true;

        if (options & ListenForChanges) {
            QDBusConnection::sessionBus().connect( QString(), "/KGlobalSettings", "org.kde.KGlobalSettings",
                                                   "notifyChange", this, SLOT(_k_slotNotifyChange(int,int)) );
        }

        if (options & ApplySettings) {
            d->kdisplaySetStyle(); // implies palette setup
            d->kdisplaySetFont();
            d->propagateQtSettings();
        }
    }
}

// Qt5 TODO: implement QPlatformIntegration::styleHint so that it reads a Qt or KDE setting,
// so that apps can just use QApplication::startDragDistance().
int KGlobalSettings::dndEventDelay()
{
    KConfigGroup g( KSharedConfig::openConfig(), "General" );
    return g.readEntry("StartDragDist", QApplication::startDragDistance());
}

bool KGlobalSettings::singleClick()
{
    KConfigGroup g( KSharedConfig::openConfig(), "KDE" );
    return g.readEntry("SingleClick", KDE_DEFAULT_SINGLECLICK );
}

bool KGlobalSettings::changeCursorOverIcon()
{
    KConfigGroup g( KSharedConfig::openConfig(), "KDE" );
    return g.readEntry("ChangeCursor", KDE_DEFAULT_CHANGECURSOR);
}

int KGlobalSettings::autoSelectDelay()
{
    KConfigGroup g( KSharedConfig::openConfig(), "KDE" );
    return g.readEntry("AutoSelectDelay", KDE_DEFAULT_AUTOSELECTDELAY);
}

KGlobalSettings::Completion KGlobalSettings::completionMode()
{
    int completion;
    KConfigGroup g( KSharedConfig::openConfig(), "General" );
    completion = g.readEntry("completionMode", -1);
    if ((completion < (int) CompletionNone) ||
        (completion > (int) CompletionPopupAuto))
      {
        completion = (int) CompletionPopup; // Default
      }
  return (Completion) completion;
}

bool KGlobalSettings::showContextMenusOnPress ()
{
    KConfigGroup g(KSharedConfig::openConfig(), "ContextMenus");
    return g.readEntry("ShowOnPress", true);
}

// NOTE: keep this in sync with kdebase/workspace/kcontrol/colors/colorscm.cpp
QColor KGlobalSettings::inactiveTitleColor()
{
#ifdef Q_OS_WIN
    return qt_colorref2qrgb(GetSysColor(COLOR_INACTIVECAPTION));
#else
    KConfigGroup g( KSharedConfig::openConfig(), "WM" );
    return g.readEntry( "inactiveBackground", QColor(224,223,222) );
#endif
}

// NOTE: keep this in sync with kdebase/workspace/kcontrol/colors/colorscm.cpp
QColor KGlobalSettings::inactiveTextColor()
{
#ifdef Q_OS_WIN
    return qt_colorref2qrgb(GetSysColor(COLOR_INACTIVECAPTIONTEXT));
#else
    KConfigGroup g( KSharedConfig::openConfig(), "WM" );
    return g.readEntry( "inactiveForeground", QColor(75,71,67) );
#endif
}

// NOTE: keep this in sync with kdebase/workspace/kcontrol/colors/colorscm.cpp
QColor KGlobalSettings::activeTitleColor()
{
#ifdef Q_OS_WIN
    return qt_colorref2qrgb(GetSysColor(COLOR_ACTIVECAPTION));
#else
    KConfigGroup g( KSharedConfig::openConfig(), "WM" );
    return g.readEntry( "activeBackground", QColor(48,174,232));
#endif
}

// NOTE: keep this in sync with kdebase/workspace/kcontrol/colors/colorscm.cpp
QColor KGlobalSettings::activeTextColor()
{
#ifdef Q_OS_WIN
    return qt_colorref2qrgb(GetSysColor(COLOR_CAPTIONTEXT));
#else
    KConfigGroup g( KSharedConfig::openConfig(), "WM" );
    return g.readEntry( "activeForeground", QColor(255,255,255) );
#endif
}

int KGlobalSettings::contrast()
{
    return KColorScheme::contrast();
}

qreal KGlobalSettings::contrastF(const KSharedConfigPtr &config)
{
    return KColorScheme::contrastF(config);
}

bool KGlobalSettings::shadeSortColumn()
{
    KConfigGroup g( KSharedConfig::openConfig(), "General" );
    return g.readEntry( "shadeSortColumn", KDE_DEFAULT_SHADE_SORT_COLUMN );
}

bool KGlobalSettings::allowDefaultBackgroundImages()
{
    KConfigGroup g( KSharedConfig::openConfig(), "General" );
    return g.readEntry( "allowDefaultBackgroundImages", KDE_DEFAULT_ALLOW_DEFAULT_BACKGROUND_IMAGES );
}

struct KFontData
{
    const char* ConfigGroupKey;
    const char* ConfigKey;
    const char* FontName;
    int Size;
    int Weight;
    QFont::StyleHint StyleHint;
};

// NOTE: keep in sync with kdebase/workspace/kcontrol/fonts/fonts.cpp
static const char GeneralId[] =      "General";
static const char DefaultFont[] =    "Sans Serif";
#ifdef Q_OS_MAC
static const char DefaultMacFont[] = "Lucida Grande";
#endif

static const KFontData DefaultFontData[KGlobalSettingsData::FontTypesCount] =
{
#ifdef Q_OS_MAC
    { GeneralId, "font",        DefaultMacFont, 13, -1, QFont::SansSerif },
    { GeneralId, "fixed",       "Monaco",       10, -1, QFont::TypeWriter },
    { GeneralId, "toolBarFont", DefaultMacFont, 11, -1, QFont::SansSerif },
    { GeneralId, "menuFont",    DefaultMacFont, 13, -1, QFont::SansSerif },
#else
    { GeneralId, "font",        DefaultFont, 9, -1, QFont::SansSerif },
    { GeneralId, "fixed",       "Monospace", 9, -1, QFont::TypeWriter },
    { GeneralId, "toolBarFont", DefaultFont,  8, -1, QFont::SansSerif },
    { GeneralId, "menuFont",    DefaultFont, 9, -1, QFont::SansSerif },
#endif
    { "WM",      "activeFont",           DefaultFont,  8, -1, QFont::SansSerif },
    { GeneralId, "taskbarFont",          DefaultFont, 9, -1, QFont::SansSerif },
    { GeneralId, "smallestReadableFont", DefaultFont,  8, -1, QFont::SansSerif }
};

QFont KGlobalSettingsData::font( FontTypes fontType )
{
    QFont* cachedFont = mFonts[fontType];

    if (!cachedFont)
    {
        const KFontData& fontData = DefaultFontData[fontType];
        cachedFont = new QFont( fontData.FontName, fontData.Size, fontData.Weight );
        cachedFont->setStyleHint( fontData.StyleHint );

        const KConfigGroup configGroup( KSharedConfig::openConfig(), fontData.ConfigGroupKey );
        *cachedFont = configGroup.readEntry( fontData.ConfigKey, *cachedFont );

        mFonts[fontType] = cachedFont;
    }

    return *cachedFont;
}

QFont KGlobalSettings::generalFont()
{
    return KGlobalSettingsData::self()->font( KGlobalSettingsData::GeneralFont );
}
QFont KGlobalSettings::fixedFont()
{
    return KGlobalSettingsData::self()->font( KGlobalSettingsData::FixedFont );
}
QFont KGlobalSettings::toolBarFont()
{
    return KGlobalSettingsData::self()->font( KGlobalSettingsData::ToolbarFont );
}
QFont KGlobalSettings::menuFont()
{
    return KGlobalSettingsData::self()->font( KGlobalSettingsData::MenuFont );
}
QFont KGlobalSettings::windowTitleFont()
{
    return KGlobalSettingsData::self()->font( KGlobalSettingsData::WindowTitleFont );
}
QFont KGlobalSettings::taskbarFont()
{
    return KGlobalSettingsData::self()->font( KGlobalSettingsData::TaskbarFont );
}
QFont KGlobalSettings::smallestReadableFont()
{
    return KGlobalSettingsData::self()->font( KGlobalSettingsData::SmallestReadableFont );
}


QFont KGlobalSettingsData::largeFont( const QString& text )
{
    QFontDatabase db;
    QStringList fam = db.families();

    // Move a bunch of preferred fonts to the front.
    // most preferred last
    static const char* const PreferredFontNames[] =
    {
        "Arial",
        "Sans Serif",
        "Verdana",
        "Tahoma",
        "Lucida Sans",
        "Lucidux Sans",
        "Nimbus Sans",
        "Gothic I"
    };
    static const unsigned int PreferredFontNamesCount = sizeof(PreferredFontNames)/sizeof(const char*);
    for( unsigned int i=0; i<PreferredFontNamesCount; ++i )
    {
        const QString fontName (PreferredFontNames[i]);
        if (fam.removeAll(fontName)>0)
            fam.prepend(fontName);
    }

    if (mLargeFont) {
        fam.prepend(mLargeFont->family());
        delete mLargeFont;
    }

    for(QStringList::ConstIterator it = fam.constBegin();
        it != fam.constEnd(); ++it)
    {
        if (db.isSmoothlyScalable(*it) && !db.isFixedPitch(*it))
        {
            QFont font(*it);
            font.setPixelSize(75);
            QFontMetrics metrics(font);
            int h = metrics.height();
            if ((h < 60) || ( h > 90))
                continue;

            bool ok = true;
            for(int i = 0; i < text.length(); i++)
            {
                if (!metrics.inFont(text[i]))
                {
                    ok = false;
                    break;
                }
            }
            if (!ok)
                continue;

            font.setPointSize(48);
            mLargeFont = new QFont(font);
            return *mLargeFont;
        }
    }
    mLargeFont = new QFont( font(GeneralFont) );
    mLargeFont->setPointSize(48);
    return *mLargeFont;
}
QFont KGlobalSettings::largeFont( const QString& text )
{
    return KGlobalSettingsData::self()->largeFont( text );
}

void KGlobalSettingsData::dropFontSettingsCache()
{
    for( int i=0; i<FontTypesCount; ++i )
    {
        delete mFonts[i];
        mFonts[i] = 0;
    }
    delete mLargeFont;
    mLargeFont = 0;
}

KGlobalSettings::KMouseSettings& KGlobalSettingsData::mouseSettings()
{
    if (!mMouseSettings)
    {
        mMouseSettings = new KGlobalSettings::KMouseSettings;
        KGlobalSettings::KMouseSettings& s = *mMouseSettings; // for convenience

#ifndef Q_OS_WIN
        KConfigGroup g( KSharedConfig::openConfig(), "Mouse" );
        QString setting = g.readEntry("MouseButtonMapping");
        if (setting == "RightHanded")
            s.handed = KGlobalSettings::KMouseSettings::RightHanded;
        else if (setting == "LeftHanded")
            s.handed = KGlobalSettings::KMouseSettings::LeftHanded;
        else
        {
#if HAVE_X11
            // get settings from X server
            // This is a simplified version of the code in input/mouse.cpp
            // Keep in sync !
            s.handed = KGlobalSettings::KMouseSettings::RightHanded;
            unsigned char map[20];
            int num_buttons = XGetPointerMapping(QX11Info::display(), map, 20);
            if( num_buttons == 2 )
            {
                if ( (int)map[0] == 1 && (int)map[1] == 2 )
                    s.handed = KGlobalSettings::KMouseSettings::RightHanded;
                else if ( (int)map[0] == 2 && (int)map[1] == 1 )
                    s.handed = KGlobalSettings::KMouseSettings::LeftHanded;
            }
            else if( num_buttons >= 3 )
            {
                if ( (int)map[0] == 1 && (int)map[2] == 3 )
                    s.handed = KGlobalSettings::KMouseSettings::RightHanded;
                else if ( (int)map[0] == 3 && (int)map[2] == 1 )
                    s.handed = KGlobalSettings::KMouseSettings::LeftHanded;
            }
#else
        // FIXME: Implement on other platforms
#endif
        }
#endif //Q_OS_WIN
    }
#ifdef Q_OS_WIN
    //not cached
#ifndef _WIN32_WCE
    mMouseSettings->handed = (GetSystemMetrics(SM_SWAPBUTTON) ?
        KGlobalSettings::KMouseSettings::LeftHanded :
        KGlobalSettings::KMouseSettings::RightHanded);
#else
// There is no mice under wince
    mMouseSettings->handed =KGlobalSettings::KMouseSettings::RightHanded;
#endif
#endif
    return *mMouseSettings;
}
// KDE5: make this a const return?
KGlobalSettings::KMouseSettings & KGlobalSettings::mouseSettings()
{
    return KGlobalSettingsData::self()->mouseSettings();
}

void KGlobalSettingsData::dropMouseSettingsCache()
{
#ifndef Q_OS_WIN
    delete mMouseSettings;
    mMouseSettings = 0;
#endif
}

QString KGlobalSettings::desktopPath()
{
    QString path = QStandardPaths::writableLocation( QStandardPaths::DesktopLocation );
    return path.isEmpty() ? QDir::homePath() : path;
}

// This was the KDE-specific autostart folder in KDEHOME.
// KDE 5 : re-evaluate this, I'd say the xdg autostart spec supersedes this, and is sufficient
// (since there's a GUI for creating the necessary desktop files)
#if 0
QString KGlobalSettings::autostartPath()
{
    QString s_autostartPath;
    KConfigGroup g( KSharedConfig::openConfig(), "Paths" );
    s_autostartPath = KGlobal::dirs()->localkdedir() + "Autostart/";
    s_autostartPath = g.readPathEntry( "Autostart" , s_autostartPath );
    s_autostartPath = QDir::cleanPath( s_autostartPath );
    if ( !s_autostartPath.endsWith( '/' ) ) {
        s_autostartPath.append( QLatin1Char( '/' ) );
    }
    return s_autostartPath;
}
#endif

QString KGlobalSettings::documentPath()
{
    QString path = QStandardPaths::writableLocation( QStandardPaths::DocumentsLocation );
    return path.isEmpty() ? QDir::homePath() : path;
}

QString KGlobalSettings::downloadPath()
{
    QString path = QStandardPaths::writableLocation( QStandardPaths::DownloadLocation );
    return path.isEmpty() ? QDir::homePath() : path;
}

QString KGlobalSettings::videosPath()
{
    QString path = QStandardPaths::writableLocation( QStandardPaths::MoviesLocation );
    return path.isEmpty() ? QDir::homePath() : path;
}

QString KGlobalSettings::picturesPath()
{
    QString path = QStandardPaths::writableLocation( QStandardPaths::PicturesLocation );
    return path.isEmpty() ? QDir::homePath() :path;
}

QString KGlobalSettings::musicPath()
{
    QString path = QStandardPaths::writableLocation( QStandardPaths::MusicLocation );
    return path.isEmpty() ? QDir::homePath() : path;
}

bool KGlobalSettings::isMultiHead()
{
#ifdef Q_OS_WIN
    return GetSystemMetrics(SM_CMONITORS) > 1;
#else
    QByteArray multiHead = qgetenv("KDE_MULTIHEAD");
    if (!multiHead.isEmpty()) {
        return (multiHead.toLower() == "true");
    }
    return false;
#endif
}

bool KGlobalSettings::wheelMouseZooms()
{
    KConfigGroup g( KSharedConfig::openConfig(), "KDE" );
    return g.readEntry( "WheelMouseZooms", KDE_DEFAULT_WHEEL_ZOOM );
}

QRect KGlobalSettings::splashScreenDesktopGeometry()
{
    return QApplication::desktop()->screenGeometry(QCursor::pos());
}

QRect KGlobalSettings::desktopGeometry(const QPoint& point)
{
    return QApplication::desktop()->screenGeometry(point);
}

QRect KGlobalSettings::desktopGeometry(const QWidget* w)
{
    return QApplication::desktop()->screenGeometry(w);
}

bool KGlobalSettings::showIconsOnPushButtons()
{
    KConfigGroup g( KSharedConfig::openConfig(), "KDE" );
    return g.readEntry("ShowIconsOnPushButtons",
                       KDE_DEFAULT_ICON_ON_PUSHBUTTON);
}

bool KGlobalSettings::naturalSorting()
{
    KConfigGroup g( KSharedConfig::openConfig(), "KDE" );
    return g.readEntry("NaturalSorting",
                       KDE_DEFAULT_NATURAL_SORTING);
}

KGlobalSettings::GraphicEffects KGlobalSettings::graphicEffectsLevel()
{
    // This variable stores whether _graphicEffects has the default value because it has not been
    // loaded yet, or if it has been loaded from the user settings or defaults and contains a valid
    // value.
    static bool _graphicEffectsInitialized = false;

    if (!_graphicEffectsInitialized) {
        _graphicEffectsInitialized = true;
        Private::reloadStyleSettings();
    }

    return _graphicEffects;
}

KGlobalSettings::GraphicEffects KGlobalSettings::graphicEffectsLevelDefault()
{
    // For now, let always enable animations by default. The plan is to make
    // this code a bit smarter. (ereslibre)

    return ComplexAnimationEffects;
}

#ifndef KDE_NO_DEPRECATED
bool KGlobalSettings::showFilePreview(const QUrl &url)
{
    KConfigGroup g(KSharedConfig::openConfig(), "PreviewSettings");
    bool defaultSetting = url.isLocalFile(); // ## incorrect, use KProtocolInfo::showFilePreview instead
    return g.readEntry(url.scheme(), defaultSetting );
}
#endif

bool KGlobalSettings::opaqueResize()
{
    KConfigGroup g( KSharedConfig::openConfig(), "KDE" );
    return g.readEntry("OpaqueResize", KDE_DEFAULT_OPAQUE_RESIZE);
}

int KGlobalSettings::buttonLayout()
{
    KConfigGroup g( KSharedConfig::openConfig(), "KDE" );
    return g.readEntry("ButtonLayout", KDE_DEFAULT_BUTTON_LAYOUT);
}

#if 0 // HAVE_X11 && QT_VERSION >= QT_VERSION_CHECK(5,0,0)
// Taken from Qt-4.x qt_x11_apply_settings_in_all_apps since Qt5 doesn't have it anymore.
// TODO: evaluate if this is still needed
// TODO: if yes, this code should be an invokable method of the qxcb platform plugin?
// TODO: it looks like the handling code for this in QPA is missing, too...
static void x11_apply_settings_in_all_apps()
{
    QByteArray stamp;
    QDataStream s(&stamp, QIODevice::WriteOnly);
    s << QDateTime::currentDateTime();

    QByteArray settings_atom_name("_QT_SETTINGS_TIMESTAMP_");
    settings_atom_name += XDisplayString(QX11Info::display());

    xcb_connection_t *xcb_conn = QX11Info::connection();
    xcb_intern_atom_cookie_t cookie = xcb_intern_atom(xcb_conn, false, settings_atom_name.size(), settings_atom_name.constData());
    xcb_intern_atom_reply_t *reply = xcb_intern_atom_reply(xcb_conn, cookie, 0);
    xcb_atom_t atom = reply->atom;
    free(reply);

    xcb_change_property(xcb_conn, XCB_PROP_MODE_REPLACE, QX11Info::appRootWindow(), atom, XCB_ATOM_ATOM,
                        8, stamp.size(), (const void *)stamp.constData());

    //XChangeProperty(QX11Info::display(), QX11Info::appRootWindow(0),
                    //ATOM(_QT_SETTINGS_TIMESTAMP), ATOM(_QT_SETTINGS_TIMESTAMP), 8,
                    //PropModeReplace, (unsigned char *)stamp.data(), stamp.size());
}
#endif

void KGlobalSettings::emitChange(ChangeType changeType, int arg)
{
    QDBusMessage message = QDBusMessage::createSignal("/KGlobalSettings", "org.kde.KGlobalSettings", "notifyChange" );
    QList<QVariant> args;
    args.append(static_cast<int>(changeType));
    args.append(arg);
    message.setArguments(args);
    QDBusConnection::sessionBus().send(message);
#if 0 // none of this exists in Qt5 anymore
    if (qApp && qApp->type() != QApplication::Tty) {
        //notify non-kde qt applications of the change
#if QT_VERSION >= QT_VERSION_CHECK(5, 0, 0)
        //x11_apply_settings_in_all_apps();
#else
        extern void qt_x11_apply_settings_in_all_apps();
        qt_x11_apply_settings_in_all_apps();
#endif
    }
#endif
}

void KGlobalSettings::Private::_k_slotNotifyChange(int changeType, int arg)
{
    switch(changeType) {
    case StyleChanged:
        if (activated) {
            KSharedConfig::openConfig()->reparseConfiguration();
            kdisplaySetStyle();
        }
        break;

    case ToolbarStyleChanged:
        KSharedConfig::openConfig()->reparseConfiguration();
        emit q->toolbarAppearanceChanged(arg);
        break;

    case PaletteChanged:
        if (activated) {
            KSharedConfig::openConfig()->reparseConfiguration();
            paletteCreated = false;
            kdisplaySetPalette();
        }
        break;

    case FontChanged:
        KSharedConfig::openConfig()->reparseConfiguration();
        KGlobalSettingsData::self()->dropFontSettingsCache();
        if (activated) {
            kdisplaySetFont();
        }
        break;

    case SettingsChanged: {
        KSharedConfig::openConfig()->reparseConfiguration();
        SettingsCategory category = static_cast<SettingsCategory>(arg);
        if (category == SETTINGS_QT) {
            if (activated) {
                propagateQtSettings();
            }
        } else {
            switch (category) {
                case SETTINGS_STYLE:
                    reloadStyleSettings();
                    break;
                case SETTINGS_MOUSE:
                    KGlobalSettingsData::self()->dropMouseSettingsCache();
                    break;
                case SETTINGS_LOCALE:
                    // QT5 TODO REPLACEMENT ? KLocale::global()->reparseConfiguration();
                    break;
                default:
                    break;
            }
            emit q->settingsChanged(category);
        }
        break;
    }
    case IconChanged:
        QPixmapCache::clear();
        KSharedConfig::openConfig()->reparseConfiguration();
        emit q->iconChanged(arg);
        break;

    case CursorChanged:
        applyCursorTheme();
        break;

    case BlockShortcuts:
        // FIXME KAccel port
        //KGlobalAccel::blockShortcuts(arg);
        emit q->blockShortcuts(arg); // see kwin
        break;

    case NaturalSortingChanged:
        emit q->naturalSortingChanged();
        break;

    default:
        qWarning() << "Unknown type of change in KGlobalSettings::slotNotifyChange: " << changeType;
    }
}

// Set by KApplication
#pragma message("FIXME: KWidgets depends on KApplication")
KWIDGETS_EXPORT QString kde_overrideStyle;

void KGlobalSettings::Private::applyGUIStyle()
{
#if 0 // Disabled for KF5. TODO Qt5: check that the KDE style is correctly applied.
  //Platform plugin only loaded on X11 systems
#if HAVE_X11
    if (!kde_overrideStyle.isEmpty()) {
        const QLatin1String currentStyleName(qApp->style()->metaObject()->className());
        if (0 != kde_overrideStyle.compare(currentStyleName, Qt::CaseInsensitive) &&
            0 != (QString(kde_overrideStyle + QLatin1String("Style"))).compare(currentStyleName, Qt::CaseInsensitive)) {
            qApp->setStyle(kde_overrideStyle);
        }
    } else {
        emit q->kdisplayStyleChanged();
    }
#else
    const QLatin1String currentStyleName(qApp->style()->metaObject()->className());

    if (kde_overrideStyle.isEmpty()) {
        const QString &defaultStyle = KStyle::defaultStyle();
        const KConfigGroup pConfig(KSharedConfig::openConfig(), "General");
        const QString &styleStr = pConfig.readEntry("widgetStyle", defaultStyle);

        if (styleStr.isEmpty() ||
                // check whether we already use the correct style to return then
                // (workaround for Qt misbehavior to avoid double style initialization)
                0 == (QString(styleStr + QLatin1String("Style"))).compare(currentStyleName, Qt::CaseInsensitive) ||
                0 == styleStr.compare(currentStyleName, Qt::CaseInsensitive)) {
            return;
        }

        QStyle* sp = QStyleFactory::create( styleStr );
        if (sp && currentStyleName == sp->metaObject()->className()) {
            delete sp;
            return;
        }

        // If there is no default style available, try falling back any available style
        if ( !sp && styleStr != defaultStyle)
            sp = QStyleFactory::create( defaultStyle );
        if ( !sp )
            sp = QStyleFactory::create( QStyleFactory::keys().first() );
        qApp->setStyle(sp);
    } else if (0 != kde_overrideStyle.compare(currentStyleName, Qt::CaseInsensitive) &&
            0 != (QString(kde_overrideStyle + QLatin1String("Style"))).compare(currentStyleName, Qt::CaseInsensitive)) {
        qApp->setStyle(kde_overrideStyle);
    }
    emit q->kdisplayStyleChanged();
#endif //HAVE_X11
#endif
}

QPalette KGlobalSettings::createApplicationPalette(const KSharedConfigPtr &config)
{
    return self()->d->createApplicationPalette(config);
}

QPalette KGlobalSettings::createNewApplicationPalette(const KSharedConfigPtr &config)
{
    return self()->d->createNewApplicationPalette(config);
}

QPalette KGlobalSettings::Private::createApplicationPalette(const KSharedConfigPtr &config)
{
    // This method is typically called once by KQGuiPlatformPlugin::palette and once again
    // by kdisplaySetPalette(), so we cache the palette to save time.
    if (config == KSharedConfig::openConfig() && paletteCreated) {
        return applicationPalette;
    }
    return createNewApplicationPalette(config);
}

QPalette KGlobalSettings::Private::createNewApplicationPalette(const KSharedConfigPtr &config)
{
    QPalette palette;

    QPalette::ColorGroup states[3] = { QPalette::Active, QPalette::Inactive,
                                       QPalette::Disabled };

    // TT thinks tooltips shouldn't use active, so we use our active colors for all states
    KColorScheme schemeTooltip(QPalette::Active, KColorScheme::Tooltip, config);

    for ( int i = 0; i < 3 ; i++ ) {
        QPalette::ColorGroup state = states[i];
        KColorScheme schemeView(state, KColorScheme::View, config);
        KColorScheme schemeWindow(state, KColorScheme::Window, config);
        KColorScheme schemeButton(state, KColorScheme::Button, config);
        KColorScheme schemeSelection(state, KColorScheme::Selection, config);

        palette.setBrush( state, QPalette::WindowText, schemeWindow.foreground() );
        palette.setBrush( state, QPalette::Window, schemeWindow.background() );
        palette.setBrush( state, QPalette::Base, schemeView.background() );
        palette.setBrush( state, QPalette::Text, schemeView.foreground() );
        palette.setBrush( state, QPalette::Button, schemeButton.background() );
        palette.setBrush( state, QPalette::ButtonText, schemeButton.foreground() );
        palette.setBrush( state, QPalette::Highlight, schemeSelection.background() );
        palette.setBrush( state, QPalette::HighlightedText, schemeSelection.foreground() );
        palette.setBrush( state, QPalette::ToolTipBase, schemeTooltip.background() );
        palette.setBrush( state, QPalette::ToolTipText, schemeTooltip.foreground() );

        palette.setColor( state, QPalette::Light, schemeWindow.shade( KColorScheme::LightShade ) );
        palette.setColor( state, QPalette::Midlight, schemeWindow.shade( KColorScheme::MidlightShade ) );
        palette.setColor( state, QPalette::Mid, schemeWindow.shade( KColorScheme::MidShade ) );
        palette.setColor( state, QPalette::Dark, schemeWindow.shade( KColorScheme::DarkShade ) );
        palette.setColor( state, QPalette::Shadow, schemeWindow.shade( KColorScheme::ShadowShade ) );

        palette.setBrush( state, QPalette::AlternateBase, schemeView.background( KColorScheme::AlternateBackground) );
        palette.setBrush( state, QPalette::Link, schemeView.foreground( KColorScheme::LinkText ) );
        palette.setBrush( state, QPalette::LinkVisited, schemeView.foreground( KColorScheme::VisitedText ) );
    }

    if (config == KSharedConfig::openConfig()) {
        paletteCreated = true;
        applicationPalette = palette;
    }

    return palette;
}

void KGlobalSettings::Private::kdisplaySetPalette()
{
#if !defined(Q_OS_WINCE)
    if (!kdeFullSession) {
        return;
    }

    QApplication::setPalette( q->createApplicationPalette() );
    emit q->kdisplayPaletteChanged();
    emit q->appearanceChanged();
#endif
}


void KGlobalSettings::Private::kdisplaySetFont()
{
#if !defined(Q_OS_WINCE)
    if (!kdeFullSession) {
        return;
    }

    KGlobalSettingsData* data = KGlobalSettingsData::self();

    QApplication::setFont( data->font(KGlobalSettingsData::GeneralFont) );
    const QFont menuFont = data->font( KGlobalSettingsData::MenuFont );
    QApplication::setFont( menuFont, "QMenuBar" );
    QApplication::setFont( menuFont, "QMenu" );
    QApplication::setFont( menuFont, "KPopupTitle" );
    QApplication::setFont( data->font(KGlobalSettingsData::ToolbarFont), "QToolBar" );
    emit q->kdisplayFontChanged();
    emit q->appearanceChanged();
#endif
}


void KGlobalSettings::Private::kdisplaySetStyle()
{
    applyGUIStyle();

    // Reread palette from config file.
    kdisplaySetPalette();
}


void KGlobalSettings::Private::reloadStyleSettings()
{
    KConfigGroup g( KSharedConfig::openConfig(), "KDE-Global GUI Settings" );

    // Asking for hasKey we do not ask for graphicEffectsLevelDefault() that can
    // contain some very slow code. If we can save that time, do it. (ereslibre)

    if (g.hasKey("GraphicEffectsLevel")) {
        _graphicEffects = ((GraphicEffects) g.readEntry("GraphicEffectsLevel", QVariant((int) NoEffects)).toInt());

        return;
    }

    _graphicEffects = KGlobalSettings::graphicEffectsLevelDefault();
}


void KGlobalSettings::Private::applyCursorTheme()
{
#if HAVE_X11 && defined(HAVE_XCURSOR)
    KConfig config("kcminputrc");
    KConfigGroup g(&config, "Mouse");

    QString theme = g.readEntry("cursorTheme", QString());
    int size      = g.readEntry("cursorSize", -1);

    // Default cursor size is 16 points
    if (size == -1)
    {
        QApplication *app = static_cast<QApplication*>(QApplication::instance());
        size = app->desktop()->screen(0)->logicalDpiY() * 16 / 72;
    }

    // Note that in X11R7.1 and earlier, calling XcursorSetTheme()
    // with a NULL theme would cause Xcursor to use "default", but
    // in 7.2 and later it will cause it to revert to the theme that
    // was configured when the application was started.
    XcursorSetTheme(QX11Info::display(), theme.isNull() ?
                    "default" : QFile::encodeName(theme));
    XcursorSetDefaultSize(QX11Info::display(), size);

    emit q->cursorChanged();
#endif
}


void KGlobalSettings::Private::propagateQtSettings()
{
    KConfigGroup cg( KSharedConfig::openConfig(), "KDE" );

#ifndef Q_OS_WIN
    int num = cg.readEntry("CursorBlinkRate", QApplication::cursorFlashTime());
    if ((num != 0) && (num < 200))
        num = 200;
    if (num > 2000)
        num = 2000;
    QApplication::setCursorFlashTime(num);
#else
    int num;
#endif
    num = cg.readEntry("DoubleClickInterval", QApplication::doubleClickInterval());
    QApplication::setDoubleClickInterval(num);
    num = cg.readEntry("StartDragTime", QApplication::startDragTime());
    QApplication::setStartDragTime(num);
    num = cg.readEntry("StartDragDist", QApplication::startDragDistance());
    QApplication::setStartDragDistance(num);
    num = cg.readEntry("WheelScrollLines", QApplication::wheelScrollLines());
    QApplication::setWheelScrollLines(num);
    bool showIcons = cg.readEntry("ShowIconsInMenuItems", !QApplication::testAttribute(Qt::AA_DontShowIconsInMenus));
    QApplication::setAttribute(Qt::AA_DontShowIconsInMenus, !showIcons);

    // KDE5: this seems fairly pointless
    emit q->settingsChanged(SETTINGS_QT);
}

#include "moc_kglobalsettings.cpp"