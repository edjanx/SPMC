/*
 *      Copyright (C) 2012-2013 Team XBMC
 *      http://xbmc.org
 *
 *  This Program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2, or (at your option)
 *  any later version.
 *
 *  This Program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with XBMC; see the file COPYING.  If not, see
 *  <http://www.gnu.org/licenses/>.
 *
 */

#include "XBMCApp.h"

#include <sstream>

#include <unistd.h>
#include <stdlib.h>
#include <dlfcn.h>
#include <string.h>

#include <android/native_window.h>
#include <android/native_window_jni.h>
#include <android/configuration.h>
#include <jni.h>

#include "input/MouseStat.h"
#include "input/XBMC_keysym.h"
#include "input/Key.h"
#include "windowing/XBMC_events.h"
#include <android/log.h>

#include "Application.h"
#include "AndroidFeatures.h"
#include "network/android/NetworkAndroid.h"
#include "settings/AdvancedSettings.h"
#include "xbmc.h"
#include "windowing/WinEvents.h"
#include "guilib/GUIWindowManager.h"
#include "guilib/GraphicContext.h"
#include "GUIInfoManager.h"
#include "guiinfo/GUIInfoLabels.h"
#include "settings/DisplaySettings.h"
#include "utils/log.h"
#include "messaging/ApplicationMessenger.h"
#include "utils/StringUtils.h"
#include "utils/Variant.h"
#include "utils/URIUtils.h"
#include "utils/SysfsUtils.h"
#include "AppParamParser.h"
#include "XbmcContext.h"
#include "TextureCache.h"
#include "filesystem/SpecialProtocol.h"
#include <android/bitmap.h>
#include "cores/AudioEngine/AEFactory.h"
#include "cores/VideoRenderers/RenderManager.h"
#include "androidjni/JNIThreading.h"
#include "androidjni/BroadcastReceiver.h"
#include "androidjni/Intent.h"
#include "androidjni/PackageManager.h"
#include "androidjni/Context.h"
#include "androidjni/PowerManager.h"
#include "androidjni/WakeLock.h"
#include "androidjni/Environment.h"
#include "androidjni/File.h"
#include "androidjni/IntentFilter.h"
#include "androidjni/NetworkInfo.h"
#include "androidjni/ConnectivityManager.h"
#include "androidjni/System.h"
#include "androidjni/ApplicationInfo.h"
#include "androidjni/StatFs.h"
#include "androidjni/CharSequence.h"
#include "androidjni/URI.h"
#include "androidjni/Cursor.h"
#include "androidjni/ContentResolver.h"
#include "androidjni/MediaStore.h"
#include "androidjni/Build.h"
#include "utils/AMLUtils.h"
#include "utils/RKutils.h"
#include "androidjni/Window.h"
#include "androidjni/WindowManager.h"
#include "androidjni/KeyEvent.h"
#include "androidjni/SystemProperties.h"
#include "androidjni/Display.h"
#include "androidjni/SystemClock.h"
#include "androidjni/BitmapFactory.h"
#include "AndroidKey.h"

#include "CompileInfo.h"
#include "filesystem/VideoDatabaseFile.h"
#include "video/videosync/VideoSyncAndroid.h"
#include "interfaces/AnnouncementManager.h"

#define GIGABYTES       1073741824
#define CAPTURE_QUEUE_MAXDEPTH 3

#define ACTION_XBMC_RESUME "android.intent.XBMC_RESUME"

#define PLAYBACK_STATE_STOPPED  0x0000
#define PLAYBACK_STATE_PLAYING  0x0001
#define PLAYBACK_STATE_VIDEO    0x0100
#define PLAYBACK_STATE_AUDIO    0x0200

using namespace std;
using namespace jni;
using namespace KODI::MESSAGING;
using namespace ANNOUNCEMENT;

template<class T, void(T::*fn)()>
void* thread_run(void* obj)
{
  (static_cast<T*>(obj)->*fn)();
  return NULL;
}

CXBMCApp* CXBMCApp::m_xbmcappinstance = NULL;
std::unique_ptr<CJNIXBMCMainView> CXBMCApp::m_mainView;
ANativeActivity *CXBMCApp::m_activity = NULL;
CJNIWakeLock *CXBMCApp::m_wakeLock = NULL;
ANativeWindow* CXBMCApp::m_window = NULL;
int CXBMCApp::m_batteryLevel = 0;
bool CXBMCApp::m_hasFocus = false;
bool CXBMCApp::m_isResumed = false;
bool CXBMCApp::m_hasAudioFocus = false;
bool CXBMCApp::m_headsetPlugged = false;
bool CXBMCApp::m_hasReqVisible = false;
bool CXBMCApp::m_hasPIP = false;
CCriticalSection CXBMCApp::m_applicationsMutex;
std::vector<androidPackage> CXBMCApp::m_applications;
std::vector<CActivityResultEvent*> CXBMCApp::m_activityResultEvents;

CCriticalSection CXBMCApp::m_captureMutex;
CCaptureEvent CXBMCApp::m_captureEvent;
std::queue<CJNIImage> CXBMCApp::m_captureQueue;

uint64_t CXBMCApp::m_vsynctime = 0;
CEvent CXBMCApp::m_vsyncEvent;
std::vector<GLuint> CXBMCApp::m_texturePool;
CJNIAudioDeviceInfos CXBMCApp::m_audiodevices;
uint32_t CXBMCApp::m_playback_state = PLAYBACK_STATE_STOPPED;
CRect CXBMCApp::m_surface_rect;

void LogAudoDevices(const char* stage, const CJNIAudioDeviceInfos& devices)
{
  CLog::Log(LOGDEBUG, "--- Audio device list: %s", stage);
  for (auto dev : devices)
  {
    CLog::Log(LOGDEBUG, "--- Found device: %s", dev.getProductName().toString().c_str());
    CLog::Log(LOGDEBUG, "    id: %d, type: %d, isSink: %s, isSource: %s", dev.getId(), dev.getType(), dev.isSink() ? "true" : "false", dev.isSource() ? "true" : "false");

    std::ostringstream oss;
    for (auto i : dev.getChannelCounts())
      oss << i << " / ";
    CLog::Log(LOGDEBUG, "    channel counts: %s", oss.str().c_str());

    oss.clear(); oss.str("");
    for (auto i : dev.getChannelIndexMasks())
      oss << i << " / ";
    CLog::Log(LOGDEBUG, "    channel index masks: %s", oss.str().c_str());

    oss.clear(); oss.str("");
    for (auto i : dev.getChannelMasks())
      oss << i << " / ";
    CLog::Log(LOGDEBUG, "    channel masks: %s", oss.str().c_str());

    oss.clear(); oss.str("");
    for (auto i : dev.getEncodings())
      oss << i << " / ";
    CLog::Log(LOGDEBUG, "    encodings: %s", oss.str().c_str());

    oss.clear(); oss.str("");
    for (auto i : dev.getSampleRates())
      oss << i << " / ";
    CLog::Log(LOGDEBUG, "    sample rates: %s", oss.str().c_str());
  }
}

