#pragma once

#include "json/json.hpp"
#include <algorithm>
#include <optional>
#include <regex>
#include <string>
#include <string_view>

namespace synth::client {

struct SocialTaskInput final {
    std::string recipient_name, topic, when, location_name, source_text;
};

// This recognizes an explicit instruction only. The server independently binds
// the same text to the selected follower, known recipient and save-visible place.
inline std::optional<SocialTaskInput> parse_social_task_input(std::string_view source,
    std::string_view performer_name = {}) {
    const auto trim=[](std::string_view value) {
        const auto first=value.find_first_not_of(" \t\r\n\v\f");
        if (first==std::string_view::npos) return std::string{};
        return std::string{value.substr(first,value.find_last_not_of(" \t\r\n\v\f")-first+1)};
    };
    const auto text_ok=[](const std::string& value,std::size_t maximum,bool empty=false) {
        return (empty || !value.empty()) && value.size()<=maximum && value.find('\0')==std::string::npos &&
            json::detail::valid_utf8(value);
    };
    if (source.size()>1200 || source.find('\0')!=std::string_view::npos || !json::detail::valid_utf8(source)) return {};
    std::string text=trim(source);
    if (text.empty()) return {};
    if (!performer_name.empty()) {
        const auto delimiter=text.find_first_of(",;:");
        if (delimiter!=std::string::npos) {
            auto prefix=trim(std::string_view{text}.substr(0,delimiter));
            const auto equal_fold=[](unsigned char a,unsigned char b) {
                const auto fold=[](unsigned char ch) { return ch>='A' && ch<='Z' ? ch+('a'-'A') : ch; };
                return fold(a)==fold(b);
            };
            if (prefix.size()==performer_name.size() && std::equal(prefix.begin(),prefix.end(),performer_name.begin(),equal_fold))
                text=trim(std::string_view{text}.substr(delimiter+1));
        }
    }
    static const auto flags=std::regex_constants::ECMAScript|std::regex_constants::icase|std::regex_constants::optimize;
    static const std::regex future_prefix{
        R"(^(?:when|once)\s+(?:we|you)\s+(?:(?:get|come|are|head)\s+back|return)(?:\s+(?:to|at|in)\s+([^,]+))?\s*,\s*(.+)$)",flags};
    static const std::regex future_suffix{
        R"(^(.+?)\s+(?:when|once)\s+(?:we|you)\s+(?:(?:get|come|are|head)\s+back|return)(?:\s+(?:to|at|in)\s+(.+?))?[.!]?$)",flags};
    static const std::regex command{
        R"(^(?:(?:can|could|would)\s+you\s+)?(?:please\s+)?(?:go\s+)?(?:talk|speak|chat)\s+(?:to|with)\s+(.+?)(?:\s+about\s+(.+))?[.!]?$)",flags};
    std::smatch match;
    std::string when="now",location;
    if (std::regex_match(text,match,future_prefix)) {
        when="on_return";location=trim(match[1].str());text=match[2].str();
    } else if (std::regex_match(text,match,future_suffix)) {
        when="on_return";location=trim(match[2].str());text=match[1].str();
    }
    if (!std::regex_match(text,match,command)) return {};
    auto name=trim(match[1].str());
    auto topic=trim(match[2].matched ? match[2].str() : "general conversation");
    for (auto* value : {&name,&topic}) {
        const auto last=value->find_last_not_of(".!");
        if (last==std::string::npos) value->clear(); else value->resize(last+1);
    }
    if (!text_ok(name,128) || !text_ok(topic,512) || !text_ok(location,128,true)) return {};
    const auto labels=name+location;
    if (labels.find_first_of("\"<>")!=std::string::npos || labels.find("\xE2\x80\x9C")!=std::string::npos ||
        labels.find("\xE2\x80\x9D")!=std::string::npos) return {};
    return SocialTaskInput{std::move(name),std::move(topic),std::move(when),std::move(location),std::string{source}};
}

} // namespace synth::client
