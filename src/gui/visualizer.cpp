#include "gui.h"
#include <imgui.h>
#include <fmt/printf.h>
#include "IconsFontAwesome4.h"

#ifdef HAVE_PROJECTM
#include <libprojectM/projectM.hpp>
#include <SDL.h>
#include <GL/gl.h>
#include <unistd.h>
#include <cstring>

typedef void(*PFNGLGENFRAMEBUFFERS)(GLsizei,GLuint*);
typedef void(*PFNGLDELETEFRAMEBUFFERS)(GLsizei,const GLuint*);
typedef void(*PFNGLBINDFRAMEBUFFER)(GLenum,GLuint);
typedef void(*PFNGLFRAMEBUFFERTEXTURE2D)(GLenum,GLenum,GLenum,GLuint,GLint);
typedef void(*PFNGLGETINTEGERV_FN)(GLenum,GLint*);

static PFNGLGENFRAMEBUFFERS pglGenFramebuffers=NULL;
static PFNGLDELETEFRAMEBUFFERS pglDeleteFramebuffers=NULL;
static PFNGLBINDFRAMEBUFFER pglBindFramebuffer=NULL;
static PFNGLFRAMEBUFFERTEXTURE2D pglFramebufferTexture2D=NULL;

static bool fboProcsLoaded=false;
static void loadFBOProcs() {
  if (fboProcsLoaded) return;
  pglGenFramebuffers=(PFNGLGENFRAMEBUFFERS)SDL_GL_GetProcAddress("glGenFramebuffers");
  pglDeleteFramebuffers=(PFNGLDELETEFRAMEBUFFERS)SDL_GL_GetProcAddress("glDeleteFramebuffers");
  pglBindFramebuffer=(PFNGLBINDFRAMEBUFFER)SDL_GL_GetProcAddress("glBindFramebuffer");
  pglFramebufferTexture2D=(PFNGLFRAMEBUFFERTEXTURE2D)SDL_GL_GetProcAddress("glFramebufferTexture2D");
  fboProcsLoaded=true;
}

static projectM* pm=NULL;
static GLuint vizFBO=0;
static GLuint vizTex=0;
static int vizTexW=0;
static int vizTexH=0;
static int vizOscReadPos=0;
static int vizGLW=0;
static int vizGLH=0;
static double vizLastRender=0.0;
static int vizTargetFPS=30;

static void ensureFBO(int w,int h) {
  loadFBOProcs();
  if (!pglGenFramebuffers||!pglBindFramebuffer) return;
  if (w==vizTexW&&h==vizTexH&&vizFBO) return;
  if (vizFBO) { pglDeleteFramebuffers(1,&vizFBO); vizFBO=0; }
  if (vizTex) { glDeleteTextures(1,&vizTex); vizTex=0; }
  vizTexW=w; vizTexH=h;
  glGenTextures(1,&vizTex);
  glBindTexture(GL_TEXTURE_2D,vizTex);
  glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA,w,h,0,GL_RGBA,GL_UNSIGNED_BYTE,NULL);
  glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_LINEAR);
  glBindTexture(GL_TEXTURE_2D,0);
  pglGenFramebuffers(1,&vizFBO);
  pglBindFramebuffer(0x8D40,vizFBO);
  pglFramebufferTexture2D(0x8D40,0x8CE0,GL_TEXTURE_2D,vizTex,0);
  pglBindFramebuffer(0x8D40,0);
}

static const char* kPresetDirs[]={
  "/home/rednoobmusic/.local/share/projectM/all-presets",
  "/home/rednoobmusic/.local/share/projectM/cream-of-the-crop",
  "/usr/share/projectM/presets",
  NULL
};

static const char* findPresetDir() {
  for (int i=0;kPresetDirs[i];i++) {
    if (access(kPresetDirs[i],R_OK)==0) return kPresetDirs[i];
  }
  return "/usr/share/projectM/presets";
}

static void initPM(int w,int h) {
  if (pm) return;
  projectM::Settings s;
  s.windowWidth=w;
  s.windowHeight=h;
  s.fps=vizTargetFPS;
  s.meshX=24;
  s.meshY=16;
  s.textureSize=512;
  s.presetURL=findPresetDir();
  s.smoothPresetDuration=5;
  s.presetDuration=12;
  s.shuffleEnabled=true;
  s.beatSensitivity=1.0f;
  pm=new projectM(s,projectM::FLAG_NONE);
}
#endif

