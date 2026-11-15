#include "json.hpp"

namespace nxp {

static const std::string kEmpty;

static bool is_ws(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; }

const std::string& JsonParser::key_at(int depth) const {
    return (depth >= 0 && depth < kMaxDepth) ? keys_[depth] : kEmpty;
}

const std::string& JsonParser::cur_key() const {
    return (depth_ > 0 && is_obj_[depth_ - 1]) ? key_ : kEmpty;
}

bool JsonParser::feed(const char* data, size_t n) {
    for (size_t i = 0; i < n && st_ != St::Error;) {
        if (step(data[i])) i++;
    }
    return st_ != St::Error;
}

bool JsonParser::finish() {
    if (st_ == St::Num || st_ == St::Lit) step(' ');
    return st_ == St::Done;
}

void JsonParser::value_done() {
    if (depth_ == 0) st_ = St::Done;
    else st_ = is_obj_[depth_ - 1] ? St::ObjNext : St::ArrNext;
}

void JsonParser::close_container() {
    depth_--;
    sink_.on_json(is_obj_[depth_] ? JsonEv::ObjEnd : JsonEv::ArrEnd, depth_, keys_[depth_], kEmpty);
    value_done();
}

void JsonParser::push_utf8(uint32_t cp) {
    if (cp < 0x80) tok_push((char)cp);
    else if (cp < 0x800) {
        tok_push((char)(0xC0 | (cp >> 6)));
        tok_push((char)(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
        tok_push((char)(0xE0 | (cp >> 12)));
        tok_push((char)(0x80 | ((cp >> 6) & 0x3F)));
        tok_push((char)(0x80 | (cp & 0x3F)));
    } else {
        tok_push((char)(0xF0 | (cp >> 18)));
        tok_push((char)(0x80 | ((cp >> 12) & 0x3F)));
        tok_push((char)(0x80 | ((cp >> 6) & 0x3F)));
        tok_push((char)(0x80 | (cp & 0x3F)));
    }
}

bool JsonParser::step(char c) {
    switch (st_) {
    case St::Value:
        if (is_ws(c)) return true;
        if (c == '{' || c == '[') {
            if (depth_ >= kMaxDepth) { st_ = St::Error; return true; }
            bool obj = c == '{';
            keys_[depth_] = cur_key();
            sink_.on_json(obj ? JsonEv::ObjBegin : JsonEv::ArrBegin, depth_, keys_[depth_], kEmpty);
            is_obj_[depth_++] = obj;
            st_ = obj ? St::ObjFirst : St::ArrFirst;
            return true;
        }
        tok_.clear();
        if (c == '"') { str_is_key_ = false; st_ = St::Str; return true; }
        if (c == '-' || (c >= '0' && c <= '9')) { tok_push(c); st_ = St::Num; return true; }
        if (c == 't' || c == 'f' || c == 'n') { tok_push(c); st_ = St::Lit; return true; }
        st_ = St::Error;
        return true;

    case St::ObjFirst:
    case St::ObjKey:
        if (is_ws(c)) return true;
        if (c == '}' && st_ == St::ObjFirst) { close_container(); return true; }
        if (c == '"') { tok_.clear(); str_is_key_ = true; st_ = St::Str; return true; }
        st_ = St::Error;
        return true;

    case St::ObjColon:
        if (is_ws(c)) return true;
        st_ = c == ':' ? St::Value : St::Error;
        return true;

    case St::ObjNext:
        if (is_ws(c)) return true;
        if (c == ',') st_ = St::ObjKey;
        else if (c == '}') close_container();
        else st_ = St::Error;
        return true;

    case St::ArrFirst:
        if (is_ws(c)) return true;
        if (c == ']') { close_container(); return true; }
        st_ = St::Value;
        return false;

    case St::ArrNext:
        if (is_ws(c)) return true;
        if (c == ',') st_ = St::Value;
        else if (c == ']') close_container();
        else st_ = St::Error;
        return true;

    case St::Str:
        if (c == '"') {
            if (uni_hi_) { push_utf8(0xFFFD); uni_hi_ = 0; }
            if (str_is_key_) { key_ = tok_; st_ = St::ObjColon; }
            else { sink_.on_json(JsonEv::String, depth_, cur_key(), tok_); value_done(); }
        } else if (c == '\\') st_ = St::StrEsc;
        else if ((unsigned char)c < 0x20) st_ = St::Error;
        else {
            if (uni_hi_) { push_utf8(0xFFFD); uni_hi_ = 0; }
            tok_push(c);
        }
        return true;

    case St::StrEsc: {
        char out = 0;
        switch (c) {
        case '"': out = '"'; break;
        case '\\': out = '\\'; break;
        case '/': out = '/'; break;
        case 'b': out = '\b'; break;
        case 'f': out = '\f'; break;
        case 'n': out = '\n'; break;
        case 'r': out = '\r'; break;
        case 't': out = '\t'; break;
        case 'u': uni_ = 0; uni_n_ = 0; st_ = St::StrUni; return true;
        default: st_ = St::Error; return true;
        }
        if (uni_hi_) { push_utf8(0xFFFD); uni_hi_ = 0; }
        tok_push(out);
        st_ = St::Str;
        return true;
    }

    case St::StrUni: {
        int v;
        if (c >= '0' && c <= '9') v = c - '0';
        else if (c >= 'a' && c <= 'f') v = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') v = c - 'A' + 10;
        else { st_ = St::Error; return true; }
        uni_ = (uni_ << 4) | (uint32_t)v;
        if (++uni_n_ < 4) return true;
        st_ = St::Str;
        if (uni_ >= 0xD800 && uni_ <= 0xDBFF) {
            if (uni_hi_) push_utf8(0xFFFD);
            uni_hi_ = uni_;  // wait for the low surrogate
        } else if (uni_ >= 0xDC00 && uni_ <= 0xDFFF) {
            push_utf8(uni_hi_ ? 0x10000 + ((uni_hi_ - 0xD800) << 10) + (uni_ - 0xDC00) : 0xFFFD);
            uni_hi_ = 0;
        } else {
            if (uni_hi_) { push_utf8(0xFFFD); uni_hi_ = 0; }
            push_utf8(uni_);
        }
        return true;
    }

    case St::Num:
        if ((c >= '0' && c <= '9') || c == '.' || c == 'e' || c == 'E' || c == '+' || c == '-') {
            tok_push(c);
            return true;
        }
        sink_.on_json(JsonEv::Number, depth_, cur_key(), tok_);
        value_done();
        return false;

    case St::Lit:
        if (c >= 'a' && c <= 'z') { tok_push(c); return true; }
        if (tok_ == "true" || tok_ == "false") sink_.on_json(JsonEv::Bool, depth_, cur_key(), tok_);
        else if (tok_ == "null") sink_.on_json(JsonEv::Null, depth_, cur_key(), tok_);
        else { st_ = St::Error; return true; }
        value_done();
        return false;

    case St::Done:
        if (!is_ws(c)) st_ = St::Error;
        return true;

    case St::Error:
        return true;
    }
    return true;
}

}  // namespace nxp
