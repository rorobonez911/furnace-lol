#include "gui.h"
#include <SDL.h>
#include <imgui.h>
#include <fmt/printf.h>

#ifdef HAVE_WEBKIT
#include <gtk/gtk.h>
#include <webkit2/webkit2.h>
#include <cairo/cairo.h>
#include <GL/gl.h>
#include <gdk/gdkx.h>
#include <X11/Xlib.h>
#include <cstdlib>
#include <cstring>

static bool gtkInited=false;
static GtkWidget* offWin=NULL;
static GtkWidget* webView=NULL;
static GLuint browserTex=0;
static int browserTexW=0;
static int browserTexH=0;
static bool browserLoadDone=false;
static String browserLoadTitle;
static cairo_surface_t* pendingSurface=NULL;
static bool snapshotPending=false;
static bool interactedThisFrame=false;

struct ImGuiKeyGDK { ImGuiKey imkey; guint gdkkey; };
static const ImGuiKeyGDK kKeyMap[]={
  {ImGuiKey_Enter,     GDK_KEY_Return},
  {ImGuiKey_Backspace, GDK_KEY_BackSpace},
  {ImGuiKey_Delete,    GDK_KEY_Delete},
  {ImGuiKey_Tab,       GDK_KEY_Tab},
  {ImGuiKey_Escape,    GDK_KEY_Escape},
  {ImGuiKey_LeftArrow, GDK_KEY_Left},
  {ImGuiKey_RightArrow,GDK_KEY_Right},
  {ImGuiKey_UpArrow,   GDK_KEY_Up},
  {ImGuiKey_DownArrow, GDK_KEY_Down},
  {ImGuiKey_Home,      GDK_KEY_Home},
  {ImGuiKey_End,       GDK_KEY_End},
  {ImGuiKey_PageUp,    GDK_KEY_Page_Up},
  {ImGuiKey_PageDown,  GDK_KEY_Page_Down},
  {ImGuiKey_None,      0}
};

static void sendX11Key(guint keyval, bool press, guint gdkstate=0) {
  if (!webView) return;
  GdkWindow* gdkw=gtk_widget_get_window(webView);
  if (!gdkw) return;
  Display* xdpy=GDK_DISPLAY_XDISPLAY(gdk_display_get_default());
  Window xwin=GDK_WINDOW_XID(gdkw);
  KeySym ksym=(KeySym)keyval;
  KeyCode kcode=XKeysymToKeycode(xdpy,ksym);
  unsigned int xstate=0;
  if (gdkstate&GDK_CONTROL_MASK) xstate|=ControlMask;
  if (gdkstate&GDK_SHIFT_MASK)   xstate|=ShiftMask;
  if (gdkstate&GDK_MOD1_MASK)    xstate|=Mod1Mask;
  XKeyEvent ev;
  memset(&ev,0,sizeof(ev));
  ev.type=press?KeyPress:KeyRelease;
  ev.display=xdpy;
  ev.window=xwin;
  ev.root=DefaultRootWindow(xdpy);
  ev.subwindow=None;
  ev.time=CurrentTime;
  ev.x=1; ev.y=1;
  ev.x_root=1; ev.y_root=1;
  ev.same_screen=True;
  ev.keycode=kcode;
  ev.state=xstate;
  XSendEvent(xdpy,xwin,False,KeyPressMask|KeyReleaseMask,(XEvent*)&ev);
  XFlush(xdpy);
}

static bool* gBrowserFocused=NULL;

static void forwardKeyboard() {
  if (!webView||!gBrowserFocused||!*gBrowserFocused) return;
  ImGuiIO& io=ImGui::GetIO();
  guint mods=0;
  if (io.KeyCtrl)  mods|=GDK_CONTROL_MASK;
  if (io.KeyShift) mods|=GDK_SHIFT_MASK;
  if (io.KeyAlt)   mods|=GDK_MOD1_MASK;
  for (int i=0;kKeyMap[i].imkey!=ImGuiKey_None;i++) {
    if (ImGui::IsKeyPressed(kKeyMap[i].imkey,false)) {
      sendX11Key(kKeyMap[i].gdkkey,true,mods);
      sendX11Key(kKeyMap[i].gdkkey,false,mods);
      interactedThisFrame=true;
    }
  }
  for (int i=0;i<io.InputQueueCharacters.Size;i++) {
    ImWchar c=io.InputQueueCharacters[i];
    guint kv=gdk_unicode_to_keyval((guint32)c);
    sendX11Key(kv,true,mods);
    sendX11Key(kv,false,mods);
    interactedThisFrame=true;
  }
  io.InputQueueCharacters.resize(0);
}