CXBMCApp::CXBMCApp(ANativeActivity* nativeActivity)
  : CJNIMainActivity(nativeActivity)
  , CJNIBroadcastReceiver(CJNIContext::getPackageName() + ".XBMCBroadcastReceiver")
  , m_videosurfaceInUse(false)
{
  m_xbmcappinstance = this;
  m_activity = nativeActivity;
  if (m_activity == NULL)
  {
    android_printf("CXBMCApp: invalid ANativeActivity instance");
    exit(1);
    return;
  }
  m_mainView.reset(new CJNIXBMCMainView(this));
  m_firstrun = true;
  m_exiting = false;
  android_printf("CXBMCApp: Created");

  CAnnouncementManager::GetInstance().AddAnnouncer(this);
}

CXBMCApp::~CXBMCApp()
{
  if (m_wakeLock->isHeld())
    m_wakeLock->release();
  delete m_wakeLock;
  m_xbmcappinstance = NULL;
}

void CXBMCApp::Announce(ANNOUNCEMENT::AnnouncementFlag flag, const char *sender, const char *message, const CVariant &data)
{
  if (strcmp(sender, "xbmc") != 0)
    return;

  if (flag & Input)
  {
    if (strcmp(message, "OnInputRequested") == 0)
      CAndroidKey::SetHandleSearchKeys(true);
    else if (strcmp(message, "OnInputFinished") == 0)
      CAndroidKey::SetHandleSearchKeys(false);
  }
  else if (flag & Player)
  {
     if (strcmp(message, "OnPlay") == 0)
      OnPlayBackStarted();
    else if (strcmp(message, "OnPause") == 0)
      OnPlayBackPaused();
    else if (strcmp(message, "OnStop") == 0)
      OnPlayBackStopped();
  }
  else if (flag & Info)
  {
     if (strcmp(message, "OnChanged") == 0)
      UpdateSessionMetadata();
  }
}

void CXBMCApp::onStart()
{
  android_printf("%s: ", __PRETTY_FUNCTION__);

#if defined(HAS_LIBAMCODEC)
  if (aml_permissions())
  {
    // non-aml boxes will ignore this intent broadcast.
    // setup aml scalers to play video as is, unscaled.
    CJNIIntent intent_aml_video_on = CJNIIntent("android.intent.action.REALVIDEO_ON");
    sendBroadcast(intent_aml_video_on);
  }
#endif

  if (m_firstrun)
  {
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_JOINABLE);
    pthread_create(&m_thread, &attr, thread_run<CXBMCApp, &CXBMCApp::run>, this);
    pthread_attr_destroy(&attr);

    // Some intent filters MUST be registered in code rather than through the manifest
    CJNIIntentFilter intentFilter;
    intentFilter.addAction("android.intent.action.BATTERY_CHANGED");
    intentFilter.addAction("android.intent.action.SCREEN_ON");
    intentFilter.addAction("android.intent.action.SCREEN_OFF");
    intentFilter.addAction("android.intent.action.HEADSET_PLUG");
    intentFilter.addAction("android.net.conn.CONNECTIVITY_CHANGE");
    registerReceiver(*this, intentFilter);

    m_mediaSession.reset(new CJNIXBMCMediaSession());
  }
}

void CXBMCApp::onResume()
{
  android_printf("%s: ", __PRETTY_FUNCTION__);

  if (!g_application.IsInScreenSaver())
    EnableWakeLock(true);
  else
    g_application.WakeUpScreenSaverAndDPMS();

  m_audiodevices.clear();
// Crashes on SATV as of FW 3.3
//  if (CJNIAudioManager::GetSDKVersion() >= 23)
//  {
//    CJNIAudioManager audioManager(getSystemService("audio"));
//    m_audiodevices = audioManager.getDevices(CJNIAudioManager::GET_DEVICES_OUTPUTS);
//    LogAudoDevices("OnResume", m_audiodevices);
//  }
  CheckHeadsetPlugged();

  m_mediaSession->activate(false);

  // Clear the applications cache. We could have installed/deinstalled apps
  {
    CSingleLock lock(m_applicationsMutex);
    m_applications.clear();
  }

  m_hasReqVisible = false;
  // Re-request Visible Behind
  if ((m_playback_state & PLAYBACK_STATE_PLAYING) && (m_playback_state & PLAYBACK_STATE_VIDEO))
    RequestVisibleBehind(true);
  
  m_isResumed = true;
}

void CXBMCApp::onPause()
{
  android_printf("%s: ", __PRETTY_FUNCTION__);

#if defined(HAS_LIBAMCODEC)
  if (aml_permissions())
  {
    // non-aml boxes will ignore this intent broadcast.
    CJNIIntent intent_aml_video_off = CJNIIntent("android.intent.action.REALVIDEO_OFF");
    sendBroadcast(intent_aml_video_off);
  }
#endif

  if (m_playback_state != PLAYBACK_STATE_STOPPED)
    m_mediaSession->activate(true);

  EnableWakeLock(false);
  m_isResumed = false;
}

void CXBMCApp::onStop()
{
  android_printf("%s: ", __PRETTY_FUNCTION__);

  if ((m_playback_state & PLAYBACK_STATE_PLAYING) && (m_playback_state & PLAYBACK_STATE_VIDEO) && !m_hasReqVisible)
    CApplicationMessenger::GetInstance().SendMsg(TMSG_GUI_ACTION, WINDOW_INVALID, -1, static_cast<void*>(new CAction(ACTION_PAUSE)));
}

void CXBMCApp::onDestroy()
{
  android_printf("%s", __PRETTY_FUNCTION__);

  unregisterReceiver(*this);

  m_mediaSession.release();

  // If android is forcing us to stop, ask XBMC to exit then wait until it's
  // been destroyed.
  if (!m_exiting)
  {
    CApplicationMessenger::GetInstance().PostMsg(TMSG_QUIT);
    pthread_join(m_thread, NULL);
    android_printf(" => XBMC finished");
  }
}

void CXBMCApp::onSaveState(void **data, size_t *size)
{
  android_printf("%s: ", __PRETTY_FUNCTION__);
  // no need to save anything as XBMC is running in its own thread
}

void CXBMCApp::onConfigurationChanged()
{
  android_printf("%s: ", __PRETTY_FUNCTION__);
  // ignore any configuration changes like screen rotation etc
}

void CXBMCApp::onLowMemory()
{
  android_printf("%s: ", __PRETTY_FUNCTION__);
  // can't do much as we don't want to close completely
}

void CXBMCApp::onCreateWindow(ANativeWindow* window)
{
  android_printf("%s: ", __PRETTY_FUNCTION__);
}

void CXBMCApp::onResizeWindow()
{
  android_printf("%s: ", __PRETTY_FUNCTION__);
  m_window = NULL;
  // no need to do anything because we are fixed in fullscreen landscape mode
}

