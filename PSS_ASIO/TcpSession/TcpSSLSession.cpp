#include "TcpSSLSession.h"
#ifdef SSL_SUPPORT

CTcpSSLSession::CTcpSSLSession(asio::ssl::stream<tcp::socket> socket) : ssl_socket_(std::move(socket))
{
}

void CTcpSSLSession::open(uint32 packet_parse_id, uint32 recv_size)
{
    connect_id_ = App_ConnectCounter::instance()->CreateCounter();

    packet_parse_interface_ = App_PacketParseLoader::instance()->GetPacketParseInfo(packet_parse_id);

    session_recv_buffer_.Init(recv_size);

    recv_data_time_ = std::chrono::steady_clock::now();

    //处理链接建立消息
    remote_ip_.m_strClientIP = ssl_socket_.lowest_layer().remote_endpoint().address().to_string();
    remote_ip_.m_u2Port = ssl_socket_.lowest_layer().remote_endpoint().port();
    local_ip_.m_strClientIP = ssl_socket_.lowest_layer().local_endpoint().address().to_string();
    local_ip_.m_u2Port = ssl_socket_.lowest_layer().local_endpoint().port();
    packet_parse_interface_->packet_connect_ptr_(connect_id_, remote_ip_, local_ip_, io_type_);

    //加入session 映射
    App_WorkThreadLogic::instance()->add_thread_session(connect_id_, shared_from_this(), local_ip_, remote_ip_);


    do_handshake();
}

void CTcpSSLSession::close(uint32 connect_id)
{
    auto self(shared_from_this());
    ssl_socket_.lowest_layer().close();

    //输出接收发送字节数
    PSS_LOGGER_DEBUG("[CTcpSession::Close]recv:{0}, send:{1} io_send_count:{2}", recv_data_size_, send_data_size_, io_send_count_);

    //断开连接
    packet_parse_interface_->packet_disconnect_ptr_(connect_id, io_type_);

    App_WorkThreadLogic::instance()->delete_thread_session(connect_id, remote_ip_, self);
}

void CTcpSSLSession::set_write_buffer(uint32 connect_id, const char* data, size_t length)
{
    std::lock_guard<std::mutex> lck(send_thread_mutex_);

    session_send_buffer_.append(data, length);
}

void CTcpSSLSession::do_read()
{
    //接收数据
    //如果缓冲已满，断开连接，不再接受数据。
    if (session_recv_buffer_.get_buffer_size() == 0)
    {
        //链接断开(缓冲撑满了)
        App_WorkThreadLogic::instance()->close_session_event(connect_id_);
    }

    ssl_socket_.async_read_some(asio::buffer(session_recv_buffer_.get_curr_write_ptr(), session_recv_buffer_.get_buffer_size()),
        [this](std::error_code ec, std::size_t length)
        {
            do_read_some(ec, length);
        });
}

void CTcpSSLSession::clear_write_buffer()
{
    session_send_buffer_.clear();
}

void CTcpSSLSession::do_read_some(std::error_code ec, std::size_t length)
{
    if (!ec)
    {
        recv_data_size_ += length;
        session_recv_buffer_.set_write_data(length);

        //处理数据拆包
        vector<std::shared_ptr<CMessage_Packet>> message_list;
        bool ret = packet_parse_interface_->packet_from_recv_buffer_ptr_(connect_id_, &session_recv_buffer_, message_list, io_type_);
        if (!ret)
        {
            //链接断开(解析包不正确)
            App_WorkThreadLogic::instance()->close_session_event(connect_id_);
        }
        else
        {
            //更新接收数据时间
            recv_data_time_ = std::chrono::steady_clock::now();

            //添加消息处理
            App_WorkThreadLogic::instance()->assignation_thread_module_logic(connect_id_, message_list, shared_from_this());
        }

        //继续读数据
        do_read();
    }
    else
    {
        //链接断开
        App_WorkThreadLogic::instance()->close_session_event(connect_id_);
    }
}

void CTcpSSLSession::do_write(uint32 connect_id)
{
    std::lock_guard<std::mutex> lck(send_thread_mutex_);

    if (is_send_finish_ == false || session_send_buffer_.size() == 0)
    {
        //上次发送没有完成或者已经发送完成
        return;
    }

    is_send_finish_ = false;

    //组装发送数据
    auto send_buffer = make_shared<CSendBuffer>();
    send_buffer->data_ = session_send_buffer_;
    send_buffer->buffer_length_ = session_send_buffer_.size();

    clear_write_buffer();

    //异步发送
    auto self(shared_from_this());
    asio::async_write(ssl_socket_, asio::buffer(send_buffer->data_.c_str(), send_buffer->buffer_length_),
        [self, send_buffer, connect_id](std::error_code ec, std::size_t length)
        {
            if (ec)
            {
                //暂时不处理
                PSS_LOGGER_DEBUG("[CTcpSession::do_write]write error({0}).", ec.message());
            }
            else
            {
                self->add_send_finish_size(connect_id, length);

                //继续发送
                self->do_write(connect_id);
            }
        });
}

void CTcpSSLSession::do_write_immediately(uint32 connect_id, const char* data, size_t length)
{
    set_write_buffer(connect_id, data, length);
    do_write(connect_id);
}

void CTcpSSLSession::add_send_finish_size(uint32 connect_id, size_t send_length)
{
    std::lock_guard<std::mutex> lck(send_thread_mutex_);

    send_data_size_ += send_length;

    io_send_count_++;

    is_send_finish_ = true;
}

EM_CONNECT_IO_TYPE CTcpSSLSession::get_io_type()
{
    return io_type_;
}

uint32 CTcpSSLSession::get_mark_id(uint32 connect_id)
{
    PSS_UNUSED_ARG(connect_id);
    return 0;
}

std::chrono::steady_clock::time_point& CTcpSSLSession::get_recv_time()
{
    return recv_data_time_;
}

bool CTcpSSLSession::format_send_packet(uint32 connect_id, std::shared_ptr<CMessage_Packet> message, std::shared_ptr<CMessage_Packet> format_message)
{
    return packet_parse_interface_->parse_format_send_buffer_ptr_(connect_id, message, format_message, get_io_type());
}

bool CTcpSSLSession::is_need_send_format()
{
    return packet_parse_interface_->is_need_send_format_ptr_();
}

void CTcpSSLSession::do_handshake()
{
  	std::cout << "[do_handshake] Begin" << std::endl;
    auto self(shared_from_this());
    ssl_socket_.async_handshake(asio::ssl::stream_base::server, 
        [this, self](const std::error_code& error)
        {
          if (!error)
          {
          	std::cout << "[do_handshake] read" << std::endl;
            do_read();
          }
          else
          {
          	std::cout << "[do_handshake] error:" << error.message() << std::endl;
          }
        });
}

#endif
