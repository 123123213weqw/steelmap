/* 应用层帧:[u32 帧长][消息内容]。
 * 拆装是传输实现的责任:TCP 字节流进来,完整帧出去。
 * 消息内容本身(类型+载荷)由 protocol.hpp 定义。
 */
#pragma once
#include "../core/bytes.hpp"
#include <cstdint>
#include <cstring>
#include <vector>

namespace gs {

constexpr uint32_t FRAME_MAX = 16u * 1024u * 1024u;   // 16MB 上限,防恶意长度

inline void frameAppend(std::vector<uint8_t>& wire,
	const uint8_t* msg, size_t msgSize)
{
	putU32(wire, (uint32_t)msgSize);
	wire.insert(wire.end(), msg, msg + msgSize);
}

/* 喂入任意长字节,产出零或多条完整消息(帧头已剥离) */
class FrameDecoder {
public:
	void feed(const uint8_t* data, size_t size,
		std::vector<std::vector<uint8_t>>& outMsgs)
	{
		buf_.insert(buf_.end(), data, data + size);
		for (;;) {
			if (buf_.size() < 4) {
				break;
			}
			uint32_t len;
			memcpy(&len, buf_.data(), 4);          // 机器小端假设,与 putU32 对偶
			if (len > FRAME_MAX) {
				broken_ = true;
				return;
			}
			if (buf_.size() < 4 + (size_t)len) {
				break;
			}
			outMsgs.emplace_back(buf_.begin() + 4,
				buf_.begin() + 4 + (long)len);
			buf_.erase(buf_.begin(),
				buf_.begin() + (long)(4 + (size_t)len));
		}
	}

	bool broken() const { return broken_; }
	size_t buffered() const { return buf_.size(); }

private:
	std::vector<uint8_t> buf_;
	bool broken_ = false;
};

} // namespace gs
