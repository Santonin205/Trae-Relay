#include "window/WindowController.h"
#include "ui/Dpi.h"
#include <algorithm>
#include <ctime>

namespace ui {
using namespace visual;
namespace {
struct CredentialTableLayout {
    static constexpr int top = 276, rowHeight = 48, contentHeight = 46;
    int edge[6];
    explicit CredentialTableLayout(int width) {
        const int flexible = width - 120 - 164;
        edge[0] = 60;
        edge[1] = edge[0] + flexible * 32 / 100;
        edge[2] = edge[0] + flexible * 66 / 100;
        edge[3] = edge[0] + flexible * 83 / 100;
        edge[4] = width - 60 - 164;
        edge[5] = width - 60;
    }
    R cell(int column, int y, int height) const { return {edge[column], y, edge[column+1]-edge[column], height}; }
};
std::wstring dateText(long long value) {if(value<=0)return L"未查询";time_t t=(time_t)value;tm local{};localtime_s(&local,&t);wchar_t b[40];wcsftime(b,40,L"%Y-%m-%d %H:%M",&local);return b;}
std::string utf8(const std::wstring& text){int n=WideCharToMultiByte(CP_UTF8,0,text.data(),(int)text.size(),nullptr,0,nullptr,nullptr);std::string out(n,0);WideCharToMultiByte(CP_UTF8,0,text.data(),(int)text.size(),out.data(),n,nullptr,nullptr);return out;}
}
void WindowController::createCredentialsControls(){
    makeControl(pageCredentials_,L"加载默认目录凭据",WS_VISIBLE|BS_OWNERDRAW|BS_NOTIFY,IDC_CRED_DEFAULT);
    makeControl(pageCredentials_,L"使用网页登录凭据",WS_VISIBLE|BS_OWNERDRAW|BS_NOTIFY,IDC_CRED_WEB);
    makeControl(pageCredentials_,L"网页登录",WS_VISIBLE|BS_OWNERDRAW|BS_NOTIFY,IDC_CRED_LOGIN);
    makeControl(pageCredentials_,L"刷新积分",WS_VISIBLE|BS_OWNERDRAW|BS_NOTIFY,IDC_CRED_REFRESH);
    makeControl(pageCredentials_,L"上一页",WS_VISIBLE|BS_OWNERDRAW|BS_NOTIFY,IDC_CRED_PREV);
    makeControl(pageCredentials_,L"下一页",WS_VISIBLE|BS_OWNERDRAW|BS_NOTIFY,IDC_CRED_NEXT);
    for(int i=0;i<7;++i)makeControl(pageCredentials_,L"",WS_VISIBLE|BS_OWNERDRAW|BS_NOTIFY,IDC_CRED_SLOT0+i);
    for(int i=0;i<CRED_PAGE_SIZE;++i){
        auto name=makeControl(pageCredentials_,L"",WS_VISIBLE|ES_AUTOHSCROLL|ES_CENTER,IDC_CRED_NAME+i);SendMessageW(name,EM_SETLIMITTEXT,60,0);
        makeControl(pageCredentials_,L"启用",WS_VISIBLE|BS_OWNERDRAW|BS_NOTIFY,IDC_CRED_ENABLE+i);
        makeControl(pageCredentials_,L"删除",WS_VISIBLE|BS_OWNERDRAW|BS_NOTIFY,IDC_CRED_DELETE+i);
    }
    refreshCredentialsPage();
}
void WindowController::layoutCredentialsPage(){
    if(!pageCredentials_)return;int w=logicalWidth(pageCredentials_),h=logicalHeight(pageCredentials_);
    place(pageCredentials_,GetDlgItem(pageCredentials_,IDC_CRED_DEFAULT),{60,92,230,32});
    place(pageCredentials_,GetDlgItem(pageCredentials_,IDC_CRED_WEB),{60,132,230,32});
    place(pageCredentials_,GetDlgItem(pageCredentials_,IDC_CRED_LOGIN),{60,180,138,32});
    place(pageCredentials_,GetDlgItem(pageCredentials_,IDC_CRED_REFRESH),{210,180,126,32});
    const CredentialTableLayout table(w);
    for(int i=0;i<CRED_PAGE_SIZE;++i){
        const int y=CredentialTableLayout::top+i*CredentialTableLayout::rowHeight;
        HWND name=GetDlgItem(pageCredentials_,IDC_CRED_NAME+i);
        HDC dc=GetDC(name);auto old=SelectObject(dc,reinterpret_cast<HFONT>(SendMessageW(name,WM_GETFONT,0,0)));
        TEXTMETRICW metrics{};GetTextMetricsW(dc,&metrics);SelectObject(dc,old);ReleaseDC(name,dc);
        const int nameHeight=MulDiv(metrics.tmHeight,96,dpi::forWindow(name));
        place(pageCredentials_,name,{table.edge[0]+12,y+(CredentialTableLayout::contentHeight-nameHeight)/2,table.edge[1]-table.edge[0]-24,nameHeight});
        const int actionX=table.edge[4]+(table.edge[5]-table.edge[4]-138)/2;
        place(pageCredentials_,GetDlgItem(pageCredentials_,IDC_CRED_ENABLE+i),{actionX,y+8,64,30});
        place(pageCredentials_,GetDlgItem(pageCredentials_,IDC_CRED_DELETE+i),{actionX+74,y+8,64,30});
    }
    int pages=std::max(1,((int)credentialRows_.size()+CRED_PAGE_SIZE-1)/CRED_PAGE_SIZE);
    UsagePagerLayout pager{pageCredentials_,credentialPage_,pages,w,h,dpi::forWindow(pageCredentials_),credentials::source()!="web"||credentialRows_.empty(),credentialPage_+1<pages};
    credentialPagerX_=layoutPager(pager,IDC_CRED_PREV,IDC_CRED_NEXT,IDC_CRED_SLOT0,credentialSlotPage_);
}
void WindowController::refreshCredentialsPage(){
    if(!pageCredentials_)return;auto list=credentials::rows();bool editing=false;for(int i=0;i<CRED_PAGE_SIZE;++i)if(GetFocus()==GetDlgItem(pageCredentials_,IDC_CRED_NAME+i))editing=true;
    if(!editing)credentialRows_=list;credentialPage_=std::clamp(credentialPage_,0,std::max(0,((int)credentialRows_.size()-1)/CRED_PAGE_SIZE));
    bool defaults=credentials::source()!="web",working=credentialWorking_||credentials::busy();
    setChecked(IDC_CRED_DEFAULT,defaults);setChecked(IDC_CRED_WEB,!defaults);
    ShowWindow(GetDlgItem(pageCredentials_,IDC_CRED_LOGIN),defaults?SW_HIDE:SW_SHOW);
    ShowWindow(GetDlgItem(pageCredentials_,IDC_CRED_REFRESH),defaults?SW_HIDE:SW_SHOW);
    for(int id:{IDC_CRED_DEFAULT,IDC_CRED_WEB,IDC_CRED_LOGIN,IDC_CRED_REFRESH})enableControl(GetDlgItem(pageCredentials_,id),!working);
    setControlText(GetDlgItem(pageCredentials_,IDC_CRED_LOGIN),credentials::busy()?L"等待登录…":L"网页登录");
    setControlText(GetDlgItem(pageCredentials_,IDC_CRED_REFRESH),credentialWorking_?L"处理中…":L"刷新积分");
    for(int i=0;i<CRED_PAGE_SIZE;++i){int idx=credentialPage_*CRED_PAGE_SIZE+i;bool exists=!defaults&&idx<(int)credentialRows_.size();
        for(int base:{IDC_CRED_NAME,IDC_CRED_ENABLE,IDC_CRED_DELETE})ShowWindow(GetDlgItem(pageCredentials_,base+i),exists?SW_SHOW:SW_HIDE);
        if(!exists)continue;auto&r=credentialRows_[idx];auto name=GetDlgItem(pageCredentials_,IDC_CRED_NAME+i);if(GetFocus()!=name)setControlText(name,toWide(r.name).c_str());
        enableControl(name,!working);enableControl(GetDlgItem(pageCredentials_,IDC_CRED_ENABLE+i),!r.enabled&&!working);
        enableControl(GetDlgItem(pageCredentials_,IDC_CRED_DELETE+i),!working);
    }
    layoutCredentialsPage();
    InvalidateRect(pageCredentials_,nullptr,FALSE);
    for(int id:{IDC_CRED_DEFAULT,IDC_CRED_WEB})InvalidateRect(GetDlgItem(pageCredentials_,id),nullptr,FALSE);
}
void WindowController::paintCredentialsPage(Canvas& dc,int w,int h){
    bool defaults=credentials::source()!="web";
    paintPageHeader(dc,w,L"凭据管理");paintPanel(dc,40,76,w-80,defaults?104:150);
    if(defaults)return;
    const CredentialTableLayout table(w);
    const wchar_t* headings[]={L"账号名称（可编辑）",L"获取时间",L"积分",L"是否启用",L"操作"};
    for(int column=0;column<5;++column)dc.text(headings[column],table.cell(column,242,28),FLabel,C_MUTED,{TextAlign::Center});
    drawDivider(dc,60,274,w-60);
    for(int i=0;i<CRED_PAGE_SIZE;++i){int idx=credentialPage_*CRED_PAGE_SIZE+i;if(idx>=(int)credentialRows_.size())break;auto&r=credentialRows_[idx];int y=CredentialTableLayout::top+i*CredentialTableLayout::rowHeight;
        dc.fill({60,y,w-120,46},i%2?C_ROW_STRIPE:C_SURFACE);
        dc.text(dateText(r.acquired),table.cell(1,y,CredentialTableLayout::contentHeight),FBody,C_TEXT,{TextAlign::Center});
        wchar_t credit[40];swprintf(credit,40,L"%.1f",r.credits);auto amount=!r.creditError.empty()?L"刷新失败":r.credits<0?L"未查询":credit;
        dc.text(amount,table.cell(2,y,CredentialTableLayout::contentHeight),FBody,C_TEXT,{TextAlign::Center});
        dc.text(r.enabled?L"已启用":L"未启用",table.cell(3,y,CredentialTableLayout::contentHeight),FBody,r.enabled?C_ACCENT_INK:C_MUTED,{TextAlign::Center});
    }
    if(credentialRows_.empty()){dc.text(L"暂无凭据，请通过网页登录添加。",{60,304,w-120,40},FBody,C_MUTED);return;}
    wchar_t page[64];swprintf(page,64,L"第 %d / %d 页",credentialPage_+1,std::max(1,((int)credentialRows_.size()+CRED_PAGE_SIZE-1)/CRED_PAGE_SIZE));
    dc.text(page,{credentialPagerX_-176,pagerY(h),160,34},FBody,C_MUTED,{TextAlign::Right});
}
bool WindowController::credentialCommand(int id,int code,HWND control){
    if(id>=IDC_CRED_NAME&&id<IDC_CRED_NAME+CRED_PAGE_SIZE){if(code==EN_KILLFOCUS){int idx=credentialPage_*CRED_PAGE_SIZE+id-IDC_CRED_NAME;if(idx<(int)credentialRows_.size()){wchar_t name[128]{};GetWindowTextW(control,name,128);std::string error;if(!credentials::rename(credentialRows_[idx].id,utf8(name),error))credentialMessage_=error;refreshCredentialsPage();}}return true;}
    if(code!=BN_CLICKED)return false;std::string error;bool handled=true;credentialMessage_.clear();
    if(id==IDC_CRED_DEFAULT||id==IDC_CRED_WEB){
        bool selected=credentials::setSource(id==IDC_CRED_DEFAULT?"default":"web",error);credentialPage_=0;
        if(selected&&id==IDC_CRED_DEFAULT){
            if(credentials::discover(error)){auto defaults=credentials::rows();auto enabled=std::find_if(defaults.begin(),defaults.end(),[](const auto& row){return row.enabled;});
                if(enabled==defaults.end()&&!defaults.empty())credentials::enable(defaults.front().id,error);
                auto active=AccountPool::instance().accounts();if(!active.empty())AccountPool::instance().refreshCreditsAsync(active.front());
            }
        }
    }
    else if(id==IDC_CRED_LOGIN){credentials::startLogin(error);}
    else if(id==IDC_CRED_REFRESH){if(!credentialWorking_.exchange(true))credentialWorker_=std::jthread([this]{credentials::refreshAll();credentialWorking_=false;});}
    else if(id==IDC_CRED_PREV){credentialPage_=std::max(0,credentialPage_-1);}
    else if(id==IDC_CRED_NEXT){++credentialPage_;}
    else if(id>=IDC_CRED_SLOT0&&id<IDC_CRED_SLOT0+7){int page=credentialSlotPage_[id-IDC_CRED_SLOT0];if(page>0)credentialPage_=page-1;}
    else if(id>=IDC_CRED_ENABLE&&id<IDC_CRED_ENABLE+CRED_PAGE_SIZE){int idx=credentialPage_*CRED_PAGE_SIZE+id-IDC_CRED_ENABLE;if(idx<(int)credentialRows_.size()&&!credentialRows_[idx].enabled){if(credentials::enable(credentialRows_[idx].id,error)){auto current=AccountPool::instance().accounts();if(!current.empty())AccountPool::instance().refreshCreditsAsync(current.front());}}}
    else if(id>=IDC_CRED_DELETE&&id<IDC_CRED_DELETE+CRED_PAGE_SIZE){int idx=credentialPage_*CRED_PAGE_SIZE+id-IDC_CRED_DELETE;if(idx<(int)credentialRows_.size())credentials::remove(credentialRows_[idx].id,error);}
    else handled=false;
    if(handled){credentialMessage_=error;refreshCredentialsPage();refreshStatusPage(hwnd_);if(!error.empty())MessageBoxW(hwnd_,toWide(error).c_str(),L"凭据管理",MB_OK|MB_ICONWARNING);}return handled;
}
}
