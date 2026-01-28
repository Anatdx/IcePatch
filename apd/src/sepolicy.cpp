#include "sepolicy.hpp"

#include "log.hpp"
#include "utils.hpp"

#include <sstream>
#include <vector>

namespace apd {

namespace {
std::vector<std::string> Tokenize(const std::string& input) {
  std::vector<std::string> tokens;
  std::stringstream ss(input);
  std::string token;
  while (ss >> token) {
    if (!token.empty() && token[0] == '#') {
      break;
    }
    if (token == "{") {
      std::string merged = "{";
      while (ss >> token) {
        merged += " " + token;
        if (!token.empty() && token.back() == '}') {
          break;
        }
      }
      tokens.push_back(merged);
    } else {
      tokens.push_back(token);
    }
  }
  return tokens;
}

bool HasMinTokens(const std::vector<std::string>& tokens, size_t n) {
  return tokens.size() >= n;
}

}  // namespace

bool CheckSepolicyRule(const std::string& rule) {
  auto tokens = Tokenize(Trim(rule));
  if (tokens.empty()) {
    LOGE("Invalid: empty rule");
    return false;
  }
  const std::string& op = tokens[0];
  if (op == "allow" || op == "deny" || op == "auditallow" || op == "dontaudit") {
    if (!HasMinTokens(tokens, 5)) {
      LOGE("Invalid rule: %s", rule.c_str());
      return false;
    }
    return true;
  }
  if (op == "allowxperm" || op == "auditallowxperm" || op == "dontauditxperm") {
    if (!HasMinTokens(tokens, 6)) {
      LOGE("Invalid xperm rule: %s", rule.c_str());
      return false;
    }
    return true;
  }
  if (op == "permissive" || op == "enforce") {
    return HasMinTokens(tokens, 2);
  }
  if (op == "type") {
    return HasMinTokens(tokens, 2);
  }
  if (op == "typeattribute" || op == "attradd") {
    return HasMinTokens(tokens, 3);
  }
  if (op == "attribute") {
    return HasMinTokens(tokens, 2);
  }
  if (op == "type_transition") {
    return HasMinTokens(tokens, 5);
  }
  if (op == "type_change" || op == "type_member") {
    return HasMinTokens(tokens, 5);
  }
  if (op == "genfscon") {
    return HasMinTokens(tokens, 4);
  }
  LOGE("Unknown sepolicy rule: %s", rule.c_str());
  return false;
}

}  // namespace apd
