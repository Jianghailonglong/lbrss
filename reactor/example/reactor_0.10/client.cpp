#include "udp_client.h"
#include <stdio.h>
#include <string.h>

//客户端业务
void busi(const char *data, uint32_t len, int msgid, NetConnection  *conn, void *user_data)
{
    //得到服务端回执的数据 
    char *str = NULL;
    
    str = (char*)malloc(len+1);
    memset(str, 0, len+1);
    memcpy(str, data, len);
    printf("recv server: [%s]\n", str);
    printf("msgid: [%d]\n", msgid);
    printf("len: [%d]\n", len);
}



int main() 
{
    EventLoop loop;

    //创建UDP客户端
    UDPClient client(&loop, "127.0.0.1", 7777);

    //注册消息路由业务
    client.add_msg_router(1, busi);

    //发消息
    int msgid = 1;
    const char *msg = "Hello UDP server";

    client.send_message(msg, strlen(msg), msgid);

    //开启事件监听
    loop.event_process();

    return 0;
}