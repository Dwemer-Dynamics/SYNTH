#include "json/json.hpp"

#include <cmath>
#include <cstdint>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>

namespace {
int checks{};
#define CHECK(x) do { ++checks; if (!(x)) throw std::runtime_error{std::string{"check failed: "} + #x}; } while (false)
template<class F> void rejects(F&& function) {
    ++checks;
    try { std::forward<F>(function)(); } catch (const synth::json::Error&) { return; }
    throw std::runtime_error{"expected JSON rejection"};
}
}

int main() {
    try {
        using namespace synth::json;
        const auto document = parse(R"({"null":null,"bool":true,"signed":-7,"unsigned":7,"real":1.25e2,"text":"A\n€😀","array":[false,0]})");
        CHECK(document.is_object());
        CHECK(document.find("null")->is_null());
        CHECK(document.find("bool")->as_bool());
        CHECK(document.find("signed")->as_int64() == -7);
        CHECK(document.find("unsigned")->as_uint64() == 7);
        CHECK(document.find("real")->as_double() == 125.0);
        CHECK(document.find("text")->as_string() == "A\n\xe2\x82\xac\xf0\x9f\x98\x80");
        CHECK(document.find("array")->as_array().size() == 2);
        const auto encoded = write(document);
        CHECK(write(parse(encoded)) == encoded);
        CHECK(write(Value::Object{{"quote", "\"\\\n"}, {"control", std::string(1, '\x01')}}) ==
              "{\"quote\":\"\\\"\\\\\\n\",\"control\":\"\\u0001\"}");

        rejects([] { (void)parse(""); });
        rejects([] { (void)parse("true false"); });
        rejects([] { (void)parse(R"({"a":1,"a":2})"); });
        rejects([] { (void)parse("[1,]"); });
        rejects([] { (void)parse("01"); });
        rejects([] { (void)parse("1."); });
        rejects([] { (void)parse("1e"); });
        rejects([] { (void)parse("NaN"); });
        rejects([] { (void)parse("Infinity"); });
        rejects([] { (void)parse("1e9999"); });
        rejects([] { (void)parse("\"\xc0\x80\""); });
        rejects([] { (void)parse("\"\xed\xa0\x80\""); });
        rejects([] { (void)parse(R"("\ud800")"); });
        rejects([] { (void)parse(R"("\udc00")"); });
        rejects([] { (void)parse(std::string{"\"line\nfeed\""}); });
        rejects([] { (void)parse("{}", {.maximum_bytes = 1}); });
        rejects([] { (void)parse(R"("abcd")", {.maximum_bytes = 20, .maximum_depth = 4,
                                                  .maximum_string_bytes = 3}); });
        rejects([] { (void)parse("[[0]]", {.maximum_bytes = 20, .maximum_depth = 2}); });
        rejects([] { (void)parse("[1,2]", {.maximum_bytes = 20, .maximum_depth = 3,
                                           .maximum_string_bytes = 10, .maximum_array_items = 1}); });
        rejects([] { (void)parse(R"({"a":1,"b":2})", {.maximum_bytes = 30, .maximum_depth = 3,
                                                          .maximum_string_bytes = 10,
                                                          .maximum_array_items = 2,
                                                          .maximum_object_members = 1}); });
        rejects([] { (void)write(Value::Object{{"x", 1}, {"x", 2}}); });
        rejects([] { (void)write(Value{"\xc0\x80"}); });
        rejects([] { (void)write(Value::Array{1, 2}, {.maximum_bytes = 4}); });
        for (const bool writing : {false, true}) {
            bool byte_limit=false;
            try {
                if (writing) (void)write(Value::Array{1,2}, {.maximum_bytes=4});
                else (void)parse("[1,2]", {.maximum_bytes=4});
            } catch(const ByteLimitError& error) {byte_limit=true;CHECK(std::string{error.what()}=="JSON byte limit exceeded");}
            CHECK(byte_limit);
        }
        bool malformed=false;
        try { (void)write(Value{"\xc0\x80"}); }
        catch(const ByteLimitError&) {throw std::runtime_error{"malformed UTF-8 classified as optional truncation"};}
        catch(const Error&) {malformed=true;}
        CHECK(malformed);
        ++checks;
        try { (void)Value{std::numeric_limits<double>::infinity()}; }
        catch (const std::invalid_argument&) { goto nonfinite_rejected; }
        throw std::runtime_error{"nonfinite value accepted"};
nonfinite_rejected:
        std::cout << "json tests passed (" << checks << " checks)\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
