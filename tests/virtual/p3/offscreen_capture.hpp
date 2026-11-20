#pragma once
// Offscreen window capture for the GUI acceptance exporters.
//
// The P2 exporter used bare PrintWindow(PW_CLIENTONLY) on the teaching panel:
// that renders only the panel's own client area, so the native Win32 child
// controls (wxChoice / wxButton / wxTextCtrl) stayed blank in the archived
// PNGs. This helper renders the DWM content surface with
// PW_RENDERFULLCONTENT (which includes every child) and keeps a recursive
// WM_PRINT / WM_PRINTCLIENT composite as the fallback path.
#include <wx/bitmap.h>
#include <wx/dcmemory.h>
#include <wx/image.h>
#include <wx/msw/wrapwin.h>
#include <set>
#ifndef PW_RENDERFULLCONTENT
#define PW_RENDERFULLCONTENT 0x00000002
#endif
namespace sigflow_acceptance {
inline bool PrintFullContent(HWND window,HDC dc){
    return ::PrintWindow(window,dc,PW_CLIENTONLY|PW_RENDERFULLCONTENT)!=0;
}
inline void PrintRecursive(HWND window,HDC dc){
    ::SendMessage(window,WM_PRINT,WPARAM(dc),LPARAM(PRF_CLIENT|PRF_ERASEBKGND));
    HWND child=::GetWindow(window,GW_CHILD);
    while(child){
        if(::IsWindowVisible(child)){
            RECT area;::GetWindowRect(child,&area);POINT origin{area.left,area.top};::ScreenToClient(window,&origin);
            int saved=::SaveDC(dc);
            POINT previous;::SetWindowOrgEx(dc,-origin.x,-origin.y,&previous);
            PrintRecursive(child,dc);
            ::RestoreDC(dc,saved);
        }
        child=::GetWindow(child,GW_HWNDNEXT);
    }
}
inline bool CaptureWindow(wxWindow* window,wxBitmap& out){
    if(!window)return false;
    const auto size=window->GetClientSize();
    if(size.x<=0 || size.y<=0)return false;
    wxBitmap bitmap(size.x,size.y);
    wxMemoryDC dc(bitmap);
    dc.SetBackground(window->GetBackgroundColour());
    dc.Clear();
    auto hwnd=reinterpret_cast<HWND>(window->GetHandle());
    auto hdc=static_cast<HDC>(dc.GetHDC());
    bool ok=PrintFullContent(hwnd,hdc);
    if(!ok)PrintRecursive(hwnd,hdc);
    dc.SelectObject(wxNullBitmap);
    out=bitmap;
    return ok;
}
inline size_t DistinctColors(const wxBitmap& bitmap){
    wxImage image=bitmap.ConvertToImage();
    if(!image.IsOk())return 0;
    const auto* data=image.GetData();
    const size_t pixels=size_t(image.GetWidth())*size_t(image.GetHeight());
    if(!data || !pixels)return 0;
    const size_t stride=pixels/997+1;std::set<unsigned> colors;
    for(size_t i=0;i<pixels;i+=stride)
        colors.insert((unsigned(data[i*3])<<16)|(unsigned(data[i*3+1])<<8)|unsigned(data[i*3+2]));
    return colors.size();
}
inline size_t DistinctColorsInRegion(const wxBitmap& bitmap,const wxRect& region){
    wxImage image=bitmap.ConvertToImage();
    if(!image.IsOk() || image.GetWidth()<=0 || image.GetHeight()<=0)return 0;
    int x=std::clamp(region.x,0,image.GetWidth()-1),y=std::clamp(region.y,0,image.GetHeight()-1);
    int w=std::clamp(region.width,0,image.GetWidth()-x),h=std::clamp(region.height,0,image.GetHeight()-y);
    if(w<=0 || h<=0)return 0;
    const auto* data=image.GetData();std::set<unsigned> colors;
    for(int row=0;row<h;++row)for(int column=0;column<w;++column){
        const auto* pixel=data+((size_t(y+row)*image.GetWidth()+size_t(x+column))*3);
        colors.insert((unsigned(pixel[0])<<16)|(unsigned(pixel[1])<<8)|unsigned(pixel[2]));
    }
    return colors.size();
}
}
