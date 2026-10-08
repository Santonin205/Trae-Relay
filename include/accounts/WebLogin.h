#pragma once
#include "accounts/AccountPool.h"
#include <functional>
namespace webLogin {
bool start(std::function<bool(std::shared_ptr<Account>, std::string&)> save, std::string& error);
void stop();
bool busy();
std::string message();
bool refresh(AuthData& auth, std::string& error);
}
