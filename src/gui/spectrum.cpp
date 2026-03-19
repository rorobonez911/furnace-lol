#include "gui.h"
#include "imgui_internal.h"
#include "IconsFontAwesome4.h"
#include <fftw3.h>
#include <math.h>

inline float scaleFuncLog(float x) {
  constexpr float base=100;
  return log((base-1)*x+1.0f)/log(base);
}

inline float scaleFuncDb(float y) {
  return log10(y)*20.0f/70.0f+1;
}

void FurnaceGUI::drawSpectrum() {
  if (nextWindow==GUI_WINDOW_SPECTRUM) {
    spectrumOpen=true;
    ImGui::SetNextWindowFocus();
    nextWindow=GUI_WINDOW_NOTHING;
  }
  if (!spectrumOpen) return;
  static int specFrame=0;
  bool doFFT=((++specFrame)%2==0);
  if (ImGui::Begin("Spectrum",&spectrumOpen,globalWinFlags,_("Spectrum"))) {
    if (ImGui::IsWindowCollapsed()) { ImGui::End(); return; }
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,ImVec2(0.0f,0.0f));
    ImDrawList* dl=ImGui::GetWindowDrawList();
    ImVec2 origin=ImGui::GetWindowPos(), size=ImGui::GetWindowSize();
    float titleBar=ImGui::GetCurrentWindow()->TitleBarHeight;
    origin.y+=titleBar;
    size.y-=titleBar;
    if (spectrum.showXScale) size.y-=ImGui::GetFontSize()+2.0f;
    if (spectrum.showYScale) {
      float padding=ImGui::GetStyle().FramePadding.x;
      ImVec2 p1,textSize;
      char buf[16];
      textSize=ImGui::CalcTextSize("-100");
      textSize.x+=5.f;
      int lineOffset=(int)(spectrum.yOffset*8);
      for (int z=lineOffset; z<=7+lineOffset; z++) {
        p1.y=origin.y+size.y*((z+1)/8.f-spectrum.yOffset)-textSize.y/2.0f;
        p1.x=origin.x+padding;
        snprintf(buf,16,"-%2d",(z+1)*10);
        dl->AddText(p1,ImGui::GetColorU32(ImGuiCol_Text),buf);
      }
      origin.x+=textSize.x+padding;
      size.x-=textSize.x+padding;
    }

    ImVec4 fillColorV=uiColors[GUI_COLOR_SPECTRUM_FILL];
    fillColorV.w=spectrum.fillAlpha;
    ImU32 fillColor=ImGui::ColorConvertFloat4ToU32(fillColorV);
    ImU32 fillColorBot=fillColor&0x00ffffff;
    ImU32 outlineColor=ImGui::GetColorU32(uiColors[GUI_COLOR_SPECTRUM_OUTLINE]);
    ImU32 peakColor=ImGui::GetColorU32(uiColors[GUI_COLOR_SPECTRUM_PEAK]);

    if (spectrum.showYGrid) {
      int lines=((size.y>450)?16:8);
      float offset=fmod(spectrum.yOffset*lines,1.0f)/lines;
      for (unsigned char z=1; z<=lines; z++) {
        dl->AddLine(
          origin+ImVec2(0,size.y*((float)z/lines-offset)),
          origin+ImVec2(size.x,size.y*((float)z/lines-offset)),
          0x22ffffff,dpiScale);
      }
    }
    if (spectrum.showXGrid||spectrum.showXScale) {
      char buf[16];
      float pos=0,prevPos=0;
      float maxRate=e->getAudioDescGot().rate/2.0f;
      for (size_t j=0; j<spectrum.frequencies.size(); j++) {
        float freq=(float)spectrum.frequencies[j];
        ImU32 color=((j%9)==0)?0x55ffffff:0x22ffffff;
        pos=spectrum.xZoom*size.x*(scaleFuncLog(freq/maxRate)-spectrum.xOffset);
        if (pos>size.x) break;
        if (spectrum.showXGrid) dl->AddLine(
          origin+ImVec2(pos,0),
          origin+ImVec2(pos,size.y),
          color,dpiScale);
        if (spectrum.showXScale) {
          if (freq>=1000) snprintf(buf,16,"%dk",(int)freq/1000);
          else snprintf(buf,16,"%d",(int)freq);
          float tw=ImGui::CalcTextSize(buf).x;
          if (pos-prevPos>tw||(j%9)==0) {
            dl->AddText(origin+ImVec2(pos-tw/2,size.y+2),ImGui::GetColorU32(ImGuiCol_Text),buf);
            prevPos=pos;
          }
        }
      }
    }

    int chans=e->getAudioDescGot().outChans;
    if (spectrum.update) {
      spectrum.update=false;
      spectrum.running=true;
      for (int i=0; i<DIV_MAX_OUTPUTS; i++) {
        if (spectrum.buffer[i]) { fftw_free(spectrum.buffer[i]); spectrum.buffer[i]=NULL; }
        if (spectrum.in[i]) { delete[] spectrum.in[i]; spectrum.in[i]=NULL; }
        if (spectrum.plan[i]) { fftw_free(spectrum.plan[i]); spectrum.plan[i]=NULL; }
        if (spectrum.plot[i]) { delete[] spectrum.plot[i]; spectrum.plot[i]=NULL; }
        if (spectrum.smooth[i]) { delete[] spectrum.smooth[i]; spectrum.smooth[i]=NULL; }
        if (spectrum.peak[i]) { delete[] spectrum.peak[i]; spectrum.peak[i]=NULL; }
      }
      for (int i=0; i<(spectrum.mono?1:chans); i++) {
        spectrum.buffer[i]=(fftw_complex*)fftw_malloc(sizeof(fftw_complex)*spectrum.bins);
        if (!spectrum.buffer[i]) spectrum.running=false;
        spectrum.in[i]=new double[spectrum.bins];
        if (!spectrum.in[i]) spectrum.running=false;
        spectrum.plan[i]=fftw_plan_dft_r2c_1d(spectrum.bins,spectrum.in[i],spectrum.buffer[i],FFTW_ESTIMATE);
        if (!spectrum.plan[i]) spectrum.running=false;
        spectrum.plot[i]=new ImVec2[spectrum.bins/2+2];
        if (!spectrum.plot[i]) spectrum.running=false;
        spectrum.smooth[i]=new float[spectrum.bins/2]();
        if (!spectrum.smooth[i]) spectrum.running=false;
        spectrum.peak[i]=new float[spectrum.bins/2]();
        if (!spectrum.peak[i]) spectrum.running=false;
      }
      spectrum.frequencies.clear();
      int maxRateI=e->getAudioDescGot().rate/2;
      for (int j=10; j<maxRateI; j*=10) {
        for (int i=1; i<10; i++) {
          float freq=(float)(i*j);
          if (freq>maxRateI) break;
          spectrum.frequencies.push_back((int)freq);
        }
        if (j*10>maxRateI) break;
      }
    }

    if (spectrum.running) {
      unsigned int count=spectrum.bins/2;
      float maxRate=e->getAudioDescGot().rate/2.0f;

      for (int z=spectrum.mono?0:(chans-1); z>=0; z--) {
        if (!spectrum.buffer[z]) {
          spectrum.buffer[z]=(fftw_complex*)fftw_malloc(sizeof(fftw_complex)*spectrum.bins);
          if (!spectrum.buffer[z]) { spectrum.running=false; break; }
        }
        if (!spectrum.in[z]) {
          spectrum.in[z]=new double[spectrum.bins];
          if (!spectrum.in[z]) { spectrum.running=false; break; }
        }
        if (!spectrum.plan[z]) {
          spectrum.plan[z]=fftw_plan_dft_r2c_1d(spectrum.bins,spectrum.in[z],spectrum.buffer[z],FFTW_ESTIMATE);
          if (!spectrum.plan[z]) { spectrum.running=false; break; }
        }
        if (!spectrum.plot[z]) {
          spectrum.plot[z]=new ImVec2[count+2];
          if (!spectrum.plot[z]) { spectrum.running=false; break; }
        }
        if (!spectrum.smooth[z]) {
          spectrum.smooth[z]=new float[count]();
          if (!spectrum.smooth[z]) { spectrum.running=false; break; }
        }
        if (!spectrum.peak[z]) {
          spectrum.peak[z]=new float[count]();
          if (!spectrum.peak[z]) { spectrum.running=false; break; }
        }

        if (doFFT) {
          memset(spectrum.in[z],0,sizeof(double)*spectrum.bins);
          int needle=e->oscReadPos-spectrum.bins;
          for (int j=0; j<spectrum.bins; j++) {
            int pos=(needle+j)&0x7fff;
            double sample=0.0;
            if (spectrum.mono) {
              for (int i=0; i<chans; i++) sample+=e->oscBuf[i][pos];
              sample/=chans;
            } else {
              sample=e->oscBuf[z][pos];
            }
            spectrum.in[z][j]=sample*(0.5*(1.0-cos(2.0*M_PI*j/(spectrum.bins-1))));
          }
          fftw_execute(spectrum.plan[z]);

          for (unsigned int i=1; i<count; i++) {
            double mag=2.0*sqrt(spectrum.buffer[z][i][0]*spectrum.buffer[z][i][0]+spectrum.buffer[z][i][1]*spectrum.buffer[z][i][1])/count;
            if (spectrum.slope!=0.0f) {
              float freqHz=fmaxf((float)i/count*maxRate,20.0f);
              float gainDb=spectrum.slope*log2f(freqHz/1000.0f);
              mag*=(double)powf(10.0f,gainDb/20.0f);
            }
            float raw=(mag>1e-9f)?(1.0f-(float)scaleFuncDb((float)mag)):1.0f;
            if (raw>spectrum.noiseFloor) raw=1.0f;
            if (raw<spectrum.smooth[z][i]) {
              spectrum.smooth[z][i]=raw;
            } else {
              spectrum.smooth[z][i]=spectrum.smooth[z][i]*spectrum.fallSpeed+raw*(1.0f-spectrum.fallSpeed);
            }
            if (raw<spectrum.peak[z][i]) {
              spectrum.peak[z][i]=raw;
            } else {
              spectrum.peak[z][i]=spectrum.peak[z][i]*spectrum.peakDecay+raw*(1.0f-spectrum.peakDecay);
            }
          }
        }

        ImGui::PushClipRect(origin,origin+size,true);

        unsigned int pts=0;
        for (unsigned int i=0; i<count; i++) {
          float x=spectrum.xZoom*size.x*(scaleFuncLog((float)i/count)-spectrum.xOffset);
          if (x<0||x>size.x) continue;
          float sy=spectrum.smooth[z][i]-spectrum.yOffset;
          float py=fmaxf(0.0f,fminf(1.0f,sy));
          spectrum.plot[z][pts++]=ImVec2(origin.x+x,origin.y+size.y*py);
        }

        if (pts>1) {
          if (spectrum.gradientFill) {
            ImVec2 uv=ImGui::GetDrawListSharedData()->TexUvWhitePixel;
            dl->PrimReserve((int)(pts-1)*6,(int)(pts-1)*4);
            for (unsigned int i=0; i<pts-1; i++) {
              ImDrawIdx idx=(ImDrawIdx)dl->_VtxCurrentIdx;
              dl->_IdxWritePtr[0]=idx;  dl->_IdxWritePtr[1]=(ImDrawIdx)(idx+1); dl->_IdxWritePtr[2]=(ImDrawIdx)(idx+2);
              dl->_IdxWritePtr[3]=idx;  dl->_IdxWritePtr[4]=(ImDrawIdx)(idx+2); dl->_IdxWritePtr[5]=(ImDrawIdx)(idx+3);
              dl->_IdxWritePtr+=6;
              dl->_VtxWritePtr[0].pos=spectrum.plot[z][i];   dl->_VtxWritePtr[0].uv=uv; dl->_VtxWritePtr[0].col=fillColor;
              dl->_VtxWritePtr[1].pos=spectrum.plot[z][i+1]; dl->_VtxWritePtr[1].uv=uv; dl->_VtxWritePtr[1].col=fillColor;
              dl->_VtxWritePtr[2].pos=ImVec2(spectrum.plot[z][i+1].x,origin.y+size.y); dl->_VtxWritePtr[2].uv=uv; dl->_VtxWritePtr[2].col=fillColorBot;
              dl->_VtxWritePtr[3].pos=ImVec2(spectrum.plot[z][i].x,  origin.y+size.y); dl->_VtxWritePtr[3].uv=uv; dl->_VtxWritePtr[3].col=fillColorBot;
              dl->_VtxWritePtr+=4;
              dl->_VtxCurrentIdx+=4;
            }
          } else {
            dl->PathLineTo(ImVec2(spectrum.plot[z][0].x,origin.y+size.y));
            for (unsigned int i=0; i<pts; i++) dl->PathLineTo(spectrum.plot[z][i]);
            dl->PathLineTo(ImVec2(spectrum.plot[z][pts-1].x,origin.y+size.y));
            dl->PathFillConcave(fillColor);
          }
          dl->AddPolyline(spectrum.plot[z],(int)pts,outlineColor,0,dpiScale);
        }

        float prevLabelX=-999.0f;
        for (unsigned int i=1; i<count-1; i++) {
          float x=spectrum.xZoom*size.x*(scaleFuncLog((float)i/count)-spectrum.xOffset);
          if (x<0||x>size.x) continue;
          float py=spectrum.peak[z][i]-spectrum.yOffset;
          py=fmaxf(0.0f,fminf(1.0f,py));
          if (py>spectrum.peakThreshold) continue;
          float gap=spectrum.smooth[z][i]-spectrum.peak[z][i];
          if (gap<spectrum.peakMinGap) continue;
          float px=origin.x+x;
          float pyw=origin.y+size.y*py;
          dl->AddCircleFilled(ImVec2(px,pyw),dpiScale*spectrum.peakDotSize,peakColor);
          if (spectrum.showPeakLabels&&px-prevLabelX>36.0f) {
            char label[16];
            if (spectrum.peakLabelMode==1) {
              float freqHz=(float)i/count*maxRate;
              if (freqHz>=1000.0f) snprintf(label,sizeof(label),"%.1fk",freqHz/1000.0f);
              else snprintf(label,sizeof(label),"%.0f",freqHz);
            } else {
              float dbVal=-spectrum.peak[z][i]*70.0f;
              snprintf(label,sizeof(label),"%.0fdB",dbVal);
            }
            float tw=ImGui::CalcTextSize(label).x;
            dl->AddText(ImVec2(px-tw/2,pyw-ImGui::GetFontSize()-1.0f),peakColor,label);
            prevLabelX=px;
          }
        }

        ImGui::PopClipRect();
      }
    }

    ImGui::PopStyleVar();

    if (ImGui::IsWindowHovered()) {
      ImGui::SetCursorPosX(ImGui::GetWindowSize().x-ImGui::GetStyle().ItemSpacing.x-ImGui::CalcTextSize(ICON_FA_BARS).x);
      ImGui::TextUnformatted(ICON_FA_BARS "##spectrumSettings");
      if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
        ImGui::SetTooltip(_("Spectrum settings"));
      }
    }
    if (ImGui::BeginPopupContextItem("spectrumSettingsPopup",ImGuiPopupFlags_MouseButtonLeft)) {
      if (ImGui::Checkbox(_("Mono##spec"),&spectrum.mono)) {
        spectrum.update=true;
      }
      if (ImGui::InputScalar(_("Bins##spec"),ImGuiDataType_U32,&spectrum.bins)) {
        if (spectrum.bins<32) spectrum.bins=32;
        if (spectrum.bins>32768) spectrum.bins=32768;
        spectrum.update=true;
      }
      ImGui::Separator();
      ImGui::Text(_("Display"));
      if (ImGui::SliderFloat(_("X Zoom##spec"),&spectrum.xZoom,1.0f,10.0f)) {
        spectrum.xZoom=fmaxf(1.0f,fminf(10.0f,spectrum.xZoom));
      }
      if (ImGui::SliderFloat(_("X Offset##spec"),&spectrum.xOffset,0.0f,1.0f)) {
        spectrum.xOffset=fmaxf(0.0f,fminf(1.0f,spectrum.xOffset));
      }
      if (ImGui::SliderFloat(_("Y Offset##spec"),&spectrum.yOffset,0.0f,1.0f)) {
        spectrum.yOffset=fmaxf(0.0f,fminf(1.0f,spectrum.yOffset));
      }
      if (ImGui::SliderFloat(_("Slope (dB/oct)##spec"),&spectrum.slope,-6.0f,6.0f)) {
        spectrum.slope=fmaxf(-6.0f,fminf(6.0f,spectrum.slope));
      }
      if (ImGui::IsItemHovered()) ImGui::SetTooltip(_("+3 = pink noise, 0 = flat, -3 = blue noise"));
      if (ImGui::SliderFloat(_("Noise floor##spec"),&spectrum.noiseFloor,0.4f,1.0f)) {
        spectrum.noiseFloor=fmaxf(0.4f,fminf(1.0f,spectrum.noiseFloor));
      }
      ImGui::Separator();
      ImGui::Text(_("Fill"));
      ImGui::Checkbox(_("Gradient fill##spec"),&spectrum.gradientFill);
      if (ImGui::SliderFloat(_("Fill alpha##spec"),&spectrum.fillAlpha,0.0f,1.0f)) {
        spectrum.fillAlpha=fmaxf(0.0f,fminf(1.0f,spectrum.fillAlpha));
      }
      ImGui::Separator();
      ImGui::Text(_("Smoothing"));
      if (ImGui::SliderFloat(_("Fall speed##spec"),&spectrum.fallSpeed,0.5f,0.999f)) {
        spectrum.fallSpeed=fmaxf(0.5f,fminf(0.999f,spectrum.fallSpeed));
      }
      ImGui::Separator();
      ImGui::Text(_("Peaks"));
      if (ImGui::SliderFloat(_("Peak decay##spec"),&spectrum.peakDecay,0.9f,0.9999f)) {
        spectrum.peakDecay=fmaxf(0.9f,fminf(0.9999f,spectrum.peakDecay));
      }
      if (ImGui::SliderFloat(_("Peak threshold##spec"),&spectrum.peakThreshold,0.0f,1.0f)) {
        spectrum.peakThreshold=fmaxf(0.0f,fminf(1.0f,spectrum.peakThreshold));
      }
      if (ImGui::IsItemHovered()) ImGui::SetTooltip(_("Only show peaks louder than this (0=always, 1=never)"));
      if (ImGui::SliderFloat(_("Peak min gap##spec"),&spectrum.peakMinGap,0.0f,0.1f,"%.4f")) {
        spectrum.peakMinGap=fmaxf(0.0f,fminf(0.1f,spectrum.peakMinGap));
      }
      if (ImGui::IsItemHovered()) ImGui::SetTooltip(_("Min gap between bar and peak dot to show dot"));
      if (ImGui::SliderFloat(_("Peak dot size##spec"),&spectrum.peakDotSize,0.5f,6.0f)) {
        spectrum.peakDotSize=fmaxf(0.5f,fminf(6.0f,spectrum.peakDotSize));
      }
      ImGui::Checkbox(_("Show peak labels##spec"),&spectrum.showPeakLabels);
      if (spectrum.showPeakLabels) {
        ImGui::SameLine();
        ImGui::SetNextItemWidth(80.0f*dpiScale);
        const char* modes[]={"dB","Hz"};
        ImGui::Combo("##peakLabelMode",&spectrum.peakLabelMode,modes,2);
      }
      ImGui::Separator();
      ImGui::Text(_("Grid / Scale"));
      ImGui::Checkbox(_("Show X Grid##spec"),&spectrum.showXGrid);
      ImGui::Checkbox(_("Show Y Grid##spec"),&spectrum.showYGrid);
      ImGui::Checkbox(_("Show X Scale##spec"),&spectrum.showXScale);
      ImGui::Checkbox(_("Show Y Scale##spec"),&spectrum.showYScale);
      ImGui::EndPopup();
    }
  }
  if (ImGui::IsWindowFocused(ImGuiFocusedFlags_ChildWindows)) curWindow=GUI_WINDOW_SPECTRUM;
  ImGui::End();
}
