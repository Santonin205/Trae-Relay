#pragma once
#include "accounts/Storage.h"

struct Account;
namespace saas {
// This release supports the verified public CN SaaS deployment only.
std::string checkedHost(const std::string& host);
void restoreIdentity(AuthData& auth);
void verifyIdentity(AuthData& auth);
void refreshToken(AuthData& auth);
void acceptCredential(AuthData& auth, const Json& callback);
void refreshUsage(Account& account);
Json usage(const Account& account);
std::string amountText(const Json& usage, bool paygo);
std::string summary(const Json& usage);
}
