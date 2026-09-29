/* XRT 传输实现:单 worker 引擎,一切回调(accept/read/close/timer)
 * 在同一根 worker 线程上串行,上层免锁。已验证:xrt-test(Mac↔Linux)。
 */
#pragma once
#include "transport.hpp"
#include <map>
#include <memory>

namespace gs {

struct XrtImpl;

class XrtTransport final : public Transport {
public:
	XrtTransport();
	~XrtTransport() override;

	void setMessageHandler(MessageHandler h) override;
	void setConnEventHandler(ConnEventHandler h) override;

	bool listen(uint16_t port) override;          // server 模式
	bool connect(const char* host, uint16_t port) override;  // client 模式

	bool send(ConnId to, const uint8_t* data, size_t size) override;
	void broadcast(const uint8_t* data, size_t size) override;
	void close(ConnId to) override;

	uint64_t after(uint64_t delayUs, std::function<void()> cb) override;
	bool post(std::function<void()> cb) override;

private:
	std::unique_ptr<XrtImpl> impl_;
};

} // namespace gs