void CXBMCApp::onDestroyWindow()
{
  android_printf("%s: ", __PRETTY_FUNCTION__);
}

void CXBMCApp::onGainFocus()
{
  android_printf("%s: ", __PRETTY_FUNCTION__);
  m_hasFocus = true;
  g_application.WakeUpScreenSaverAndDPMS();
}

void CXBMCApp::onLostFocus()
{
  android_printf("%s: ", __PRETTY_FUNCTION__);
  m_hasFocus = false;
}

bool CXBMCApp::EnableWakeLock(bool on)
{
  if (!m_wakeLock)
  {
    std::string appName = CCompileInfo::GetAppName();
    StringUtils::ToLower(appName);
    std::string className = CCompileInfo::GetPackage();
    StringUtils::ToLower(className);
    // SCREEN_BRIGHT_WAKE_LOCK is marked as deprecated but there is no real alternatives for now
    m_wakeLock = new CJNIWakeLock(CJNIPowerManager(getSystemService("power")).newWakeLock(CJNIPowerManager::SCREEN_BRIGHT_WAKE_LOCK, className.c_str()));
    if (m_wakeLock)
      m_wakeLock->setReferenceCounted(false);
    else
      return false;
  }

  if (on)
  {
    if (!m_wakeLock->isHeld())
    {
      m_wakeLock->acquire();
      android_printf("%s: %s", __PRETTY_FUNCTION__, "acquired");
    }
  }
  else
  {
    if (m_wakeLock->isHeld())
    {
      m_wakeLock->release();
      android_printf("%s: %s", __PRETTY_FUNCTION__, "released");
    }
  }

  return true;
}

bool CXBMCApp::AcquireAudioFocus()
{
  if (!m_xbmcappinstance)
    return false;

  if (m_hasAudioFocus)
    return true;

  CJNIAudioManager audioManager(getSystemService("audio"));
  if (!audioManager)
  {
    CXBMCApp::android_printf("Cannot get audiomanger");
    return false;
  }

  // Request audio focus for playback
  int result = audioManager.requestAudioFocus(m_audioFocusListener,
                                              // Use the music stream.
                                              CJNIAudioManager::STREAM_MUSIC,
                                              // Request permanent focus.
                                              CJNIAudioManager::AUDIOFOCUS_GAIN);

  if (result != CJNIAudioManager::AUDIOFOCUS_REQUEST_GRANTED)
  {
    CXBMCApp::android_printf("Audio Focus request failed");
    return false;
  }
  m_hasAudioFocus = true;
  return true;
}

bool CXBMCApp::ReleaseAudioFocus()
{
  if (!m_xbmcappinstance)
    return false;

  if (!m_hasAudioFocus)
    return true;

  CJNIAudioManager audioManager(getSystemService("audio"));
  if (!audioManager)
  {
    CXBMCApp::android_printf("Cannot get audiomanger");
    return false;
  }

  // Release audio focus after playback
  int result = audioManager.abandonAudioFocus(m_audioFocusListener);
  if (result != CJNIAudioManager::AUDIOFOCUS_REQUEST_GRANTED)
  {
    CXBMCApp::android_printf("Audio Focus abandon failed");
    return false;
  }
  m_hasAudioFocus = false;
  return true;
}

void CXBMCApp::RequestVisibleBehind(bool requested)
{
  if (requested == m_hasReqVisible)
    return;

  m_hasReqVisible = requestVisibleBehind(requested);
  CLog::Log(LOGDEBUG, "Visible Behind request: %s", m_hasReqVisible ? "true" : "false");
}

void CXBMCApp::RequestPictureInPictureMode()
{
  // PIP and VisbleBehind are exclusive
  if (m_hasReqVisible)
    RequestVisibleBehind(false);

  enterPictureInPictureMode();
  CLog::Log(LOGDEBUG, "Entering PIP mode");
}

void CXBMCApp::CheckHeadsetPlugged()
{
  bool oldstate = m_headsetPlugged;

  CLog::Log(LOGDEBUG, "CXBMCApp::CheckHeadsetPlugged");
  CJNIAudioManager audioManager(getSystemService("audio"));
  m_headsetPlugged = audioManager.isWiredHeadsetOn() || audioManager.isBluetoothA2dpOn();

  if (!m_audiodevices.empty())
  {
    for (auto dev : m_audiodevices)
    {
      if (dev.getType() == CJNIAudioDeviceInfo::TYPE_DOCK && dev.isSink() && StringUtils::CompareNoCase(dev.getProductName().toString(), "SHIELD Android TV") == 0)
      {
        // SHIELD specifics: Gamepad headphone is inserted
        m_headsetPlugged = true;
        CLog::Log(LOGINFO, "SHIELD: Wifi direct headset inserted");
      }
    }
  }

  if (m_headsetPlugged != oldstate)
    CAEFactory::DeviceChange();
}

bool CXBMCApp::IsHeadsetPlugged()
{
  return m_headsetPlugged;
}

void CXBMCApp::run()
{
  int status = 0;

  SetupEnv();
  XBMC::Context context;

  m_firstrun=false;
  android_printf(" => running XBMC_Run...");
  try
  {
    status = XBMC_Run(true);
    android_printf(" => XBMC_Run finished with %d", status);
  }
  catch(...)
  {
    android_printf("ERROR: Exception caught on main loop. Exiting");
  }

  // If we are have not been force by Android to exit, notify its finish routine.
  // This will cause android to run through its teardown events, it calls:
  // onPause(), onLostFocus(), onDestroyWindow(), onStop(), onDestroy().
  ANativeActivity_finish(m_activity);
  m_exiting=true;
}

void CXBMCApp::XBMC_SetupDisplay()
{
  android_printf("XBMC_SetupDisplay()");
  CApplicationMessenger::GetInstance().PostMsg(TMSG_DISPLAY_SETUP);
}

void CXBMCApp::XBMC_DestroyDisplay()
{
  android_printf("XBMC_DestroyDisplay()");
  CApplicationMessenger::GetInstance().PostMsg(TMSG_DISPLAY_DESTROY);
}

int CXBMCApp::SetBuffersGeometry(int width, int height)
{
  return ANativeWindow_setBuffersGeometry(m_window, width, height, WINDOW_FORMAT_RGBA_8888);
}

#include "threads/Event.h"
#include <time.h>

void CXBMCApp::SetRefreshRateCallback(CVariant* rateVariant)
{
  float rate = rateVariant->asFloat();
  delete rateVariant;

  CJNIWindow window = getWindow();
  if (window)
  {
    CJNIWindowManagerLayoutParams params = window.getAttributes();
    if (params.getpreferredRefreshRate() != rate)
    {
      params.setpreferredRefreshRate(rate);
      if (params.getpreferredRefreshRate() > 0.0)
        window.setAttributes(params);
    }
  }
}

