//***************************************************************
//	created:	2020/08/01
//	author:		arthur wong
//***************************************************************

#pragma once


#include <memory>
#include <string>
#include <chrono>
#include <type_traits>

namespace knet {
	namespace tcp {

		using asio::ip::tcp;

		template <class T>
		class TcpSocket : public std::enable_shared_from_this<TcpSocket<T>> {
		public:
			enum class SocketStatus {
				SOCKET_IDLE,
				SOCKET_INIT = 1,
				SOCKET_OPEN,
				SOCKET_CLOSING,
				SOCKET_RECONNECT,
				SOCKET_CLOSED,
			};
			enum { kReadBufferSize = 1024 * 8, kMaxPackageLimit = 8 * 1024  };
			using TPtr = std::shared_ptr<T>;//NOTICY not weak_ptr 
			TcpSocket(const std::thread::id& tid, asio::io_context& ctx, void* = nullptr)
				: io_context(ctx)
				, tcp_sock(ctx) {
				worker_tid = tid;
				m.status = SocketStatus::SOCKET_INIT; 
				m.send_buffer.reserve(1024); 
				m.cache_buffer.reserve(1024); 
			}

			void init(TPtr conn) {
				m.connection = conn;
			}

			bool connect(const std::string& host, uint32_t port, const std::string& localAddr = "0.0.0.0", uint32_t localPort = 0) {
				tcp::resolver resolver(io_context);
				auto result = resolver.resolve(host, std::to_string(port));
				dlog("connect to server {}:{}", host.c_str(), port);
				auto self = this->shared_from_this();
				if (localPort > 0) {
					asio::ip::tcp::endpoint laddr(asio::ip::make_address(localAddr), localPort);
					tcp_sock.bind(laddr);
				}

				async_connect(tcp_sock, result,
					[self, host, port ](asio::error_code ec, typename decltype(result)::endpoint_type endpoint) {
						if (!ec) {
							self->init_read(); 
						}else {
							dlog("connect to server failed, {}:{}", host.c_str(), port);
							self->tcp_sock.close();
						}
					});
				return true;
			}

			template <class F>
			void run_inloop(const F & fn) {
				asio::dispatch(io_context, fn);
			}
			void init_read(){
				if (m.connection) {
					m.connection->process_event(EVT_CONNECT);
					do_read(); 
				}				
			}

			void do_read() {
				if (tcp_sock.is_open() ) {					
					m.status = SocketStatus::SOCKET_OPEN;
					auto self = this->shared_from_this();
					auto buf = asio::buffer((char*)m.read_buffer + read_buffer_pos, kReadBufferSize - read_buffer_pos);
					if (kReadBufferSize > read_buffer_pos){
						tcp_sock.async_read_some(
							buf, [this, self](std::error_code ec, std::size_t bytes_transferred) {
								if (!ec) {
									dlog("received data length {}", bytes_transferred);								 
									process_data(bytes_transferred);
									self->do_read();
								}
								else {
									dlog("read error, close connection {} ", ec.value());
									do_close();
								}
							});

					}else {						
						wlog("read buffer {} is full, increase your receive buffer size,read pos is {}", kReadBufferSize, read_buffer_pos); 
						process_data(0);
						if (read_buffer_pos >= kReadBufferSize ){
							//packet size exceed the limit, so we close it. 
							self->do_close();
						}						
					}	
				}
				else {
					dlog("socket is not open");
				}
			}

			int32_t send_inloop(const char* pData, uint32_t dataLen) {
				asio::async_write(tcp_sock, asio::buffer(pData, dataLen), [this](std::error_code ec, std::size_t length) {
					if (ec) {
						//elog("send in loop error : {} , {}", ec, ec.message());
						this->do_close();
					}
					});
				return 0;
			}


			int32_t send(const char* pData, uint32_t dataLen) { 
				if (is_inloop()) {
					return send_inloop(pData, dataLen);
				}
				return msend(std::string(pData, dataLen));
			}

			int32_t send(const std::string& msg) {
				if (is_inloop()) {
					return send_inloop(msg.data(), msg.length());
				}
				return msend(std::string(msg.data(), msg.length()));
			}
			
			template <typename P> 
			inline bool is_empty(const P & param) {
				return false; 
			}
			inline bool is_empty(const std::string & param) {
				return param.empty(); 
			}
			inline bool is_empty(const char *  param) {
				return strlen(param) == 0 ; 
			}
			template <class P, class... Args>
			int32_t msend(const P& first, const Args&... rest) {
				if (tcp_sock.is_open()) {
					if (!is_empty(first) ) {
						this->mpush(first, rest...);					 
					}
					return 0;
				}
				return -1;
			}

		template <typename P >  
 		inline void write_data  (  const P & data,std::true_type ){
				m.send_buffer.append(std::string((const char*)&data, sizeof(P)));
			}
  

		template <typename P >  
			inline void write_data( const P &  data, std::false_type){
				m.send_buffer.append(data); 
			}
		

			inline void write_data( const std::string &  data, std::false_type){
				m.send_buffer.append(data ); 
			}

			template <typename F, typename ... Args>
			void mpush(const F &  data, Args... rest) {  
				//std::lock_guard<std::mutex> guard(m.mutex); 						 
				m.mutex.lock(); 
				this->write_data<F>(data, std::is_integral<F> () );  
				m.mutex.unlock(); 
				mpush(rest...);
			}