static void onLoadChanged(WebKitWebView*, WebKitLoadEvent ev, gpointer) {
  if (ev==WEBKIT_LOAD_FINISHED) {
    browserLoadDone=true;
    const gchar* t=webkit_web_view_get_title(WEBKIT_WEB_VIEW(webView));
    browserLoadTitle=t?t:"";
  }
}

static void onSnapshotReady(GObject* src, GAsyncResult* res, gpointer) {
  GError* err=NULL;
  cairo_surface_t* surf=webkit_web_view_get_snapshot_finish(WEBKIT_WEB_VIEW(src),res,&err);
  if (err) { g_error_free(err); snapshotPending=false; return; }
  if (pendingSurface) cairo_surface_destroy(pendingSurface);
  pendingSurface=surf;
  snapshotPending=false;
}

static void uploadSurface(cairo_surface_t* surf) {
  if (!surf) return;
  int sw=cairo_image_surface_get_width(surf);
  int sh=cairo_image_surface_get_height(surf);
  if (sw<=0||sh<=0) return;
  unsigned char* data=cairo_image_surface_get_data(surf);
  if (!browserTex) glGenTextures(1,&browserTex);
  glBindTexture(GL_TEXTURE_2D,browserTex);
  glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_LINEAR);
  glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA,sw,sh,0,GL_BGRA,GL_UNSIGNED_BYTE,data);
  glBindTexture(GL_TEXTURE_2D,0);
  browserTexW=sw;
  browserTexH=sh;
}

static void initGTK() {
  if (gtkInited) return;
  putenv((char*)"GDK_BACKEND=x11");
  gtk_init(NULL,NULL);
  gtkInited=true;
  offWin=gtk_window_new(GTK_WINDOW_TOPLEVEL);
  gtk_window_set_decorated(GTK_WINDOW(offWin),FALSE);
  gtk_window_set_skip_taskbar_hint(GTK_WINDOW(offWin),TRUE);
  gtk_window_set_skip_pager_hint(GTK_WINDOW(offWin),TRUE);
  gtk_window_set_type_hint(GTK_WINDOW(offWin),GDK_WINDOW_TYPE_HINT_UTILITY);
  gtk_window_resize(GTK_WINDOW(offWin),1280,720);
  webView=webkit_web_view_new();
  gtk_container_add(GTK_CONTAINER(offWin),webView);
  gtk_widget_show_all(offWin);
  gtk_widget_realize(offWin);
  gtk_widget_set_opacity(offWin,0.0);
  GdkWindow* gdkRoot=gtk_widget_get_window(offWin);
  if (gdkRoot) {
    gdk_window_lower(gdkRoot);
  }
  g_signal_connect(webView,"load-changed",G_CALLBACK(onLoadChanged),NULL);
}

static void pumpGTK(int iters=16) {
  for (int i=0;i<iters;i++) g_main_context_iteration(NULL,FALSE);
}

static void requestSnapshot(int w,int h) {
  gtk_widget_set_size_request(webView,w,h);
  gtk_window_resize(GTK_WINDOW(offWin),w,h);
  if (snapshotPending) return;
  snapshotPending=true;
  webkit_web_view_get_snapshot(
    WEBKIT_WEB_VIEW(webView),
    WEBKIT_SNAPSHOT_REGION_VISIBLE,
    WEBKIT_SNAPSHOT_OPTIONS_NONE,
    NULL,
    onSnapshotReady,
    NULL
  );
}