void CXBMCApp::SetDisplayModeCallback(CVariant* modeVariant)
{
  int mode = modeVariant->asFloat();
  delete modeVariant;

  CJNIWindow window = getWindow();
  if (window)
  {
    CJNIWindowManagerLayoutParams params = window.getAttributes();
    if (params.getpreferredDisplayModeId() != mode)
    {
      params.setpreferredDisplayModeId(mode);
      window.setAttributes(params);
    }
  }
}

void CXBMCApp::SetRefreshRate(float rate)
{
  if (rate < 1.0)
    return;

  CVariant *variant = new CVariant(rate);
  runNativeOnUiThread(SetRefreshRateCallback, variant);
}

void CXBMCApp::SetDisplayMode(int mode)
{
  if (mode < 1.0)
    return;

  CVariant *variant = new CVariant(mode);
  runNativeOnUiThread(SetDisplayModeCallback, variant);
}

int CXBMCApp::android_printf(const char *format, ...)
{
  // For use before CLog is setup by XBMC_Run()
  va_list args;
  va_start(args, format);
  int result = __android_log_vprint(ANDROID_LOG_VERBOSE, "SPMC", format, args);
  va_end(args);
  return result;
}

void CXBMCApp::BringToFront()
{
  if (!m_isResumed)
  {
    CLog::Log(LOGERROR, "CXBMCApp::BringToFront");
    StartActivity(getPackageName());
  }
}

std::vector<GLuint> &CXBMCApp::GetTexturePool()
{
  return m_texturePool;
}

int CXBMCApp::GetDPI()
{
  if (m_activity == NULL || m_activity->assetManager == NULL)
    return 0;

  // grab DPI from the current configuration - this is approximate
  // but should be close enough for what we need
  AConfiguration *config = AConfiguration_new();
  AConfiguration_fromAssetManager(config, m_activity->assetManager);
  int dpi = AConfiguration_getDensity(config);
  AConfiguration_delete(config);

  return dpi;
}

CPointInt CXBMCApp::GetMaxDisplayResolution()
{
  // Find larger possible resolution
  RESOLUTION_INFO res_info = CDisplaySettings::GetInstance().GetResolutionInfo(g_graphicsContext.GetVideoResolution());
  for (unsigned int i=0; i<CDisplaySettings::GetInstance().ResolutionInfoSize(); ++i)
  {
    RESOLUTION_INFO res = CDisplaySettings::GetInstance().GetResolutionInfo(i);
    if (res.iWidth > res_info.iWidth || res.iHeight > res_info.iHeight)
      res_info = res;
  }

  // Android might go even higher via surface
  std::string displaySize = CJNISystemProperties::get("sys.display-size", "");
  if (!displaySize.empty())
  {
    std::vector<std::string> aSize = StringUtils::Split(displaySize, "x");
    if (aSize.size() == 2)
    {
      res_info.iWidth = StringUtils::IsInteger(aSize[0]) ? atoi(aSize[0].c_str()) : 0;
      res_info.iHeight = StringUtils::IsInteger(aSize[1]) ? atoi(aSize[1].c_str()) : 0;
    }
  }

  // AML, always different
  std::string valstr;
  if (SysfsUtils::GetString("/sys/class/amhdmitx/amhdmitx0/disp_cap", valstr) == 0)
  {
    std::vector<std::string> probe_str = StringUtils::Split(valstr, "\n");

    RESOLUTION_INFO res;
    for (size_t i = 0; i < probe_str.size(); i++)
    {
      if(aml_mode_to_resolution(probe_str[i].c_str(), &res))
      {
        if (res.iWidth > res_info.iWidth || res.iHeight > res_info.iHeight)
          res_info = res;
      }
    }
  }

  // RK, same
  if (SysfsUtils::GetString("/sys/class/display/display0.HDMI/modes", valstr) == 0)
  {
    std::vector<std::string> probe_str = StringUtils::Split(valstr, "\n");

    RESOLUTION_INFO res;
    for (size_t i = 0; i < probe_str.size(); i++)
    {
      if(rk_mode_to_resolution(probe_str[i].c_str(), &res))
      {
        if (res.iWidth > res_info.iWidth || res.iHeight > res_info.iHeight)
          res_info = res;
      }
    }
  }


  return CPointInt(res_info.iWidth, res_info.iHeight);
}

CRect CXBMCApp::MapRenderToDroid(const CRect& srcRect)
{
  float scaleX = 1.0;
  float scaleY = 1.0;

  if(m_xbmcappinstance && m_surface_rect.x2 && m_surface_rect.y2)
  {
    RESOLUTION_INFO renderRes = CDisplaySettings::GetInstance().GetResolutionInfo(g_renderManager.GetResolution());
    scaleX = (double)m_surface_rect.x2 / renderRes.iWidth;
    scaleY = (double)m_surface_rect.y2 / renderRes.iHeight;
  }

  return CRect(srcRect.x1 * scaleX, srcRect.y1 * scaleY, srcRect.x2 * scaleX, srcRect.y2 * scaleY);
}

CPoint CXBMCApp::GetDroidToGuiRatio()
{
  float scaleX = 1.0;
  float scaleY = 1.0;

  if(m_xbmcappinstance && m_surface_rect.x2 && m_surface_rect.y2)
  {
    CRect gui = CRect(0, 0, CDisplaySettings::GetInstance().GetCurrentResolutionInfo().iWidth, CDisplaySettings::GetInstance().GetCurrentResolutionInfo().iHeight);
    scaleX = gui.Width() / (double)m_surface_rect.x2;
    scaleY = gui.Height() / (double)m_surface_rect.y2;
  }

  return CPoint(scaleX, scaleY);
}

void CXBMCApp::UpdateSessionMetadata()
{
  CJNIMediaMetadataBuilder builder;
  builder
      .putString(CJNIMediaMetadata::METADATA_KEY_DISPLAY_TITLE, g_infoManager.GetLabel(PLAYER_TITLE))
      .putString(CJNIMediaMetadata::METADATA_KEY_TITLE, g_infoManager.GetLabel(PLAYER_TITLE))
      .putLong(CJNIMediaMetadata::METADATA_KEY_DURATION, g_application.m_pPlayer->GetTotalTime())
//      .putString(CJNIMediaMetadata::METADATA_KEY_ART_URI, thumb)
//      .putString(CJNIMediaMetadata::METADATA_KEY_DISPLAY_ICON_URI, thumb)
//      .putString(CJNIMediaMetadata::METADATA_KEY_ALBUM_ART_URI, thumb)
      ;

  std::string thumb;
  if (m_playback_state & PLAYBACK_STATE_VIDEO)
  {
    builder
        .putString(CJNIMediaMetadata::METADATA_KEY_DISPLAY_SUBTITLE, g_infoManager.GetLabel(VIDEOPLAYER_TAGLINE))
        .putString(CJNIMediaMetadata::METADATA_KEY_ARTIST, g_infoManager.GetLabel(VIDEOPLAYER_DIRECTOR))
        ;
    thumb = g_infoManager.GetImage(VIDEOPLAYER_COVER, -1);
  }
  else if (m_playback_state & PLAYBACK_STATE_AUDIO)
  {
    builder
        .putString(CJNIMediaMetadata::METADATA_KEY_DISPLAY_SUBTITLE, g_infoManager.GetLabel(MUSICPLAYER_ARTIST))
        .putString(CJNIMediaMetadata::METADATA_KEY_ARTIST, g_infoManager.GetLabel(MUSICPLAYER_ARTIST))
        ;
    thumb = g_infoManager.GetImage(MUSICPLAYER_COVER, -1);
  }
  bool needrecaching = false;
  std::string cachefile = CTextureCache::GetInstance().CheckCachedImage(thumb, false, needrecaching);
  if (!cachefile.empty())
  {
    std::string actualfile = CSpecialProtocol::TranslatePath(cachefile);
    CJNIBitmap bmp = CJNIBitmapFactory::decodeFile(actualfile);
    if (bmp)
      builder.putBitmap(CJNIMediaMetadata::METADATA_KEY_ART, bmp);
  }
  m_mediaSession->updateMetadata(builder.build());
}

