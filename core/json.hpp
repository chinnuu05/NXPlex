// Incremental (push) SAX JSON parser. Feed it bytes as they arrive from the
// network; it holds at most one token in memory, which lets the sysmodule
// (tiny heap) read Plex's very verbose responses without building a DOM.
#pragma once
#include <cstddef>
#include <cstdint>
#include <string>

namespace nxp {

enum class JsonEv : uint8_t { ObjBegin, ObjEnd, ArrBegin, ArrEnd, String, Number, Bool, Null };

class JsonParser;

struct JsonSink {
    // depth: number of containers enclosing this value (root value = 0).
    //        *Begin and the matching *End report the same depth.
    // key:   member name when the parent is an object, "" inside arrays.
    // val:   scalar text ("true"/"false" for Bool); empty for containers.
    virtual void on_json(JsonEv ev, int depth, const std::string& key, const std::string& val) = 0;
    virtual ~JsonSink() = default;
};

class JsonParser {
public:
    explicit JsonParser(JsonSink& sink, size_t max_token = 2048) : sink_(sink), max_token_(max_token) {}

    // Returns false once the input is known to be malformed.
    bool feed(const char* data, size_t n);
    // Call after the last feed(); true if exactly one complete document was parsed.
    bool finish();
    bool failed() const { return st_ == St::Error; }

    // Key under which the container at `depth` lives ("" for array elements / root).
    const std::string& key_at(int depth) const;

    static const int kMaxDepth = 32;

private:
    enum class St : uint8_t {
        Value, ObjFirst, ObjKey, ObjColon, ObjNext, ArrFirst, ArrNext,
        Str, StrEsc, StrUni, Num, Lit, Done, Error
    };

    bool step(char c);  // false => c was not consumed, process again
    void value_done();
    void close_container();
    const std::string& cur_key() const;
    void push_utf8(uint32_t cp);
    void tok_push(char c) {
        if (tok_.size() < max_token_) tok_.push_back(c);
    }

    JsonSink& sink_;
    size_t max_token_;
    St st_ = St::Value;
    bool str_is_key_ = false;
    bool is_obj_[kMaxDepth] = {};
    std::string keys_[kMaxDepth];
    int depth_ = 0;  // open containers
    std::string tok_;
    std::string key_;
    uint32_t uni_ = 0, uni_hi_ = 0;
    int uni_n_ = 0;
};

}  // namespace nxp
