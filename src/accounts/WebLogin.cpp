#include "accounts/WebLogin.h"
#include "accounts/Saas.h"
#include "common/Crypto.h"
#include "common/Http.h"
#include "common/HttpServer.h"
#include "common/Settings.h"
#include <windows.h>
#include <bcrypt.h>
#include <shellapi.h>
#include <algorithm>
#include <ctime>
#include <map>
#include <stdexcept>

namespace webLogin {
namespace {
constexpr auto api = "https://api.trae.cn";
constexpr auto client = "ono9krqynydwx5";
constexpr auto exchangePath = "/trae/api/v3/oauth/ExchangeToken";
std::mutex mutex;
std::shared_ptr<HttpServer> server;
std::atomic<bool> pending{false};
std::string hint;
std::jthread timer;
struct Attempt {
    std::mutex mutex;
    AuthData auth;
    std::string verifier;
    std::string traceId;
    int port = 0;
    bool completed = false;
    std::chrono::steady_clock::time_point started = std::chrono::steady_clock::now();
};
void status(const std::string& text) { std::lock_guard lock(mutex); hint = text; }
std::string version() { auto v=settings::get()->ideVersion; return v.empty()?"3.4.0":v; }
std::string urlEncode(const std::string& s) {
    std::string out; const char* hex="0123456789ABCDEF";
    for(unsigned char c:s) { if((c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='-'||c=='_'||c=='.'||c=='~')out+=c;
        else {out+='%';out+=hex[c>>4];out+=hex[c&15];} } return out;
}
std::string decode(const std::string& s) {
    std::string out; auto digit=[](char c)->int {if(c>='0'&&c<='9')return c-'0';if(c>='a'&&c<='f')return c-'a'+10;if(c>='A'&&c<='F')return c-'A'+10;return -1;};
    for(size_t i=0;i<s.size();++i) {if(s[i]=='%'&&i+2<s.size()&&digit(s[i+1])>=0&&digit(s[i+2])>=0){out+=char(digit(s[i+1])*16+digit(s[i+2]));i+=2;}else out+=s[i]=='+'?' ':s[i];}return out;
}
std::string getCode(const std::string& query) {
    for(auto q:{query,decode(query)}) {size_t pos=0;while(pos<q.size()){auto end=q.find('&',pos);auto pair=q.substr(pos,end==std::string::npos?end:end-pos);auto eq=pair.find('=');
        if(eq!=std::string::npos&&decode(pair.substr(0,eq))=="authCodeInfo"){Json j;if(Json::parse(decode(pair.substr(eq+1)),j))return j.get("AuthCode",Json("")).asString();}
        if(end==std::string::npos)break;pos=end+1;}}return {};
}
Json callbackFields(const std::string& query) {
    Json fields = Json::object();
    size_t pos = 0;
    while (pos < query.size()) {
        auto end = query.find('&', pos);
        auto pair = query.substr(pos, end == std::string::npos ? end : end - pos);
        auto eq = pair.find('=');
        if (eq != std::string::npos) {
            auto key = decode(pair.substr(0, eq));
            if (fields.find(key)) throw std::runtime_error("重复的授权回调参数");
            fields.set(key, decode(pair.substr(eq + 1)));
        }
        if (end == std::string::npos) break;
        pos = end + 1;
    }
    return fields;
}
std::string b64url(const std::vector<uint8_t>& bytes){auto s=crypto::base64Encode(bytes);std::replace(s.begin(),s.end(),'+','-');std::replace(s.begin(),s.end(),'/','_');while(!s.empty()&&s.back()=='=')s.pop_back();return s;}
std::vector<uint8_t> hash256(const std::string& s) {
    BCRYPT_ALG_HANDLE alg=nullptr;std::vector<uint8_t> out(32);
    if(BCryptOpenAlgorithmProvider(&alg,BCRYPT_SHA256_ALGORITHM,nullptr,0)<0)throw std::runtime_error("SHA256 初始化失败");
    auto result=BCryptHash(alg,nullptr,0,(PUCHAR)s.data(),(ULONG)s.size(),out.data(),32);BCryptCloseAlgorithmProvider(alg,0);
    if(result<0)throw std::runtime_error("SHA256 失败");return out;
}
struct Key { BCRYPT_ALG_HANDLE alg=nullptr;BCRYPT_KEY_HANDLE key=nullptr;
    Key(){if(BCryptOpenAlgorithmProvider(&alg,BCRYPT_ECDSA_P256_ALGORITHM,nullptr,0)<0)throw std::runtime_error("设备密钥初始化失败");}
    ~Key(){if(key)BCryptDestroyKey(key);if(alg)BCryptCloseAlgorithmProvider(alg,0);}
};
std::vector<uint8_t> exportKey(Key& k,LPCWSTR type){ULONG n=0;if(BCryptExportKey(k.key,nullptr,type,nullptr,0,&n,0)<0)throw std::runtime_error("设备密钥导出失败");std::vector<uint8_t>b(n);if(BCryptExportKey(k.key,nullptr,type,b.data(),n,&n,0)<0)throw std::runtime_error("设备密钥导出失败");return b;}
void createDevice(AuthData& auth) {
    Key k;if(BCryptGenerateKeyPair(k.alg,&k.key,256,0)<0||BCryptFinalizeKeyPair(k.key,0)<0)throw std::runtime_error("设备密钥生成失败");
    auto privateBlob=exportKey(k,BCRYPT_ECCPRIVATE_BLOB);auth.devicePrivateBlob=crypto::base64Encode(privateBlob);SecureZeroMemory(privateBlob.data(),privateBlob.size());
    auto pub=exportKey(k,BCRYPT_ECCPUBLIC_BLOB);
    const uint8_t prefix[]={0x30,0x59,0x30,0x13,0x06,0x07,0x2a,0x86,0x48,0xce,0x3d,0x02,0x01,0x06,0x08,0x2a,0x86,0x48,0xce,0x3d,0x03,0x01,0x07,0x03,0x42,0x00,0x04};
    std::vector<uint8_t> der(std::begin(prefix),std::end(prefix));der.insert(der.end(),pub.begin()+sizeof(BCRYPT_ECCKEY_BLOB),pub.end());
    auto b64=crypto::base64Encode(der);std::string pem="-----BEGIN PUBLIC KEY-----\n";for(size_t i=0;i<b64.size();i+=64)pem+=b64.substr(i,64)+"\n";pem+="-----END PUBLIC KEY-----\n";
    Json d=Json::object();d.set("DeviceID",crypto::genUuid());d.set("MachineID",crypto::sha512Hex(crypto::genUuid()));d.set("PlatformCode","IDE_PC");d.set("DeviceType","PC");
    d.set("DeviceName","TraeRelay");d.set("DeviceModel","");d.set("DeviceBrand","");d.set("DeviceCPU","");d.set("OSInfo","Windows");d.set("OSVersion","");d.set("ClientVersion",version());d.set("DevicePublicKey",pem);auth.deviceInfo=d;auth.clientId=client;auth.host=api;
}
std::string signature(const AuthData& auth,const std::string& content){
    Key k;std::vector<uint8_t> blob;if(!crypto::base64Decode(auth.devicePrivateBlob,blob))throw std::runtime_error("设备私钥格式错误");
    auto result=BCryptImportKeyPair(k.alg,nullptr,BCRYPT_ECCPRIVATE_BLOB,&k.key,blob.data(),(ULONG)blob.size(),0);SecureZeroMemory(blob.data(),blob.size());if(result<0)throw std::runtime_error("设备私钥读取失败");
    auto hash=hash256(content);std::vector<uint8_t> raw(64);ULONG n=0;if(BCryptSignHash(k.key,nullptr,hash.data(),32,raw.data(),64,&n,0)<0||n!=64)throw std::runtime_error("设备签名失败");
    std::vector<uint8_t> contentDer;for(size_t start:{size_t(0),size_t(32)}){auto end=start+32;while(start+1<end&&raw[start]==0)++start;bool pad=(raw[start]&0x80)!=0;contentDer.push_back(2);contentDer.push_back(uint8_t(end-start+(pad?1:0)));if(pad)contentDer.push_back(0);contentDer.insert(contentDer.end(),raw.begin()+start,raw.begin()+end);}
    std::vector<uint8_t> der{0x30,uint8_t(contentDer.size())};der.insert(der.end(),contentDer.begin(),contentDer.end());return crypto::base64Encode(der);
}
Json post(const std::string& path,const Json& body,const std::string& token=""){
    http::Headers headers;headers.set("Content-Type","application/json");headers.set("x-cloudide-token",token);
    auto r=http::send("POST",std::string(api)+path,headers,body.dump(),30000);if(r.status!=200)throw std::runtime_error("登录接口请求失败，HTTP "+std::to_string(r.status));
    Json j;if(!Json::parse(r.body,j))throw std::runtime_error("登录接口响应格式错误");
    auto meta=j.get("ResponseMetadata",Json::object()).get("Error",Json());if(!meta.isNull())throw std::runtime_error("授权被服务器拒绝，请重新登录");
    auto v=j.get("Result",Json());if(!v.isObject())throw std::runtime_error("登录接口未返回凭据");return v;
}
void applyToken(AuthData& a,const Json& v){auto token=v.get("Token",Json("")).asString();if(token.empty())throw std::runtime_error("未取得有效登录令牌");a.accessToken=token;auto rt=v.get("RefreshToken",Json("")).asString();if(!rt.empty())a.refreshToken=rt;auto expiry=v.get("TokenExpireAt",Json());if(expiry.isString())a.expiredRaw=expiry.asString();else if(expiry.isNumber())a.expiredRaw=std::to_string(expiry.asInt());a.expiredTs=Storage::parseExpiry(a.expiredRaw);}
}
bool refresh(AuthData& auth,std::string& error){try{Json b=Json::object();auto stamp=(long long)time(nullptr);auto nonce=crypto::genUuid();nonce.erase(std::remove(nonce.begin(),nonce.end(),'-'),nonce.end());auto c=auth.clientId.empty()?client:auth.clientId;
    auto sig=signature(auth,std::string("POST\n")+exchangePath+"\n"+c+"\n"+auth.refreshToken+"\n"+std::to_string(stamp)+"\n"+nonce);Json proof=Json::object();proof.set("Signature",sig);proof.set("Timestamp",Json((int64_t)stamp));proof.set("Nonce",nonce);
    b.set("ClientID",c);b.set("ClientSecret","");b.set("RefreshToken",auth.refreshToken);b.set("DeviceInfo",auth.deviceInfo);b.set("DeviceProof",proof);b.set("IDEVersion",version());applyToken(auth,post(exchangePath,b,auth.accessToken));return true;
}catch(const std::exception& e){error=e.what();return false;}}
bool busy(){return pending.load();}
std::string message(){std::lock_guard lock(mutex);return hint;}
void stop(){pending=false;timer.request_stop();if(timer.joinable())timer.join();if(server)server->stop();server.reset();pending=false;}
bool start(std::function<bool(std::shared_ptr<Account>,std::string&)> save,std::string& error){
    if(pending){error="请先完成已打开的网页登录";return false;}stop();
    try{auto attempt=std::make_shared<Attempt>();createDevice(attempt->auth);attempt->traceId=crypto::genUuid();attempt->verifier=b64url(crypto::randomBytes(48));auto challenge=b64url(hash256(attempt->verifier));
        server=std::make_shared<HttpServer>();server->handler=[attempt,save](const HttpRequest& req,HttpResponseWriter& res){
            const std::vector<std::pair<std::string,std::string>> h={{"Cache-Control","no-store"},{"Referrer-Policy","no-referrer"}};
            if(req.method=="OPTIONS"){res.respond(204,"text/plain","");return;}
            auto host=req.header("host");if(!host||*host!="127.0.0.1:"+std::to_string(attempt->port)||req.method!="GET"||req.path!="/authorize"||req.query.size()>65536){res.respond(400,"text/plain","Invalid callback",h);return;}
            std::lock_guard lock(attempt->mutex);if(attempt->completed||!pending||std::chrono::steady_clock::now()-attempt->started>std::chrono::minutes(30)){res.respond(410,"text/plain","Login expired",h);return;}
            try{
                auto fields = callbackFields(req.query);
                Json user = Json::object();
                auto scope = fields.get("scope", Json("")).asString();
                if (scope == "saas") {
                    if (fields.get("loginTraceID", Json("")).asString() != attempt->traceId)
                        throw std::runtime_error("企业授权会话不匹配，请重新登录");
                    saas::acceptCredential(attempt->auth, fields);
                    user.set("Name", attempt->auth.raw.get("account", Json()).get("username", Json("企业账号")));
                } else {
                    if (!scope.empty() && scope != "trae" && scope != "marscode")
                        throw std::runtime_error("当前仅支持个人账号和 SaaS 企业账号");
                    auto code=getCode(req.query);if(code.empty())throw std::runtime_error("未收到授权码，请重新登录");Json b=Json::object();b.set("ClientID",client);b.set("AuthCode",code);b.set("CodeVerifier",attempt->verifier);b.set("DeviceInfo",attempt->auth.deviceInfo);b.set("IDEVersion",version());applyToken(attempt->auth,post(exchangePath,b));
                Json info=Json::object();info.set("ReqSource","IDE");info.set("IDEVersion",version());user=post("/cloudide/api/v3/trae/GetUserInfo",info,attempt->auth.accessToken);attempt->auth.userId=user.get("UserID",Json("")).asString();if(attempt->auth.userId.empty())throw std::runtime_error("账号查询未通过验证");
                }
                auto account=std::make_shared<Account>();account->auth=attempt->auth;account->machineId=attempt->auth.deviceInfo.get("MachineID",Json("")).asString();account->deviceId=attempt->auth.deviceInfo.get("DeviceID",Json("")).asString();account->editionId="cn";account->nickname=user.get("Username",user.get("Name",Json("Trae-"+account->auth.userId.substr(account->auth.userId.size()>4?account->auth.userId.size()-4:0)))).asString();
                std::string err;if(!save(account,err))throw std::runtime_error(err);attempt->completed=true;status("网页登录成功，凭据已保存；请在列表中启用账号");res.respond(200,"text/html; charset=utf-8","<meta charset='utf-8'><h1>登录成功</h1><p>凭据已加密保存，请返回 TraeRelay 启用账号。</p>",h);
                pending=false;
            }catch(const std::exception& e){status(e.what());res.respond(400,"text/html; charset=utf-8","<meta charset='utf-8'><h1>登录未完成</h1><p>请返回 TraeRelay 查看提示并重新登录。</p>",h);pending=false;}
        };
        if(!server->start("127.0.0.1",0,error))return false;attempt->port=server->port();
        std::map<std::string,std::string> params={{"login_version","1"},{"auth_from","trae"},{"login_channel","native_ide"},{"plugin_version","2.3.90286"},{"auth_type","local"},{"client_id",client},{"redirect","0"},{"login_trace_id",attempt->traceId},{"auth_callback_url","http://127.0.0.1:"+std::to_string(attempt->port)+"/authorize"},{"machine_id",attempt->auth.deviceInfo.get("MachineID",Json("")).asString()},{"device_id",attempt->auth.deviceInfo.get("DeviceID",Json("")).asString()},{"x_app_version",version()},{"x_app_type","stable"},{"x_device_type","Windows"},{"code_challenge",challenge},{"code_challenge_method","S256"}};params["x_device_id"]=params["device_id"];params["x_machine_id"]=params["machine_id"];
        std::string url="https://www.trae.cn/authorization?";for(auto& [k,v]:params)url+=k+"="+urlEncode(v)+"&";std::wstring wide(url.begin(),url.end());pending=true;status("等待官方网页登录，30 分钟内有效");
        if((INT_PTR)ShellExecuteW(nullptr,L"open",wide.c_str(),nullptr,nullptr,SW_SHOWNORMAL)<=32){stop();error="无法打开默认浏览器";return false;}
        auto srv=server;timer=std::jthread([attempt,srv](std::stop_token st){while(!st.stop_requested()&&pending&&std::chrono::steady_clock::now()-attempt->started<std::chrono::minutes(30))Sleep(100);if(pending&&!st.stop_requested()){pending=false;status("登录已超时，请重新打开网页登录");}{std::lock_guard lock(attempt->mutex);srv->stop();}});return true;
    }catch(const std::exception&e){error=e.what();stop();return false;}
}
}