void CXBMCApp::UpdateSessionState()
{
  CJNIPlaybackStateBuilder builder;
  int state = CJNIPlaybackState::STATE_NONE;
  int64_t pos = 0;
  float speed = 0.0;
  if (m_playback_state != PLAYBACK_STATE_STOPPED)
  {
    pos = g_application.m_pPlayer->GetTime();
    speed = g_application.m_pPlayer->GetPlaySpeed();
    if (m_playback_state & PLAYBACK_STATE_PLAYING)
      state = CJNIPlaybackState::STATE_PLAYING;
    else
      state = CJNIPlaybackState::STATE_PAUSED;
  }
  else
    state = CJNIPlaybackState::STATE_STOPPED;
  builder
      .setState(state, pos, speed, CJNISystemClock::elapsedRealtime())
      .setActions(0xffffffffffffffff)
      ;
  m_mediaSession->updatePlaybackState(builder.build());
}

void CXBMCApp::OnPlayBackStarted()
{
  CLog::Log(LOGDEBUG, "%s", __PRETTY_FUNCTION__);

  m_playback_state = PLAYBACK_STATE_PLAYING;
  if (g_application.m_pPlayer->HasVideo())
    m_playback_state |= PLAYBACK_STATE_VIDEO;
  if (g_application.m_pPlayer->HasAudio())
    m_playback_state |= PLAYBACK_STATE_AUDIO;

  m_mediaSession->activate(true);

  CJNIIntent intent(ACTION_XBMC_RESUME, CJNIURI::EMPTY, *this, get_class(CJNIContext::get_raw()));
  m_mediaSession->updateIntent(intent);

  m_xbmcappinstance->AcquireAudioFocus();
  CAndroidKey::SetHandleMediaKeys(true);

  if (m_isResumed)
    RequestVisibleBehind(true);
}

void CXBMCApp::OnPlayBackPaused()
{
  CLog::Log(LOGDEBUG, "%s", __PRETTY_FUNCTION__);

  m_playback_state &= ~PLAYBACK_STATE_PLAYING;

  RequestVisibleBehind(false);
  m_xbmcappinstance->ReleaseAudioFocus();
}

void CXBMCApp::OnPlayBackStopped()
{
  CLog::Log(LOGDEBUG, "%s", __PRETTY_FUNCTION__);

  m_playback_state = PLAYBACK_STATE_STOPPED;
  UpdateSessionState();
  m_mediaSession->activate(false);

  RequestVisibleBehind(false);
  CAndroidKey::SetHandleMediaKeys(false);
  m_xbmcappinstance->ReleaseAudioFocus();
}

void CXBMCApp::ProcessSlow()
{
  if (m_mediaSession->isActive())
    UpdateSessionState();
}

std::vector<androidPackage> CXBMCApp::GetApplications()
{
  CSingleLock lock(m_applicationsMutex);
  if (m_applications.empty())
  {
    CJNIList<CJNIApplicationInfo> packageList = GetPackageManager().getInstalledApplications(CJNIPackageManager::GET_ACTIVITIES);
    int numPackages = packageList.size();
    for (int i = 0; i < numPackages; i++)
    {
      CJNIIntent intent;
      if (CAndroidFeatures::IsLeanback())
        intent = GetPackageManager().getLeanbackLaunchIntentForPackage(packageList.get(i).packageName);
      if (!intent)
        intent = GetPackageManager().getLaunchIntentForPackage(packageList.get(i).packageName);
      if (!intent)
        continue;

      androidPackage newPackage;
      newPackage.packageName = packageList.get(i).packageName;
      newPackage.packageLabel = GetPackageManager().getApplicationLabel(packageList.get(i)).toString();
      newPackage.icon = packageList.get(i).icon;
      m_applications.push_back(newPackage);
    }
  }

  return m_applications;
}

bool CXBMCApp::HasLaunchIntent(const string &package)
{
  return GetPackageManager().getLaunchIntentForPackage(package) != NULL;
}

// Note intent, dataType, dataURI all default to ""
bool CXBMCApp::StartActivity(const string &package, const string &intent, const string &dataType, const string &dataURI)
{
  CJNIIntent newIntent;
  if (!intent.empty())
    newIntent = CJNIIntent(intent);

  if (!newIntent)
  {
    if (CAndroidFeatures::IsLeanback())
      newIntent = GetPackageManager().getLeanbackLaunchIntentForPackage(package);
    if (!newIntent)
      newIntent = GetPackageManager().getLaunchIntentForPackage(package);
  }

  if (!newIntent)
    return false;

  if (!dataURI.empty())
  {
    CJNIURI jniURI = CJNIURI::parse(dataURI);

    if (!jniURI)
      return false;

    newIntent.setDataAndType(jniURI, dataType);
  }

  newIntent.setPackage(package);
  startActivity(newIntent);
  if (xbmc_jnienv()->ExceptionCheck())
  {
    CLog::Log(LOGERROR, "CXBMCApp::StartActivity - ExceptionOccurred launching %s", package.c_str());
    xbmc_jnienv()->ExceptionClear();
    return false;
  }

  return true;
}

int CXBMCApp::GetBatteryLevel()
{
  return m_batteryLevel;
}

