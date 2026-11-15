// Unit tests for the streaming JSON parser: every input is also fed one byte
// at a time to prove chunk boundaries don't matter.
#include <cstdio>
#include <string>
#include <vector>

#include "json.hpp"

using namespace nxp;

struct Rec : JsonSink {
    std::string out;
    void on_json(JsonEv ev, int depth, const std::string& key, const std::string& val) override {
        static const char* names[] = {"{", "}", "[", "]", "s", "n", "b", "null"};
        out += std::to_string(depth) + names[(int)ev];
        if (!key.empty()) out += "(" + key + ")";
        if (!val.empty() && ev != JsonEv::Null) out += "=" + val;
        out += " ";
    }
};

static int failures = 0;

static void check(const char* name, const std::string& in, bool want_ok, const std::string& want) {
    for (int mode = 0; mode < 2; mode++) {
        Rec r;
        JsonParser p(r);
        bool ok = true;
        if (mode == 0) ok = p.feed(in.data(), in.size());
        else for (char c : in) ok = p.feed(&c, 1) && ok;
        ok = p.finish() && ok;
        bool pass = ok == want_ok && (!want_ok || r.out == want);
        if (!pass) {
            failures++;
            printf("FAIL %s (%s): ok=%d\n  got:  %s\n  want: %s\n", name, mode ? "bytewise" : "whole", ok,
                   r.out.c_str(), want.c_str());
        }
    }
}

int main() {
    check("scalar", "42", true, "0n=42 ");
    check("object", R"({"a":1,"b":"x","c":true,"d":null})", true, "0{ 1n(a)=1 1s(b)=x 1b(c)=true 1null(d) 0} ");
    check("nested", R"({"MediaContainer":{"size":2,"Metadata":[{"title":"A"},{"title":"B"}]}})", true,
          "0{ 1{(MediaContainer) 2n(size)=2 2[(Metadata) 3{ 4s(title)=A 3} 3{ 4s(title)=B 3} 2](Metadata) "
          "1}(MediaContainer) 0} ");
    check("empty containers", R"({"a":[],"b":{}})", true, "0{ 1[(a) 1](a) 1{(b) 1}(b) 0} ");
    check("whitespace", " \n{ \"a\" : [ 1 , -2.5e3 ] }\r\n", true, "0{ 1[(a) 2n=1 2n=-2.5e3 1](a) 0} ");
    check("escapes", R"(["a\"b\\c\/d\n"])", true, "0[ 1s=a\"b\\c/d\n 0] ");
    check("unicode", R"(["é中"])", true, "0[ 1s=\xc3\xa9\xe4\xb8\xad 0] ");
    check("surrogate pair", R"(["🎵"])", true, "0[ 1s=\xf0\x9f\x8e\xb5 0] ");
    check("lone surrogate", R"(["\ud83cx"])", true, "0[ 1s=\xef\xbf\xbdx 0] ");
    check("raw utf8", "[\"caf\xc3\xa9\"]", true, "0[ 1s=caf\xc3\xa9 0] ");
    check("trailing garbage", "{} x", false, "");
    check("missing colon", R"({"a" 1})", false, "");
    check("trailing comma", "[1,]", false, "");
    check("bad literal", "[tru]", false, "");
    check("truncated", R"({"a":[1,2)", false, "");
    check("control char", "[\"a\nb\"]", false, "");

    // key_at() exposes the enclosing container names.
    {
        struct S : JsonSink {
            JsonParser* p = nullptr;
            std::string seen;
            void on_json(JsonEv ev, int depth, const std::string& key, const std::string&) override {
                if (ev == JsonEv::String && key == "key" && p->key_at(depth - 2) == "Part") seen += "part ";
            }
        } s;
        JsonParser p(s);
        s.p = &p;
        std::string in = R"({"Media":[{"Part":[{"key":"/x"}]}],"Other":[{"key":"/y"}]})";
        p.feed(in.data(), in.size());
        if (!p.finish() || s.seen != "part ") {
            failures++;
            printf("FAIL key_at: %s\n", s.seen.c_str());
        }
    }

    if (failures) {
        printf("%d failure(s)\n", failures);
        return 1;
    }
    printf("json_test: all passed\n");
    return 0;
}
