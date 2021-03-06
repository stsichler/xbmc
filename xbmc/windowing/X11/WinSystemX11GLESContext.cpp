/*
 *  Copyright (C) 2005-2018 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */


#include "WinSystemX11GLESContext.h"
#include "GLContextEGL.h"
#include "utils/log.h"
#include "windowing/GraphicContext.h"
#include "guilib/DispResource.h"
#include "threads/SingleLock.h"
#include "Application.h"

#include "cores/RetroPlayer/process/X11/RPProcessInfoX11.h"
#include "cores/RetroPlayer/rendering/VideoRenderers/RPRendererOpenGLES.h"
#include "cores/VideoPlayer/DVDCodecs/DVDFactoryCodec.h"
#include "cores/VideoPlayer/Process/X11/ProcessInfoX11.h"
#include "cores/VideoPlayer/VideoRenderers/LinuxRendererGLES.h"
#include "cores/VideoPlayer/VideoRenderers/RenderFactory.h"

#include "OptionalsReg.h"
#include "platform/linux/OptionalsReg.h"

using namespace KODI;

std::unique_ptr<CWinSystemBase> CWinSystemBase::CreateWinSystem()
{
  std::unique_ptr<CWinSystemBase> winSystem(new CWinSystemX11GLESContext());
  return winSystem;
}

CWinSystemX11GLESContext::CWinSystemX11GLESContext()
{
  std::string envSink;
  if (getenv("KODI_AE_SINK"))
    envSink = getenv("KODI_AE_SINK");
  if (StringUtils::EqualsNoCase(envSink, "ALSA"))
  {
    OPTIONALS::ALSARegister();
  }
  else if (StringUtils::EqualsNoCase(envSink, "PULSE"))
  {
    OPTIONALS::PulseAudioRegister();
  }
  else if (StringUtils::EqualsNoCase(envSink, "OSS"))
  {
    OPTIONALS::OSSRegister();
  }
  else if (StringUtils::EqualsNoCase(envSink, "SNDIO"))
  {
    OPTIONALS::SndioRegister();
  }
  else
  {
    if (!OPTIONALS::PulseAudioRegister())
    {
      if (!OPTIONALS::ALSARegister())
      {
        if (!OPTIONALS::SndioRegister())
        {
          OPTIONALS::OSSRegister();
        }
      }
    }
  }

  m_lirc.reset(OPTIONALS::LircRegister());
}

CWinSystemX11GLESContext::~CWinSystemX11GLESContext()
{
  delete m_pGLContext;
}

void CWinSystemX11GLESContext::PresentRenderImpl(bool rendered)
{
  if (rendered && m_pGLContext)
    m_pGLContext->SwapBuffers();

  if (m_delayDispReset && m_dispResetTimer.IsTimePast())
  {
    m_delayDispReset = false;
    CSingleLock lock(m_resourceSection);
    // tell any shared resources
    for (std::vector<IDispResource *>::iterator i = m_resources.begin(); i != m_resources.end(); ++i)
      (*i)->OnResetDisplay();
  }
}

void CWinSystemX11GLESContext::SetVSyncImpl(bool enable)
{
  m_pGLContext->SetVSync(enable);
}

bool CWinSystemX11GLESContext::IsExtSupported(const char* extension) const
{
  if(strncmp(extension, m_pGLContext->ExtPrefix().c_str(), 4) != 0)
    return CRenderSystemGLES::IsExtSupported(extension);

  return m_pGLContext->IsExtSupported(extension);
}

EGLDisplay CWinSystemX11GLESContext::GetEGLDisplay() const 
{
  return m_pGLContext->m_eglDisplay;
}

EGLSurface CWinSystemX11GLESContext::GetEGLSurface() const
{
  return m_pGLContext->m_eglSurface;
}

EGLContext CWinSystemX11GLESContext::GetEGLContext() const
{
  return m_pGLContext->m_eglContext;
}

EGLConfig CWinSystemX11GLESContext::GetEGLConfig() const
{
  return  m_pGLContext->m_eglConfig;
}

bool CWinSystemX11GLESContext::SetWindow(int width, int height, bool fullscreen, const std::string &output, int *winstate)
{
  int newwin = 0;

  CWinSystemX11::SetWindow(width, height, fullscreen, output, &newwin);
  if (newwin)
  {
    RefreshGLContext(m_currentOutput.compare(output) != 0);
    XSync(m_dpy, false);
    CServiceBroker::GetWinSystem()->GetGfxContext().Clear(0);
    CServiceBroker::GetWinSystem()->GetGfxContext().Flip(true, false);
    ResetVSync();

    m_windowDirty = false;
    m_bIsInternalXrr = false;

    if (!m_delayDispReset)
    {
      CSingleLock lock(m_resourceSection);
      // tell any shared resources
      for (std::vector<IDispResource *>::iterator i = m_resources.begin(); i != m_resources.end(); ++i)
        (*i)->OnResetDisplay();
    }
  }
  return true;
}

bool CWinSystemX11GLESContext::CreateNewWindow(const std::string& name, bool fullScreen, RESOLUTION_INFO& res)
{
  CLog::Log(LOGNOTICE, "CWinSystemX11GLESContext::CreateNewWindow");
  if(!CWinSystemX11::CreateNewWindow(name, fullScreen, res) || !m_pGLContext)
    return false;

  m_pGLContext->QueryExtensions();
  return true;
}