bool CXBMCApp::GetExternalStorage(std::string &path, const std::string &type /* = "" */)
{
  std::string sType;
  std::string mountedState;
  bool mounted = false;

  if(type == "files" || type.empty())
  {
    CJNIFile external = CJNIEnvironment::getExternalStorageDirectory();
    if (external)
      path = external.getAbsolutePath();
  }
  else
  {
    if (type == "music")
      sType = "Music"; // Environment.DIRECTORY_MUSIC
    else if (type == "videos")
      sType = "Movies"; // Environment.DIRECTORY_MOVIES
    else if (type == "pictures")
      sType = "Pictures"; // Environment.DIRECTORY_PICTURES
    else if (type == "photos")
      sType = "DCIM"; // Environment.DIRECTORY_DCIM
    else if (type == "downloads")
      sType = "Download"; // Environment.DIRECTORY_DOWNLOADS
    if (!sType.empty())
    {
      CJNIFile external = CJNIEnvironment::getExternalStoragePublicDirectory(sType);
      if (external)
        path = external.getAbsolutePath();
    }
  }
  mountedState = CJNIEnvironment::getExternalStorageState();
  mounted = (mountedState == "mounted" || mountedState == "mounted_ro");
  return mounted && !path.empty();
}

bool CXBMCApp::GetStorageUsage(const std::string &path, std::string &usage)
{
#define PATH_MAXLEN 50

  if (path.empty())
  {
    std::ostringstream fmt;
    fmt.width(PATH_MAXLEN);  fmt << std::left  << "Filesystem";
    fmt.width(12);  fmt << std::right << "Size";
    fmt.width(12);  fmt << "Used";
    fmt.width(12);  fmt << "Avail";
    fmt.width(12);  fmt << "Use %";

    usage = fmt.str();
    return false;
  }

  CJNIStatFs fileStat(path);
  if (!fileStat)
  {
    CLog::Log(LOGERROR, "CXBMCApp::GetStorageUsage cannot stat %s", path.c_str());
    return false;
  }
  int blockSize = fileStat.getBlockSize();
  int blockCount = fileStat.getBlockCount();
  int freeBlocks = fileStat.getFreeBlocks();

  if (blockSize <= 0 || blockCount <= 0 || freeBlocks < 0)
    return false;

  float totalSize = (float)blockSize * blockCount / GIGABYTES;
  float freeSize = (float)blockSize * freeBlocks / GIGABYTES;
  float usedSize = totalSize - freeSize;
  float usedPercentage = usedSize / totalSize * 100;

  std::ostringstream fmt;
  fmt << std::fixed;
  fmt.precision(1);
  fmt.width(PATH_MAXLEN);  fmt << std::left  << (path.size() < PATH_MAXLEN-1 ? path : StringUtils::Left(path, PATH_MAXLEN-4) + "...");
  fmt.width(12);  fmt << std::right << totalSize << "G"; // size in GB
  fmt.width(12);  fmt << usedSize << "G"; // used in GB
  fmt.width(12);  fmt << freeSize << "G"; // free
  fmt.precision(0);
  fmt.width(12);  fmt << usedPercentage << "%"; // percentage used

  usage = fmt.str();
  return true;
}

// Used in Application.cpp to figure out volume steps
int CXBMCApp::GetMaxSystemVolume()
{
  JNIEnv* env = xbmc_jnienv();
  static int maxVolume = -1;
  if (maxVolume == -1)
  {
    maxVolume = GetMaxSystemVolume(env);
  }
  //android_printf("CXBMCApp::GetMaxSystemVolume: %i",maxVolume);
  return maxVolume;
}

int CXBMCApp::GetMaxSystemVolume(JNIEnv *env)
{
  CJNIAudioManager audioManager(getSystemService("audio"));
  if (audioManager)
    return audioManager.getStreamMaxVolume();
  android_printf("CXBMCApp::SetSystemVolume: Could not get Audio Manager");
  return 0;
}

float CXBMCApp::GetSystemVolume()
{
  CJNIAudioManager audioManager(getSystemService("audio"));
  if (audioManager)
    return (float)audioManager.getStreamVolume() / GetMaxSystemVolume();
  else
  {
    android_printf("CXBMCApp::GetSystemVolume: Could not get Audio Manager");
    return 0;
  }
}

void CXBMCApp::SetSystemVolume(float percent)
{
  CJNIAudioManager audioManager(getSystemService("audio"));
  int maxVolume = (int)(GetMaxSystemVolume() * percent);
  if (audioManager)
    audioManager.setStreamVolume(maxVolume);
  else
    android_printf("CXBMCApp::SetSystemVolume: Could not get Audio Manager");
}

void CXBMCApp::onReceive(CJNIIntent intent)
{
  std::string action = intent.getAction();
  android_printf("CXBMCApp::onReceive Got intent. Action: %s", action.c_str());
  if (action == "android.intent.action.BATTERY_CHANGED")
    m_batteryLevel = intent.getIntExtra("level",-1);
  else if (action == "android.intent.action.DREAMING_STOPPED" || action == "android.intent.action.SCREEN_ON")
  {
    if (HasFocus())
      g_application.WakeUpScreenSaverAndDPMS();
  }
  else if (action == "android.intent.action.SCREEN_OFF")
  {
    if ((m_playback_state & PLAYBACK_STATE_PLAYING) && (m_playback_state & PLAYBACK_STATE_VIDEO))
      CApplicationMessenger::GetInstance().SendMsg(TMSG_GUI_ACTION, WINDOW_INVALID, -1, static_cast<void*>(new CAction(ACTION_STOP)));
  }
  else if (action == "android.intent.action.HEADSET_PLUG" || action == "android.bluetooth.a2dp.profile.action.CONNECTION_STATE_CHANGED")
  {
    CheckHeadsetPlugged();
  }
  else if (action == "android.intent.action.MEDIA_BUTTON")
  {
    if (m_playback_state == PLAYBACK_STATE_STOPPED)
    {
      CLog::Log(LOGINFO, "Ignore MEDIA_BUTTON intent: no media playing");
      return;
    }
    CJNIKeyEvent keyevt = (CJNIKeyEvent)intent.getParcelableExtra(CJNIIntent::EXTRA_KEY_EVENT);

    int keycode = keyevt.getKeyCode();
    bool up = (keyevt.getAction() == CJNIKeyEvent::ACTION_UP);

    CLog::Log(LOGINFO, "Got MEDIA_BUTTON intent: %d, up:%s", keycode, up ? "true" : "false");
    if (keycode == CJNIKeyEvent::KEYCODE_MEDIA_RECORD)
      CAndroidKey::XBMC_Key(keycode, XBMCK_RECORD, 0, 0, up);
    else if (keycode == CJNIKeyEvent::KEYCODE_MEDIA_EJECT)
      CAndroidKey::XBMC_Key(keycode, XBMCK_EJECT, 0, 0, up);
    else if (keycode == CJNIKeyEvent::KEYCODE_MEDIA_FAST_FORWARD)
      CAndroidKey::XBMC_Key(keycode, XBMCK_MEDIA_FASTFORWARD, 0, 0, up);
    else if (keycode == CJNIKeyEvent::KEYCODE_MEDIA_NEXT)
      CAndroidKey::XBMC_Key(keycode, XBMCK_MEDIA_NEXT_TRACK, 0, 0, up);
    else if (keycode == CJNIKeyEvent::KEYCODE_MEDIA_PAUSE)
      CAndroidKey::XBMC_Key(keycode, XBMCK_MEDIA_PLAY_PAUSE, 0, 0, up);
    else if (keycode == CJNIKeyEvent::KEYCODE_MEDIA_PLAY)
      CAndroidKey::XBMC_Key(keycode, XBMCK_MEDIA_PLAY_PAUSE, 0, 0, up);
    else if (keycode == CJNIKeyEvent::KEYCODE_MEDIA_PLAY_PAUSE)
      CAndroidKey::XBMC_Key(keycode, XBMCK_MEDIA_PLAY_PAUSE, 0, 0, up);
    else if (keycode == CJNIKeyEvent::KEYCODE_MEDIA_PREVIOUS)
      CAndroidKey::XBMC_Key(keycode, XBMCK_MEDIA_PREV_TRACK, 0, 0, up);
    else if (keycode == CJNIKeyEvent::KEYCODE_MEDIA_REWIND)
      CAndroidKey::XBMC_Key(keycode, XBMCK_MEDIA_REWIND, 0, 0, up);
    else if (keycode == CJNIKeyEvent::KEYCODE_MEDIA_STOP)
      CAndroidKey::XBMC_Key(keycode, XBMCK_MEDIA_STOP, 0, 0, up);
  }
  else if (action == "android.net.conn.CONNECTIVITY_CHANGE")
  {
    if (g_application.IsInitialized())
    {
      CNetwork& net = g_application.getNetwork();
      CNetworkAndroid* netdroid = static_cast<CNetworkAndroid*>(&net);
      netdroid->RetrieveInterfaces();
    }
  }
}

