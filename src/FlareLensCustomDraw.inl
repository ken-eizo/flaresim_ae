// Compact monochrome AE Effect Controls optical prescription editor.
// Per-event host Drawbot lifetime; no GDI/Vulkan backing resources.
PF_Err draw(PF_InData* in,PF_OutData* out,const Document& doc,PF_EventExtra* e){
 DRAWBOT_Suites db{};
 PF_Err err=AEFX_AcquireDrawbotSuites(in,out,&db);
 if(err)return err;
 if(!db.drawbot_suiteP||!db.supplier_suiteP||!db.surface_suiteP||
    !db.path_suiteP){
  AEFX_ReleaseDrawbotSuites(in,out);return PF_Err_BAD_CALLBACK_PARAM;
 }
 PF_EffectCustomUISuite1* custom=nullptr;
 DRAWBOT_DrawRef dr=nullptr;
 err=AEFX_AcquireSuite(in,out,kPFEffectCustomUISuite,
                       kPFEffectCustomUISuiteVersion1,nullptr,(void**)&custom);
 if(!err&&custom&&custom->PF_GetDrawingReference)
  err=custom->PF_GetDrawingReference(e->contextH,&dr);
 if(custom)AEFX_ReleaseSuite(in,out,kPFEffectCustomUISuite,
                             kPFEffectCustomUISuiteVersion1,nullptr);
 if(err||!dr){
  AEFX_ReleaseDrawbotSuites(in,out);return err?err:PF_Err_BAD_CALLBACK_PARAM;
 }
 DRAWBOT_SupplierRef sup=nullptr;
 DRAWBOT_SurfaceRef surf=nullptr;
 if(!err)err=db.drawbot_suiteP->GetSupplier(dr,&sup);
 if(!err)err=db.drawbot_suiteP->GetSurface(dr,&surf);
 if(err||!sup||!surf){
  AEFX_ReleaseDrawbotSuites(in,out);return err?err:PF_Err_BAD_CALLBACK_PARAM;
 }
 const auto& r=e->window_union.effect_win.current_frame;
 const float x=float(r.left),y=float(r.top);
 const float w=float(r.right-r.left),h=float(r.bottom-r.top);
 if(w<125||h<80||w>8192||h>8192){
  AEFX_ReleaseDrawbotSuites(in,out);return PF_Err_NONE;
 }
 const auto bg=DRAWBOT_ColorRGBA{.123f,.123f,.123f,1.f};
 const auto muted=DRAWBOT_ColorRGBA{.62f,.62f,.62f,1.f};
 const auto ink=DRAWBOT_ColorRGBA{.90f,.90f,.90f,1.f};
 const auto subtle=DRAWBOT_ColorRGBA{.29f,.29f,.29f,1.f};
 const auto boundary=DRAWBOT_ColorRGBA{.70f,.70f,.70f,1.f};
 const auto fillcolor=DRAWBOT_ColorRGBA{.70f,.70f,.70f,.17f};
 const auto selectedfill=DRAWBOT_ColorRGBA{.9f,.9f,.9f,.26f};
 auto box=[&](float px,float py,float ww,float hh,const DRAWBOT_ColorRGBA& c){
  if(err||ww<=0||hh<=0)return;
  const DRAWBOT_RectF32 rect{px,py,ww,hh};
  err=db.surface_suiteP->PaintRect(surf,&c,&rect);
 };
 box(x,y,w,h,bg);
 // Drawbot pen, font and brush lifetimes are scoped to the single DRAW.
 std::array<DRAWBOT_PenRef,3> pens{};
 std::array<DRAWBOT_BrushRef,4> brushes{};
 DRAWBOT_FontRef font=nullptr;
 auto release=[&](DRAWBOT_ObjectRef p){
  if(p)db.supplier_suiteP->ReleaseObject(p);
 };
 const DRAWBOT_ColorRGBA pen_colors[3]={subtle,boundary,ink};
 for(int i=0;i<3;i++)
  if(!err)err=db.supplier_suiteP->NewPen(
   sup,&pen_colors[i],i==2?1.5f:1.f,&pens[i]);
 const DRAWBOT_ColorRGBA brush_colors[4]={fillcolor,selectedfill,ink,muted};
 for(int i=0;i<4;i++)
  if(!err)err=db.supplier_suiteP->NewBrush(sup,&brush_colors[i],&brushes[i]);
 if(!err)err=db.supplier_suiteP->NewDefaultFont(sup,10.f,&font);
 auto pathcall=[&](const auto& path_fn,const auto& submit){
  if(err)return;
  DRAWBOT_PathRef path=nullptr;
  err=db.supplier_suiteP->NewPath(sup,&path);
  if(!err)path_fn(path);
  if(!err)err=submit(path);
  release(reinterpret_cast<DRAWBOT_ObjectRef>(path));
 };
 auto stroke=[&](DRAWBOT_PenRef pen,const auto& fn){
  if(!pen)return;
  pathcall(fn,[&](DRAWBOT_PathRef p){
   return db.surface_suiteP->StrokePath(surf,pen,p);
  });
 };
 auto fill=[&](DRAWBOT_BrushRef b,const auto& fn){
  if(!b)return;
  pathcall(fn,[&](DRAWBOT_PathRef p){
   return db.surface_suiteP->FillPath(surf,b,p,kDRAWBOT_FillType_Default);
  });
 };
 auto label=[&](float px,float py,const char* s,bool secondary=false){
  if(err||!font||!brushes[2]||!brushes[3])return;
  DRAWBOT_UTF16Char text[120]{};
  for(size_t i=0;i<119&&s[i];i++)text[i]=DRAWBOT_UTF16Char(static_cast<unsigned char>(s[i]));
  const DRAWBOT_PointF32 origin{px,py};
  err=db.surface_suiteP->DrawString(
    surf,brushes[secondary?3:2],font,text,&origin,
    kDRAWBOT_TextAlignment_Default,kDRAWBOT_TextTruncation_None,0.f);
 };
 const float right=x+w;
 if(!valid(doc)){
  label(x+9,y+19,"Lens data unavailable");
 }else{
  const LensSystem lens=make_lens(doc);
  const Layout l(r,lens);
  char buf[64]{};
  std::snprintf(buf,sizeof(buf),"Surface  %u / %u",doc.selected+1,doc.count);
  label(x+9,y+16,buf);
  // Four 21x19 controls: previous, next, add, remove.
  constexpr const char* buttons[4]={"<",">","+","-"};
  for(int i=0;i<4;i++){
   const float bx=right-113.f+float(i)*28.f;
   box(bx,y+2,22,19,subtle);
   label(bx+8,y+15,buttons[i]);
  }
  const float axis_y=l.cy;
  stroke(pens[0],[&](DRAWBOT_PathRef p){
   db.path_suiteP->MoveTo(p,x+9,axis_y);
   db.path_suiteP->LineTo(p,right-9,axis_y);
  });
  // Shade only optically transmissive glass gaps; extremely thin gaps
  // are skipped rather than generating subpixel polygons.
  for(int i=0;i<lens.num_surfaces()-1&&!err;i++){
   const auto& a=lens.surfaces[size_t(i)];
   const auto& b=lens.surfaces[size_t(i+1)];
   if(a.ior<=1.03f||a.is_stop||std::abs(l.x(b.z)-l.x(a.z))<1.f)continue;
   fill(i==int(doc.selected)?brushes[1]:brushes[0],[&](DRAWBOT_PathRef p){
    constexpr int n=14;
    for(int j=0;j<=n;j++){
     const float hh=(float(j)*2.f/n-1.f)*a.semi_aperture;
     const float px=l.x(a.z+sag(a,hh)),py=l.y(hh);
     if(j==0)db.path_suiteP->MoveTo(p,px,py);
     else db.path_suiteP->LineTo(p,px,py);
    }
    for(int j=n;j>=0;j--){
     const float hh=(float(j)*2.f/n-1.f)*b.semi_aperture;
     db.path_suiteP->LineTo(p,l.x(b.z+sag(b,hh)),l.y(hh));
    }
    db.path_suiteP->Close(p);
   });
  }
  // All unselected surfaces in one Drawbot Path rather than allocating
  // a new path for every line (up to 64 surfaces).
  auto faces=[&](DRAWBOT_PathRef p,bool selected){
   for(int i=0;i<lens.num_surfaces();i++){
    if((i==int(doc.selected))!=selected)continue;
    const auto& s=lens.surfaces[size_t(i)];
    constexpr int steps=18;
    for(int j=0;j<=steps;j++){
     const float hh=s.semi_aperture*(float(j)*2.f/steps-1.f);
     const float xx=l.x(s.z+sag(s,hh)),yy=l.y(hh);
     if(j==0)db.path_suiteP->MoveTo(p,xx,yy);
     else db.path_suiteP->LineTo(p,xx,yy);
    }
   }
  };
  stroke(pens[1],[&](DRAWBOT_PathRef p){faces(p,false);});
  stroke(pens[2],[&](DRAWBOT_PathRef p){faces(p,true);});
  // Stop plane and sensor are graphical geometry, not decoration.
  stroke(pens[1],[&](DRAWBOT_PathRef p){
   for(const auto& s:lens.surfaces)if(s.is_stop){
    const float px=l.x(s.z);
    db.path_suiteP->MoveTo(p,px,axis_y-6);
    db.path_suiteP->LineTo(p,px,axis_y+6);
   }
   const float sx=l.x(lens.sensor_z);
   db.path_suiteP->MoveTo(p,sx,y+33);
   db.path_suiteP->LineTo(p,sx,y+128);
  });
  const auto& focus=lens.surfaces[size_t(doc.selected)];
  box(l.x(focus.z)-2.f,axis_y-2.f,4,4,ink);
  box(x+8,y+141,w-16,1.f,subtle);
  label(x+9,y+155,"Lens surface",true);
  label(right-69,y+155,"mm",true);
  const auto& s=doc.elements[doc.selected];
  char values[8][28]{};
  std::snprintf(values[0],28,"%.2f",s.radius);
  std::snprintf(values[1],28,"%.3f",s.thickness);
  std::snprintf(values[2],28,"%.2f",s.semi*2.f);
  std::snprintf(values[3],28,"%.4f",s.ior);
  std::snprintf(values[4],28,"%.1f",s.abbe);
  std::snprintf(values[5],28,"%d",s.coating);
  std::snprintf(values[6],28,"%s",s.stop?"On":"Off");
  std::snprintf(values[7],28,"%.1f",doc.focal);
  constexpr const char* labels[8]={
   "Radius","Spacing","Diameter","Refr. index",
   "Abbe","Coating","Stop","Focal length"};
  const float t0=l.track_start(),t1=std::max(t0+8.f,l.track_end());
  for(int row=0;row<8;row++){
   const float ry=y+kRowsStart+float(row)*kRowStep;
   label(x+11,ry+13,labels[row],true);
   // Narrow AE-like track with visible level and a 4px thumb.
   const float frac=std::clamp(track_fraction(kModes[row],doc),0.f,1.f);
   const float tx=t0+frac*(t1-t0);
   const float line_y=ry+9;
   box(t0,line_y-1,t1-t0,2,subtle);
   if(row!=6)box(t0,line_y-1,std::max(1.f,tx-t0),2,muted);
   box(tx-2.f,ry+4,4,10,ink);
   label(right-78,ry+13,values[row]);
  }
 }
 for(auto p:pens)release(reinterpret_cast<DRAWBOT_ObjectRef>(p));
 for(auto p:brushes)release(reinterpret_cast<DRAWBOT_ObjectRef>(p));
 release(reinterpret_cast<DRAWBOT_ObjectRef>(font));
 const PF_Err release_err=AEFX_ReleaseDrawbotSuites(in,out);
 if(!err)err=release_err;
 if(!err)e->evt_out_flags|=PF_EO_HANDLED_EVENT;
 return err;
}