static void forwardMouse(ImVec2 imgPos,ImVec2 imgSize) {
  if (!webView||browserTexW<=0||browserTexH<=0) return;
  ImVec2 mp=ImGui::GetMousePos();
  float rx=(mp.x-imgPos.x)/imgSize.x;
  float ry=(mp.y-imgPos.y)/imgSize.y;
  if (rx<0||ry<0||rx>1||ry>1) return;
  double gx=rx*browserTexW,gy=ry*browserTexH;
  GdkWindow* gdkw=gtk_widget_get_window(webView);
  if (!gdkw) return;
  Display* xdpy=GDK_DISPLAY_XDISPLAY(gdk_display_get_default());
  Window xwin=GDK_WINDOW_XID(gdkw);
  static bool wasDown[3]={false,false,false};
  for (int btn=0;btn<3;btn++) {
    bool down=ImGui::IsMouseDown(btn);
    if (down!=wasDown[btn]) {
      wasDown[btn]=down;
      XButtonEvent ev;
      memset(&ev,0,sizeof(ev));
      ev.type=down?ButtonPress:ButtonRelease;
      ev.display=xdpy;
      ev.window=xwin;
      ev.root=DefaultRootWindow(xdpy);
      ev.x=(int)gx; ev.y=(int)gy;
      ev.x_root=(int)gx; ev.y_root=(int)gy;
      ev.button=btn+1;
      ev.same_screen=True;
      ev.time=CurrentTime;
      XSendEvent(xdpy,xwin,True,ButtonPressMask|ButtonReleaseMask,(XEvent*)&ev);
      interactedThisFrame=true;
    }
  }
  XMotionEvent mev;
  memset(&mev,0,sizeof(mev));
  mev.type=MotionNotify;
  mev.display=xdpy;
  mev.window=xwin;
  mev.root=DefaultRootWindow(xdpy);
  mev.x=(int)gx; mev.y=(int)gy;
  mev.same_screen=True;
  mev.time=CurrentTime;
  XSendEvent(xdpy,xwin,True,PointerMotionMask,(XEvent*)&mev);
  XFlush(xdpy);
}
#endif