void CXBMCApp::onNewIntent(CJNIIntent intent)
{
  std::string action = intent.getAction();
  CXBMCApp::android_printf("Got Intent: %s", action.c_str());
  std::string targetFile = GetFilenameFromIntent(intent);
  CXBMCApp::android_printf("-- targetFile: %s", targetFile.c_str());
  if (action == "android.intent.action.VIEW")
  {
    CFileItem* item = new CFileItem(targetFile, false);
    if (item->IsVideoDb())
    {
      *(item->GetVideoInfoTag()) = XFILE::CVideoDatabaseFile::GetVideoTag(CURL(item->GetPath()));
      item->SetPath(item->GetVideoInfoTag()->m_strFileNameAndPath);
    }
    CApplicationMessenger::GetInstance().PostMsg(TMSG_MEDIA_PLAY, 0, 0, static_cast<void*>(item));
  }
  else if (action == "android.intent.action.GET_CONTENT")
  {
    CURL targeturl(targetFile);
    if (targeturl.IsProtocol("videodb"))
    {
      std::vector<std::string> params;
      params.push_back(targeturl.Get());
      params.push_back("return");
      CApplicationMessenger::GetInstance().PostMsg(TMSG_GUI_ACTIVATE_WINDOW, WINDOW_VIDEO_NAV, 0, nullptr, "", params);
    }
    else if (targeturl.IsProtocol("musicdb"))
    {
      std::vector<std::string> params;
      params.push_back(targeturl.Get());
      params.push_back("return");
      CApplicationMessenger::GetInstance().PostMsg(TMSG_GUI_ACTIVATE_WINDOW, WINDOW_MUSIC_NAV, 0, nullptr, "", params);
    }
  }
  else if (action == ACTION_XBMC_RESUME)
  {
    if (m_playback_state != PLAYBACK_STATE_STOPPED)
    {
      if (m_playback_state & PLAYBACK_STATE_VIDEO)
        RequestVisibleBehind(true);
      if (!(m_playback_state & PLAYBACK_STATE_PLAYING))
        CApplicationMessenger::GetInstance().SendMsg(TMSG_GUI_ACTION, WINDOW_INVALID, -1, static_cast<void*>(new CAction(ACTION_PAUSE)));
    }
  }
}

void CXBMCApp::onActivityResult(int requestCode, int resultCode, CJNIIntent resultData)
{
  for (auto it = m_activityResultEvents.begin(); it != m_activityResultEvents.end(); ++it)
  {
    if ((*it)->GetRequestCode() == requestCode)
    {
      m_activityResultEvents.erase(it);
      (*it)->SetResultCode(resultCode);
      (*it)->SetResultData(resultData);
      (*it)->Set();
      break;
    }
  }
}

bool CXBMCApp::GetCapture(CJNIImage& img)
{
  CSingleLock lock(m_captureMutex);
  if (m_captureQueue.empty())
    return false;

  img = m_captureQueue.front();
  m_captureQueue.pop();
  return true;
}

void CXBMCApp::TakeScreenshot()
{
  takeScreenshot();
}

void CXBMCApp::StopCapture()
{
  CSingleLock lock(m_captureMutex);
  while (!m_captureQueue.empty())
  {
    CJNIImage img = m_captureQueue.front();
    img.close();
    m_captureQueue.pop();
  }
  CJNIMainActivity::stopCapture();
}

void CXBMCApp::onCaptureAvailable(CJNIImage image)
{
  CSingleLock lock(m_captureMutex);

  m_captureQueue.push(image);
  if (m_captureQueue.size() > CAPTURE_QUEUE_MAXDEPTH)
  {
    CJNIImage img = m_captureQueue.front();
    img.close();
    m_captureQueue.pop();
  }
}

void CXBMCApp::onScreenshotAvailable(CJNIImage image)
{
  CSingleLock lock(m_captureMutex);

  m_captureEvent.SetImage(image);
  m_captureEvent.Set();
}

void CXBMCApp::onAudioDeviceAdded(CJNIAudioDeviceInfos devices)
{
  m_audiodevices = devices;
  LogAudoDevices("onAudioDeviceAdded", m_audiodevices);
  CheckHeadsetPlugged();
}

void CXBMCApp::onAudioDeviceRemoved(CJNIAudioDeviceInfos devices)
{
  m_audiodevices = devices;
  LogAudoDevices("onAudioDeviceRemoved", m_audiodevices);
  CheckHeadsetPlugged();
}

void CXBMCApp::onVisibleBehindCanceled()
{
  CLog::Log(LOGDEBUG, "Visible Behind Cancelled");
  m_hasReqVisible = false;

  // Pressing the pause button calls OnStop() (cf. https://code.google.com/p/android/issues/detail?id=186469)
  if ((m_playback_state & PLAYBACK_STATE_PLAYING) && (m_playback_state & PLAYBACK_STATE_VIDEO))
    CApplicationMessenger::GetInstance().SendMsg(TMSG_GUI_ACTION, WINDOW_INVALID, -1, static_cast<void*>(new CAction(ACTION_PAUSE)));
}

void CXBMCApp::onMultiWindowModeChanged(bool isInMultiWindowMode)
{
  android_printf("%s: %s", __PRETTY_FUNCTION__, isInMultiWindowMode ? "true" : "false");
}

