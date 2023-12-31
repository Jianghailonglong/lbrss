#include "reactor.h"
#include "main_server.h"
#include <string>
#include <pthread.h>

// 只要thread_queue有数据，loop就会触发此回调函数来处理业务
void new_report_request(EventLoop *loop, int fd, void *args)
{
    TCPClient *client = (TCPClient*)args;

    //1. 将请求数据从thread_queue中取出，
    std::queue<lbrss::ReportStatusRequest>  msgs;

    //2. 将数据放在queue队列中
    report_queue->recv(msgs);
    
    //3. 遍历队列，通过client依次将每个msg发送给reporter service
    while (!msgs.empty()) {
        lbrss::ReportStatusRequest req = msgs.front();
        msgs.pop();

        std::string requestString;
        req.SerializeToString(&requestString);

        //client 发送数据
        client->send_message(requestString.c_str(), requestString.size(), lbrss::ID_ReportStatusRequest);
    }
}


void *report_client_thread(void* args)
{
    printf("report client thread start\n");
    
    EventLoop loop;

    //1 加载配置文件得到repoter ip + port
    std::string ip = ConfigFile::instance()->GetString("reporter", "ip", "");
    short port = ConfigFile::instance()->GetNumber("reporter", "port", 0);

    //2 创建客户端
    TCPClient client(&loop, ip.c_str(), port, "reporter client");

    //3 将 thread_queue消息回调事件，绑定到loop中
    report_queue->set_loop(&loop);
    report_queue->set_callback(new_report_request, &client);

    //4 启动事件监听
    loop.event_process();

    return NULL;
}

void start_report_client()
{
    //开辟一个线程
    pthread_t tid;

    //启动线程业务函数
    int ret = pthread_create(&tid, NULL, report_client_thread, NULL);
    if (ret == -1) {
        perror("pthread_create");
        exit(1);
    }

    //设置分离模式
    pthread_detach(tid);
}