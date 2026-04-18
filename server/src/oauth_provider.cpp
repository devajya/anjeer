#include "server/oauth_provider.h"

#include <sstream>
#include <stdexcept>
#include <string>

#include <curl/curl.h>
#include <nlohmann/json.hpp>

namespace anjeer::server {

// ---------------------------------------------------------------------------
// libcurl helpers (private to this TU)
// ---------------------------------------------------------------------------

// AGENT-CTX: write_cb must be a plain function pointer — CURLOPT_WRITEFUNCTION
// does not accept std::function or capturing lambdas. The void* userdata is
// always a std::string* allocated on the caller's stack.
static size_t write_cb(char* ptr, size_t size, size_t nmemb, void* userdata) {
    auto* buf = static_cast<std::string*>(userdata);
    buf->append(ptr, size * nmemb);
    return size * nmemb;
}

// AGENT-CTX: url_encode allocates a fresh CURL handle per call. OAuth logins
// are not in the hot path (at most once per player per session), so the
// allocation cost is acceptable. Sharing a CURL handle across threads would
// require a mutex; creating one per call is simpler and correct.
static std::string url_encode(std::string_view value) {
    CURL* curl = curl_easy_init();
    if (!curl) throw std::runtime_error("curl_easy_init failed in url_encode");
    char* escaped = curl_easy_escape(curl, value.data(), static_cast<int>(value.size()));
    if (!escaped) {
        curl_easy_cleanup(curl);
        throw std::runtime_error("curl_easy_escape returned null");
    }
    std::string result(escaped);
    curl_free(escaped);
    curl_easy_cleanup(curl);
    return result;
}

std::string curl_get(std::string_view url, std::string_view bearer_token) {
    CURL* curl = curl_easy_init();
    if (!curl) throw std::runtime_error("curl_easy_init failed in curl_get");

    std::string body;
    curl_easy_setopt(curl, CURLOPT_URL, std::string(url).c_str());
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &body);

    struct curl_slist* headers = nullptr;
    if (!bearer_token.empty()) {
        const std::string auth = "Authorization: Bearer " + std::string(bearer_token);
        headers = curl_slist_append(headers, auth.c_str());
    }
    headers = curl_slist_append(headers, "Accept: application/json");
    headers = curl_slist_append(headers, "User-Agent: anjeer-server/1.0");
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);

    const CURLcode res = curl_easy_perform(curl);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    if (res != CURLE_OK) {
        throw std::runtime_error(std::string("HTTP GET failed: ") + curl_easy_strerror(res));
    }
    return body;
}

std::string curl_post(std::string_view url, std::string_view body,
                       std::string_view content_type) {
    CURL* curl = curl_easy_init();
    if (!curl) throw std::runtime_error("curl_easy_init failed in curl_post");

    std::string response;
    const std::string body_str(body);

    curl_easy_setopt(curl, CURLOPT_URL, std::string(url).c_str());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body_str.c_str());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(body_str.size()));
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);

    struct curl_slist* headers = nullptr;
    const std::string ct = "Content-Type: " + std::string(content_type);
    headers = curl_slist_append(headers, ct.c_str());
    headers = curl_slist_append(headers, "Accept: application/json");
    headers = curl_slist_append(headers, "User-Agent: anjeer-server/1.0");
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);

    const CURLcode res = curl_easy_perform(curl);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    if (res != CURLE_OK) {
        throw std::runtime_error(std::string("HTTP POST failed: ") + curl_easy_strerror(res));
    }
    return response;
}

// ---------------------------------------------------------------------------
// Shared token-exchange helper
// ---------------------------------------------------------------------------

// AGENT-CTX: All three providers use application/x-www-form-urlencoded for the
// token exchange POST body. The fields differ slightly per provider but the
// structure is identical: code + client credentials + redirect_uri + grant_type.
// Extracted to avoid copy-paste across the three exchange_code implementations.
static std::string exchange_token(
    const HttpPostFn&                                      post_fn,
    const ServerConfig::AuthConfig::OAuthProviderConfig&   cfg,
    std::string_view                                       token_endpoint,
    std::string_view                                       code)
{
    const std::string body =
        "code="          + url_encode(std::string(code))             +
        "&client_id="    + url_encode(cfg.client_id)                 +
        "&client_secret="+ url_encode(cfg.client_secret)             +
        "&redirect_uri=" + url_encode(cfg.redirect_uri)              +
        "&grant_type=authorization_code";

    const std::string raw = post_fn(token_endpoint, body,
                                     "application/x-www-form-urlencoded");

    // Trim leading/trailing whitespace — GitHub occasionally sends a leading CRLF.
    const auto s = raw.find_first_not_of(" \t\r\n");
    const auto e = raw.find_last_not_of(" \t\r\n");
    const std::string response = (s == std::string::npos) ? "" : raw.substr(s, e - s + 1);

    if (!response.empty() && response.front() == '{') {
        const auto json = nlohmann::json::parse(response);
        if (json.contains("error")) {
            throw std::runtime_error("OAuth token exchange failed: " +
                json.value("error_description", json["error"].get<std::string>()));
        }
        return json["access_token"].get<std::string>();
    }

    // Form-encoded fallback: parse access_token=xxx&token_type=bearer&...
    std::string access_token;
    std::string error_val;
    std::istringstream ss(response);
    std::string pair;
    while (std::getline(ss, pair, '&')) {
        const auto eq = pair.find('=');
        if (eq == std::string::npos) continue;
        const auto key = pair.substr(0, eq);
        const auto val = pair.substr(eq + 1);
        if (key == "access_token") access_token = val;
        else if (key == "error")   error_val    = val;
    }

    if (!error_val.empty()) {
        throw std::runtime_error("OAuth token exchange failed: " + error_val);
    }
    if (access_token.empty()) {
        throw std::runtime_error("token endpoint returned unexpected response: " + raw);
    }
    return access_token;
}