void CXBMCApp::onPictureInPictureModeChanged(bool isInPictureInPictureMode)
{
  android_printf("%s: %s", __PRETTY_FUNCTION__, isInPictureInPictureMode ? "true" : "false");
  m_hasPIP = isInPictureInPictureMode;
}

int CXBMCApp::WaitForActivityResult(const CJNIIntent &intent, int requestCode, CJNIIntent &result)
{
  int ret = 0;
  CActivityResultEvent* event = new CActivityResultEvent(requestCode);
  m_activityResultEvents.push_back(event);
  startActivityForResult(intent, requestCode);
  if (event->Wait())
  {
    result = event->GetResultData();
    ret = event->GetResultCode();
  }
  delete event;
  return ret;
}

bool CXBMCApp::WaitForCapture(CJNIImage& image)
{
  bool ret = false;
  if (m_captureEvent.WaitMSec(2000))
  {
    image = m_captureEvent.GetImage();
    ret = true;
  }
  m_captureEvent.Reset();
  return ret;
}

void CXBMCApp::onVolumeChanged(int volume)
{
  // System volume was used; Reset Kodi volume to 100% if it'not, already
  if (g_application.GetVolume(false) != 1.0)
    CApplicationMessenger::GetInstance().PostMsg(TMSG_GUI_ACTION, WINDOW_INVALID, -1, static_cast<void*>(
                                                 new CAction(ACTION_VOLUME_SET, static_cast<float>(CXBMCApp::GetMaxSystemVolume()))));
}

void CXBMCApp::onAudioFocusChange(int focusChange)
{
  CXBMCApp::android_printf("Audio Focus changed: %d", focusChange);
  if (focusChange == CJNIAudioManager::AUDIOFOCUS_LOSS ||
      focusChange == CJNIAudioManager::AUDIOFOCUS_LOSS_TRANSIENT)
  {
    m_hasAudioFocus = false;

    if (m_playback_state & PLAYBACK_STATE_PLAYING)
      CApplicationMessenger::GetInstance().SendMsg(TMSG_GUI_ACTION, WINDOW_INVALID, -1, static_cast<void*>(new CAction(ACTION_PAUSE)));
  }
/*
  else if (focusChange == CJNIAudioManager::AUDIOFOCUS_GAIN)
  {
    m_hasAudioFocus = true;
    registerMediaButtonEventReceiver();
    if (m_playback_state && !(m_playback_state & PLAYBACK_STATE_PLAYING))
      CApplicationMessenger::GetInstance().SendMsg(TMSG_GUI_ACTION, WINDOW_INVALID, -1, static_cast<void*>(new CAction(ACTION_PLAYER_PLAY)));
  }
*/
}

void CXBMCApp::doFrame(int64_t frameTimeNanos)
{
  m_vsynctime = frameTimeNanos;
  m_vsyncEvent.Set();
}

bool CXBMCApp::WaitVSync(unsigned int milliSeconds)
{
  return m_vsyncEvent.WaitMSec(milliSeconds);
}

void CXBMCApp::SetupEnv()
{
  setenv("XBMC_ANDROID_SYSTEM_LIBS", CJNISystem::getProperty("java.library.path").c_str(), 0);
  setenv("XBMC_ANDROID_LIBS", getApplicationInfo().nativeLibraryDir.c_str(), 0);
  setenv("XBMC_ANDROID_APK", getPackageResourcePath().c_str(), 0);

  std::string appName = CCompileInfo::GetAppName();
  StringUtils::ToLower(appName);
  std::string className = "org.xbmc." + appName;

  std::string xbmcHome = CJNISystem::getProperty("xbmc.home", "");
  if (xbmcHome.empty())
  {
    std::string cacheDir = getCacheDir().getAbsolutePath();
    setenv("KODI_BIN_HOME", (cacheDir + "/apk/assets").c_str(), 0);
    setenv("KODI_HOME", (cacheDir + "/apk/assets").c_str(), 0);
  }
  else
  {
    setenv("KODI_BIN_HOME", (xbmcHome + "/assets").c_str(), 0);
    setenv("KODI_HOME", (xbmcHome + "/assets").c_str(), 0);
  }

  std::string externalDir = CJNISystem::getProperty("xbmc.data", "");
  if (externalDir.empty())
  {
    CJNIFile androidPath = getExternalFilesDir("");
    if (!androidPath)
      androidPath = getDir(className.c_str(), 1);

    if (androidPath)
      externalDir = androidPath.getAbsolutePath();
  }

  if (!externalDir.empty())
    setenv("HOME", externalDir.c_str(), 0);
  else
    setenv("HOME", getenv("KODI_TEMP"), 0);

  std::string apkPath = getenv("XBMC_ANDROID_APK");
  apkPath += "/assets/python2.7";
  setenv("PYTHONHOME", apkPath.c_str(), 1);
  setenv("PYTHONPATH", "", 1);
  setenv("PYTHONOPTIMIZE","", 1);
  setenv("PYTHONNOUSERSITE", "1", 1);
}

std::string CXBMCApp::GetFilenameFromIntent(const CJNIIntent &intent)
{
    std::string ret;
    if (!intent)
      return ret;
    CJNIURI data = intent.getData();
    if (!data)
      return ret;
    std::string scheme = data.getScheme();
    StringUtils::ToLower(scheme);
    if (scheme == "content")
    {
      std::vector<std::string> filePathColumn;
      filePathColumn.push_back(CJNIMediaStoreMediaColumns::DATA);
      CJNICursor cursor = getContentResolver().query(data, filePathColumn, std::string(), std::vector<std::string>(), std::string());
      if(cursor.moveToFirst())
      {
        int columnIndex = cursor.getColumnIndex(filePathColumn[0]);
        ret = cursor.getString(columnIndex);
      }
      cursor.close();
    }
    else if(scheme == "file")
      ret = data.getPath();
    else
      ret = data.toString();
  return ret;
}

const ANativeWindow** CXBMCApp::GetNativeWindow(int timeout)
{
  if (m_window)
    return (const ANativeWindow**)&m_window;

  if (m_mainView)
    m_mainView->waitForSurface(timeout);

  return (const ANativeWindow**)&m_window;
}

void CXBMCApp::surfaceChanged(CJNISurfaceHolder holder, int format, int width, int height)
{
  m_surface_rect.x2 = width;
  m_surface_rect.y2 = height;
}

void CXBMCApp::surfaceCreated(CJNISurfaceHolder holder)
{
  m_window = ANativeWindow_fromSurface(xbmc_jnienv(), holder.getSurface().get_raw());
  if (m_window == NULL)
  {
    android_printf(" => invalid ANativeWindow object");
    return;
  }
  XBMC_SetupDisplay();
}

void CXBMCApp::surfaceDestroyed(CJNISurfaceHolder holder)
{
  // If we have exited XBMC, it no longer exists.
  if (!m_exiting)
  {
    XBMC_DestroyDisplay();
    m_window = NULL;
  }
}
