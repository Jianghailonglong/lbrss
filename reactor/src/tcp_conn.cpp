#include <unistd.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <string.h>

#include "tcp_conn.h"
#include "message.h"
#include "tcp_server.h"

// 回显业务
void callback_busi(const char *data, uint32_t len, int msgid, void *args, TCPConn *conn)
{
    conn->send_message(data, len, msgid);
}


// 连接的读事件回调
static void conn_rd_callback(EventLoop *loop, int fd, void *args)
{
    TCPConn *conn = (TCPConn*)args;
    conn->do_read();
}

// 连接的写事件回调
static void conn_wt_callback(EventLoop *loop, int fd, void *args)
{
    TCPConn *conn = (TCPConn*)args;
    conn->do_write();
}

// 初始化TCPConn
TCPConn::TCPConn(int connfd, EventLoop *loop)
{
    _connfd = connfd;
    _loop = loop;
    // 1、 将connfd设置成非阻塞状态
    int flag = fcntl(_connfd, F_GETFL, 0);
    fcntl(_connfd, F_SETFL, O_NONBLOCK|flag);

    // 2、 设置TCP_NODELAY禁止做读写缓存，降低小包延迟
    int op = 1;
    setsockopt(_connfd, IPPROTO_TCP, TCP_NODELAY, &op, sizeof(op));//need netinet/in.h netinet/tcp.h

    // 3、判断用户是否注册连接建立hook
    if(TCPServer::conn_start_cb)
    {
        TCPServer::conn_start_cb(this, TCPServer::conn_start_cb_args);
    }

    // 4、 将该链接的读事件让event_loop监控 
    _loop->add_io_event(_connfd, conn_rd_callback, EPOLLIN, this);

    // 5、 将该链接集成到对应的tcp_server中
    TCPServer::increase_conn(_connfd, this);
}

int TCPConn::get_fd()
{
    return this->_connfd;
}

// 处理读业务
void TCPConn::do_read()
{
    // 1. 从套接字读取数据
    int ret = ibuf.read_data(_connfd);
    if (ret == -1) {
        fprintf(stderr, "read data from socket\n");
        this->clean_conn();
        return ;
    }
    else if ( ret == 0) {
        // 对端正常关闭
        printf("connection closed by peer\n");
        this->clean_conn();
        return ;
    }

    // 2. 解析msg_head数据    
    MsgHead head;    
    
    // [这里用while，可能一次性读取多个完整包过来]
    while (ibuf.length() >= MESSAGE_HEAD_LEN)  {
        // 2.1 读取msg_head头部，固定长度MESSAGE_HEAD_LEN    
        memcpy(&head, ibuf.data(), MESSAGE_HEAD_LEN);
        if(head.msglen > MESSAGE_LENGTH_LIMIT || head.msglen < 0) {
            fprintf(stderr, "data format error, need close, msglen = %d\n", head.msglen);
            this->clean_conn();
            break;
        }
        if (ibuf.length() < MESSAGE_HEAD_LEN + head.msglen) {
            // 缓存buf中剩余的数据，小于实际上应该接受的数据
            // 说明是一个不完整的包，应该抛弃
            break;
        }

        // 2.2 再根据头长度读取数据体，然后针对数据体处理 业务

        // 头部处理完了，往后偏移MESSAGE_HEAD_LEN长度
        ibuf.pop(MESSAGE_HEAD_LEN);
        
        // 处理ibuf.data()业务数据
        // printf("read data: %s\n", ibuf.data());

        // 消息包路由模式
        TCPServer::router.call(head.msgid, head.msglen, ibuf.data(), this);
        
        // 回显业务
        // callback_busi(ibuf.data(), head.msglen, head.msgid, NULL, this);

        // 消息体处理完了,往后便宜msglen长度
        ibuf.pop(head.msglen);
    }

    ibuf.adjust();
    
    return ;
}

// 处理写业务
void TCPConn::do_write()
{
    // do_write是触发完event事件要处理的事情，
    // 应该是直接将out_buf数据io写会对方客户端 
    // 而不是在这里组装一个message再发
    // 组装message的过程应该是主动调用
    
    // 只要obuf中有数据就写
    while (obuf.length()) {
        int ret = obuf.write2fd(_connfd);
        if (ret == -1) {
            fprintf(stderr, "write2fd error, close conn!\n");
            this->clean_conn();
            return ;
        }
        if (ret == 0) {
            // 不是错误，仅返回0表示不可继续写
            break;
        }
    }

    if (obuf.length() == 0) {
        // 数据已经全部写完，将_connfd的写事件取消掉
        _loop->del_io_event(_connfd, EPOLLOUT);
    }

    return ;
}

// 发送消息的方法
int TCPConn::send_message(const char *data, int msglen, int msgid)
{
    // printf("server send_message: %s:%d, msgid = %d\n", data, msglen, msgid);
    bool active_epollout = false; 
    if(obuf.length() == 0) {
        // 如果现在已经数据都发送完了，那么是一定要激活写事件的
        // 如果有数据，说明数据还没有完全写完到对端，那么没必要再激活等写完再激活
        active_epollout = true;
    }

    // 1 先封装message消息头
    MsgHead head;
    head.msgid = msgid;
    head.msglen = msglen;
     
    // 1.1 写消息头
    int ret = obuf.send_data((const char *)&head, MESSAGE_HEAD_LEN);
    if (ret != 0) {
        fprintf(stderr, "send head error\n");
        return -1;
    }

    // 1.2 写消息体
    ret = obuf.send_data(data, msglen);
    if (ret != 0) {
        // 如果写消息体失败，那就回滚将消息头的发送也取消
        obuf.pop(MESSAGE_HEAD_LEN);
        return -1;
    }

    if (active_epollout == true) {
        // 2. 激活EPOLLOUT写事件
        _loop->add_io_event(_connfd, conn_wt_callback, EPOLLOUT, this);
    }


    return 0;
}

// 销毁tcp_conn
void TCPConn::clean_conn()
{
    // 如果注册了连接销毁函数，则调用
    if(TCPServer::conn_close_cb)
    {
        TCPServer::conn_close_cb(this, TCPServer::conn_close_cb_args);
    }

    // 链接清理工作
    // 1 将该链接从tcp_server摘除掉    
    TCPServer::decrease_conn(_connfd);
    // 2 将该链接从event_loop中摘除
    _loop->del_io_event(_connfd);
    // 3 buf清空
    ibuf.clear(); 
    obuf.clear();
    // 4 关闭原始套接字
    int fd = _connfd;
    _connfd = -1;
    close(fd);
}