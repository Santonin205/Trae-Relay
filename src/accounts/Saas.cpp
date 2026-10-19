#include "accounts/Saas.h"
#include "accounts/AccountPool.h"
#include "common/Http.h"
#include <cstdio>
#include <ctime>
#include <stdexcept>

namespace saas {
namespace {
std::string str(const Json& j, const char* key) {
    auto v = j.get(key, Json());
    return v.isString() ? v.asString() : v.isNumber() ? std::to_string(v.asInt()) : "";
}
Json post(const AuthData& auth, const std::string& path, const Json& body) {
    http::Headers headers;
    headers.set("Content-Type", "application/json");
    headers.set("x-cloudide-token", auth.accessToken);
    headers.set("Authorization", "Cloud-IDE-JWT " + auth.accessToken);
    headers.set("x-uid", auth.userId);
    auto r = http::send("POST", checkedHost(auth.host) + path, headers, body.dump(), 30000);
    if (!r.ok()) throw std::runtime_error("企业接口请求失败，HTTP " + std::to_string(r.status));
    Json j;
    if (!Json::parse(r.body, j) || !j.get("code", Json()).isNumber())
        throw std::runtime_error("企业接口响应格式错误");
    auto code = j.get("code", Json(-1)).asInt(-1);
    if (code != 0) throw std::runtime_error("企业接口 " + path.substr(path.rfind('/') + 1) + " 拒绝请求，错误码 " + std::to_string(code));
    return j;
}
void applyToken(AuthData& auth, const Json& token) {
    auto value = str(token, "Token");
    auto expiry = str(token, "TokenExpireAt");
    if (value.empty() || Storage::parseExpiry(expiry) <= (long long)time(nullptr))
        throw std::runtime_error("企业授权令牌无效或已过期，请重新登录");
    auth.accessToken = value;
    auth.expiredRaw = expiry;
    auth.expiredTs = Storage::parseExpiry(expiry);
    auto rt = str(token, "RefreshToken");
    if (!rt.empty()) auth.refreshToken = rt;
}

}
std::string checkedHost(const std::string& input) {
    std::string host = input;
    while (!host.empty() && host.back() == '/') host.pop_back();
    if (host != "https://console.enterprise.trae.cn")
        throw std::runtime_error("当前仅支持 TRAE 国内公共 SaaS 企业账号");
    return host;
}
void restoreIdentity(AuthData& auth) {
    auto account = auth.raw.get("account", Json::object());
    if (account.find("scope")) auth.scope = str(account, "scope");
    auth.loginScope = str(account, "loginScope");
    auth.tenantId = str(account, "tenant_id");
    auth.tenantName = str(account, "tenant_name");
    if (auth.scope == "saas") {
        if (!auth.isSaas()) throw std::runtime_error("当前仅支持 SaaS 企业账号，暂不支持私有部署");
        auto boot = account.get("saasBootConfig", Json::object());
        auto host = str(boot, "apiHost");
        auth.host = checkedHost(host.empty() ? auth.host : host);
        if (auth.tenantId.empty()) throw std::runtime_error("企业凭据缺少企业 ID，请重新登录");
    }
}
void verifyIdentity(AuthData& auth) {
    Json request = Json::object(), attributes = Json::array();
    attributes.push_back("currency_pricing"); request.set("Attributes", attributes);
    auto j = post(auth, "/cloudide/api/v3/trae/GetUserInfo", request);
    auto data = j.get("Data", Json::object());
    auto user = data.get("UserInfo", Json::object());
    auto uid = str(user, "UserID"), tenant = str(user, "TenantID");
    if (str(user, "Scope") != "saas" || uid.empty() || tenant.empty())
        throw std::runtime_error("服务器未确认 SaaS 企业身份");
    if ((!auth.userId.empty() && auth.userId != uid) ||
        (!auth.tenantId.empty() && auth.tenantId != tenant))
        throw std::runtime_error("企业授权身份发生变化，请重新登录");
    auth.host = checkedHost(str(j.get("HostInfo", Json::object()), "Api"));
    auth.userId = uid;
    Json account = Json::object();
    account.set("scope", "saas"); account.set("loginScope", "saas");
    account.set("tenant_id", tenant);
    account.set("tenant_name", str(data.get("TenantInfoBase", Json::object()), "TenantName"));
    account.set("username", str(user, "Name"));
    Json boot = Json::object(); boot.set("apiHost", auth.host);
    account.set("saasBootConfig", boot);
    // Persist only identity/billing fields, never the server's full UserInfo (contains Password).
    account.set("currencyPricing", j.get("Attributes", Json::object()).get("currency_pricing", Json::object()).get("Enable", Json(false)));
    auth.raw.set("account", account);
    restoreIdentity(auth);
}
void refreshToken(AuthData& auth) {
    if (auth.refreshToken.empty()) throw std::runtime_error("企业刷新凭据缺失，请重新登录");
    AuthData next = auth;
    Json body = Json::object(); body.set("RefreshToken", auth.refreshToken);
    body.set("ClientSecret", "-"); body.set("UserID", "");
    applyToken(next, post(auth, "/cloudide/api/v3/trae/oauth/ExchangeToken", body).get("Data", Json()));
    verifyIdentity(next);
    auth = std::move(next);
}
void acceptCredential(AuthData& auth, const Json& callback) {
    AuthData next = auth;
    next.host = checkedHost(str(callback, "host"));
    next.refreshToken = str(callback, "refreshToken");
    if (next.refreshToken.empty()) throw std::runtime_error("企业授权缺少刷新凭据");
    // Exchange on the trusted endpoint and verify identity; do not trust callback userInfo/JWT.
    next.accessToken.clear(); next.devicePrivateBlob.clear();
    refreshToken(next);
    auth = std::move(next);
}
Json usage(const Account& account) {
    std::lock_guard lock(account.usageMtx);
    return account.enterpriseUsage;
}
void refreshUsage(Account& account) {
    std::lock_guard lock(account.refreshMtx);
    verifyIdentity(account.auth);
    if (!account.auth.raw.get("account", Json()).get("currencyPricing", Json(false)).asBool())
        throw std::runtime_error("当前企业套餐未启用金额计费，暂不显示金额额度");
    Json request = Json::object(); request.set("attributes", Json::array());
    auto plan = post(account.auth, "/trae/gtm/tob/api/v1/config/plan_attribute", request).get("Data", Json::object());
    auto attributes = plan.get("Attributes", Json::array());
    Json quota;
    for (size_t i = 0; i < attributes.size(); ++i)
        if (str(attributes.at(i), "Attribute") == "currency_pricing" && attributes.at(i).get("Enable", Json(false)).asBool())
            quota = attributes.at(i).get("Value", Json());
    if (!quota.isNumber()) throw std::runtime_error("企业套餐未返回金额额度");
    Json u = Json::object(); u.set("unit", "CNY");
    u.set("planQuota", quota);
    u.set("detailAvailable", false);
    u.set("detailsUrl", "https://console.enterprise.trae.cn/personal/usage");
    u.set("periodStart", plan.get("StartTime", Json(0)));
    u.set("periodEnd", plan.get("EndTime", Json(0)));
    u.set("checkedAt", Json((int64_t)time(nullptr)));
    { std::lock_guard usageLock(account.usageMtx); account.enterpriseUsage = u; }
    account.credits.store(-1);
    account.creditsCheckedAt.store((long long)time(nullptr));
    account.creditsUpdated.fetch_add(1);
}
std::string amountText(const Json& u, bool paygo) {
    if (!u.get("error", Json("")).asString().empty()) return "刷新失败";
    if (u.get("planQuota", Json()).isNumber()) {
        if (paygo) return "控制台查看";
        char text[64]; snprintf(text, sizeof(text), "¥%.2f", u.get("planQuota", Json()).asDouble());
        return text;
    }
    return "未查询";
}
std::string summary(const Json& u) { return "套餐 " + amountText(u, false) + "；用量 " + amountText(u, true); }
}
