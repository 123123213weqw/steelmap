/* XRT C API 的 C++ 封装。只动这一层就能换掉整个传输实现。
 * 线程模型:XRT 单 worker,以下所有回调与 after() 定时器同线程串行。
 */
#include "xrt_transport.hpp"
#include "frame.hpp"
#include <cstring>
#include <xrt.h>

namespace gs {

struct TimerCb {
	std::function<void()> fn;
	XrtImpl* impl;
};

struct XrtImpl {
	xnetengine* engine = nullptr;
	xnetserver* server = nullptr;      // server 模式
	bool started = false;

	ConnId nextConn = 1;
	std::map<ConnId, xnetstream*> streams;
	std::map<xnetstream*, ConnId> byStream;
	std::map<ConnId, FrameDecoder> decoders;
	/* Close 当拍销毁仍嫌早(XRT 内部收尾未完,UAF);隔一拍再销毁 */
	std::vector<xnetstream*> deadStreams;
	std::vector<xnetstream*> dyingStreams;

	MessageHandler onMsg;
	ConnEventHandler onEvt;

	/* ---- worker 线程上的 XRT 回调,pData 恒为本结构指针 ---- */

	static void streamOpen(xnetstream* s, ptr data)
	{
		XrtImpl* self = (XrtImpl*)data;
		if (self->onEvt) {
			self->onEvt(self->byStream[s], ConnEvent::Opened);
		}
	}

	/* 官方范式:Read 回调直接消费 pBuffer(参见 xrt examples tcp_server)。
	 * 历史教训:走 xrtNetStreamRead 会丢每个完成式读取单元的首块数据,
	 * 小消息恰好不可见,大帧(>ReadSize)必丢——勿改回。
	 */
	static void streamRead(xnetstream* s, xnetbuf* pBuffer, ptr data)
	{
		XrtImpl* self = (XrtImpl*)data;
		const ConnId c = self->byStream[s];
		FrameDecoder& dec = self->decoders[c];
		std::vector<std::vector<uint8_t>> msgs;
		std::vector<xnetspan> spans(
			xrtNetBufSpanCount(pBuffer));
		const size_t got = xrtNetBufSpans(pBuffer,
			spans.empty() ? nullptr : spans.data(), spans.size());
		for (size_t i = 0; i < got; i++) {
			dec.feed((const uint8_t*)spans[i].Data, spans[i].Size,
				msgs);
		}
		(void)xrtNetBufConsume(pBuffer, xrtNetBufSize(pBuffer));

		if (dec.broken()) {
			self->onEvt(c, ConnEvent::Closed);
			return;
		}
		if (self->onMsg) {
			for (auto& m : msgs) {
				self->onMsg(c, m.data(), m.size());
			}
		}
	}

	/* 教训:Close 回调里立即 Destroy 会让 XRT 内部访问已释放对象(堆损坏,
	 * 下一个 broadcast 崩溃)。只记账,销毁延迟到定时器节拍/析构。
	 */
	static void streamClose(xnetstream* s, xnetresult, const xerror*, ptr data)
	{
		XrtImpl* self = (XrtImpl*)data;
		const auto it = self->byStream.find(s);
		if (it == self->byStream.end()) {
			return;                  /* 幂等:重复 Close 直接忽略 */
		}
		const ConnId c = it->second;
		self->byStream.erase(it);
		self->streams.erase(c);
		self->decoders.erase(c);
		self->deadStreams.push_back(s);
		if (self->onEvt) {
			self->onEvt(c, ConnEvent::Closed);
		}
	}

	static bool serverAccept(xnetserver*, size_t, xnetstream* s, ptr data)
	{
		XrtImpl* self = (XrtImpl*)data;
		const ConnId c = self->nextConn++;
		self->streams[c] = s;
		self->byStream[s] = c;
		self->decoders[c];
		if (self->onEvt) {
			self->onEvt(c, ConnEvent::Opened);
		}
		return true;                   // true = 接管 Stream 引用
	}

	static void drainDead(XrtImpl* self)
	{
		for (xnetstream* s : self->dyingStreams) {
			xrtNetStreamDestroy(s);          /* 上一拍的,现在安全 */
		}
		self->dyingStreams.clear();
		self->dyingStreams.swap(self->deadStreams);
	}

	static void postFire(xnetworker*, ptr data)
	{
		TimerCb* t = (TimerCb*)data;
		t->fn();
		delete t;
	}

	static void timerFire(xnetworker*, uint64, xnetresult, ptr data)
	{
		TimerCb* t = (TimerCb*)data;
		t->fn();                          /* 先跑完本拍(含广播) */
		drainDead(t->impl);               /* 再销毁死流,避开重入窗口 */
		delete t;
	}