bool CWinSystemX11GLESContext::ResizeWindow(int newWidth, int newHeight, int newLeft, int newTop)
{
  m_newGlContext = false;
  CWinSystemX11::ResizeWindow(newWidth, newHeight, newLeft, newTop);
  CRenderSystemGLES::ResetRenderSystem(newWidth, newHeight);

  if (m_newGlContext)
    g_application.ReloadSkin();

  return true;
}

void CWinSystemX11GLESContext::FinishWindowResize(int newWidth, int newHeight)
{
  m_newGlContext = false;
  CWinSystemX11::FinishWindowResize(newWidth, newHeight);
  CRenderSystemGLES::ResetRenderSystem(newWidth, newHeight);

  if (m_newGlContext)
    g_application.ReloadSkin();
}

bool CWinSystemX11GLESContext::SetFullScreen(bool fullScreen, RESOLUTION_INFO& res, bool blankOtherDisplays)
{
  m_newGlContext = false;
  CWinSystemX11::SetFullScreen(fullScreen, res, blankOtherDisplays);
  CRenderSystemGLES::ResetRenderSystem(res.iWidth, res.iHeight);

  if (m_newGlContext)
    g_application.ReloadSkin();

  return true;
}

bool CWinSystemX11GLESContext::DestroyWindowSystem()
{
  if (m_pGLContext)
    m_pGLContext->Destroy();
  return CWinSystemX11::DestroyWindowSystem();
}

bool CWinSystemX11GLESContext::DestroyWindow()
{
  if (m_pGLContext)
    m_pGLContext->Detach();
  return CWinSystemX11::DestroyWindow();
}

XVisualInfo* CWinSystemX11GLESContext::GetVisual()
{
  EGLDisplay eglDisplay;

  PFNEGLGETPLATFORMDISPLAYEXTPROC eglGetPlatformDisplayEXT =
    (PFNEGLGETPLATFORMDISPLAYEXTPROC)eglGetProcAddress("eglGetPlatformDisplayEXT");
  if (eglGetPlatformDisplayEXT)
  {
    EGLint attribs[] =
    {
      EGL_PLATFORM_X11_SCREEN_EXT, m_screen,
      EGL_NONE
    };
    eglDisplay = eglGetPlatformDisplayEXT(EGL_PLATFORM_X11_EXT,(EGLNativeDisplayType)m_dpy, attribs);
  }
  else
    eglDisplay = eglGetDisplay((EGLNativeDisplayType)m_dpy);

  if (eglDisplay == EGL_NO_DISPLAY)
  {
    CLog::Log(LOGERROR, "failed to get egl display\n");
    return nullptr;
  }
  if (!eglInitialize(eglDisplay, nullptr, nullptr))
  {
    CLog::Log(LOGERROR, "failed to initialize egl display\n");
    return nullptr;
  }

  GLint att[] =
  {
    EGL_RED_SIZE, 8,
    EGL_GREEN_SIZE, 8,
    EGL_BLUE_SIZE, 8,
    EGL_ALPHA_SIZE, 8,
    EGL_BUFFER_SIZE, 32,
    EGL_DEPTH_SIZE, 24,
    EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
    EGL_NONE
  };
  EGLint numConfigs;
  EGLConfig eglConfig = 0;
  if (!eglChooseConfig(eglDisplay, att, &eglConfig, 1, &numConfigs) || numConfigs == 0) {
    CLog::Log(LOGERROR, "Failed to choose a config %d\n", eglGetError());
    return nullptr;
  }

  XVisualInfo x11_visual_info_template;
  if (!eglGetConfigAttrib(eglDisplay, eglConfig, EGL_NATIVE_VISUAL_ID, (EGLint*)&x11_visual_info_template.visualid)) {
    CLog::Log(LOGERROR, "Failed to query native visual id\n");
    return nullptr;
  }
  int num_visuals;
  XVisualInfo *visual = 
    XGetVisualInfo(m_dpy, VisualIDMask, &x11_visual_info_template, &num_visuals);
  return visual;
}

bool CWinSystemX11GLESContext::RefreshGLContext(bool force)
{
  bool success = false;
  if (m_pGLContext)
  {
    success = m_pGLContext->Refresh(force, m_screen, m_glWindow, m_newGlContext);
    if (!success)
    {
      success = m_pGLContext->CreatePB();
      m_newGlContext = true;
    }
    return success;
  }

  VIDEOPLAYER::CProcessInfoX11::Register();
  RETRO::CRPProcessInfoX11::Register();
  RETRO::CRPProcessInfoX11::RegisterRendererFactory(new RETRO::CRendererFactoryOpenGLES);
  CDVDFactoryCodec::ClearHWAccels();
  VIDEOPLAYER::CRendererFactory::ClearRenderer();
  CLinuxRendererGLES::Register();

  std::string gli = (getenv("KODI_GL_INTERFACE") != nullptr) ? getenv("KODI_GL_INTERFACE") : "";

  m_pGLContext = new CGLContextEGL(m_dpy,EGL_OPENGL_ES_API);
  success = m_pGLContext->Refresh(force, m_screen, m_glWindow, m_newGlContext);
  if (!success && gli == "EGL_PB")
  {
    success = m_pGLContext->CreatePB();
    m_newGlContext = true;
  }

  if (!success)
  {
    delete m_pGLContext;
    m_pGLContext = nullptr;
  }
  return success;
}
