#pragma once
#include "accounts/AccountPool.h"
#include <functional>

struct CredentialRow {
    std::string id, name, source, creditError;
    long long acquired = 0, creditTime = 0;
    double credits = -1;
    bool enabled = false;
    bool enterprise = false;
    std::string tenantName;
    Json enterpriseUsage = Json::object();
};

namespace credentials {
void initialize();
std::vector<CredentialRow> rows();
std::string source();
std::string message();
std::string displayName(const Account& account);
bool busy();
bool setSource(const std::string& source, std::string& error);
bool enable(const std::string& id, std::string& error);
bool remove(const std::string& id, std::string& error);
bool rename(const std::string& id, const std::string& name, std::string& error);
bool discover(std::string& error);
void refreshAll();
bool startLogin(std::string& error);
void stopLogin();
bool refreshWeb(AuthData& auth, std::string& error);
bool persist(const Account& account, std::string& error);
}
