/* 传输层抽象:core/server 不直接依赖 XRT。
 * 换传输实现(XRT / 回环 / 未来的 KCP)不动上层一行代码。
 *
 * 线程契约:消息回调、连接回调、after 定时回调运行在同一根线程上
 * (XRT:engine worker;Loopback:投递线程/定时线程),因此上层回调里
 * 的状态访问无需加锁。listen/connect 由主线程在事件开始前调用。
 */
#pragma once
#include <cstddef>
#include <cstdint>
#include <functional>
#include <vector>

namespace gs {

using ConnId = uint32_t;
constexpr ConnId INVALID_CONN = 0;

enum class ConnEvent : uint8_t {
	Opened,
	Closed,
};

/* 收到完整应用层消息(帧头已剥离):[u16 消息类型][载荷] */
using MessageHandler =
	std::function<void(ConnId, const uint8_t* data, size_t size)>;
using ConnEventHandler = std::function<void(ConnId, ConnEvent)>;

class Transport {
public:
	virtual ~Transport() = default;

	virtual void setMessageHandler(MessageHandler h) = 0;
	virtual void setConnEventHandler(ConnEventHandler h) = 0;

	/* 连接维度:server 监听后连接陆续到来;client 只有 1 条 */
	virtual bool listen(uint16_t port) = 0;
	virtual bool connect(const char* host, uint16_t port) = 0;

	virtual bool send(ConnId to, const uint8_t* data, size_t size) = 0;
	virtual void broadcast(const uint8_t* data, size_t size) = 0;
	virtual void close(ConnId to) = 0;

	/* 延时回调(微秒),返回定时器 id,失败返回 0。与消息回调同线程。 */
	virtual uint64_t after(uint64_t delayUs, std::function<void()> cb) = 0;

	/* 跨线程投递任务到回调线程执行(xrtNetStreamSend 只能在
	 * worker 线程调用,跨线程静默丢弃——已实测,勿绕) */
	virtual bool post(std::function<void()> cb) = 0;
};

} // namespace gs
