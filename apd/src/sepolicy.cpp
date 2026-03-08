#include "sepolicy.hpp"

#include "log.hpp"
#include "utils.hpp"

#include <cctype>
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

std::string NormalizeRule(const std::string& rule) {
  std::string normalized = Trim(rule);
  while (!normalized.empty() && normalized.back() == ';') {
    normalized.pop_back();
    normalized = Trim(normalized);
  }
  return normalized;
}

bool IsSepolicyWordChar(char c) {
  unsigned char uc = static_cast<unsigned char>(c);
  return std::isalnum(uc) || c == '_' || c == '-';
}

bool IsSepolicyWord(const std::string& token) {
  if (token.empty()) {
    return false;
  }
  for (char c : token) {
    if (!IsSepolicyWordChar(c)) {
      return false;
    }
  }
  return true;
}

bool IsSepolicyObj(const std::string& token, bool allow_star) {
  if (allow_star && token == "*") {
    return true;
  }
  if (token.size() >= 2 && token.front() == '{' && token.back() == '}') {
    std::string inner = Trim(token.substr(1, token.size() - 2));
    if (inner.empty()) {
      return false;
    }
    std::stringstream ss(inner);
    std::string word;
    bool has_word = false;
    while (ss >> word) {
      has_word = true;
      if (!IsSepolicyWord(word)) {
        return false;
      }
    }
    return has_word;
  }
  return IsSepolicyWord(token);
}

bool CheckTokenCount(const std::vector<std::string>& tokens, size_t expect) {
  return tokens.size() == expect;
}

bool CheckTokenCountRange(const std::vector<std::string>& tokens, size_t min_count,
                          size_t max_count) {
  return tokens.size() >= min_count && tokens.size() <= max_count;
}

}  // namespace

bool CheckSepolicyRule(const std::string& rule) {
  auto tokens = Tokenize(NormalizeRule(rule));
  if (tokens.empty()) {
    LOGE("Invalid: empty rule");
    return false;
  }
  const std::string& op = tokens[0];
  if (op == "allow" || op == "deny" || op == "auditallow" || op == "dontaudit") {
    return CheckTokenCount(tokens, 5) && IsSepolicyObj(tokens[1], true) &&
           IsSepolicyObj(tokens[2], true) && IsSepolicyObj(tokens[3], true) &&
           IsSepolicyObj(tokens[4], true);
  }
  if (op == "allowxperm" || op == "auditallowxperm" || op == "dontauditxperm") {
    return CheckTokenCount(tokens, 6) && IsSepolicyObj(tokens[1], true) &&
           IsSepolicyObj(tokens[2], true) && IsSepolicyObj(tokens[3], true) &&
           IsSepolicyWord(tokens[4]) && IsSepolicyWord(tokens[5]);
  }
  if (op == "permissive" || op == "enforce") {
    return CheckTokenCount(tokens, 2) && IsSepolicyObj(tokens[1], false);
  }
  if (op == "type") {
    if (CheckTokenCount(tokens, 2)) {
      return IsSepolicyWord(tokens[1]);
    }
    return CheckTokenCount(tokens, 3) && IsSepolicyWord(tokens[1]) &&
           IsSepolicyObj(tokens[2], false);
  }
  if (op == "typeattribute" || op == "attradd") {
    return CheckTokenCount(tokens, 3) && IsSepolicyObj(tokens[1], false) &&
           IsSepolicyObj(tokens[2], false);
  }
  if (op == "attribute") {
    return CheckTokenCount(tokens, 2) && IsSepolicyWord(tokens[1]);
  }
  if (op == "type_transition" || op == "name_transition") {
    if (!CheckTokenCountRange(tokens, 5, 6)) {
      return false;
    }
    for (size_t i = 1; i < tokens.size(); ++i) {
      if (!IsSepolicyWord(tokens[i])) {
        return false;
      }
    }
    return true;
  }
  if (op == "type_change" || op == "type_member") {
    return CheckTokenCount(tokens, 5) && IsSepolicyWord(tokens[1]) && IsSepolicyWord(tokens[2]) &&
           IsSepolicyWord(tokens[3]) && IsSepolicyWord(tokens[4]);
  }
  if (op == "genfscon") {
    return CheckTokenCount(tokens, 4) && IsSepolicyWord(tokens[1]) && IsSepolicyWord(tokens[2]) &&
           IsSepolicyWord(tokens[3]);
  }
  LOGE("Unknown sepolicy rule: %s", rule.c_str());
  return false;
}

}  // namespace apd
