#include "accounts/Credentials.h"
#include "accounts/WebLogin.h"
#include "common/Crypto.h"
#include "upstream/ModelCatalog.h"
#include <windows.h>
#include <wincrypt.h>
#include <filesystem>
#include <fstream>
#include <algorithm>
#include <ctime>

namespace credentials {
namespace {
std::recursive_mutex guard;
Json registry=Json::object();
bool initialized=false, healthy=true;
std::string notice;
std::atomic<bool> refreshing{false};
std::filesystem::path root(){return std::filesystem::u8path(exeDir())/L"auth";}
void migrateLegacyDirectory(){
    auto legacy=std::filesystem::u8path(exeDir())/L"credentials";
    if(std::filesystem::exists(root())||!std::filesystem::exists(legacy))return;
    std::error_code error;
    std::filesystem::rename(legacy,root(),error);
    if(error)throw std::runtime_error("旧凭据目录迁移到 auth 失败，请关闭其他实例后重试");
}
std::string str(const Json& j,const char* k){return j.get(k,Json("")).asString();}
std::filesystem::path vault(const std::string& id){if(id.empty()||id.find_first_not_of("0123456789abcdef-")!=std::string::npos)throw std::runtime_error("凭据标识无效");return root()/std::filesystem::u8path(id+".dpapi");}
std::string readFile(const std::filesystem::path& p){std::ifstream in(p,std::ios::binary);if(!in)throw std::runtime_error("无法读取凭据文件");return std::string(std::istreambuf_iterator<char>(in),{});}
void atomicWrite(const std::filesystem::path& p,const std::string& text){
    std::filesystem::create_directories(p.parent_path());auto tmp=p;tmp+=L".tmp";
    {std::ofstream out(tmp,std::ios::binary|std::ios::trunc);if(!out||!out.write(text.data(),text.size())||!out.flush())throw std::runtime_error("凭据保存失败");}
    if(!MoveFileExW(tmp.c_str(),p.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH))throw std::runtime_error("凭据文件替换失败");
}
void commit(const Json& next){if(!healthy)throw std::runtime_error("凭据目录损坏，请先恢复 index.json，已停止覆盖");atomicWrite(root()/L"index.json",next.dump(true));registry=next;}
std::string protect(const std::string& bytes,bool decrypt){
    DATA_BLOB in{(DWORD)bytes.size(),(BYTE*)bytes.data()},out{};
    BOOL ok=decrypt?CryptUnprotectData(&in,nullptr,nullptr,nullptr,nullptr,CRYPTPROTECT_UI_FORBIDDEN,&out):CryptProtectData(&in,L"TraeRelay account",nullptr,nullptr,nullptr,CRYPTPROTECT_UI_FORBIDDEN,&out);
    if(!ok)throw std::runtime_error("无法解密或加密凭据，请使用保存时的 Windows 用户");
    std::string result((char*)out.pbData,out.cbData);SecureZeroMemory(out.pbData,out.cbData);LocalFree(out.pbData);return result;
}
Json pack(const Account& a){
    Json j=a.auth.raw.isObject()?a.auth.raw:Json::object();
    j.set("generation",a.auth.credentialGeneration);j.set("token",a.auth.accessToken);j.set("refreshToken",a.auth.refreshToken);j.set("userId",a.auth.userId);j.set("expiredAt",a.auth.expiredRaw);j.set("host",a.auth.host);j.set("clientId",a.auth.clientId);j.set("privateBlob",a.auth.devicePrivateBlob);j.set("deviceInfo",a.auth.deviceInfo);j.set("machineId",a.machineId);j.set("deviceId",a.deviceId);j.set("editionId",a.editionId);return j;
}
void writeAccount(const Account& a){auto text=pack(a).dump();auto encrypted=protect(text,false);SecureZeroMemory(text.data(),text.size());atomicWrite(vault(a.auth.credentialId),encrypted);}
std::shared_ptr<Account> load(const Json& row){
    auto text=protect(readFile(vault(str(row,"id"))),true);Json j;bool ok=Json::parse(text,j);SecureZeroMemory(text.data(),text.size());if(!ok)throw std::runtime_error("凭据内容损坏");
    auto a=std::make_shared<Account>();auto& d=a->auth;d.raw=j;d.credentialId=str(row,"id");d.credentialGeneration=str(row,"generation");d.accessToken=str(j,"token");d.refreshToken=str(j,"refreshToken");d.userId=str(j,"userId");d.expiredRaw=str(j,"expiredAt");d.expiredTs=Storage::parseExpiry(d.expiredRaw);d.host=str(j,"host");d.clientId=str(j,"clientId");d.devicePrivateBlob=str(j,"privateBlob");d.deviceInfo=j.get("deviceInfo",Json());a->machineId=str(j,"machineId");a->deviceId=str(j,"deviceId");a->editionId=str(j,"editionId");a->nickname=str(row,"name");a->credits=row.get("credits",Json(-1)).asDouble(-1);
    if(d.accessToken.empty())throw std::runtime_error("凭据缺少 token，请重新登录");return a;
}
int find(const std::string& id){auto& rows=registry["rows"];for(size_t i=0;i<rows.size();++i)if(str(rows[i],"id")==id)return (int)i;return -1;}
bool switchTo(const std::string& id,Json next,std::string& error){
    try {
        auto current=AccountPool::instance().accounts();
        std::unique_lock<std::recursive_mutex> tokenLock;
        if(!current.empty()){
            tokenLock=std::unique_lock<std::recursive_mutex>(current.front()->refreshMtx,std::try_to_lock);
            if(!tokenLock.owns_lock()){error="正在刷新当前账号凭据，请稍后再切换或删除";return false;}
        }
        std::shared_ptr<Account>a;int i=find(id);if(!id.empty()){if(i<0)throw std::runtime_error("凭据不存在");a=load(registry["rows"][i]);}
        next.set("active",id);
        if(!AccountPool::instance().replaceActive(a,[&]{commit(next);})){error="当前账号仍有请求，请等请求结束后再切换或删除";return false;}
        ModelCatalog::instance().requestRefresh();
        return true;
    }catch(const std::exception&e){error=e.what();return false;}
}
bool add(std::shared_ptr<Account>a,const std::string& src,std::string& error){
    std::lock_guard lock(guard);try{auto current=AccountPool::instance().accounts();std::unique_lock<std::recursive_mutex> tokenLock;if(!current.empty()){tokenLock=std::unique_lock<std::recursive_mutex>(current.front()->refreshMtx,std::try_to_lock);if(!tokenLock.owns_lock()){error="Credential refresh in progress; retry shortly";return false;}}Json next=registry;int existing=-1;auto fingerprint=crypto::sha512Hex(a->auth.userId+"|"+a->editionId);for(size_t i=0;i<next["rows"].size();++i){auto& r=next["rows"][i];if(str(r,"fingerprint")==fingerprint&&str(r,"source")==src){existing=(int)i;break;}}
        Json r=existing>=0?next["rows"][existing]:Json::object();std::string id=existing>=0?str(r,"id"):crypto::genUuid();a->auth.credentialId=id;a->auth.credentialGeneration=crypto::genUuid();
        if(str(registry,"active")==id){auto current=AccountPool::instance().accounts();if(!current.empty()&&current.front()->active.load()>0){error="该账号有请求进行中，请结束后重新登录";return false;}}
        r.set("id",id);r.set("generation",a->auth.credentialGeneration);r.set("source",src);r.set("fingerprint",fingerprint);if(str(r,"name").empty())r.set("name",a->nickname);else a->nickname=str(r,"name");if(existing<0||src=="web")r.set("acquired",Json((int64_t)time(nullptr)));r.set("credits",Json(-1));r.set("creditTime",Json(0));r.set("creditError","");writeAccount(*a);
        if(existing>=0)next["rows"][existing]=r;else next["rows"].push_back(r);
        if(str(registry,"active")==id){if(!AccountPool::instance().replaceActive(a,[&]{commit(next);})){error="Account is busy; saved credentials will be used on restart";return false;}}else commit(next);return true;
    }catch(const std::exception&e){error=e.what();return false;}
}
}
void initialize(){
    std::lock_guard lock(guard);if(initialized)return;initialized=true;registry.set("source","default");registry.set("active","");registry.set("rows",Json::array());
    try {migrateLegacyDirectory();auto p=root()/L"index.json";if(std::filesystem::exists(p)){Json j;if(!Json::parse(readFile(p),j)||!j.get("rows",Json()).isArray()||(str(j,"source")!="default"&&str(j,"source")!="web"))throw std::runtime_error("凭据索引损坏，未加载账号");registry=j;
            int i=find(str(j,"active"));if(i>=0&&str(j["rows"][i],"source")==str(j,"source"))AccountPool::instance().replaceActive(load(j["rows"][i]));
        } else {std::string err;discover(err);if(!err.empty())notice=err;auto rows=registry["rows"];if(rows.size())switchTo(str(rows[0],"id"),registry,err);}
    }catch(const std::exception&e){healthy=false;notice=e.what();}
}
std::string displayName(const Account& a){std::lock_guard lock(guard);int i=find(a.auth.credentialId);return i>=0?str(registry["rows"][i],"name"):a.nickname;}
std::string source(){std::lock_guard lock(guard);return str(registry,"source");}
std::string message(){std::lock_guard lock(guard);auto login=webLogin::message();return login.empty()?notice:login;}
bool busy(){return refreshing||webLogin::busy();}
std::vector<CredentialRow> rows(){std::lock_guard lock(guard);std::vector<CredentialRow>out;auto accounts=AccountPool::instance().accounts();
    for(size_t i=0;i<registry["rows"].size();++i){auto&r=registry["rows"][i];if(str(r,"source")!=str(registry,"source"))continue;CredentialRow v;v.id=str(r,"id");v.name=str(r,"name");v.source=str(r,"source");v.acquired=r.get("acquired",Json(0)).asInt();v.credits=r.get("credits",Json(-1)).asDouble(-1);v.creditTime=r.get("creditTime",Json(0)).asInt();v.creditError=str(r,"creditError");v.enabled=str(registry,"active")==v.id&&!accounts.empty();if(v.enabled){v.credits=accounts.front()->credits.load();if(accounts.front()->creditsCheckedAt.load()>v.creditTime){v.creditTime=accounts.front()->creditsCheckedAt.load();v.creditError.clear();}}out.push_back(v);}return out;
}
bool discover(std::string& error){std::lock_guard lock(guard);if(str(registry,"source")!="default"){error="请先选择默认目录来源";return false;}bool any=false;
    for(auto&ed:Storage::discoverEditions()){auto a=std::make_shared<Account>();if(!Storage::readAuth(ed.userDir,a->auth,error))continue;a->machineId=Storage::readMachineId(ed.userDir);a->deviceId=Storage::readAhaDeviceId(ed.userDir);if(a->deviceId.empty())a->deviceId=Storage::readDeviceId(ed.userDir);if(a->deviceId.empty())a->deviceId=a->machineId;a->editionId=ed.id;a->nickname="Trae-"+a->auth.userId.substr(a->auth.userId.size()>4?a->auth.userId.size()-4:0);if(!add(a,"default",error))return false;any=true;}
    if(!any&&error.empty())error="默认目录中未发现已登录账号";if(any)error.clear();return any;
}
bool setSource(const std::string& src,std::string& error){std::lock_guard lock(guard);if(src!="default"&&src!="web")return false;if(source()==src)return true;Json next=registry;next.set("source",src);return switchTo("",next,error);}
bool enable(const std::string& id,std::string& error){std::lock_guard lock(guard);int i=find(id);if(i<0||str(registry["rows"][i],"source")!=source()){error="请选择当前来源中的账号";return false;}return switchTo(id,registry,error);}
bool remove(const std::string& id,std::string& error){std::lock_guard lock(guard);int idx=find(id);if(idx<0){error="账号已删除";return false;}Json next=registry;Json rows=Json::array();for(size_t i=0;i<registry["rows"].size();++i)if((int)i!=idx)rows.push_back(registry["rows"][i]);next.set("rows",rows);
    try{if(str(registry,"active")==id){if(!switchTo("",next,error))return false;}else commit(next);std::error_code ec;std::filesystem::remove(vault(id),ec);if(ec){error="账号已停用并移出列表，但加密文件删除失败";return false;}return true;}catch(const std::exception&e){error=e.what();return false;}
}
bool rename(const std::string& id,const std::string& name,std::string& error){std::lock_guard lock(guard);try{int i=find(id);if(i<0||name.find_first_not_of(" \t\r\n")==std::string::npos||name.size()>200){error="名称不能为空，最多 200 字节";return false;}auto next=registry;next["rows"][i].set("name",name);commit(next);return true;}catch(const std::exception&e){error=e.what();return false;}}
bool persist(const Account& a,std::string& error){std::lock_guard lock(guard);if(a.auth.credentialId.empty())return true;int i=find(a.auth.credentialId);if(i<0||str(registry["rows"][i],"generation")!=a.auth.credentialGeneration)return true;try{writeAccount(a);return true;}catch(const std::exception&e){error=e.what();return false;}}
bool refreshWeb(AuthData& auth,std::string& error){return webLogin::refresh(auth,error);}
void refreshAll(){if(refreshing.exchange(true))return;try{auto list=rows();for(auto&r:list){std::shared_ptr<Account>a;{std::lock_guard lock(guard);int i=find(r.id);if(i<0)continue;auto active=AccountPool::instance().accounts();if(!active.empty()&&active.front()->auth.credentialId==r.id)a=active.front();else a=load(registry["rows"][i]);}
        auto before = a->creditsUpdated.load();
        AccountPool::instance().refreshCredits(a.get());{std::lock_guard lock(guard);int i=find(r.id);if(i>=0){auto next=registry;auto&row=next["rows"][i];row.set("credits",Json(a->credits.load()));if(a->creditsUpdated.load()>before)row.set("creditTime",Json((int64_t)time(nullptr)));row.set("creditError",a->creditsUpdated.load()==before?"刷新失败":"");commit(next);}}}
        std::lock_guard lock(guard);notice="积分刷新完成";
    }catch(const std::exception&e){std::lock_guard lock(guard);notice=e.what();}refreshing=false;
}
bool startLogin(std::string& error){if(source()!="web"){error="请先选择网页登录凭据";return false;}return webLogin::start([](std::shared_ptr<Account>a,std::string&e){return add(a,"web",e);},error);}
void stopLogin(){webLogin::stop();}
}
