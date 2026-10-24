#include <cosmos/console_stream.hpp>
#include <cosmos/corona.hpp>
#include <cosmos/input.hpp>
#include <cosmos/serial.hpp>

namespace lens {

namespace {

class ConsoleStream final : public Stream {
public:
    const char* name() const override { return "console"; }

    Result<usize> read(Bytes dst) override {
        usize n = 0;
        while (n < dst.len) {
            if (pending_pos_ == pending_len_ && !refill()) break;
            dst.ptr[n++] = pending_[pending_pos_++];
        }
        if (n == 0 && dst.len != 0) return Result<usize>::fail(Status::NotReady);
        return Result<usize>::success(n);
    }

    Result<usize> write(ConstBytes src) override {
        if (src.len == 0) return Result<usize>::success(0);
        usize done = 0;
        while (done < src.len) {
            u32 chunk = src.len - done > 4096 ? 4096 : static_cast<u32>(src.len - done);
            const char* text = reinterpret_cast<const char*>(src.ptr) + done;
            corona::write(text, chunk);
            serial::console_write(text, chunk);
            done += chunk;
        }
        return Result<usize>::success(src.len);
    }

    Readiness ready() override {
        Readiness r = Ready::Writable;
        if (pending_pos_ != pending_len_ || input::pending() != 0) r = r | Ready::Readable;
        return r;
    }

    void retain() override {}
    void release() override {}

private:
    bool refill() {
        input::Key key;
        while (input::try_read(key)) {
            u32 n = 0;
            if (key.is_text()) {
                if (key.ctrl() && key.cp < 0x80) {
                    pending_[n++] = static_cast<char>(key.cp & 0x1F);
                } else {
                    n = utf8::encode(key.cp, pending_);
                }
            } else if (key.sym == input::Sym::Enter) {
                pending_[n++] = '\n';
            } else if (key.sym == input::Sym::Tab) {
                pending_[n++] = '\t';
            } else if (key.sym == input::Sym::Backspace) {
                pending_[n++] = 0x7F;
            } else if (key.sym == input::Sym::Escape) {
                pending_[n++] = 0x1B;
            }
            if (n) {
                pending_len_ = n;
                pending_pos_ = 0;
                return true;
            }
        }
        return false;
    }

    char pending_[4]{};
    u32 pending_len_ = 0;
    u32 pending_pos_ = 0;
};

constinit ConsoleStream g_console;

}

Stream* console_stream() { return &g_console; }

}
