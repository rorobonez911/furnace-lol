#include "gui.h"
#include <imgui.h>
#include <cmath>

void FurnaceGUI::applyRGBSync() {
  float h=(float)fmod(ImGui::GetTime()*0.1,1.0);

  auto c=[&](float s,float v,float a=1.0f)->ImVec4 {
    ImVec4 r; r.w=a;
    ImGui::ColorConvertHSVtoRGB(h,s,v,r.x,r.y,r.z);
    return r;
  };

  ImGuiStyle& sty=ImGui::GetStyle();

  sty.Colors[ImGuiCol_Button]                    =c(0.80f,0.65f);
  sty.Colors[ImGuiCol_ButtonHovered]             =c(0.75f,0.80f);
  sty.Colors[ImGuiCol_ButtonActive]              =c(0.85f,0.50f);

  sty.Colors[ImGuiCol_Tab]                       =c(0.80f,0.50f);
  sty.Colors[ImGuiCol_TabHovered]                =c(0.75f,0.75f);
  sty.Colors[ImGuiCol_TabSelected]               =c(0.80f,0.65f);
  sty.Colors[ImGuiCol_TabSelectedOverline]       =c(0.85f,0.90f);
  sty.Colors[ImGuiCol_TabDimmed]                 =c(0.55f,0.30f);
  sty.Colors[ImGuiCol_TabDimmedSelected]         =c(0.65f,0.45f);
  sty.Colors[ImGuiCol_TabDimmedSelectedOverline] =c(0.70f,0.55f);

  sty.Colors[ImGuiCol_Header]                    =c(0.70f,0.45f);
  sty.Colors[ImGuiCol_HeaderHovered]             =c(0.70f,0.60f);
  sty.Colors[ImGuiCol_HeaderActive]              =c(0.80f,0.75f);

  sty.Colors[ImGuiCol_FrameBg]                   =c(0.60f,0.28f);
  sty.Colors[ImGuiCol_FrameBgHovered]            =c(0.65f,0.42f);
  sty.Colors[ImGuiCol_FrameBgActive]             =c(0.75f,0.58f);

  sty.Colors[ImGuiCol_ResizeGrip]                =c(0.75f,0.48f);
  sty.Colors[ImGuiCol_ResizeGripHovered]         =c(0.80f,0.65f);
  sty.Colors[ImGuiCol_ResizeGripActive]          =c(0.90f,0.82f);

  sty.Colors[ImGuiCol_SliderGrab]                =c(0.85f,0.88f);
  sty.Colors[ImGuiCol_SliderGrabActive]          =c(0.90f,1.00f);

  sty.Colors[ImGuiCol_TitleBg]                   =c(0.75f,0.18f);
  sty.Colors[ImGuiCol_TitleBgActive]             =c(0.82f,0.52f);
  sty.Colors[ImGuiCol_TitleBgCollapsed]          =c(0.65f,0.13f);

  sty.Colors[ImGuiCol_MenuBarBg]                 =c(0.78f,0.16f);

  sty.Colors[ImGuiCol_ScrollbarGrab]             =c(0.72f,0.52f);
  sty.Colors[ImGuiCol_ScrollbarGrabHovered]      =c(0.75f,0.68f);
  sty.Colors[ImGuiCol_ScrollbarGrabActive]       =c(0.85f,0.82f);

  sty.Colors[ImGuiCol_Separator]                 =c(0.70f,0.52f);
  sty.Colors[ImGuiCol_SeparatorHovered]          =c(0.75f,0.68f);
  sty.Colors[ImGuiCol_SeparatorActive]           =c(0.85f,0.82f);

  sty.Colors[ImGuiCol_CheckMark]                 =c(0.88f,1.00f);
  sty.Colors[ImGuiCol_TextLink]                  =c(0.80f,0.88f);
  sty.Colors[ImGuiCol_TextSelectedBg]            =c(0.72f,0.42f,0.70f);

  sty.Colors[ImGuiCol_Border]                    =c(0.72f,0.58f);
  sty.Colors[ImGuiCol_BorderShadow]              =c(0.50f,0.08f,0.40f);

  sty.Colors[ImGuiCol_DockingPreview]            =c(0.75f,0.68f,0.70f);

  sty.Colors[ImGuiCol_TableHeaderBg]             =c(0.72f,0.32f);
  sty.Colors[ImGuiCol_TableBorderStrong]         =c(0.68f,0.52f);
  sty.Colors[ImGuiCol_TableBorderLight]          =c(0.55f,0.32f);

  uiColors[GUI_COLOR_ACCENT_PRIMARY]             =c(0.82f,0.72f);
  uiColors[GUI_COLOR_ACCENT_SECONDARY]           =c(0.68f,0.55f);

  uiColors[GUI_COLOR_PATTERN_ROW_INDEX]          =c(0.65f,1.00f);
  uiColors[GUI_COLOR_PATTERN_ROW_INDEX_HI1]      =c(0.55f,1.00f);
  uiColors[GUI_COLOR_PATTERN_ROW_INDEX_HI2]      =c(0.50f,0.95f);

  uiColors[GUI_COLOR_OSC_BG1]                   =c(0.85f,0.28f);
  uiColors[GUI_COLOR_OSC_BG2]                   =c(0.80f,0.22f);
  uiColors[GUI_COLOR_OSC_BG3]                   =c(0.90f,0.12f);
  uiColors[GUI_COLOR_OSC_BG4]                   =c(0.88f,0.08f);
  uiColors[GUI_COLOR_OSC_GUIDE]                  =c(0.72f,0.55f,0.13f);
  uiColors[GUI_COLOR_OSC_BORDER]                =c(0.75f,0.80f);
  uiColors[GUI_COLOR_OSC_WAVE]                  =c(0.55f,1.00f);

  uiColors[GUI_COLOR_ORDER_ROW_INDEX]            =c(0.60f,1.00f);
  uiColors[GUI_COLOR_ORDER_ACTIVE]               =c(0.82f,0.75f,0.30f);
  uiColors[GUI_COLOR_ORDER_SELECTED]             =c(0.78f,0.90f,0.80f);
  uiColors[GUI_COLOR_ORDER_SIMILAR]              =c(0.65f,0.85f,1.00f);
  uiColors[GUI_COLOR_SONG_LOOP]                  =c(0.80f,0.65f,0.40f);

  uiColors[GUI_COLOR_PIANO_ROLL_NOTE]            =c(0.85f,0.88f);
  uiColors[GUI_COLOR_PIANO_ROLL_NOTE_OFF]        =c(0.50f,0.80f);
  uiColors[GUI_COLOR_PIANO_ROLL_NOTE_REL]        =c(0.30f,0.90f);
  uiColors[GUI_COLOR_PIANO_ROLL_FX_VOL]         =c(0.75f,0.82f,0.90f);
  uiColors[GUI_COLOR_PIANO_ROLL_FX_NUM]         =c(0.55f,0.78f,0.90f);
  uiColors[GUI_COLOR_PIANO_ROLL_FX_VAL]         =c(0.65f,0.85f,0.90f);

  uiColors[GUI_COLOR_SPECTRUM_FILL]              =c(0.80f,0.65f,spectrum.fillAlpha);
  uiColors[GUI_COLOR_SPECTRUM_OUTLINE]           =c(0.70f,0.95f);
  uiColors[GUI_COLOR_SPECTRUM_PEAK]              =c(0.60f,1.00f);
}
