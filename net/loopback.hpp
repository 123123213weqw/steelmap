/* 进程内环回传输:同进程里把 server/client 逻辑接在一起,
 * 用于核心链路测试与本地单机模式(本机起"服务器")。
 * 注意:after() 用独立线程 + 睡眠实现,回调顺序不保证,
 * 只用于单机/测试,不用于联机路径。
 */
#pragma once
#include "transport.hpp"
#include <atomic>
#include <chrono>
#include <thread>

namespace gs {

class LoopbackTransport final : public Transport {
public:
	~LoopbackTransport() override { running_ = false; }

	void setMessageHandler(MessageHandler h) override { onMessage_ = h; }
	void setConnEventHandler(ConnEventHandler h) override { onEvent_ = h; }

	/* 测试辅助:手动注册一条"已连接"的连接 */
	ConnId addConnection()
	{
		const ConnId id = nextId_++;
		conns_.push_back(id);
		if (onEvent_) {
			onEvent_(id, ConnEvent::Opened);
		}
		return id;
	}

	bool listen(uint16_t) override { return true; }
	bool connect(const char*, uint16_t) override
	{
		addConnection();
		return true;
	}

	bool send(ConnId to, const uint8_t* data, size_t size) override
	{
		return sendTo(to, data, size);
	}

	void broadcast(const uint8_t* data, size_t size) override
	{
		for (ConnId c : conns_) {
			(void)sendTo(c, data, size);
		}
	}

	void close(ConnId to) override
	{
		std::vector<ConnId> keep;
		for (ConnId c : conns_) {
			if (c != to) {
				keep.push_back(c);
			}
		}
		conns_ = std::move(keep);
		if (onEvent_) {
			onEvent_(to, ConnEvent::Closed);
		}
	}

	bool post(std::function<void()> cb) override
	{
		cb();
		return true;
	}

	uint64_t after(uint64_t delayUs, std::function<void()> cb) override
	{
		const uint64_t id = nextTimer_++;
		std::thread([this, id, delayUs, cb]() {
			const auto deadline =
				std::chrono::steady_clock::now()
				+ std::chrono::microseconds(delayUs);
			while (running_.load(std::memory_order_relaxed)
				&& std::chrono::steady_clock::now() < deadline) {
				std::this_thread::sleep_for(
					std::chrono::milliseconds(1));
			}
			if (running_.load(std::memory_order_relaxed)) {
				cb();
			}
			(void)id;
		}).detach();
		return id;
	}

private:
	bool sendTo(ConnId to, const uint8_t* data, size_t size)
	{
		if (onMessage_) {
			onMessage_(to, data, size);
		}
		return true;
	}

	ConnId nextId_ = 1;
	uint64_t nextTimer_ = 1;
	std::atomic<bool> running_{ true };
	std::vector<ConnId> conns_;
	MessageHandler onMessage_;
	ConnEventHandler onEvent_;
};

} // namespace gs