void FurnaceGUI::drawBrowser() {
  if (!browserOpen) return;

  ImVec4 bgColor=uiColors[GUI_COLOR_BACKGROUND];
  ImVec4 textColor=uiColors[GUI_COLOR_TEXT];
  ImVec4 dimColor=ImVec4(textColor.x*0.6f,textColor.y*0.6f,textColor.z*0.6f,textColor.w);

  auto navigate=[&](const char* url) {
    String u=url;
    if (u.empty()) return;
    if (u.find("://")==String::npos) u="https://"+u;
    if (browserHistoryPos<(int)browserHistory.size()-1)
      browserHistory.resize(browserHistoryPos+1);
    browserHistory.push_back(u);
    browserHistoryPos=(int)browserHistory.size()-1;
    browserCurrentURL=u;
    strncpy(browserURLBuf,u.c_str(),sizeof(browserURLBuf)-1);
    browserURLBuf[sizeof(browserURLBuf)-1]=0;
#ifdef HAVE_WEBKIT
    initGTK();
    webkit_web_view_load_uri(WEBKIT_WEB_VIEW(webView),u.c_str());
    browserStatus=fmt::sprintf("Loading: %s",u);
#else
    browserStatus=SDL_OpenURL(u.c_str())==0
      ?fmt::sprintf("Opened: %s",u)
      :fmt::sprintf("Failed: %s",u);
#endif
  };

  ImGui::SetNextWindowSize(ImVec2(900*dpiScale,600*dpiScale),ImGuiCond_FirstUseEver);
  if (ImGui::Begin("Browser",&browserOpen,ImGuiWindowFlags_NoScrollbar)) {
    if (ImGui::IsWindowCollapsed()) {
      ImGui::End();
      return;
    }

    float bW=28.0f*dpiScale;
    float gap=3.0f*dpiScale;
    bool canBack=(browserHistoryPos>0);
    bool canFwd=(browserHistoryPos<(int)browserHistory.size()-1);

    if (!canBack) ImGui::BeginDisabled();
    if (ImGui::Button("<##brBack",ImVec2(bW,0))) {
      browserHistoryPos--;
      browserCurrentURL=browserHistory[browserHistoryPos];
      strncpy(browserURLBuf,browserCurrentURL.c_str(),sizeof(browserURLBuf)-1);
#ifdef HAVE_WEBKIT
      initGTK();
      webkit_web_view_load_uri(WEBKIT_WEB_VIEW(webView),browserCurrentURL.c_str());
      browserStatus=fmt::sprintf("Loading: %s",browserCurrentURL);
#else
      SDL_OpenURL(browserCurrentURL.c_str());
      browserStatus=fmt::sprintf("Opened: %s",browserCurrentURL);
#endif
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Back");
    if (!canBack) ImGui::EndDisabled();

    ImGui::SameLine(0,gap);
    if (!canFwd) ImGui::BeginDisabled();
    if (ImGui::Button(">##brFwd",ImVec2(bW,0))) {
      browserHistoryPos++;
      browserCurrentURL=browserHistory[browserHistoryPos];
      strncpy(browserURLBuf,browserCurrentURL.c_str(),sizeof(browserURLBuf)-1);
#ifdef HAVE_WEBKIT
      initGTK();
      webkit_web_view_load_uri(WEBKIT_WEB_VIEW(webView),browserCurrentURL.c_str());
      browserStatus=fmt::sprintf("Loading: %s",browserCurrentURL);
#else
      SDL_OpenURL(browserCurrentURL.c_str());
      browserStatus=fmt::sprintf("Opened: %s",browserCurrentURL);
#endif
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Forward");
    if (!canFwd) ImGui::EndDisabled();

    ImGui::SameLine(0,gap);
    if (ImGui::Button("R##brRefresh",ImVec2(bW,0))) {
      if (!browserCurrentURL.empty()) {
#ifdef HAVE_WEBKIT
        initGTK();
        webkit_web_view_reload(WEBKIT_WEB_VIEW(webView));
        browserStatus=fmt::sprintf("Refreshing: %s",browserCurrentURL);
#else
        SDL_OpenURL(browserCurrentURL.c_str());
        browserStatus=fmt::sprintf("Refreshed: %s",browserCurrentURL);
#endif
      }
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Refresh");

    ImGui::SameLine(0,gap);
    float goW=36.0f*dpiScale;
    float urlW=ImGui::GetContentRegionAvail().x-goW-gap;
    ImGui::SetNextItemWidth(urlW);
    bool enter=ImGui::InputText("##brURL",browserURLBuf,sizeof(browserURLBuf),ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::SameLine(0,gap);
    if (ImGui::Button("Go##brGo",ImVec2(goW,0))||enter) {
      navigate(browserURLBuf);
    }

    ImGui::Separator();

    float statusH=ImGui::GetFrameHeightWithSpacing()+4.0f*dpiScale;
    float contentH=ImGui::GetContentRegionAvail().y-statusH;

#ifdef HAVE_WEBKIT
    if (!browserCurrentURL.empty()) {
      initGTK();
      gBrowserFocused=&browserFocused;
      interactedThisFrame=false;

      int cW=(int)ImGui::GetContentRegionAvail().x;
      int cH=(int)contentH;
      if (cW<16) cW=16;
      if (cH<16) cH=16;

      pumpGTK(interactedThisFrame?64:30);

      if (pendingSurface&&!snapshotPending) {
        uploadSurface(pendingSurface);
        cairo_surface_destroy(pendingSurface);
        pendingSurface=NULL;
      }

      if (browserLoadDone) {
        if (!browserLoadTitle.empty())
          browserStatus=fmt::sprintf("%s — %s",browserLoadTitle,browserCurrentURL);
        browserLoadDone=false;
        snapshotPending=false;
        pumpGTK(100);
      }

      if (!snapshotPending) requestSnapshot(cW,cH);

      if (browserTex) {
        ImVec2 imgPos=ImGui::GetCursorScreenPos();
        ImVec2 imgSize=ImVec2((float)cW,(float)cH);
        ImGui::Image((ImTextureID)(intptr_t)browserTex,imgSize);

        if (ImGui::IsItemClicked(0)) {
          browserFocused=true;
          gtk_widget_grab_focus(webView);
        }
        if (ImGui::IsMouseClicked(0)&&!ImGui::IsItemHovered()) {
          browserFocused=false;
        }
        if (browserFocused) {
          ImGui::SetNextFrameWantCaptureKeyboard(false);
          forwardKeyboard();
        }
        if (ImGui::IsItemHovered()) {
          forwardMouse(imgPos,imgSize);
          float wheel=ImGui::GetIO().MouseWheel;
          if (wheel!=0) {
            GdkWindow* gdkw=gtk_widget_get_window(webView);
            if (gdkw) {
              Display* xdpy=GDK_DISPLAY_XDISPLAY(gdk_display_get_default());
              Window xwin=GDK_WINDOW_XID(gdkw);
              XButtonEvent ev;
              memset(&ev,0,sizeof(ev));
              ev.type=ButtonPress;
              ev.display=xdpy;
              ev.window=xwin;
              ev.root=DefaultRootWindow(xdpy);
              ev.button=wheel>0?4:5;
              ev.same_screen=True;
              ev.time=CurrentTime;
              XSendEvent(xdpy,xwin,True,ButtonPressMask,(XEvent*)&ev);
              ev.type=ButtonRelease;
              XSendEvent(xdpy,xwin,True,ButtonReleaseMask,(XEvent*)&ev);
              XFlush(xdpy);
              interactedThisFrame=true;
            }
          }
        }
        if (interactedThisFrame) {
          pumpGTK(64);
          snapshotPending=false;
          requestSnapshot(cW,cH);
        }
      } else {
        ImGui::SetCursorPosY(ImGui::GetCursorPosY()+contentH*0.4f);
        ImGui::SetCursorPosX((ImGui::GetContentRegionAvail().x-100.0f*dpiScale)*0.5f);
        ImGui::TextColored(dimColor,"Loading...");
      }
    } else {
      ImGui::PushStyleColor(ImGuiCol_ChildBg,ImVec4(bgColor.x*0.85f,bgColor.y*0.85f,bgColor.z*0.85f,1.0f));
      if (ImGui::BeginChild("##brEmpty",ImVec2(0,contentH),true)) {
        float pw=ImGui::GetWindowWidth();
        float ph=ImGui::GetWindowHeight();
        ImGui::SetCursorPos(ImVec2(pw*0.5f-100.0f*dpiScale,ph*0.4f));
        ImGui::TextColored(dimColor,"Enter a URL above and press Go");
        ImGui::SetCursorPos(ImVec2(pw*0.5f-60.0f*dpiScale,ph*0.4f+ImGui::GetFrameHeightWithSpacing()));
        ImGui::TextColored(dimColor,"Renders inside Furnace");
      }
      ImGui::EndChild();
      ImGui::PopStyleColor();
    }
#else
    ImGui::PushStyleColor(ImGuiCol_ChildBg,ImVec4(bgColor.x*0.85f,bgColor.y*0.85f,bgColor.z*0.85f,1.0f));
    if (ImGui::BeginChild("##brContent",ImVec2(0,contentH),true)) {
      float pw=ImGui::GetWindowWidth();
      float ph=ImGui::GetWindowHeight();
      if (browserCurrentURL.empty()) {
        ImGui::SetCursorPos(ImVec2(pw*0.5f-100.0f*dpiScale,ph*0.4f));
        ImGui::TextColored(dimColor,"(by rednoobmusic)");
        ImGui::SetCursorPos(ImVec2(pw*0.5f-80.0f*dpiScale,ph*0.4f+ImGui::GetFrameHeightWithSpacing()));
        ImGui::TextColored(dimColor,"Enter a URL above and press Go");
      } else {
        ImGui::Spacing();
        ImGui::Indent(8.0f*dpiScale);
        ImGui::TextColored(uiColors[GUI_COLOR_ACCENT_PRIMARY],">> %s",browserCurrentURL.c_str());
        ImGui::Spacing();
        ImGui::TextColored(dimColor,"Page opened in system browser.");
      }
    }
    ImGui::EndChild();
    ImGui::PopStyleColor();
#endif

    if (!browserStatus.empty()) {
      ImGui::SetCursorPosY(ImGui::GetCursorPosY()+2.0f*dpiScale);
      ImGui::TextColored(dimColor,"%s",browserStatus.c_str());
    }
  }
  ImGui::End();
}