// ---------------------------------------------------------------------------
// GitHubOAuthProvider
// ---------------------------------------------------------------------------

GitHubOAuthProvider::GitHubOAuthProvider(
    const ServerConfig::AuthConfig::OAuthProviderConfig& cfg,
    HttpGetFn  get_fn,
    HttpPostFn post_fn)
    : cfg_(cfg), get_(std::move(get_fn)), post_(std::move(post_fn)) {}

std::string GitHubOAuthProvider::authorization_url(std::string_view state) const {
    // AGENT-CTX: scope=user:email requests read access to the user's email address
    // (even if it is private). Without this scope, the /user endpoint may return
    // null for the email field and we skip the email entirely (it's optional).
    return "https://github.com/login/oauth/authorize"
           "?client_id="    + url_encode(cfg_.client_id)    +
           "&redirect_uri=" + url_encode(cfg_.redirect_uri) +
           "&scope=user%3Aemail"                              +
           "&state="        + url_encode(std::string(state));
}

OAuthUserInfo GitHubOAuthProvider::exchange_code(std::string_view code) {
    const std::string access_token = exchange_token(
        post_, cfg_, "https://github.com/login/oauth/access_token", code);

    const std::string profile_str = get_("https://api.github.com/user", access_token);
    const auto profile = nlohmann::json::parse(profile_str);

    OAuthUserInfo info;
    info.provider_id = std::to_string(profile["id"].get<int64_t>());
    info.username    = profile["login"].get<std::string>();

    // AGENT-CTX: GitHub returns email:null when the user has set their email
    // to private. The email field is optional in our Player model so this is
    // fine — the game does not require an email address to function.
    if (!profile["email"].is_null()) {
        info.email = profile["email"].get<std::string>();
    }

    return info;
}

// ---------------------------------------------------------------------------
// GoogleOAuthProvider
// ---------------------------------------------------------------------------

GoogleOAuthProvider::GoogleOAuthProvider(
    const ServerConfig::AuthConfig::OAuthProviderConfig& cfg,
    HttpGetFn  get_fn,
    HttpPostFn post_fn)
    : cfg_(cfg), get_(std::move(get_fn)), post_(std::move(post_fn)) {}

std::string GoogleOAuthProvider::authorization_url(std::string_view state) const {
    return "https://accounts.google.com/o/oauth2/v2/auth"
           "?client_id="     + url_encode(cfg_.client_id)    +
           "&redirect_uri="  + url_encode(cfg_.redirect_uri) +
           "&scope=openid+email+profile"                      +
           "&response_type=code"                              +
           "&state="         + url_encode(std::string(state));
}

OAuthUserInfo GoogleOAuthProvider::exchange_code(std::string_view code) {
    const std::string access_token = exchange_token(
        post_, cfg_, "https://oauth2.googleapis.com/token", code);

    const std::string profile_str = get_(
        "https://www.googleapis.com/oauth2/v3/userinfo", access_token);
    const auto profile = nlohmann::json::parse(profile_str);

    OAuthUserInfo info;
    // AGENT-CTX: Google's OpenID Connect userinfo uses "sub" as the stable
    // identifier (not a numeric id). "sub" is guaranteed unique and stable per
    // Google account. Do not use "email" as the identifier — emails can change.
    info.provider_id = profile["sub"].get<std::string>();
    // Prefer given_name (first name) as username base; fall back to full name.
    // AuthService::sanitize_username will lowercase and strip spaces.
    info.username = profile.value("given_name", profile["name"].get<std::string>());
    if (profile.contains("email")) {
        info.email = profile["email"].get<std::string>();
    }

    return info;
}

} // namespace anjeer::server
