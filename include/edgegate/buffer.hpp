#pragma once

#include <cassert>
#include <cstddef>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

namespace eg {

// 连续可读区间 + 尾部可写空间。读指针只前进，空间不足时压缩或扩容，
// 避免每读一小段就 erase 造成 O(n²)。
class Buffer {
public:
    Buffer() = default;

    std::size_t size() const { return w_ - r_; }
    bool empty() const { return r_ == w_; }
    std::size_t capacity() const { return buf_.size(); }
    std::size_t readable() const { return size(); }

    const char* data() const { return buf_.data() + r_; }
    char* data() { return buf_.data() + r_; }
    std::string_view view() const { return std::string_view(data(), size()); }

    // 只推进读指针，不搬移内存：调用方可能还持有指向已读区间的视图。
    // 空间在 reserve()/compact() 时才回收。
    void consume(std::size_t n) {
        assert(n <= size());
        r_ += n;
    }

    void clear() {
        r_ = 0;
        w_ = 0;
    }

    void append(const void* p, std::size_t n) {
        reserve(n);
        std::memcpy(buf_.data() + w_, p, n);
        w_ += n;
    }

    void append(std::string_view s) { append(s.data(), s.size()); }
    void append(const std::string& s) { append(s.data(), s.size()); }
    void append(const char* s) { append(s, std::strlen(s)); }

    // 确保尾部至少有 n 字节可写。必要时先压缩，再按 1.5x/2x 扩容。
    void reserve(std::size_t n) {
        if (buf_.size() - w_ >= n) return;
        if (r_ > 0) {
            const std::size_t readable = w_ - r_;
            std::memmove(buf_.data(), buf_.data() + r_, readable);
            r_ = 0;
            w_ = readable;
            if (buf_.size() - w_ >= n) return;
        }
        std::size_t want = w_ + n;
        std::size_t next = buf_.empty() ? 4096 : buf_.size();
        while (next < want) next = next + next / 2 + 1;
        buf_.resize(next);
    }

    char* write_ptr() { return buf_.data() + w_; }
    std::size_t writable() const { return buf_.size() - w_; }
    void commit(std::size_t n) {
        assert(n <= writable());
        w_ += n;
    }

    // 丢弃已读前缀，保留可读内容。仅在需要释放内存或重新开始一轮时调用。
    void compact() {
        if (r_ == 0) return;
        const std::size_t readable = w_ - r_;
        if (readable > 0) std::memmove(buf_.data(), buf_.data() + r_, readable);
        r_ = 0;
        w_ = readable;
    }

private:
    std::vector<char> buf_;
    std::size_t r_ = 0;
    std::size_t w_ = 0;
};

}  // namespace eg
