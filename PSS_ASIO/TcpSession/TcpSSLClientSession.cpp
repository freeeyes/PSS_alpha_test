﻿#ifdef SSL_SUPPORT
#include "TcpSSLClientSession.h"
#include "TcpSSLSession.h"

CTcpSSLClientSession::CTcpSSLClientSession(asio::io_context* io_context) :
    io_context_(io_context),
    ssl_ctx_(asio::ssl::context(asio::ssl::context::sslv23)),
    ssl_socket_(*io_context, ssl_ctx_)
{
}

bool CTcpSSLClientSession::start(const CConnect_IO_Info& io_info)
{
    server_id_ = io_info.server_id;
    packet_parse_id_ = io_info.packet_parse_id;

    session_recv_buffer_.Init(io_info.recv_size);
    session_send_buffer_.Init(io_info.send_size);

    //验证证书的合法性
    asio::error_code pem_ec;

    PSS_LOGGER_DEBUG("[tcp_test_ssl_connect_synchronize_server]pem_ec={0}", io_info.client_pem_file);
    ssl_ctx_.set_verify_mode(asio::ssl::verify_peer | asio::ssl::verify_fail_if_no_peer_cert);
    ssl_ctx_.load_verify_file(io_info.client_pem_file, pem_ec);

    if (pem_ec)
    {
        PSS_LOGGER_DEBUG("[tcp_test_ssl_connect_synchronize_server]error={0}", pem_ec.message());
        return false;
    }

    ssl_socket_.set_verify_mode(asio::ssl::verify_peer);
    ssl_socket_.set_verify_callback(
        std::bind(&CTcpSSLClientSession::verify_certificate, this, _1, _2));

    //建立连接(异步)
    tcp::resolver resolver(*io_context_);
    auto endpoints = resolver.resolve(io_info.server_ip, std::to_string(io_info.server_port));
    asio::error_code connect_error;

    asio::async_connect(ssl_socket_.lowest_layer(), endpoints,
        [this](const std::error_code& error,
            const tcp::endpoint& /*endpoint*/)
        {
            if (!error)
            {
                //连接远程SSL成功，进行异步握手
                handshake();
            }
            else
            {
                //连接远程SSL失败
                PSS_LOGGER_DEBUG("[CTcpSSLClientSession::start]Connect failed:{0}", error.message());

                //发送消息给逻辑块
                App_WorkThreadLogic::instance()->add_frame_events(LOGIC_CONNECT_SERVER_ERROR,
                    server_id_,
                    remote_ip_.m_strClientIP,
                    remote_ip_.m_u2Port,
                    io_type_);
            }
        });

    return true;
}

void CTcpSSLClientSession::close(uint32 connect_id)
{
    auto self(shared_from_this());
    ssl_socket_.lowest_layer().close();

    //输出接收发送字节数
    PSS_LOGGER_DEBUG("[CTcpSSLClientSession::Close]recv:{0}, send:{1}", recv_data_size_, send_data_size_);

    //断开连接
    packet_parse_interface_->packet_disconnect_ptr_(connect_id_, io_type_);

    //发送链接断开消息
    App_WorkThreadLogic::instance()->delete_thread_session(connect_id, remote_ip_, self);
}

void CTcpSSLClientSession::set_write_buffer(uint32 connect_id, const char* data, size_t length)
{
    if (session_send_buffer_.get_buffer_size() <= length)
    {
        //发送些缓冲已经满了
        PSS_LOGGER_DEBUG("[CTcpSession::set_write_buffer]connect_id={} is full.", connect_id);
        return;
    }

    std::memcpy(session_send_buffer_.get_curr_write_ptr(),
        data,
        length);
    session_send_buffer_.set_write_data(length);
}

void CTcpSSLClientSession::do_read()
{
    //接收数据
    auto self(shared_from_this());
    auto connect_id = connect_id_;

    //如果缓冲已满，断开连接，不再接受数据。
    if (session_recv_buffer_.get_buffer_size() == 0)
    {
        //链接断开(缓冲撑满了)
        App_tms::instance()->AddMessage(1, [self, connect_id]() {
            self->close(connect_id);
            });
    }

    ssl_socket_.async_read_some(asio::buffer(session_recv_buffer_.get_curr_write_ptr(), session_recv_buffer_.get_buffer_size()),
        [this, self](std::error_code ec, std::size_t length)
        {
            do_read_some(ec, length);
        });
}

void CTcpSSLClientSession::do_write_immediately(uint32 connect_id, const char* data, size_t length)
{
    //组装发送数据
    auto send_buffer = make_shared<CSendBuffer>();
    send_buffer->data_.append(data, length);
    send_buffer->buffer_length_ = length;

    //异步发送
    auto self(shared_from_this());
    asio::async_write(ssl_socket_, asio::buffer(send_buffer->data_.c_str(), send_buffer->buffer_length_),
        [self, connect_id, send_buffer](std::error_code ec, std::size_t send_length)
        {
            if (ec)
            {
                //暂时不处理
                PSS_LOGGER_DEBUG("[CTcpSSLClientSession::do_write_immediately]({0}), message({1})", connect_id, ec.message());
            }
            else
            {
                self->add_send_finish_size(connect_id, send_length);
            }
        });
}