void FurnaceGUI::drawVisualizer() {
  if (!visualizerOpen) return;
#ifdef HAVE_PROJECTM
  ImGui::SetNextWindowSize(ImVec2(640*dpiScale,480*dpiScale),ImGuiCond_FirstUseEver);
  if (ImGui::Begin("projectm",&visualizerOpen,ImGuiWindowFlags_NoScrollbar)) {
    bool collapsed=ImGui::IsWindowCollapsed();

    float cw=ImGui::GetContentRegionAvail().x;
    float ch=ImGui::GetContentRegionAvail().y-ImGui::GetFrameHeightWithSpacing();
    if (cw<16) cw=16;
    if (ch<16) ch=16;
    int iw=(int)cw, ih=(int)ch;

    if (!collapsed) {
      initPM(iw,ih);
      ensureFBO(iw,ih);
    }

    int outChans=e->getAudioDescGot().outChans;
    int writePos=e->oscWritePos;
    int available=(writePos-vizOscReadPos+32768)&0x7fff;
    if (available>4096) available=4096;
    if (available>0&&outChans>0) {
      if (!collapsed&&pm) {
        float pcm[2][4096];
        memset(pcm,0,sizeof(pcm));
        for (int i=0;i<available;i++) {
          int p=(vizOscReadPos+i)&0x7fff;
          pcm[0][i]=e->oscBuf[0][p];
          pcm[1][i]=outChans>1?e->oscBuf[1][p]:pcm[0][i];
        }
        pm->pcm()->addPCMfloat_2ch((float*)pcm,available);
      }
      vizOscReadPos=(vizOscReadPos+available)&0x7fff;
    }

    if (!collapsed&&pm&&vizFBO&&pglBindFramebuffer) {
      // only resetGL when the window size actually changes
      if (iw!=vizGLW||ih!=vizGLH) {
        pm->projectM_resetGL(iw,ih);
        vizGLW=iw; vizGLH=ih;
      }

      double now=ImGui::GetTime();
      double interval=1.0/vizTargetFPS;
      if (now-vizLastRender>=interval) {
        vizLastRender=now;

        GLint prevFBO=0;
        glGetIntegerv(0x8CA6,&prevFBO);
        GLint prevVP[4];
        glGetIntegerv(GL_VIEWPORT,prevVP);

        pglBindFramebuffer(0x8D40,vizFBO);
        glViewport(0,0,iw,ih);
        pm->renderFrame();
        pglBindFramebuffer(0x8D40,(GLuint)prevFBO);
        glViewport(prevVP[0],prevVP[1],prevVP[2],prevVP[3]);
      }

      ImGui::Image((ImTextureID)(intptr_t)vizTex,ImVec2(cw,ch),ImVec2(0,1),ImVec2(1,0));

      if (ImGui::IsItemHovered()&&ImGui::IsMouseDoubleClicked(0)) {
        pm->selectNext(false);
      }
    }

    ImGui::Separator();
    if (pm) {
      if (ImGui::SmallButton("<<")) pm->selectPrevious(false);
      ImGui::SameLine();
      if (ImGui::SmallButton(">>")) pm->selectNext(false);
      ImGui::SameLine();
      if (ImGui::SmallButton(ICON_FA_RANDOM)) pm->selectRandom(false);
      ImGui::SameLine();
      bool locked=pm->isPresetLocked();
      if (ImGui::Checkbox("lock",&locked)) pm->setPresetLock(locked);
      ImGui::SameLine();
      ImGui::SetNextItemWidth(80*dpiScale);
      ImGui::SliderInt("fps",&vizTargetFPS,5,60);
    }
  }
  ImGui::End();
#else
  ImGui::SetNextWindowSize(ImVec2(300*dpiScale,80*dpiScale),ImGuiCond_FirstUseEver);
  if (ImGui::Begin("projectm",&visualizerOpen)) {
    ImGui::TextDisabled("libprojectM not found at build time.");
  }
  ImGui::End();
#endif
}