			void mpush() {
				auto self = this->shared_from_this();
				asio::post(io_context, [this, self]() {
					if (tcp_sock.is_open()) {
						if (m.cache_buffer.empty()) {
							do_async_write();
						}
					}
					});
			}

			bool do_async_write() {

				if (m.cache_buffer.empty())
				{
					if (m.mutex.try_lock()) {
						m.send_buffer.swap(m.cache_buffer);
						m.mutex.unlock();
					} 
				} 


				if (!m.cache_buffer.empty())
				{
					auto self = this->shared_from_this();
					asio::async_write(tcp_sock,asio::buffer(m.cache_buffer.data(), m.cache_buffer.size()),
						[this, self](std::error_code ec, std::size_t length) {
							if (!ec && tcp_sock.is_open() && length > 0) {
								//	m.connection->process_event(EVT_SEND);
								m.cache_buffer.clear(); 
								if (m.send_buffer.size() == 0) {
									return;
								}
								self->do_async_write();
							}else {
								self->do_close();
							}
						});

				}
				return true;
			}
 
			bool is_open() {
				//		dlog("status is {}", static_cast<uint32_t>(m.status ));
				return tcp_sock.is_open() && m.status == SocketStatus::SOCKET_OPEN;
			}

			bool process_data(uint32_t nread) {
				if (!m.connection  || nread <= 0) {
					return false;
				}
			 	m.connection->process_event(EVT_RECV);				
				read_buffer_pos += nread; 
				int32_t pkgLen = this->m.connection->process_package((char*)m.read_buffer, read_buffer_pos); 
				if (pkgLen > kReadBufferSize) {
					elog("single packet size ({}) error, close connection", pkgLen);
					m.connection->close();
					return false;
				}

				uint32_t readPos = 0;
				while (pkgLen > 0) {
					dlog("process data package size is {} ,read buffer pos is {}  readPos is {}", pkgLen, read_buffer_pos, readPos);
					if (readPos + pkgLen <= read_buffer_pos) {
						char* pkgEnd = (char*)m.read_buffer + readPos + pkgLen + 1;
						char endChar = *pkgEnd;
						*pkgEnd = 0;
						this->m.connection->process_data(std::string((const char*)m.read_buffer + readPos, pkgLen));
						*pkgEnd = endChar;
						readPos += pkgLen;
					}else {
						break;
					}

					if (readPos < read_buffer_pos) {
						pkgLen = this->m.connection->process_package( (char*)m.read_buffer + readPos, read_buffer_pos - readPos);
						if (pkgLen > kReadBufferSize) {
							elog("single packet size ({}) error, close connection", pkgLen);
							m.connection->close();
						}else if (pkgLen <= 0) {
							dlog("moving buffer to front {} ", read_buffer_pos - readPos);
							memmove(m.read_buffer, (const char*)m.read_buffer + readPos, read_buffer_pos - readPos);
							read_buffer_pos -= readPos;
							break;
						}
					}else {
						read_buffer_pos = 0;
						break;
					}
				} 
				return true; 
			}

			void close() { do_close(true); }
			void do_close(bool force = false) {

				if (m.status == SocketStatus::SOCKET_CLOSING || m.status == SocketStatus::SOCKET_CLOSED) {
					dlog("already in closing status {}", m.status);
					return;
				}

				if (force) {
					m.status = SocketStatus::SOCKET_CLOSING;
				}

				auto self = this->shared_from_this();
				if (m.send_buffer.size() > 0 && !m.send_buffer.empty()) {
					do_async_write(); //try last write
				}
				asio::post(io_context, [this, self]() {
					   auto & conn = self->m.connection; 
						if (conn) {
							conn->process_event(EVT_DISCONNECT);					
							if (conn->need_reconnect()) {
								self->m.status = SocketStatus::SOCKET_RECONNECT;
							}else {
								self->m.status = SocketStatus::SOCKET_CLOSED;
								if (tcp_sock.is_open()) {
									tcp_sock.close();
								}
								conn->release();
								conn.reset();
							}
						}else {
							self->m.status = SocketStatus::SOCKET_CLOSED;
							if (tcp_sock.is_open()) {
								tcp_sock.close();							
							}
						}
						self->read_buffer_pos = 0;
					});
			}

			tcp::endpoint local_endpoint() {
				return tcp_sock.local_endpoint();
			}

			tcp::endpoint remote_endpoint() {
				return tcp_sock.remote_endpoint();
			}

			inline tcp::socket& socket() { return tcp_sock; }

			inline bool is_inloop() {
				return worker_tid == std::this_thread::get_id();
			}

			inline asio::io_context& context() { return io_context; }
		private:
			asio::io_context& io_context;
			tcp::socket tcp_sock;				
			struct {
				TPtr connection;
				char read_buffer[kReadBufferSize+4];
				std::mutex mutex; 
				std::string send_buffer; 
				std::string cache_buffer; 
				SocketStatus status = SocketStatus::SOCKET_IDLE;
			} m;

			std::thread::id worker_tid;
			uint32_t read_buffer_pos = 0;
			uint32_t need_package_length = 0;
		};


	} // namespace tcp

}; // namespace knet
