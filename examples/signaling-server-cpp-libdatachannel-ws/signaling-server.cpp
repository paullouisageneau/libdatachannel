/**
 * signaling server example for libdatachannel
 * Copyright (c) 2023 Tim Schneider
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

#include <rtc/rtc.h>

#include <ctype.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <nlohmann/json.hpp>

#define MAX_PATH_LENGTH 1024
#define MAX_CLIENTS 1024

struct ConnectedClients 
{ 
    int ids[MAX_CLIENTS];
    char* client[MAX_CLIENTS];
} my_ConnectedClients;

static void RTC_API my_rtcOpenCallbackFunc(int id, void *ptr){
    fprintf(stdout, "websocket #%d opened\n", id);
    
    char path[MAX_PATH_LENGTH];
    memset(path, 0, MAX_PATH_LENGTH);
    int size = rtcGetWebSocketPath(id, path, MAX_PATH_LENGTH);
    path[size] = 0;
    fprintf(stdout, "path:%s\n", path);

    char* user = "";
    const char* delim = "/";
    char * segment = strtok(path, delim);
    while(segment != NULL){
        user = (char*)segment;
        segment = strtok(NULL, delim);
    }
    fprintf(stdout, "new user login:%s\n", user);
    
    // login user
    for (size_t i = 0; i <= MAX_CLIENTS; i++)
    {
        if(i == MAX_CLIENTS){
            const char MSG[] = "Too many clients connected already, try again after some Client leaves\n";
            fprintf(stderr, MSG);
            rtcSendMessage(id, MSG, -(int)sizeof(MSG)); // negative size to send the message as text (not binary)
            rtcClose(id);
            return;
        }
        
        if(my_ConnectedClients.ids[i] <= 0 ){
            my_ConnectedClients.ids[i] = id;
            size_t len = strlen(user);
            my_ConnectedClients.client[i] = (char*) malloc(len+1);
            strncpy(my_ConnectedClients.client[i], user, len);
            ((char*)my_ConnectedClients.client[i])[len]=0; 
            break;
        }
    }
}

static void RTC_API my_rtcClosedCallbackFunc(int id, void* ptr){
    fprintf(stdout, "websocket #%d closed\n", id);
    for (size_t i = 0; i < MAX_CLIENTS; i++)
    {
        if(my_ConnectedClients.ids[i] != id) continue;

        my_ConnectedClients.ids[i] = -1;
        free(my_ConnectedClients.client[i]);
        return;
    }
    rtcDeleteWebSocket(id);
}

static void RTC_API my_rtcErrorCallbackFunc(int id, const char *error, void *ptr){
    fprintf(stderr, "websocket #%d error: %s\n", id, error);
    rtcClose(id);
}

static void RTC_API my_rtcMessageCallback(int id, const char *message, int size, void *ptr){
    bool is_binary = size > 0;
    fprintf(stdout, "message (%s) from websocket #%d (len %d):%s\n", is_binary?"binary":"text", id, size, message);

    using nlohmann::json;
    try
    {
        json j = json::parse(message);
        if(j.contains("id")){
            std::string dest_id = j["id"];
            int i_dest = -1;
            int i_src = -1;
            for (int i = 0; i < MAX_CLIENTS; i++)
            {
                if(my_ConnectedClients.ids[i] < 0) continue;
                if(my_ConnectedClients.ids[i] == id) i_src = i;
                if(i_dest < 0 && strcmp(my_ConnectedClients.client[i], dest_id.data()) == 0) i_dest = i;
                if(i_dest >= 0 && i_src >= 0) break;
            }
            
            if(i_dest < 0){
                fprintf(stderr, "No client %s connected.\n", dest_id.data());
                return;
            }
            if(i_src >= 0){
                j["id"] = std::string(my_ConnectedClients.client[i_src]);
                auto const new_message = j.dump();
                int sent = rtcSendMessage(my_ConnectedClients.ids[i_dest], new_message.data(), -(int)new_message.size());
                return;
            }
        };
    }
    catch (json::parse_error& ex)
    {
        fprintf(stderr, "parse error at byte %zd of %s\n", ex.byte, message);
    }
    // if either no json or no matching client found
}

static void RTC_API my_rtcWebSocketClientCallbackFunc(int wsserver, int ws, void *user_ptr){
    fprintf(stdout, "websocket #%d connected to websocket-server %d\n",  ws, wsserver);

    rtcSetUserPointer(ws, &my_ConnectedClients);
    rtcSetOpenCallback(ws, my_rtcOpenCallbackFunc);
    rtcSetMessageCallback(ws, my_rtcMessageCallback);
    rtcSetClosedCallback(ws, my_rtcClosedCallbackFunc);
    rtcSetErrorCallback(ws, my_rtcErrorCallbackFunc);
}

int main(int argc, char* argv[])
{
    // Check command line arguments.
    if (argc != 2)
    {
        fprintf( stderr, 
         "Usage: %s <port>\n" 
         "Example:\n"
         "    %s 8000 \n"
        , argv[0], argv[0]);
        return EXIT_FAILURE;
    }
    const auto port = atoi(argv[1]);
    memset(&my_ConnectedClients, 0, sizeof(my_ConnectedClients));

    rtcWsServerConfiguration config;
    config.port = port;
    config.enableTls = false;
    config.certificatePemFile = NULL;
    config.keyPemFile = NULL;
    config.keyPemPass = NULL;

    int wsserver = rtcCreateWebSocketServer(&config, my_rtcWebSocketClientCallbackFunc);

    if(wsserver < 0){
        fprintf(stderr, "Error creating WebsocketServer");
        return EXIT_FAILURE;
    }

    printf("press any key to exit...\n");
    int c = getchar();
    rtcDeleteWebSocketServer(wsserver);

    return EXIT_SUCCESS;
}