void CTcpSSLClientSession::do_write(uint32 connect_id)
{
    //组装发送数据
    auto send_buffer = make_shared<CSendBuffer>();
    send_buffer->data_.append(session_send_buffer_.read(), session_send_buffer_.get_write_size());
    send_buffer->buffer_length_ = session_send_buffer_.get_write_size();

    clear_write_buffer();

    //异步发送
    auto self(shared_from_this());
    asio::async_write(ssl_socket_, asio::buffer(send_buffer->data_.c_str(), send_buffer->buffer_length_),
        [self, send_buffer, connect_id](std::error_code ec, std::size_t length)
        {
            if (ec)
            {
                //暂时不处理
                PSS_LOGGER_DEBUG("[CTcpSSLClientSession::do_write]write error({0}).", ec.message());
            }
            else
            {
                self->add_send_finish_size(connect_id, length);
            }
        });
}

void CTcpSSLClientSession::add_send_finish_size(uint32 connect_id, size_t send_length)
{
    send_data_size_ += send_length;
}

EM_CONNECT_IO_TYPE CTcpSSLClientSession::get_io_type()
{
    return io_type_;
}

uint32 CTcpSSLClientSession::get_mark_id(uint32 connect_id)
{
    PSS_UNUSED_ARG(connect_id);
    return server_id_;
}

std::chrono::steady_clock::time_point& CTcpSSLClientSession::get_recv_time()
{
    return recv_data_time_;
}

bool CTcpSSLClientSession::format_send_packet(uint32 connect_id, std::shared_ptr<CMessage_Packet> message, std::shared_ptr<CMessage_Packet> format_message)
{
    return packet_parse_interface_->parse_format_send_buffer_ptr_(connect_id, message, format_message, get_io_type());
}

bool CTcpSSLClientSession::is_need_send_format()
{
    return packet_parse_interface_->is_need_send_format_ptr_();
}

void CTcpSSLClientSession::clear_write_buffer()
{
    session_send_buffer_.move(session_send_buffer_.get_write_size());
}

void CTcpSSLClientSession::do_read_some(std::error_code ec, std::size_t length)
{
    if (!ec)
    {
        recv_data_size_ += length;
        session_recv_buffer_.set_write_data(length);
        PSS_LOGGER_DEBUG("[CTcpSSLClientSession::do_write]recv length={}.", length);

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
            recv_data_time_ = std::chrono::steady_clock::now();

            //添加消息处理
            App_WorkThreadLogic::instance()->assignation_thread_module_logic(connect_id_, message_list, shared_from_this());
        }

        session_recv_buffer_.move(length);
        //继续读数据
        do_read();
    }
    else
    {
        //链接断开
        App_WorkThreadLogic::instance()->close_session_event(connect_id_);
    }
}

bool CTcpSSLClientSession::verify_certificate(bool preverified, asio::ssl::verify_context& ctx)
{
    //证书验证
    char subject_name[500];
    X509* cert = X509_STORE_CTX_get_current_cert(ctx.native_handle());
    X509_NAME_oneline(X509_get_subject_name(cert), subject_name, 500);
    PSS_LOGGER_DEBUG("[CTcpSSLClientSession::verify_certificate]server_id_={0},Verifying={1}", server_id_, subject_name);
    PSS_LOGGER_DEBUG("[CTcpSSLClientSession::verify_certificate]server_id_={0},preverified={1}", server_id_, preverified);

    return preverified;
}

void CTcpSSLClientSession::handshake()
{
    ssl_socket_.async_handshake(asio::ssl::stream_base::client,
        [this](const std::error_code& error)
        {
            if (!error)
            {
                //握手成功，开始准备接收数据
                connect_id_ = App_ConnectCounter::instance()->CreateCounter();

                recv_data_time_ = std::chrono::steady_clock::now();

                packet_parse_interface_ = App_PacketParseLoader::instance()->GetPacketParseInfo(packet_parse_id_);

                //处理链接建立消息
                remote_ip_.m_strClientIP = ssl_socket_.lowest_layer().remote_endpoint().address().to_string();
                remote_ip_.m_u2Port = ssl_socket_.lowest_layer().remote_endpoint().port();
                local_ip_.m_strClientIP = ssl_socket_.lowest_layer().local_endpoint().address().to_string();
                local_ip_.m_u2Port = ssl_socket_.lowest_layer().local_endpoint().port();

                PSS_LOGGER_DEBUG("[CTcpSSLClientSession::start]remote({0}:{1})", remote_ip_.m_strClientIP, remote_ip_.m_u2Port);
                PSS_LOGGER_DEBUG("[CTcpSSLClientSession::start]local({0}:{1})", local_ip_.m_strClientIP, local_ip_.m_u2Port);

                packet_parse_interface_->packet_connect_ptr_(connect_id_, remote_ip_, local_ip_, io_type_);

                //添加映射关系
                App_WorkThreadLogic::instance()->add_thread_session(connect_id_, shared_from_this(), local_ip_, remote_ip_);

                do_read();
            }
            else
            {
                //握手失败
                PSS_LOGGER_DEBUG("[CTcpSSLClientSession::handshake]Handshake failed:{0}", error.message());
            }
        });
}

#endif