	bool startEngine()
	{
		xnetengineconfig cfg;
		xrtNetEngineConfigInit(&cfg);
		cfg.Workers = 1;               // 单 worker:回调全在同一线程
		engine = xrtNetEngineCreate(&cfg);
		started = engine != nullptr && xrtNetEngineStart(engine);
		return started;
	}

	bool sendWire(xnetstream* s, const uint8_t* data, size_t size)
	{
		std::vector<uint8_t> wire;
		wire.reserve(size + 4);
		frameAppend(wire, data, size);
		return xrtNetStreamSend(s, wire.data(), wire.size())
			== XNET_RESULT_OK;
	}
};

XrtTransport::XrtTransport()
{
	impl_ = std::make_unique<XrtImpl>();
}

XrtTransport::~XrtTransport()
{
	if (impl_->engine != nullptr) {
		XrtImpl::drainDead(impl_.get());
		(void)xrtNetEngineDestroy(impl_->engine);
	}
}

void XrtTransport::setMessageHandler(MessageHandler h)
{
	impl_->onMsg = std::move(h);
}

void XrtTransport::setConnEventHandler(ConnEventHandler h)
{
	impl_->onEvt = std::move(h);
}

bool XrtTransport::listen(uint16_t port)
{
	if (!impl_->startEngine()) {
		return false;
	}
	static const xnetserverevents sev = {
		XrtImpl::serverAccept, nullptr, nullptr
	};
	static const xnetstreamevents tev = {
		XrtImpl::streamOpen, XrtImpl::streamRead, nullptr,
		nullptr, nullptr, nullptr, XrtImpl::streamClose
	};
	xnetserverconfig cfg;
	xrtNetServerConfigInit(&cfg);
	if (!xrtNetAddrAny(&cfg.Listen.Address, XNET_FAMILY_IPV4, port)) {
		return false;
	}
	impl_->server = xrtNetServerStart(
		impl_->engine, &cfg, &sev, &tev, (ptr)impl_.get());
	return impl_->server != nullptr;
}

bool XrtTransport::connect(const char* host, uint16_t port)
{
	if (!impl_->startEngine()) {
		return false;
	}
	static const xnetstreamevents tev = {
		XrtImpl::streamOpen, XrtImpl::streamRead, nullptr,
		nullptr, nullptr, nullptr, XrtImpl::streamClose
	};
	xnetaddr addr;
	if (!xrtNetAddrParse(&addr, (cstr)host, port)) {
		return false;
	}
	xnetstream* s = xrtNetStreamConnect(
		impl_->engine, &addr, 0, NULL, &tev, (ptr)impl_.get());
	if (s == nullptr) {
		return false;
	}
	impl_->streams[1] = s;
	impl_->byStream[s] = 1;
	impl_->decoders[1];
	return true;
}

bool XrtTransport::send(ConnId to, const uint8_t* data, size_t size)
{
	const auto it = impl_->streams.find(to);
	if (it == impl_->streams.end()) {
		return false;
	}
	return impl_->sendWire(it->second, data, size);
}

void XrtTransport::broadcast(const uint8_t* data, size_t size)
{
	/* 教训:send 在 worker 线程上会被 XRT 重入派发 Close 回调,
	 * 回调里 erase streams 会炸掉正在进行的迭代。先拍快照再发。
	 */
	std::vector<xnetstream*> targets;
	targets.reserve(impl_->streams.size());
	for (auto& kv : impl_->streams) {
		targets.push_back(kv.second);
	}
	for (xnetstream* s : targets) {
		(void)impl_->sendWire(s, data, size);
	}
}

void XrtTransport::close(ConnId to)
{
	const auto it = impl_->streams.find(to);
	if (it != impl_->streams.end()) {
		(void)xrtNetStreamClose(it->second);   // Close 回调里清理映射
	}
}

bool XrtTransport::post(std::function<void()> cb)
{
	TimerCb* t = new TimerCb{ std::move(cb), impl_.get() };
	if (xrtNetEnginePost(impl_->engine, 0, XrtImpl::postFire, (ptr)t)) {
		return true;
	}
	delete t;
	return false;
}

uint64_t XrtTransport::after(uint64_t delayUs, std::function<void()> cb)
{
	TimerCb* t = new TimerCb{ std::move(cb), impl_.get() };
	const uint64_t id = xrtNetEngineAfter(
		impl_->engine, 0, delayUs, XrtImpl::timerFire, (ptr)t);
	if (id == 0) {
		delete t;
	}
	return id;
}

} // namespace gs
